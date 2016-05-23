/*	$OpenBSD$ */

/*
 * Copyright (c) 2004, 2005 Claudio Jeker <claudio@openbsd.org>
 * Copyright (c) 2004 Esben Norby <norby@openbsd.org>
 * Copyright (c) 2003, 2004 Henning Brauer <henning@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <netmpls/mpls.h>
#include <arpa/inet.h>
#include <err.h>
#include <errno.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <pwd.h>
#include <unistd.h>
#include <event.h>

#include "ldp.h"
#include "ldpd.h"
#include "ldpe.h"
#include "log.h"
#include "lde.h"

void		 lde_sig_handler(int sig, short, void *);
void		 lde_shutdown(void);
void		 lde_dispatch_imsg(int, short, void *);
void		 lde_dispatch_parent(int, short, void *);

struct lde_nbr	*lde_nbr_find(uint32_t);
struct lde_nbr	*lde_nbr_new(uint32_t, struct in_addr *);
void		 lde_nbr_del(struct lde_nbr *);
void		 lde_nbr_clear(void);

void		 lde_map_free(void *);
void		 lde_address_list_free(struct lde_nbr *);

struct ldpd_conf	*ldeconf = NULL, *nconf = NULL;
struct imsgev		*iev_ldpe;
struct imsgev		*iev_main;

static __inline int lde_nbr_compare(struct lde_nbr *, struct lde_nbr *);

RB_HEAD(nbr_tree, lde_nbr);
RB_PROTOTYPE(nbr_tree, lde_nbr, entry, lde_nbr_compare)
RB_GENERATE(nbr_tree, lde_nbr, entry, lde_nbr_compare)

struct nbr_tree lde_nbrs = RB_INITIALIZER(&lde_nbrs);

/* ARGSUSED */
void
lde_sig_handler(int sig, short event, void *arg)
{
	/*
	 * signal handler rules don't apply, libevent decouples for us
	 */

	switch (sig) {
	case SIGINT:
	case SIGTERM:
		lde_shutdown();
		/* NOTREACHED */
	default:
		fatalx("unexpected signal");
	}
}

/* label decision engine */
pid_t
lde(struct ldpd_conf *xconf, int pipe_parent2lde[2], int pipe_ldpe2lde[2],
    int pipe_parent2ldpe[2])
{
	struct event		 ev_sigint, ev_sigterm;
	struct timeval		 now;
	struct passwd		*pw;
	pid_t			 pid;
	struct l2vpn		*l2vpn;

	switch (pid = fork()) {
	case -1:
		fatal("cannot fork");
		/* NOTREACHED */
	case 0:
		break;
	default:
		return (pid);
	}

	ldeconf = xconf;

	setproctitle("label decision engine");
	ldpd_process = PROC_LDE_ENGINE;

	if ((pw = getpwnam(LDPD_USER)) == NULL)
		fatal("getpwnam");

	if (chroot(pw->pw_dir) == -1)
		fatal("chroot");
	if (chdir("/") == -1)
		fatal("chdir(\"/\")");

	if (setgroups(1, &pw->pw_gid) ||
	    setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) ||
	    setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid))
		fatal("can't drop privileges");

	if (pledge("stdio", NULL) == -1)
		fatal("pledge");

	event_init();

	/* setup signal handler */
	signal_set(&ev_sigint, SIGINT, lde_sig_handler, NULL);
	signal_set(&ev_sigterm, SIGTERM, lde_sig_handler, NULL);
	signal_add(&ev_sigint, NULL);
	signal_add(&ev_sigterm, NULL);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, SIG_IGN);

	/* setup pipes */
	close(pipe_ldpe2lde[0]);
	close(pipe_parent2lde[0]);
	close(pipe_parent2ldpe[0]);
	close(pipe_parent2ldpe[1]);

	if ((iev_ldpe = malloc(sizeof(struct imsgev))) == NULL ||
	    (iev_main = malloc(sizeof(struct imsgev))) == NULL)
		fatal(NULL);
	imsg_init(&iev_ldpe->ibuf, pipe_ldpe2lde[1]);
	iev_ldpe->handler = lde_dispatch_imsg;
	imsg_init(&iev_main->ibuf, pipe_parent2lde[1]);
	iev_main->handler = lde_dispatch_parent;

	/* setup event handler */
	iev_ldpe->events = EV_READ;
	event_set(&iev_ldpe->ev, iev_ldpe->ibuf.fd, iev_ldpe->events,
	    iev_ldpe->handler, iev_ldpe);
	event_add(&iev_ldpe->ev, NULL);

	iev_main->events = EV_READ;
	event_set(&iev_main->ev, iev_main->ibuf.fd, iev_main->events,
	    iev_main->handler, iev_main);
	event_add(&iev_main->ev, NULL);

	/* setup and start the LIB garbage collector */
	evtimer_set(&gc_timer, lde_gc_timer, NULL);
	lde_gc_start_timer();

	gettimeofday(&now, NULL);
	global.uptime = now.tv_sec;

	/* initialize l2vpns */
	LIST_FOREACH(l2vpn, &ldeconf->l2vpn_list, entry)
		l2vpn_init(l2vpn);

	event_dispatch();

	lde_shutdown();
	/* NOTREACHED */

	return (0);
}

