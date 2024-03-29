#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <linux/if_link.h>

#include "mempool.h"
#include "events.h"
#include "log.h"
#include "ppp.h"
#include "pwdb.h"
#include "ipdb.h"
#include "ppp_auth.h"

#include "radius_p.h"
#include "attr_defs.h"

#include "memdebug.h"

int conf_max_try = 3;
int conf_timeout = 3;
int conf_acct_timeout = 3;
int conf_acct_delay_time;
char *conf_nas_identifier;
in_addr_t conf_nas_ip_address;
in_addr_t conf_gw_ip_address;
in_addr_t conf_bind;
int conf_verbose;
int conf_interim_verbose;

in_addr_t conf_dm_coa_server;
int conf_dm_coa_port = 3799;
char *conf_dm_coa_secret;

int conf_sid_in_auth;
int conf_require_nas_ident;
int conf_acct_interim_interval;

int conf_accounting;
int conf_fail_time;
int conf_req_limit;

static const char *conf_default_realm;
static int conf_default_realm_len;

const char *conf_attr_tunnel_type;

static LIST_HEAD(sessions);
static pthread_rwlock_t sessions_lock = PTHREAD_RWLOCK_INITIALIZER;

static void *pd_key;
static struct ipdb_t ipdb;

static mempool_t rpd_pool;
static mempool_t auth_ctx_pool;

int rad_proc_attrs(struct rad_req_t *req)
{
	struct rad_attr_t *attr;
	struct ipv6db_addr_t *a;
	struct ev_dns_t dns;
	struct ev_wins_t wins;
	int res = 0;

	dns.ses = NULL;
	wins.ses = NULL;
	req->rpd->acct_interim_interval = conf_acct_interim_interval;

	list_for_each_entry(attr, &req->reply->attrs, entry) {
		if (attr->vendor && attr->vendor->id == Vendor_Microsoft) {
			switch (attr->attr->id) {
				case MS_Primary_DNS_Server:
					dns.ses = req->rpd->ses;
					dns.dns1 = attr->val.ipaddr;
					break;
				case MS_Secondary_DNS_Server:
					dns.ses = req->rpd->ses;
					dns.dns2 = attr->val.ipaddr;
					break;
				case MS_Primary_NBNS_Server:
					wins.ses = req->rpd->ses;
					wins.wins1 = attr->val.ipaddr;
					break;
				case MS_Secondary_NBNS_Server:
					wins.ses = req->rpd->ses;
					wins.wins2 = attr->val.ipaddr;
					break;
			}
			continue;
		} else if (attr->vendor)
			continue;

		switch(attr->attr->id) {
			case Framed_IP_Address:
				if (!conf_gw_ip_address && req->rpd->ses->ctrl->ppp)
					log_ppp_warn("radius: gw-ip-address not specified, cann't assign IP address...\n");
				else if (attr->val.ipaddr != 0xfffffffe) {
					req->rpd->ipv4_addr.owner = &ipdb;
					req->rpd->ipv4_addr.peer_addr = attr->val.ipaddr;
					req->rpd->ipv4_addr.addr = req->rpd->ses->ctrl->ppp ? conf_gw_ip_address : 0;
				}
				break;
			case Acct_Interim_Interval:
				req->rpd->acct_interim_interval = attr->val.integer;
				break;
			case Session_Timeout:
				req->rpd->session_timeout.expire_tv.tv_sec = attr->val.integer;
				break;
			case Idle_Timeout:
				req->rpd->idle_timeout.period = attr->val.integer * 1000;
				break;
			case Class:
				if (!req->rpd->attr_class)
					req->rpd->attr_class = _malloc(attr->len);
				else if (req->rpd->attr_class_len != attr->len)
					req->rpd->attr_class = _realloc(req->rpd->attr_class, attr->len);
				memcpy(req->rpd->attr_class, attr->val.octets, attr->len);
				req->rpd->attr_class_len = attr->len;
				break;
			case State:
				if (!req->rpd->attr_state)
					req->rpd->attr_state = _malloc(attr->len);
				else if (req->rpd->attr_state_len != attr->len)
					req->rpd->attr_state = _realloc(req->rpd->attr_state, attr->len);
				memcpy(req->rpd->attr_state, attr->val.octets, attr->len);
				req->rpd->attr_state_len = attr->len;	
				break;
			case Termination_Action:
				req->rpd->termination_action = attr->val.integer; 
				break;
			case Framed_Interface_Id:
				req->rpd->ipv6_addr.peer_intf_id = attr->val.ifid;
				break;
			case Framed_IPv6_Prefix:
				a = _malloc(sizeof(*a));
				a->prefix_len = attr->val.ipv6prefix.len;
				a->addr = attr->val.ipv6prefix.prefix;
				list_add_tail(&a->entry, &req->rpd->ipv6_addr.addr_list);
				break;
			case Delegated_IPv6_Prefix:
				a = _malloc(sizeof(*a));
				a->prefix_len = attr->val.ipv6prefix.len;
				a->addr = attr->val.ipv6prefix.prefix;
				list_add_tail(&a->entry, &req->rpd->ipv6_dp.prefix_list);
				break;
			case NAS_Port_Id:
				ap_session_rename(req->rpd->ses, attr->val.string, attr->len);
				break;
		}
	}

	if (dns.ses)
		triton_event_fire(EV_DNS, &dns);

	if (wins.ses)
		triton_event_fire(EV_WINS, &wins);

	return res;
}

