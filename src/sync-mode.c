/*
 * (C) 2006-2007 by Pablo Neira Ayuso <pablo@netfilter.org>
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdlib.h>
#include "cache.h"
#include "conntrackd.h"
#include <libnfnetlink/libnfnetlink.h>
#include <libnetfilter_conntrack/libnetfilter_conntrack.h>
#include <errno.h>
#include "us-conntrack.h"
#include <signal.h>
#include <sys/select.h>
#include "sync.h"
#include "network.h"

/* handler for multicast messages received */
static void mcast_handler()
{
	int ret;
	unsigned int type;
	char __net[4096];
	struct nethdr *net = (struct nethdr *) __net;
	struct nlmsghdr *nlh = (struct nlmsghdr *) (__net + NETHDR_SIZ);
	char __ct[nfct_maxsize()];
	struct nf_conntrack *ct = (struct nf_conntrack *) __ct;
	struct us_conntrack *u = NULL;

	ret = mcast_recv_netmsg(STATE_SYNC(mcast_server), net, sizeof(__net));
	if (ret <= 0) {
		STATE(malformed)++;
		return;
	}

	if (STATE_SYNC(sync)->recv(net))
		return;

	memset(ct, 0, sizeof(__ct));

	if ((type = parse_network_msg(ct, nlh)) == NFCT_T_ERROR) {
		STATE(malformed)++;
		return;
	}

	nfct_attr_unset(ct, ATTR_TIMEOUT);
	nfct_attr_unset(ct, ATTR_ORIG_COUNTER_BYTES);
	nfct_attr_unset(ct, ATTR_ORIG_COUNTER_PACKETS);
	nfct_attr_unset(ct, ATTR_REPL_COUNTER_BYTES);
	nfct_attr_unset(ct, ATTR_REPL_COUNTER_PACKETS);

	switch(type) {
	case NFCT_T_NEW:
retry:		
		if ((u = cache_add(STATE_SYNC(external), ct))) {
			debug_ct(u->ct, "external new");
		} else {
		        /*
			 * One certain connection A arrives to the cache but 
			 * another existing connection B in the cache has 
			 * the same configuration, therefore B clashes with A.
			 */
			if (errno == EEXIST) {
				cache_del(STATE_SYNC(external), ct);
				goto retry;
			}
			debug_ct(ct, "can't add");
		}
		break;
	case NFCT_T_UPDATE:
		if ((u = cache_update_force(STATE_SYNC(external), ct))) {
			debug_ct(u->ct, "external update");
		} else
			debug_ct(ct, "can't update");
		break;
	case NFCT_T_DESTROY:
		if (cache_del(STATE_SYNC(external), ct))
			debug_ct(ct, "external destroy");
		else
			debug_ct(ct, "can't destroy");
		break;
	default:
		dlog(STATE(log), "mcast received unknown msg type %d\n", type);
		break;
	}
}

static int init_sync(void)
{
	int ret;

	state.sync = malloc(sizeof(struct ct_sync_state));
	if (!state.sync) {
		dlog(STATE(log), "[FAIL] can't allocate memory for state sync");
		return -1;
	}
	memset(state.sync, 0, sizeof(struct ct_sync_state));

	if (CONFIG(flags) & SYNC_MODE_NACK)
		STATE_SYNC(sync) = &nack;
	else
		/* default to persistent mode */
		STATE_SYNC(sync) = &notrack;

	if (STATE_SYNC(sync)->init)
		STATE_SYNC(sync)->init();

	STATE_SYNC(internal) =
		cache_create("internal", 
			     STATE_SYNC(sync)->internal_cache_flags,
			     CONFIG(family),
			     STATE_SYNC(sync)->internal_cache_extra);

	if (!STATE_SYNC(internal)) {
		dlog(STATE(log), "[FAIL] can't allocate memory for "
				 "the internal cache");
		return -1;
	}

	STATE_SYNC(external) = 
		cache_create("external",
			     STATE_SYNC(sync)->external_cache_flags,
			     CONFIG(family),
			     NULL);

	if (!STATE_SYNC(external)) {
		dlog(STATE(log), "[FAIL] can't allocate memory for the "
				 "external cache");
		return -1;
	}

	/* multicast server to receive events from the wire */
	STATE_SYNC(mcast_server) = mcast_server_create(&CONFIG(mcast));
	if (STATE_SYNC(mcast_server) == NULL) {
		dlog(STATE(log), "[FAIL] can't open multicast server!");
		return -1;
	}

	/* multicast client to send events on the wire */
	STATE_SYNC(mcast_client) = mcast_client_create(&CONFIG(mcast));
	if (STATE_SYNC(mcast_client) == NULL) {
		dlog(STATE(log), "[FAIL] can't open client multicast socket!");
		return -1;
	}

	/* initialization of multicast sequence generation */
	STATE_SYNC(last_seq_sent) = time(NULL);

	if (create_alarm_thread() == -1) {
		dlog(STATE(log), "[FAIL] can't initialize alarm thread");
		return -1;
	}

	return 0;
}