void
lde_shutdown(void)
{
	lde_gc_stop_timer();
	lde_nbr_clear();
	fec_tree_clear();

	config_clear(ldeconf);

	msgbuf_clear(&iev_ldpe->ibuf.w);
	free(iev_ldpe);
	msgbuf_clear(&iev_main->ibuf.w);
	free(iev_main);

	log_info("label decision engine exiting");
	_exit(0);
}

/* imesg */
int
lde_imsg_compose_parent(int type, pid_t pid, void *data, uint16_t datalen)
{
	return (imsg_compose_event(iev_main, type, 0, pid, -1, data, datalen));
}

int
lde_imsg_compose_ldpe(int type, uint32_t peerid, pid_t pid, void *data,
    uint16_t datalen)
{
	return (imsg_compose_event(iev_ldpe, type, peerid, pid,
	     -1, data, datalen));
}

/* ARGSUSED */
void
lde_dispatch_imsg(int fd, short event, void *bula)
{
	struct imsgev		*iev = bula;
	struct imsgbuf		*ibuf = &iev->ibuf;
	struct imsg		 imsg;
	struct lde_nbr		*ln;
	struct map		 map;
	struct in_addr		 addr;
	struct notify_msg	 nm;
	ssize_t			 n;
	int			 shut = 0, verbose;

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read error");
		if (n == 0)	/* connection closed */
			shut = 1;
	}
	if (event & EV_WRITE) {
		if ((n = msgbuf_write(&ibuf->w)) == -1 && errno != EAGAIN)
			fatal("msgbuf_write");
		if (n == 0)	/* connection closed */
			shut = 1;
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("lde_dispatch_imsg: imsg_get error");
		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_LABEL_MAPPING_FULL:
			ln = lde_nbr_find(imsg.hdr.peerid);
			if (ln == NULL) {
				log_debug("%s: cannot find lde neighbor",
				    __func__);
				break;
			}

			fec_snap(ln);
			break;
		case IMSG_LABEL_MAPPING:
		case IMSG_LABEL_REQUEST:
		case IMSG_LABEL_RELEASE:
		case IMSG_LABEL_WITHDRAW:
		case IMSG_LABEL_ABORT:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(map))
				fatalx("lde_dispatch_imsg: wrong imsg len");
			memcpy(&map, imsg.data, sizeof(map));

			ln = lde_nbr_find(imsg.hdr.peerid);
			if (ln == NULL) {
				log_debug("%s: cannot find lde neighbor",
				    __func__);
				break;
			}

			switch (imsg.hdr.type) {
			case IMSG_LABEL_MAPPING:
				lde_check_mapping(&map, ln);
				break;
			case IMSG_LABEL_REQUEST:
				lde_check_request(&map, ln);
				break;
			case IMSG_LABEL_RELEASE:
				if (map.type == MAP_TYPE_WILDCARD)
					lde_check_release_wcard(&map, ln);
				else
					lde_check_release(&map, ln);
				break;
			case IMSG_LABEL_WITHDRAW:
				if (map.type == MAP_TYPE_WILDCARD)
					lde_check_withdraw_wcard(&map, ln);
				else
					lde_check_withdraw(&map, ln);
				break;
			case IMSG_LABEL_ABORT:
				/* not necessary */
				break;
			}
			break;
		case IMSG_ADDRESS_ADD:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(addr))
				fatalx("lde_dispatch_imsg: wrong imsg len");
			memcpy(&addr, imsg.data, sizeof(addr));

			ln = lde_nbr_find(imsg.hdr.peerid);
			if (ln == NULL) {
				log_debug("%s: cannot find lde neighbor",
				    __func__);
				break;
			}

			if (lde_address_add(ln, &addr) < 0) {
				log_debug("%s: cannot add address %s, it "
				    "already exists", __func__,
				    inet_ntoa(addr));
			}

			break;
		case IMSG_ADDRESS_DEL:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(addr))
				fatalx("lde_dispatch_imsg: wrong imsg len");
			memcpy(&addr, imsg.data, sizeof(addr));

			ln = lde_nbr_find(imsg.hdr.peerid);
			if (ln == NULL) {
				log_debug("%s: cannot find lde neighbor",
				    __func__);
				break;
			}

			if (lde_address_del(ln, &addr) < 0) {
				log_debug("%s: cannot delete address %s, it "
				    "does not exist", __func__,
				    inet_ntoa(addr));
			}

			break;
		case IMSG_NOTIFICATION:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(nm))
				fatalx("lde_dispatch_imsg: wrong imsg len");
			memcpy(&nm, imsg.data, sizeof(nm));

			ln = lde_nbr_find(imsg.hdr.peerid);
			if (ln == NULL) {
				log_debug("%s: cannot find lde neighbor",
				    __func__);
				break;
			}

			switch (nm.status) {
			case S_PW_STATUS:
				l2vpn_recv_pw_status(ln, &nm);
				break;
			default:
				break;
			}
			break;
		case IMSG_NEIGHBOR_UP:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(addr))
				fatalx("lde_dispatch_imsg: wrong imsg len");
			memcpy(&addr, imsg.data, sizeof(addr));

			if (lde_nbr_find(imsg.hdr.peerid))
				fatalx("lde_dispatch_imsg: "
				    "neighbor already exists");
			lde_nbr_new(imsg.hdr.peerid, &addr);
			break;
		case IMSG_NEIGHBOR_DOWN:
			lde_nbr_del(lde_nbr_find(imsg.hdr.peerid));
			break;
		case IMSG_CTL_SHOW_LIB:
			rt_dump(imsg.hdr.pid);

			lde_imsg_compose_ldpe(IMSG_CTL_END, 0,
			    imsg.hdr.pid, NULL, 0);
			break;
		case IMSG_CTL_SHOW_L2VPN_PW:
			l2vpn_pw_ctl(imsg.hdr.pid);

			lde_imsg_compose_ldpe(IMSG_CTL_END, 0,
			    imsg.hdr.pid, NULL, 0);
			break;
		case IMSG_CTL_SHOW_L2VPN_BINDING:
			l2vpn_binding_ctl(imsg.hdr.pid);

			lde_imsg_compose_ldpe(IMSG_CTL_END, 0,
			    imsg.hdr.pid, NULL, 0);
			break;
		case IMSG_CTL_LOG_VERBOSE:
			/* already checked by ldpe */
			memcpy(&verbose, imsg.data, sizeof(verbose));
			log_verbose(verbose);
			break;
		default:
			log_debug("%s: unexpected imsg %d", __func__,
			    imsg.hdr.type);
			break;
		}
		imsg_free(&imsg);
	}
	if (!shut)
		imsg_event_add(iev);
	else {
		/* this pipe is dead, so remove the event handler */
		event_del(&iev->ev);
		event_loopexit(NULL);
	}
}