static int rad_pwdb_check(struct pwdb_t *pwdb, struct ap_session *ses, pwdb_callback cb, void *cb_arg, const char *username, int type, va_list _args)
{
	int r = PWDB_NO_IMPL;
	va_list args;
	int chap_type;
	struct radius_pd_t *rpd = find_pd(ses);
	char username1[256];

	if (conf_default_realm && !strchr(username, '@')) {
		int len = strlen(username);
		if (len + conf_default_realm_len >= 256 - 2) {
			log_ppp_error("radius: username is too large to append realm\n");
			return PWDB_DENIED;
		}
		
		memcpy(username1, username, len);
		username1[len] = '@';
		memcpy(username1 + len + 1, conf_default_realm, conf_default_realm_len);
		username1[len + 1 + conf_default_realm_len] = 0;
		username = username1;
	}
	
	rpd->auth_ctx = mempool_alloc(auth_ctx_pool);
	memset(rpd->auth_ctx, 0, sizeof(*rpd->auth_ctx));

	rpd->auth_ctx->cb = cb;
	rpd->auth_ctx->cb_arg = cb_arg;

	va_copy(args, _args);

	switch(type) {
		case PPP_PAP:
			r = rad_auth_pap(rpd, username, args);
			break;
		case PPP_CHAP:
			chap_type = va_arg(args, int);
			switch(chap_type) {
				case CHAP_MD5:
					r = rad_auth_chap_md5(rpd, username, args);
					break;
				case MSCHAP_V1:
					r = rad_auth_mschap_v1(rpd, username, args);
					break;
				case MSCHAP_V2:
					r = rad_auth_mschap_v2(rpd, username, args);
					break;
			}
			break;
		case 0:
			r = rad_auth_null(rpd, username, args);
			break;
	}

	va_end(args);

	if (r == PWDB_DENIED) {
		if (rpd->auth_ctx->req)
			rad_req_free(rpd->auth_ctx->req);
		mempool_free(rpd->auth_ctx);
		rpd->auth_ctx = NULL;
	}

	return r;
}

static struct ipv4db_item_t *get_ipv4(struct ap_session *ses)
{
	struct radius_pd_t *rpd = find_pd(ses);
	
	if (rpd->ipv4_addr.peer_addr)
		return &rpd->ipv4_addr;
	return NULL;
}