static int add_fds_to_set_sync(fd_set *readfds) 
{
	FD_SET(STATE_SYNC(mcast_server->fd), readfds);

	return STATE_SYNC(mcast_server->fd);
}

static void step_sync(fd_set *readfds)
{
	/* multicast packet has been received */
	if (FD_ISSET(STATE_SYNC(mcast_server->fd), readfds))
		mcast_handler();
}

static void kill_sync()
{
	cache_destroy(STATE_SYNC(internal));
	cache_destroy(STATE_SYNC(external));

	mcast_server_destroy(STATE_SYNC(mcast_server));
	mcast_client_destroy(STATE_SYNC(mcast_client));

	destroy_alarm_thread();

	if (STATE_SYNC(sync)->kill)
		STATE_SYNC(sync)->kill();
}

static dump_stats_sync(int fd)
{
	char buf[512];
	int size;

	size = sprintf(buf, "multicast sequence tracking:\n"
			    "%20llu Pckts mfrm "
			    "%20llu Pckts lost\n\n",
			STATE(malformed),
			STATE_SYNC(packets_lost));

	send(fd, buf, size, 0);
}

/* handler for requests coming via UNIX socket */
static int local_handler_sync(int fd, int type, void *data)
{
	int ret = 1;

	switch(type) {
	case DUMP_INTERNAL:
		ret = fork();
		if (ret == 0) {
			cache_dump(STATE_SYNC(internal), fd, NFCT_O_PLAIN);
			exit(EXIT_SUCCESS);
		}
		break;
	case DUMP_EXTERNAL:
		ret = fork();
		if (ret == 0) {
			cache_dump(STATE_SYNC(external), fd, NFCT_O_PLAIN);
			exit(EXIT_SUCCESS);
		} 
		break;
	case DUMP_INT_XML:
		ret = fork();
		if (ret == 0) {
			cache_dump(STATE_SYNC(internal), fd, NFCT_O_XML);
			exit(EXIT_SUCCESS);
		}
		break;
	case DUMP_EXT_XML:
		ret = fork();
		if (ret == 0) {
			cache_dump(STATE_SYNC(external), fd, NFCT_O_XML);
			exit(EXIT_SUCCESS);
		}
		break;
	case COMMIT:
		dlog(STATE(log), "[REQ] commit external cache to master table");
		cache_commit(STATE_SYNC(external));
		break;
	case FLUSH_CACHE:
		dlog(STATE(log), "[REQ] flushing caches");
		cache_flush(STATE_SYNC(internal));
		cache_flush(STATE_SYNC(external));
		break;
	case KILL:
		killer();
		break;
	case STATS:
		cache_stats(STATE_SYNC(internal), fd);
		cache_stats(STATE_SYNC(external), fd);
		dump_traffic_stats(fd);
		mcast_dump_stats(fd, STATE_SYNC(mcast_client), 
				     STATE_SYNC(mcast_server));
		dump_stats_sync(fd);
		break;
	case SEND_BULK:
		dlog(STATE(log), "[REQ] sending bulk update");
		cache_bulk(STATE_SYNC(internal));
		break;
	default:
		if (STATE_SYNC(sync)->local)
			ret = STATE_SYNC(sync)->local(fd, type, data);
		break;
	}

	return ret;
}

static void dump_sync(struct nf_conntrack *ct, struct nlmsghdr *nlh)
{
	/* This is required by kernels < 2.6.20 */
	nfct_attr_unset(ct, ATTR_TIMEOUT);
	nfct_attr_unset(ct, ATTR_ORIG_COUNTER_BYTES);
	nfct_attr_unset(ct, ATTR_ORIG_COUNTER_PACKETS);
	nfct_attr_unset(ct, ATTR_REPL_COUNTER_BYTES);
	nfct_attr_unset(ct, ATTR_REPL_COUNTER_PACKETS);
	nfct_attr_unset(ct, ATTR_USE);

	if (cache_update_force(STATE_SYNC(internal), ct))
		debug_ct(ct, "resync");
}

static void mcast_send_sync(struct nlmsghdr *nlh,
			    struct us_conntrack *u,
			    struct nf_conntrack *ct,
			    int type)
{
	char __net[4096];
	struct nethdr *net = (struct nethdr *) __net;

	memset(__net, 0, sizeof(__net));

	if (!state_helper_verdict(type, ct))
		return;

	memcpy(__net + NETHDR_SIZ, nlh, nlh->nlmsg_len);
	mcast_send_netmsg(STATE_SYNC(mcast_client), net);
	if (STATE_SYNC(sync)->send)
		STATE_SYNC(sync)->send(type, net, u);
}