/* ARGSUSED */
void
lde_dispatch_parent(int fd, short event, void *bula)
{
	struct iface		*niface;
	struct tnbr		*ntnbr;
	struct nbr_params	*nnbrp;
	static struct l2vpn	*nl2vpn;
	struct l2vpn_if		*nlif;
	struct l2vpn_pw		*npw;
	struct imsg		 imsg;
	struct kroute		 kr;
	struct imsgev		*iev = bula;
	struct imsgbuf		*ibuf = &iev->ibuf;
	ssize_t			 n;
	int			 shut = 0;
	struct fec		 fec;

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read error");
		if (n == 0)	/* connection closed */
			shut = 1;
	}
	if (event & EV_WRITE) {
		if ((n = msgbuf_write(&ibuf->w)) == -1 && errno != EAGAIN)
			fatal("msgbuf_write");
		if (n == 0)	/* connection closed */
			shut = 1;
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("lde_dispatch_parent: imsg_get error");
		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_NETWORK_ADD:
			if (imsg.hdr.len != IMSG_HEADER_SIZE + sizeof(kr)) {
				log_warnx("%s: wrong imsg len", __func__);
				break;
			}
			memcpy(&kr, imsg.data, sizeof(kr));

			fec.type = FEC_TYPE_IPV4;
			fec.u.ipv4.prefix = kr.prefix;
			fec.u.ipv4.prefixlen = kr.prefixlen;
			lde_kernel_insert(&fec, kr.nexthop,
			    kr.flags & F_CONNECTED, NULL);
			break;
		case IMSG_NETWORK_DEL:
			if (imsg.hdr.len != IMSG_HEADER_SIZE + sizeof(kr)) {
				log_warnx("%s: wrong imsg len", __func__);
				break;
			}
			memcpy(&kr, imsg.data, sizeof(kr));

			fec.type = FEC_TYPE_IPV4;
			fec.u.ipv4.prefix = kr.prefix;
			fec.u.ipv4.prefixlen = kr.prefixlen;
			lde_kernel_remove(&fec, kr.nexthop);
			break;
		case IMSG_RECONF_CONF:
			if ((nconf = malloc(sizeof(struct ldpd_conf))) ==
			    NULL)
				fatal(NULL);
			memcpy(nconf, imsg.data, sizeof(struct ldpd_conf));

			LIST_INIT(&nconf->iface_list);
			LIST_INIT(&nconf->tnbr_list);
			LIST_INIT(&nconf->nbrp_list);
			LIST_INIT(&nconf->l2vpn_list);
			break;
		case IMSG_RECONF_IFACE:
			if ((niface = malloc(sizeof(struct iface))) == NULL)
				fatal(NULL);
			memcpy(niface, imsg.data, sizeof(struct iface));

			LIST_INIT(&niface->addr_list);
			LIST_INIT(&niface->adj_list);

			LIST_INSERT_HEAD(&nconf->iface_list, niface, entry);
			break;
		case IMSG_RECONF_TNBR:
			if ((ntnbr = malloc(sizeof(struct tnbr))) == NULL)
				fatal(NULL);
			memcpy(ntnbr, imsg.data, sizeof(struct tnbr));

			LIST_INSERT_HEAD(&nconf->tnbr_list, ntnbr, entry);
			break;
		case IMSG_RECONF_NBRP:
			if ((nnbrp = malloc(sizeof(struct nbr_params))) == NULL)
				fatal(NULL);
			memcpy(nnbrp, imsg.data, sizeof(struct nbr_params));

			LIST_INSERT_HEAD(&nconf->nbrp_list, nnbrp, entry);
			break;
		case IMSG_RECONF_L2VPN:
			if ((nl2vpn = malloc(sizeof(struct l2vpn))) == NULL)
				fatal(NULL);
			memcpy(nl2vpn, imsg.data, sizeof(struct l2vpn));

			LIST_INIT(&nl2vpn->if_list);
			LIST_INIT(&nl2vpn->pw_list);

			LIST_INSERT_HEAD(&nconf->l2vpn_list, nl2vpn, entry);
			break;
		case IMSG_RECONF_L2VPN_IF:
			if ((nlif = malloc(sizeof(struct l2vpn_if))) == NULL)
				fatal(NULL);
			memcpy(nlif, imsg.data, sizeof(struct l2vpn_if));

			nlif->l2vpn = nl2vpn;
			LIST_INSERT_HEAD(&nl2vpn->if_list, nlif, entry);
			break;
		case IMSG_RECONF_L2VPN_PW:
			if ((npw = malloc(sizeof(struct l2vpn_pw))) == NULL)
				fatal(NULL);
			memcpy(npw, imsg.data, sizeof(struct l2vpn_pw));

			npw->l2vpn = nl2vpn;
			LIST_INSERT_HEAD(&nl2vpn->pw_list, npw, entry);
			break;
		case IMSG_RECONF_END:
			merge_config(ldeconf, nconf);
			nconf = NULL;
			break;
		default:
			log_debug("%s: unexpected imsg %d", __func__,
			    imsg.hdr.type);
			break;
		}
		imsg_free(&imsg);
	}
	if (!shut)
		imsg_event_add(iev);
	else {
		/* this pipe is dead, so remove the event handler */
		event_del(&iev->ev);
		event_loopexit(NULL);
	}
}