static struct ipv6db_item_t *get_ipv6(struct ap_session *ses)
{
	struct radius_pd_t *rpd = find_pd(ses);

	rpd->ipv6_addr.intf_id = 0;

	if (!list_empty(&rpd->ipv6_addr.addr_list))
		return &rpd->ipv6_addr;

	return NULL;
}

static struct ipv6db_prefix_t *get_ipv6_prefix(struct ap_session *ses)
{
	struct radius_pd_t *rpd = find_pd(ses);

	if (!list_empty(&rpd->ipv6_dp.prefix_list)) {
		rpd->ipv6_dp_assigned = 1;
		return &rpd->ipv6_dp;
	}

	return NULL;
}

static void session_timeout(struct triton_timer_t *t)
{
	struct radius_pd_t *rpd = container_of(t, typeof(*rpd), session_timeout);

	log_ppp_msg("radius: session timed out\n");

	if (rpd->ses->stop_time)
		return;

	if (rpd->termination_action == Termination_Action_RADIUS_Request && rpd->ses->ctrl->ppp) {
		if (ppp_auth_restart(container_of(rpd->ses, struct ppp_t, ses)))
			ap_session_terminate(rpd->ses, TERM_SESSION_TIMEOUT, 0);
	} else
		ap_session_terminate(rpd->ses, TERM_SESSION_TIMEOUT, 0);
}

static void idle_timeout(struct triton_timer_t *t)
{
	struct radius_pd_t *rpd = container_of(t, typeof(*rpd), idle_timeout);

	if (rpd->ses->stop_time)
		return;

	ap_session_read_stats(rpd->ses, NULL);

	if (_time() - rpd->ses->idle_time > t->period / 1000) {
		log_ppp_msg("radius: idle timed out\n");
		ap_session_terminate(rpd->ses, TERM_IDLE_TIMEOUT, 0);
	}
}

static void ses_starting(struct ap_session *ses)
{
	struct radius_pd_t *rpd = mempool_alloc(rpd_pool);

	memset(rpd, 0, sizeof(*rpd));
	rpd->pd.key = &pd_key;
	rpd->ses = ses;
	rpd->refs = 1;
	pthread_mutex_init(&rpd->lock, NULL);
	INIT_LIST_HEAD(&rpd->plugin_list);
	INIT_LIST_HEAD(&rpd->ipv6_addr.addr_list);
	INIT_LIST_HEAD(&rpd->ipv6_dp.prefix_list);
	
	rpd->ipv4_addr.owner = &ipdb;
	rpd->ipv6_addr.owner = &ipdb;
	rpd->ipv6_dp.owner = &ipdb;

	list_add_tail(&rpd->pd.entry, &ses->pd_list);

	pthread_rwlock_wrlock(&sessions_lock);
	list_add_tail(&rpd->entry, &sessions);
	pthread_rwlock_unlock(&sessions_lock);

#ifdef USE_BACKUP
	if (ses->state == AP_STATE_RESTORE && ses->backup)
		radius_restore_session(ses, rpd);
#endif
}

static void ses_acct_start(struct ap_session *ses)
{
	struct radius_pd_t *rpd = find_pd(ses);
	
	if (!conf_accounting)
		return;
	
	if (!rpd->authenticated)
		return;

	if (rad_acct_start(rpd)) {
		ap_session_terminate(rpd->ses, TERM_NAS_ERROR, 0);
		return;
	}

	ses->acct_start++;
}

static void ses_started(struct ap_session *ses)
{
	struct radius_pd_t *rpd = find_pd(ses);

	if (rpd->session_timeout.expire_tv.tv_sec) {
		rpd->session_timeout.expire = session_timeout;
		triton_timer_add(ses->ctrl->ctx, &rpd->session_timeout, 0);
	}

	if (rpd->idle_timeout.period) {
		rpd->idle_timeout.expire = idle_timeout;
		triton_timer_add(ses->ctrl->ctx, &rpd->idle_timeout, 0);
	}
}