static int overrun_cb(enum nf_conntrack_msg_type type,
		      struct nf_conntrack *ct,
		      void *data)
{
	struct us_conntrack *u;

	if (ignore_conntrack(ct))
		return NFCT_CB_CONTINUE;

	/* This is required by kernels < 2.6.20 */
	nfct_attr_unset(ct, ATTR_TIMEOUT);
	nfct_attr_unset(ct, ATTR_ORIG_COUNTER_BYTES);
	nfct_attr_unset(ct, ATTR_ORIG_COUNTER_PACKETS);
	nfct_attr_unset(ct, ATTR_REPL_COUNTER_BYTES);
	nfct_attr_unset(ct, ATTR_REPL_COUNTER_PACKETS);
	nfct_attr_unset(ct, ATTR_USE);

	if (!cache_test(STATE_SYNC(internal), ct)) {
		if ((u = cache_update_force(STATE_SYNC(internal), ct))) {
			debug_ct(u->ct, "overrun resync");
			mcast_build_send_update(u);
		}
	}

	return NFCT_CB_CONTINUE;
}

static int overrun_purge_step(void *data1, void *data2)
{
	int ret;
	struct nfct_handle *h = data1;
	struct us_conntrack *u = data2;

	ret = nfct_query(h, NFCT_Q_GET, u->ct);
	if (ret == -1 && errno == ENOENT) {
		debug_ct(u->ct, "overrun purge resync");
		mcast_build_send_destroy(u);
		__cache_del(STATE_SYNC(internal), u->ct);
	}

	return 0;
}

/* it's likely that we're losing events, just try to do our best here */
static void overrun_sync()
{
	int ret;
	struct nfct_handle *h;
	int family = CONFIG(family);

	h = nfct_open(CONNTRACK, 0);
	if (!h) {
		dlog(STATE(log), "can't open overrun handler");
		return;
	}

	nfct_callback_register(h, NFCT_T_ALL, overrun_cb, NULL);

	ret = nfct_query(h, NFCT_Q_DUMP, &family);
	if (ret == -1)
		dlog(STATE(log), "overrun query error %s", strerror(errno));

	nfct_callback_unregister(h);

	cache_iterate(STATE_SYNC(internal), h, overrun_purge_step);

	nfct_close(h);
}

static void event_new_sync(struct nf_conntrack *ct, struct nlmsghdr *nlh)
{
	struct us_conntrack *u;

	/* required by linux kernel <= 2.6.20 */
	nfct_attr_unset(ct, ATTR_ORIG_COUNTER_BYTES);
	nfct_attr_unset(ct, ATTR_ORIG_COUNTER_PACKETS);
	nfct_attr_unset(ct, ATTR_REPL_COUNTER_BYTES);
	nfct_attr_unset(ct, ATTR_REPL_COUNTER_PACKETS);
	nfct_attr_unset(ct, ATTR_TIMEOUT);
retry:
	if ((u = cache_add(STATE_SYNC(internal), ct))) {
		mcast_send_sync(nlh, u, ct, NFCT_T_NEW);
		debug_ct(u->ct, "internal new");
	} else {
		if (errno == EEXIST) {
			cache_del(STATE_SYNC(internal), ct);
			mcast_send_sync(nlh, NULL, ct, NFCT_T_DESTROY);
			goto retry;
		}

		dlog(STATE(log), "can't add to internal cache: "
				      "%s\n", strerror(errno));
		debug_ct(ct, "can't add");
	}
}

static void event_update_sync(struct nf_conntrack *ct, struct nlmsghdr *nlh)
{
	struct us_conntrack *u;

	nfct_attr_unset(ct, ATTR_TIMEOUT);

	if ((u = cache_update_force(STATE_SYNC(internal), ct)) == NULL) {
		debug_ct(ct, "can't update");
		return;
	}
	debug_ct(u->ct, "internal update");
	mcast_send_sync(nlh, u, ct, NFCT_T_UPDATE);
}

static int event_destroy_sync(struct nf_conntrack *ct, struct nlmsghdr *nlh)
{
	nfct_attr_unset(ct, ATTR_TIMEOUT);

	if (cache_del(STATE_SYNC(internal), ct)) {
		mcast_send_sync(nlh, NULL, ct, NFCT_T_DESTROY);
		debug_ct(ct, "internal destroy");
	} else
		debug_ct(ct, "can't destroy");
}

struct ct_mode sync_mode = {
	.init 			= init_sync,
	.add_fds_to_set 	= add_fds_to_set_sync,
	.step			= step_sync,
	.local			= local_handler_sync,
	.kill			= kill_sync,
	.dump			= dump_sync,
	.overrun		= overrun_sync,
	.event_new		= event_new_sync,
	.event_upd		= event_update_sync,
	.event_dst		= event_destroy_sync
};