uint32_t
lde_assign_label(void)
{
	static uint32_t label = MPLS_LABEL_RESERVED_MAX;

	/* XXX some checks needed */
	label++;
	return label;
}

void
lde_send_change_klabel(struct fec_node *fn, struct fec_nh *fnh)
{
	struct kroute	kr;
	struct kpw	kpw;
	struct l2vpn_pw	*pw;

	switch (fn->fec.type) {
	case FEC_TYPE_IPV4:
		memset(&kr, 0, sizeof(kr));
		kr.prefix = fn->fec.u.ipv4.prefix;
		kr.prefixlen = fn->fec.u.ipv4.prefixlen;
		kr.local_label = fn->local_label;
		kr.nexthop = fnh->nexthop;
		kr.remote_label = fnh->remote_label;

		lde_imsg_compose_parent(IMSG_KLABEL_CHANGE, 0, &kr,
		    sizeof(kr));

		if (fnh->remote_label != NO_LABEL &&
		    fn->fec.u.ipv4.prefixlen == 32)
			l2vpn_sync_pws(fn->fec.u.ipv4.prefix);
		break;
	case FEC_TYPE_PWID:
		if (fn->local_label == NO_LABEL ||
		    fnh->remote_label == NO_LABEL)
			return;

		pw = (struct l2vpn_pw *) fn->data;
		pw->flags |= F_PW_STATUS_UP;

		memset(&kpw, 0, sizeof(kpw));
		kpw.ifindex = pw->ifindex;
		kpw.pw_type = fn->fec.u.pwid.type;
		kpw.nexthop = fnh->nexthop;
		kpw.local_label = fn->local_label;
		kpw.remote_label = fnh->remote_label;
		kpw.flags = pw->flags;

		lde_imsg_compose_parent(IMSG_KPWLABEL_CHANGE, 0, &kpw,
		    sizeof(kpw));
		break;
	}
}