static void ses_finishing(struct ap_session *ses)
{
	struct radius_pd_t *rpd = find_pd(ses);

	if (rpd->auth_ctx) {
		rad_server_req_cancel(rpd->auth_ctx->req, 1);
		rad_req_free(rpd->auth_ctx->req);
		mempool_free(rpd->auth_ctx);
		rpd->auth_ctx = NULL;
	}

	if (rpd->acct_started || rpd->acct_req)
	    rad_acct_stop(rpd);
}

static void ses_finished(struct ap_session *ses)
{
	struct radius_pd_t *rpd = find_pd(ses);
	struct ipv6db_addr_t *a;

	pthread_rwlock_wrlock(&sessions_lock);
	pthread_mutex_lock(&rpd->lock);
	list_del(&rpd->entry);
	pthread_mutex_unlock(&rpd->lock);
	pthread_rwlock_unlock(&sessions_lock);

	if (rpd->auth_ctx) {
		rad_server_req_cancel(rpd->auth_ctx->req, 1);
		rad_req_free(rpd->auth_ctx->req);
		mempool_free(rpd->auth_ctx);
		rpd->auth_ctx = NULL;
	}

	if (rpd->acct_req) {
		if (rpd->acct_started)
			rad_acct_stop_defer(rpd);
		else {
			rad_server_req_cancel(rpd->acct_req, 1);
			rad_req_free(rpd->acct_req);
		}
	}

	if (rpd->dm_coa_req)
		dm_coa_cancel(rpd);

	if (rpd->session_timeout.tpd)
		triton_timer_del(&rpd->session_timeout);
	
	if (rpd->idle_timeout.tpd)
		triton_timer_del(&rpd->idle_timeout);

	if (rpd->attr_class)
		_free(rpd->attr_class);
	
	if (rpd->attr_state)
		_free(rpd->attr_state);

	while (!list_empty(&rpd->ipv6_addr.addr_list)) {
		a = list_entry(rpd->ipv6_addr.addr_list.next, typeof(*a), entry);
		list_del(&a->entry);
		_free(a);
	}

	while (!list_empty(&rpd->ipv6_dp.prefix_list)) {
		a = list_entry(rpd->ipv6_dp.prefix_list.next, typeof(*a), entry);
		list_del(&a->entry);
		_free(a);
	}

	list_del(&rpd->pd.entry);
	
	release_pd(rpd);
}

struct radius_pd_t *find_pd(struct ap_session *ses)
{
	struct ap_private *pd;
	struct radius_pd_t *rpd;

	list_for_each_entry(pd, &ses->pd_list, entry) {
		if (pd->key == &pd_key) {
			rpd = container_of(pd, typeof(*rpd), pd);
			return rpd;
		}
	}
	log_emerg("radius:BUG: rpd not found\n");
	abort();
}

void hold_pd(struct radius_pd_t *rpd)
{
	rpd->refs++;
}

void release_pd(struct radius_pd_t *rpd)
{
	if (--rpd->refs == 0)
		mempool_free(rpd);
}

struct radius_pd_t *rad_find_session(const char *sessionid, const char *username, const char *port_id, int port, in_addr_t ipaddr, const char *csid)
{
	struct radius_pd_t *rpd;
	
	pthread_rwlock_rdlock(&sessions_lock);
	list_for_each_entry(rpd, &sessions, entry) {
		if (!rpd->ses->username)
			continue;
		if (sessionid && strcmp(sessionid, rpd->ses->sessionid))
			continue;
		if (username && strcmp(username, rpd->ses->username))
			continue;
		if (port >= 0 && port != rpd->ses->unit_idx)
			continue;
		if (port_id && strcmp(port_id, rpd->ses->ifname))
			continue;
		if (ipaddr && rpd->ses->ipv4 && ipaddr != rpd->ses->ipv4->peer_addr)
			continue;
		if (csid && rpd->ses->ctrl->calling_station_id && strcmp(csid, rpd->ses->ctrl->calling_station_id))
			continue;
		pthread_mutex_lock(&rpd->lock);
		pthread_rwlock_unlock(&sessions_lock);
		return rpd;
	}
	pthread_rwlock_unlock(&sessions_lock);
	return NULL;
}

struct radius_pd_t *rad_find_session_pack(struct rad_packet_t *pack)
{
	struct rad_attr_t *attr;
	const char *sessionid = NULL;
	const char *username = NULL;
	const char *csid = NULL;
	int port = -1;
	const char *port_id = NULL;
	in_addr_t ipaddr = 0;
	
	list_for_each_entry(attr, &pack->attrs, entry) {
		if (attr->vendor)
			continue;
		switch(attr->attr->id) {
			case Acct_Session_Id:
				sessionid = attr->val.string;
				break;
			case User_Name:
				username = attr->val.string;
				break;
			case NAS_Port:
				port = attr->val.integer;
				break;
			case NAS_Port_Id:
				port_id = attr->val.string;
				break;
			case Framed_IP_Address:
				ipaddr = attr->val.ipaddr;
				break;
			case Calling_Station_Id:
				csid = attr->val.string;
				break;
		}
	}

	if (!sessionid && !username && !port_id && port == -1 && ipaddr == 0 && !csid)
		return NULL;

	if (username && !sessionid && port == -1 && ipaddr == 0 && !port_id)
		return NULL;
	
	return rad_find_session(sessionid, username, port_id, port, ipaddr, csid);
}

int rad_check_nas_pack(struct rad_packet_t *pack)
{
	struct rad_attr_t *attr;
	const char *ident = NULL;
	in_addr_t ipaddr = 0;
	
	list_for_each_entry(attr, &pack->attrs, entry) {
		if (!strcmp(attr->attr->name, "NAS-Identifier"))
			ident = attr->val.string;
		else if (!strcmp(attr->attr->name, "NAS-IP-Address"))
			ipaddr = attr->val.ipaddr;
	}

	if (conf_require_nas_ident && !ident && !ipaddr)
		return -1;

	if (conf_nas_identifier && ident && strcmp(conf_nas_identifier, ident))
		return -1;

	if (conf_nas_ip_address && ipaddr && conf_nas_ip_address != ipaddr)
		return -1;
	
	return 0;
}

void __export rad_register_plugin(struct ap_session *ses, struct rad_plugin_t *plugin)
{
	struct radius_pd_t *rpd = find_pd(ses);

	if (!rpd)
		return;

	list_add_tail(&plugin->entry, &rpd->plugin_list);
}

static struct ipdb_t ipdb = {
	.get_ipv4 = get_ipv4,
	.get_ipv6 = get_ipv6,
	.get_ipv6_prefix = get_ipv6_prefix,
};

static struct pwdb_t pwdb = {
	.check = rad_pwdb_check,
};

static int parse_server(const char *opt, in_addr_t *addr, int *port, char **secret)
{
	char *str = _strdup(opt);
	char *p1, *p2;

	p1 = strstr(str, ":");
	p2 = strstr(str, ",");

	if (p1)
		*p1 = 0;
	if (p2)
		*p2 = 0;
	else
		return -1;
	
	*addr = inet_addr(str);
	
	if (p1) {
		*port = atoi(p1 + 1);
		if (*port <=0 )
			return -1;
	}

	p1 = _strdup(p2 + 1);
	p2 = *secret;
	*secret = p1;
	if (p2)
		_free(p2);
	
	_free(str);

	return 0;
}