void
lde_send_delete_klabel(struct fec_node *fn, struct fec_nh *fnh)
{
	struct kroute	 kr;
	struct kpw	 kpw;
	struct l2vpn_pw	*pw;

	switch (fn->fec.type) {
	case FEC_TYPE_IPV4:
		memset(&kr, 0, sizeof(kr));
		kr.prefix = fn->fec.u.ipv4.prefix;
		kr.prefixlen = fn->fec.u.ipv4.prefixlen;
		kr.local_label = fn->local_label;
		kr.nexthop = fnh->nexthop;
		kr.remote_label = fnh->remote_label;

		lde_imsg_compose_parent(IMSG_KLABEL_DELETE, 0, &kr,
		    sizeof(kr));

		if (fn->fec.u.ipv4.prefixlen == 32)
			l2vpn_sync_pws(fn->fec.u.ipv4.prefix);
		break;
	case FEC_TYPE_PWID:
		pw = (struct l2vpn_pw *) fn->data;
		if (!(pw->flags & F_PW_STATUS_UP))
			return;
		pw->flags &= ~F_PW_STATUS_UP;

		memset(&kpw, 0, sizeof(kpw));
		kpw.ifindex = pw->ifindex;
		kpw.pw_type = fn->fec.u.pwid.type;
		kpw.nexthop = fnh->nexthop;
		kpw.local_label = fn->local_label;
		kpw.remote_label = fnh->remote_label;
		kpw.flags = pw->flags;

		lde_imsg_compose_parent(IMSG_KPWLABEL_DELETE, 0, &kpw,
		    sizeof(kpw));
		break;
	}
}

void
lde_fec2map(struct fec *fec, struct map *map)
{
	memset(map, 0, sizeof(*map));

	switch (fec->type) {
	case FEC_TYPE_IPV4:
		map->type = MAP_TYPE_PREFIX;
		map->fec.ipv4.prefix = fec->u.ipv4.prefix;
		map->fec.ipv4.prefixlen = fec->u.ipv4.prefixlen;
		break;
	case FEC_TYPE_PWID:
		map->type = MAP_TYPE_PWID;
		map->fec.pwid.type = fec->u.pwid.type;
		map->fec.pwid.group_id = 0;
		map->flags |= F_MAP_PW_ID;
		map->fec.pwid.pwid = fec->u.pwid.pwid;
		break;
	}
}

void
lde_map2fec(struct map *map, struct in_addr lsr_id, struct fec *fec)
{
	memset(fec, 0, sizeof(*fec));

	switch (map->type) {
	case MAP_TYPE_PREFIX:
		fec->type = FEC_TYPE_IPV4;
		fec->u.ipv4.prefix = map->fec.ipv4.prefix;
		fec->u.ipv4.prefixlen = map->fec.ipv4.prefixlen;
		break;
	case MAP_TYPE_PWID:
		fec->type = FEC_TYPE_PWID;
		fec->u.pwid.type = map->fec.pwid.type;
		fec->u.pwid.pwid = map->fec.pwid.pwid;
		fec->u.pwid.lsr_id = lsr_id;
		break;
	}
}