static int load_config(void)
{
	char *opt;

	opt = conf_get_opt("radius", "max-try");
	if (opt && atoi(opt) > 0)
		conf_max_try = atoi(opt);
	
	opt = conf_get_opt("radius", "timeout");
	if (opt && atoi(opt) > 0)
		conf_timeout = atoi(opt);

	opt = conf_get_opt("radius", "acct-timeout");
	if (opt && atoi(opt) >= 0)
		conf_acct_timeout = atoi(opt);

	opt = conf_get_opt("radius", "verbose");
	if (opt && atoi(opt) >= 0)
		conf_verbose = atoi(opt) > 0;

	opt = conf_get_opt("radius", "interim-verbose");
	if (opt && atoi(opt) >= 0)
		conf_interim_verbose = atoi(opt) > 0;

	opt = conf_get_opt("radius", "nas-ip-address");
	if (opt)
		conf_nas_ip_address = inet_addr(opt);
	
	if (conf_nas_identifier)
		_free(conf_nas_identifier);
	opt = conf_get_opt("radius", "nas-identifier");
	if (opt)
		conf_nas_identifier = _strdup(opt);
	else
		conf_nas_identifier = NULL;
	
	opt = conf_get_opt("radius", "gw-ip-address");
	if (opt)
		conf_gw_ip_address = inet_addr(opt);

	opt = conf_get_opt("radius", "bind");
	if (opt)
		conf_bind = inet_addr(opt);
	else if (conf_nas_ip_address)
		conf_bind = conf_nas_ip_address;

	opt = conf_get_opt("radius", "dae-server");
	if (opt && parse_server(opt, &conf_dm_coa_server, &conf_dm_coa_port, &conf_dm_coa_secret)) {
		log_emerg("radius: failed to parse dae-server\n");
		return -1;
	}

	opt = conf_get_opt("radius", "sid_in_auth");
	if (opt)
		conf_sid_in_auth = atoi(opt);
	
	opt = conf_get_opt("radius", "require-nas-identification");
	if (opt)
		conf_require_nas_ident = atoi(opt);
	
	opt = conf_get_opt("radius", "acct-interim-interval");
	if (opt && atoi(opt) > 0)
		conf_acct_interim_interval = atoi(opt);

	opt = conf_get_opt("radius", "acct-delay-time");
	if (opt)
		conf_acct_delay_time = atoi(opt);

	conf_attr_tunnel_type = conf_get_opt("radius", "attr-tunnel-type");
	
	conf_default_realm = conf_get_opt("radius", "default-realm");
	if (conf_default_realm)
		conf_default_realm_len = strlen(conf_default_realm);

	return 0;
}

static void radius_init(void)
{
	const char *dict = NULL;
	struct conf_sect_t *s = conf_get_section("radius");
	struct conf_option_t *opt1;

	rpd_pool = mempool_create(sizeof(struct radius_pd_t));
	auth_ctx_pool = mempool_create(sizeof(struct radius_auth_ctx));

	if (load_config())
		_exit(EXIT_FAILURE);

	
	list_for_each_entry(opt1, &s->items, entry) {
		if (strcmp(opt1->name, "dictionary") || !opt1->val)
			continue;
		dict = opt1->val;
		if (rad_dict_load(dict))
			_exit(0);
	}

	if (!dict && rad_dict_load(DICTIONARY))
		_exit(0);

	pwdb_register(&pwdb);
	ipdb_register(&ipdb);

	triton_event_register_handler(EV_SES_STARTING, (triton_event_func)ses_starting);
	triton_event_register_handler(EV_SES_STARTED, (triton_event_func)ses_started);
	triton_event_register_handler(EV_SES_ACCT_START, (triton_event_func)ses_acct_start);
	triton_event_register_handler(EV_SES_FINISHING, (triton_event_func)ses_finishing);
	triton_event_register_handler(EV_SES_FINISHED, (triton_event_func)ses_finished);
	triton_event_register_handler(EV_CONFIG_RELOAD, (triton_event_func)load_config);
}

DEFINE_INIT(51, radius_init);