void
lde_send_labelmapping(struct lde_nbr *ln, struct fec_node *fn, int single)
{
	struct lde_req	*lre;
	struct lde_map	*me;
	struct map	 map;
	struct l2vpn_pw	*pw;

	/*
	 * This function skips SL.1 - 3 and SL.9 - 14 because the label
	 * allocation is done way earlier (because of the merging nature of
	 * ldpd).
	 */

	lde_fec2map(&fn->fec, &map);
	if (fn->fec.type == FEC_TYPE_PWID) {
		pw = (struct l2vpn_pw *) fn->data;
		if (pw == NULL || pw->lsr_id.s_addr != ln->id.s_addr)
			/* not the remote end of the pseudowire */
			return;

		map.flags |= F_MAP_PW_IFMTU;
		map.fec.pwid.ifmtu = pw->l2vpn->mtu;
		if (pw->flags & F_PW_CWORD)
			map.flags |= F_MAP_PW_CWORD;
		if (pw->flags & F_PW_STATUSTLV) {
			map.flags |= F_MAP_PW_STATUS;
			/* VPLS are always up */
			map.pw_status = PW_FORWARDING;
		}
	}
	map.label = fn->local_label;

	/* SL.6: is there a pending request for this mapping? */
	lre = (struct lde_req *)fec_find(&ln->recv_req, &fn->fec);
	if (lre) {
		/* set label request msg id in the mapping response. */
		map.requestid = lre->msgid;
		map.flags = F_MAP_REQ_ID;

		/* SL.7: delete record of pending request */
		lde_req_del(ln, lre, 0);
	}

	/* SL.4: send label mapping */
	lde_imsg_compose_ldpe(IMSG_MAPPING_ADD, ln->peerid, 0,
	    &map, sizeof(map));
	if (single)
		lde_imsg_compose_ldpe(IMSG_MAPPING_ADD_END, ln->peerid, 0,
		    NULL, 0);

	/* SL.5: record sent label mapping */
	me = (struct lde_map *)fec_find(&ln->sent_map, &fn->fec);
	if (me == NULL)
		me = lde_map_add(ln, fn, 1);
	me->map = map;
}

void
lde_send_labelwithdraw(struct lde_nbr *ln, struct fec_node *fn, uint32_t label)
{
	struct lde_wdraw	*lw;
	struct map		 map;
	struct fec		*f;
	struct l2vpn_pw		*pw;

	if (fn) {
		lde_fec2map(&fn->fec, &map);
		map.label = fn->local_label;
		if (fn->fec.type == FEC_TYPE_PWID) {
			pw = (struct l2vpn_pw *) fn->data;
			if (pw == NULL || pw->lsr_id.s_addr != ln->id.s_addr)
				/* not the remote end of the pseudowire */
				return;

			if (pw->flags & F_PW_CWORD)
				map.flags |= F_MAP_PW_CWORD;
		}
	} else {
		memset(&map, 0, sizeof(map));
		map.type = MAP_TYPE_WILDCARD;
		map.label = label;
	}

	/* SWd.1: send label withdraw. */
	lde_imsg_compose_ldpe(IMSG_WITHDRAW_ADD, ln->peerid, 0,
 	    &map, sizeof(map));
	lde_imsg_compose_ldpe(IMSG_WITHDRAW_ADD_END, ln->peerid, 0, NULL, 0);

	/* SWd.2: record label withdraw. */
	if (fn) {
		lw = (struct lde_wdraw *)fec_find(&ln->sent_wdraw, &fn->fec);
		if (lw == NULL)
			lw = lde_wdraw_add(ln, fn);
		lw->label = map.label;
	} else {
		RB_FOREACH(f, fec_tree, &ft) {
			fn = (struct fec_node *)f;

			lw = (struct lde_wdraw *)fec_find(&ln->sent_wdraw,
			    &fn->fec);
			if (lw == NULL)
				lw = lde_wdraw_add(ln, fn);
			lw->label = map.label;
		}
	}
}

void
lde_send_labelwithdraw_all(struct fec_node *fn, uint32_t label)
{
	struct lde_nbr		*ln;

	RB_FOREACH(ln, nbr_tree, &lde_nbrs)
		lde_send_labelwithdraw(ln, fn, label);
}

void
lde_send_labelrelease(struct lde_nbr *ln, struct fec_node *fn, uint32_t label)
{
	struct map		 map;
	struct l2vpn_pw		*pw;

	if (fn) {
		lde_fec2map(&fn->fec, &map);
		if (fn->fec.type == FEC_TYPE_PWID) {
			pw = (struct l2vpn_pw *) fn->data;
			if (pw == NULL || pw->lsr_id.s_addr != ln->id.s_addr)
				/* not the remote end of the pseudowire */
				return;

			if (pw->flags & F_PW_CWORD)
				map.flags |= F_MAP_PW_CWORD;
		}
	} else {
		memset(&map, 0, sizeof(map));
		map.type = MAP_TYPE_WILDCARD;
	}
	map.label = label;

	lde_imsg_compose_ldpe(IMSG_RELEASE_ADD, ln->peerid, 0,
	    &map, sizeof(map));
	lde_imsg_compose_ldpe(IMSG_RELEASE_ADD_END, ln->peerid, 0, NULL, 0);
}

void
lde_send_notification(uint32_t peerid, uint32_t code, uint32_t msgid,
    uint16_t type)
{
	struct notify_msg nm;

	memset(&nm, 0, sizeof(nm));
	nm.status = code;
	/* 'msgid' and 'type' should be in network byte order */
	nm.messageid = msgid;
	nm.type = type;

	lde_imsg_compose_ldpe(IMSG_NOTIFICATION_SEND, peerid, 0,
	    &nm, sizeof(nm));
}

static __inline int
lde_nbr_compare(struct lde_nbr *a, struct lde_nbr *b)
{
	return (a->peerid - b->peerid);
}

struct lde_nbr *
lde_nbr_new(uint32_t peerid, struct in_addr *id)
{
	struct lde_nbr	*ln;

	if ((ln = calloc(1, sizeof(*ln))) == NULL)
		fatal(__func__);

	ln->id = *id;
	ln->peerid = peerid;
	fec_init(&ln->recv_map);
	fec_init(&ln->sent_map);
	fec_init(&ln->recv_req);
	fec_init(&ln->sent_req);
	fec_init(&ln->sent_wdraw);

	TAILQ_INIT(&ln->addr_list);

	if (RB_INSERT(nbr_tree, &lde_nbrs, ln) != NULL)
		fatalx("lde_nbr_new: RB_INSERT failed");

	return (ln);
}

void
lde_nbr_del(struct lde_nbr *ln)
{
	struct fec		*f;
	struct fec_node		*fn;
	struct fec_nh		*fnh;
	struct l2vpn_pw		*pw;

	if (ln == NULL)
		return;

	/* uninstall received mappings */
	RB_FOREACH(f, fec_tree, &ft) {
		fn = (struct fec_node *)f;

		LIST_FOREACH(fnh, &fn->nexthops, entry) {
			switch (f->type) {
			case FEC_TYPE_IPV4:
				if (!lde_address_find(ln, &fnh->nexthop))
					continue;
				break;
			case FEC_TYPE_PWID:
				if (f->u.pwid.lsr_id.s_addr != ln->id.s_addr)
					continue;
				pw = (struct l2vpn_pw *) fn->data;
				if (pw)
					l2vpn_pw_reset(pw);
				break;
			default:
				break;
			}

			lde_send_delete_klabel(fn, fnh);
			fnh->remote_label = NO_LABEL;
		}
	}

	lde_address_list_free(ln);

	fec_clear(&ln->recv_map, lde_map_free);
	fec_clear(&ln->sent_map, lde_map_free);
	fec_clear(&ln->recv_req, free);
	fec_clear(&ln->sent_req, free);
	fec_clear(&ln->sent_wdraw, free);

	RB_REMOVE(nbr_tree, &lde_nbrs, ln);

	free(ln);
}

struct lde_nbr *
lde_nbr_find(uint32_t peerid)
{
	struct lde_nbr		 ln;

	ln.peerid = peerid;

	return (RB_FIND(nbr_tree, &lde_nbrs, &ln));
}

struct lde_nbr *
lde_nbr_find_by_lsrid(struct in_addr addr)
{
	struct lde_nbr		*ln;

	RB_FOREACH(ln, nbr_tree, &lde_nbrs)
		if (ln->id.s_addr == addr.s_addr)
			return (ln);

	return (NULL);
}

struct lde_nbr *
lde_nbr_find_by_addr(struct in_addr addr)
{
	struct lde_nbr		*ln;

	RB_FOREACH(ln, nbr_tree, &lde_nbrs)
		if (lde_address_find(ln, &addr) != NULL)
			return (ln);

	return (NULL);
}

void
lde_nbr_clear(void)
{
	struct lde_nbr	*ln;

	 while ((ln = RB_ROOT(&lde_nbrs)) != NULL)
		lde_nbr_del(ln);
}

struct lde_map *
lde_map_add(struct lde_nbr *ln, struct fec_node *fn, int sent)
{
	struct lde_map  *me;

	me = calloc(1, sizeof(*me));
	if (me == NULL)
		fatal(__func__);

	me->fec = fn->fec;
	me->nexthop = ln;

	if (sent) {
		LIST_INSERT_HEAD(&fn->upstream, me, entry);
		if (fec_insert(&ln->sent_map, &me->fec))
			log_warnx("failed to add %s to sent map",
			    log_fec(&me->fec));
			/* XXX on failure more cleanup is needed */
	} else {
		LIST_INSERT_HEAD(&fn->downstream, me, entry);
		if (fec_insert(&ln->recv_map, &me->fec))
			log_warnx("failed to add %s to recv map",
			    log_fec(&me->fec));
	}

	return (me);
}

void
lde_map_del(struct lde_nbr *ln, struct lde_map *me, int sent)
{
	if (sent)
		fec_remove(&ln->sent_map, &me->fec);
	else
		fec_remove(&ln->recv_map, &me->fec);

	lde_map_free(me);
}

void
lde_map_free(void *ptr)
{
	struct lde_map	*map = ptr;

	LIST_REMOVE(map, entry);
	free(map);
}

struct lde_req *
lde_req_add(struct lde_nbr *ln, struct fec *fec, int sent)
{
	struct fec_tree	*t;
	struct lde_req	*lre;

	t = sent ? &ln->sent_req : &ln->recv_req;

	lre = calloc(1, sizeof(*lre));
	if (lre != NULL) {
		lre->fec = *fec;

		if (fec_insert(t, &lre->fec)) {
			log_warnx("failed to add %s/%u to %s req",
			    log_fec(&lre->fec), sent ? "sent" : "recv");
			free(lre);
			return (NULL);
		}
	}

	return (lre);
}

void
lde_req_del(struct lde_nbr *ln, struct lde_req *lre, int sent)
{
	if (sent)
		fec_remove(&ln->sent_req, &lre->fec);
	else
		fec_remove(&ln->recv_req, &lre->fec);

	free(lre);
}

struct lde_wdraw *
lde_wdraw_add(struct lde_nbr *ln, struct fec_node *fn)
{
	struct lde_wdraw  *lw;

	lw = calloc(1, sizeof(*lw));
	if (lw == NULL)
		fatal(__func__);

	lw->fec = fn->fec;

	if (fec_insert(&ln->sent_wdraw, &lw->fec))
		log_warnx("failed to add %s to sent wdraw",
		    log_fec(&lw->fec));

	return (lw);
}

void
lde_wdraw_del(struct lde_nbr *ln, struct lde_wdraw *lw)
{
	fec_remove(&ln->sent_wdraw, &lw->fec);
	free(lw);
}

void
lde_change_egress_label(int was_implicit)
{
	struct lde_nbr	*ln;
	struct fec	*f;
	struct fec_node	*fn;

	/* explicit withdraw */
	if (was_implicit)
		lde_send_labelwithdraw_all(NULL, MPLS_LABEL_IMPLNULL);
	else
		lde_send_labelwithdraw_all(NULL, MPLS_LABEL_IPV4NULL);

	/* update label of connected prefixes */
	RB_FOREACH(ln, nbr_tree, &lde_nbrs) {
		RB_FOREACH(f, fec_tree, &ft) {
			fn = (struct fec_node *)f;
			if (fn->local_label > MPLS_LABEL_RESERVED_MAX)
				continue;

			fn->local_label = egress_label(fn->fec.type);
			lde_send_labelmapping(ln, fn, 0);
		}

		lde_imsg_compose_ldpe(IMSG_MAPPING_ADD_END, ln->peerid, 0,
		    NULL, 0);
	}
}

int
lde_address_add(struct lde_nbr *ln, struct in_addr *addr)
{
	struct lde_addr		*address;

	if (lde_address_find(ln, addr) != NULL)
		return (-1);

	if ((address = calloc(1, sizeof(*address))) == NULL)
		fatal(__func__);

	address->addr = *addr;

	TAILQ_INSERT_TAIL(&ln->addr_list, address, entry);

	log_debug("%s: added %s", __func__, inet_ntoa(*addr));

	return (0);
}

int
lde_address_del(struct lde_nbr *ln, struct in_addr *addr)
{
	struct lde_addr		*address;

	address = lde_address_find(ln, addr);
	if (address == NULL)
		return (-1);

	TAILQ_REMOVE(&ln->addr_list, address, entry);

	free(address);

	log_debug("%s: deleted %s", __func__, inet_ntoa(*addr));

	return (0);
}

struct lde_addr *
lde_address_find(struct lde_nbr *ln, struct in_addr *addr)
{
	struct lde_addr		*address = NULL;

	TAILQ_FOREACH(address, &ln->addr_list, entry) {
		if (address->addr.s_addr == addr->s_addr)
			return (address);
	}

	return (NULL);
}

void
lde_address_list_free(struct lde_nbr *ln)
{
	struct lde_addr		*addr;

	while ((addr = TAILQ_FIRST(&ln->addr_list)) != NULL) {
		TAILQ_REMOVE(&ln->addr_list, addr, entry);
		free(addr);
	}
}
