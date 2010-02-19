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

void		 lde_nbr_init(u_int32_t);
void		 lde_nbr_free(void);
struct lde_nbr	*lde_nbr_find(u_int32_t);
struct lde_nbr	*lde_nbr_new(u_int32_t, struct lde_nbr *);
void		 lde_nbr_del(struct lde_nbr *);

void		 lde_address_list_free(struct lde_nbr *);
void		 lde_req_list_free(struct lde_nbr *);
void		 lde_map_list_free(struct lde_nbr *);

struct ldpd_conf	*ldeconf = NULL, *nconf = NULL;
struct imsgev		*iev_ldpe;
struct imsgev		*iev_main;
struct lde_nbr		*nbrself;

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

	if ((pw = getpwnam(LDPD_USER)) == NULL)
		fatal("getpwnam");

	if (chroot(pw->pw_dir) == -1)
		fatal("chroot");
	if (chdir("/") == -1)
		fatal("chdir(\"/\")");

	setproctitle("label decision engine");
	ldpd_process = PROC_LDE_ENGINE;

	if (setgroups(1, &pw->pw_gid) ||
	    setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) ||
	    setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid))
		fatal("can't drop privileges");

	event_init();
	lde_nbr_init(NBR_HASHSIZE);

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

	rt_init();

	gettimeofday(&now, NULL);
	ldeconf->uptime = now.tv_sec;

	event_dispatch();

	lde_shutdown();
	/* NOTREACHED */

	return (0);
}

void
lde_shutdown(void)
{
	rt_clear();

	lde_nbr_free();

	msgbuf_clear(&iev_ldpe->ibuf.w);
	free(iev_ldpe);
	msgbuf_clear(&iev_main->ibuf.w);
	free(iev_main);
	free(ldeconf);

	log_info("label decision engine exiting");
	_exit(0);
}

int
lde_imsg_compose_ldpe(int type, u_int32_t peerid, pid_t pid, void *data,
    u_int16_t datalen)
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
	struct lde_nbr		 rn, *nbr;
	struct map		 map;
	struct timespec		 tp;
	struct in_addr		 addr;
	ssize_t			 n;
	time_t			 now;
	int			 state, shut = 0, verbose;

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1)
			fatal("imsg_read error");
		if (n == 0)	/* connection closed */
			shut = 1;
	}
	if (event & EV_WRITE) {
		if (msgbuf_write(&ibuf->w) == -1)
			fatal("msgbuf_write");
	}

	clock_gettime(CLOCK_MONOTONIC, &tp);
	now = tp.tv_sec;

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("lde_dispatch_imsg: imsg_read error");
		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_LABEL_MAPPING:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(map))
				fatalx("invalid size of OE request");
			memcpy(&map, imsg.data, sizeof(map));

			nbr = lde_nbr_find(imsg.hdr.peerid);
			if (nbr == NULL) {
				log_debug("lde_dispatch_imsg: cannot find "
				    "lde neighbor");
				return;
			}

			lde_check_mapping(&map, nbr);
			break;
		case IMSG_LABEL_MAPPING_FULL:
			rt_snap(imsg.hdr.peerid);

			lde_imsg_compose_ldpe(IMSG_MAPPING_ADD_END,
			    imsg.hdr.peerid, 0, 0, 0);

			break;
		case IMSG_LABEL_REQUEST:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(map))
				fatalx("invalid size of OE request");
			memcpy(&map, imsg.data, sizeof(map));

			nbr = lde_nbr_find(imsg.hdr.peerid);
			if (nbr == NULL) {
				log_debug("lde_dispatch_imsg: cannot find "
				    "lde neighbor");
				return;
			}

			lde_check_request(&map, nbr);
			break;
		case IMSG_ADDRESS_ADD:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(addr))
				fatalx("invalid size of OE request");
			memcpy(&addr, imsg.data, sizeof(addr));

			nbr = lde_nbr_find(imsg.hdr.peerid);
			if (nbr == NULL) {
				log_debug("lde_dispatch_imsg: cannot find "
				    "lde neighbor");
				return;
			}

			if (lde_address_add(nbr, &addr) < 0) {
				log_debug("lde_dispatch_imsg: cannot add "
				    "address %s, it already exists",
				    inet_ntoa(addr));
			}

			break;
		case IMSG_ADDRESS_DEL:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(addr))
				fatalx("invalid size of OE request");
			memcpy(&addr, imsg.data, sizeof(addr));

			nbr = lde_nbr_find(imsg.hdr.peerid);
			if (nbr == NULL) {
				log_debug("lde_dispatch_imsg: cannot find "
				    "lde neighbor");
				return;
			}

			if (lde_address_del(nbr, &addr) < 0) {
				log_debug("lde_dispatch_imsg: cannot delete "
				    "address %s, it does not exists",
				    inet_ntoa(addr));
			}

			break;
		case IMSG_NEIGHBOR_UP:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(rn))
				fatalx("invalid size of OE request");
			memcpy(&rn, imsg.data, sizeof(rn));

			if (lde_nbr_find(imsg.hdr.peerid))
				fatalx("lde_dispatch_imsg: "
				    "neighbor already exists");
			lde_nbr_new(imsg.hdr.peerid, &rn);
			break;
		case IMSG_NEIGHBOR_DOWN:
			lde_nbr_del(lde_nbr_find(imsg.hdr.peerid));
			break;
		case IMSG_NEIGHBOR_CHANGE:
			if (imsg.hdr.len - IMSG_HEADER_SIZE != sizeof(state))
				fatalx("invalid size of OE request");
			memcpy(&state, imsg.data, sizeof(state));

			nbr = lde_nbr_find(imsg.hdr.peerid);
			if (nbr == NULL)
				break;

			nbr->state = state;
			break;
		case IMSG_CTL_SHOW_LIB:
			rt_dump(imsg.hdr.pid);

			imsg_compose_event(iev_ldpe, IMSG_CTL_END, 0,
			    imsg.hdr.pid, -1, NULL, 0);
			break;
		case IMSG_CTL_LOG_VERBOSE:
			/* already checked by ldpe */
			memcpy(&verbose, imsg.data, sizeof(verbose));
			log_verbose(verbose);
			break;
		default:
			log_debug("lde_dispatch_imsg: unexpected imsg %d",
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
	struct imsg		 imsg;
	struct kroute		 kr;
	struct rroute		 rr;
	struct imsgev		*iev = bula;
	struct imsgbuf		*ibuf = &iev->ibuf;
	ssize_t			 n;
	int			 shut = 0;

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1)
			fatal("imsg_read error");
		if (n == 0)	/* connection closed */
			shut = 1;
	}
	if (event & EV_WRITE) {
		if (msgbuf_write(&ibuf->w) == -1)
			fatal("msgbuf_write");
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("lde_dispatch_parent: imsg_read error");
		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_NETWORK_ADD:
			if (imsg.hdr.len != IMSG_HEADER_SIZE + sizeof(rr)) {
				log_warnx("lde_dispatch_parent: "
				    "wrong imsg len");
				break;
			}
			memcpy(&rr, imsg.data, sizeof(rr));

			lde_kernel_insert(&rr.kr);

			break;
		case IMSG_NETWORK_DEL:
			if (imsg.hdr.len != IMSG_HEADER_SIZE + sizeof(kr)) {
				log_warnx("lde_dispatch_parent: "
				    "wrong imsg len");
				break;
			}
			memcpy(&kr, imsg.data, sizeof(kr));

			lde_kernel_remove(&kr);

			break;
		case IMSG_RECONF_CONF:
			if ((nconf = malloc(sizeof(struct ldpd_conf))) ==
			    NULL)
				fatal(NULL);
			memcpy(nconf, imsg.data, sizeof(struct ldpd_conf));

			break;
		case IMSG_RECONF_AREA:
			break;
		case IMSG_RECONF_IFACE:
			break;
		case IMSG_RECONF_END:
			break;
		default:
			log_debug("lde_dispatch_parent: unexpected imsg %d",
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

u_int32_t
lde_router_id(void)
{
	return (ldeconf->rtr_id.s_addr);
}

void
lde_send_insert_klabel(struct rt_node *r)
{
	struct kroute	kr;

	bzero(&kr, sizeof(kr));
	kr.prefix.s_addr = r->prefix.s_addr;
	kr.nexthop.s_addr = r->nexthop.s_addr;
	kr.local_label = r->local_label;
	kr.remote_label = r->remote_label;
	kr.prefixlen = r->prefixlen;
	kr.ext_tag = r->ext_tag;

	imsg_compose_event(iev_main, IMSG_KLABEL_INSERT, 0, 0, -1,
	     &kr, sizeof(kr));
}

void
lde_send_change_klabel(struct rt_node *r)
{
	struct kroute	kr;

	bzero(&kr, sizeof(kr));
	kr.prefix.s_addr = r->prefix.s_addr;
	kr.nexthop.s_addr = r->nexthop.s_addr;
	kr.local_label = r->local_label;
	kr.remote_label = r->remote_label;
	kr.prefixlen = r->prefixlen;
	kr.ext_tag = r->ext_tag;

	imsg_compose_event(iev_main, IMSG_KLABEL_CHANGE, 0, 0, -1,
	     &kr, sizeof(kr));
}

void
lde_send_delete_klabel(struct rt_node *r)
{
	struct kroute	 kr;

	bzero(&kr, sizeof(kr));
	kr.prefix.s_addr = r->prefix.s_addr;
	kr.prefixlen = r->prefixlen;

	imsg_compose_event(iev_main, IMSG_KLABEL_DELETE, 0, 0, -1,
	     &kr, sizeof(kr));
}

void
lde_send_labelrequest(u_int32_t peerid, struct map *map)
{
	imsg_compose_event(iev_ldpe, IMSG_REQUEST_ADD, peerid, 0,
	     -1, map, sizeof(*map));
	imsg_compose_event(iev_ldpe, IMSG_REQUEST_ADD_END, peerid, 0,
	     -1, NULL, 0);
}

void
lde_send_labelmapping(u_int32_t peerid, struct map *map)
{
	imsg_compose_event(iev_ldpe, IMSG_MAPPING_ADD, peerid, 0,
	     -1, map, sizeof(*map));
	imsg_compose_event(iev_ldpe, IMSG_MAPPING_ADD_END, peerid, 0,
	     -1, NULL, 0);
}

void
lde_send_labelrelease(u_int32_t peerid, struct map *map)
{
	imsg_compose_event(iev_ldpe, IMSG_RELEASE_ADD, peerid, 0,
	    -1, map, sizeof(*map));
	imsg_compose_event(iev_ldpe, IMSG_RELEASE_ADD_END, peerid, 0,
	    -1, NULL, 0);
}

void
lde_send_notification(u_int32_t peerid, u_int32_t code, u_int32_t msgid,
    u_int32_t type)
{
	struct notify_msg nm;

	bzero(&nm, sizeof(nm));

	/* Every field is in host byte order, to keep things clear */
	nm.status = code;
	nm.messageid = ntohl(msgid);
	nm.type = type;

	imsg_compose_event(iev_ldpe, IMSG_NOTIFICATION_SEND, peerid, 0,
	    -1, &nm, sizeof(nm));
}

LIST_HEAD(lde_nbr_head, lde_nbr);

struct nbr_table {
	struct lde_nbr_head	*hashtbl;
	u_int32_t		 hashmask;
} ldenbrtable;

#define LDE_NBR_HASH(x)		\
	&ldenbrtable.hashtbl[(x) & ldenbrtable.hashmask]

void
lde_nbr_init(u_int32_t hashsize)
{
	struct lde_nbr_head	*head;
	u_int32_t		 hs, i;

	for (hs = 1; hs < hashsize; hs <<= 1)
		;
	ldenbrtable.hashtbl = calloc(hs, sizeof(struct lde_nbr_head));
	if (ldenbrtable.hashtbl == NULL)
		fatal("lde_nbr_init");

	for (i = 0; i < hs; i++)
		LIST_INIT(&ldenbrtable.hashtbl[i]);

	ldenbrtable.hashmask = hs - 1;

	if ((nbrself = calloc(1, sizeof(*nbrself))) == NULL)
		fatal("lde_nbr_init");

	nbrself->id.s_addr = lde_router_id();
	nbrself->peerid = NBR_IDSELF;
	nbrself->state = NBR_STA_DOWN;
	nbrself->self = 1;
	head = LDE_NBR_HASH(NBR_IDSELF);
	LIST_INSERT_HEAD(head, nbrself, hash);
}

void
lde_nbr_free(void)
{
	free(nbrself);
	free(ldenbrtable.hashtbl);
}

struct lde_nbr *
lde_nbr_find(u_int32_t peerid)
{
	struct lde_nbr_head	*head;
	struct lde_nbr		*nbr;

	head = LDE_NBR_HASH(peerid);

	LIST_FOREACH(nbr, head, hash) {
		if (nbr->peerid == peerid)
			return (nbr);
	}

	return (NULL);
}

struct lde_nbr *
lde_nbr_new(u_int32_t peerid, struct lde_nbr *new)
{
	struct lde_nbr_head	*head;
	struct lde_nbr		*nbr;
	struct iface		*iface;

	if (lde_nbr_find(peerid))
		return (NULL);

	if ((nbr = calloc(1, sizeof(*nbr))) == NULL)
		fatal("lde_nbr_new");

	memcpy(nbr, new, sizeof(*nbr));
	nbr->peerid = peerid;

	TAILQ_INIT(&nbr->addr_list);
	TAILQ_INIT(&nbr->req_list);
	TAILQ_INIT(&nbr->sent_map_list);
	TAILQ_INIT(&nbr->recv_map_list);
	TAILQ_INIT(&nbr->labels_list);

	head = LDE_NBR_HASH(peerid);
	LIST_INSERT_HEAD(head, nbr, hash);

	LIST_FOREACH(iface, &ldeconf->iface_list, entry) {
		if (iface->ifindex == new->ifindex) {
			LIST_INSERT_HEAD(&iface->lde_nbr_list, nbr, entry);
			break;
		}
	}

	return (nbr);
}

void
lde_nbr_del(struct lde_nbr *nbr)
{
	if (nbr == NULL)
		return;

	lde_req_list_free(nbr);
	lde_map_list_free(nbr);
	lde_address_list_free(nbr);
	lde_label_list_free(nbr);

	LIST_REMOVE(nbr, hash);
	LIST_REMOVE(nbr, entry);

	free(nbr);
}

int
lde_address_add(struct lde_nbr *lr, struct in_addr *addr)
{
	struct lde_nbr_address	*address;

	if (lde_address_find(lr, addr) != NULL)
		return (-1);

	if ((address = calloc(1, sizeof(*address))) == NULL)
		fatal("lde_address_add");

	address->addr.s_addr = addr->s_addr;

	TAILQ_INSERT_TAIL(&lr->addr_list, address, entry);

	log_debug("lde_address_add: added %s", inet_ntoa(*addr));

	return (0);
}

struct lde_nbr_address *
lde_address_find(struct lde_nbr *lr, struct in_addr *addr)
{
	struct lde_nbr_address	*address = NULL;

	TAILQ_FOREACH(address, &lr->addr_list, entry) {
		if (address->addr.s_addr == addr->s_addr)
			return (address);
	}

	return (NULL);
}

int
lde_address_del(struct lde_nbr *lr, struct in_addr *addr)
{
	struct lde_nbr_address	*address;

	address = lde_address_find(lr, addr);
	if (address == NULL)
		return (-1);

	TAILQ_REMOVE(&lr->addr_list, address, entry);

	free(address);

	log_debug("lde_address_del: deleted %s", inet_ntoa(*addr));

	return (0);
}

void
lde_address_list_free(struct lde_nbr *nbr)
{
	struct lde_nbr_address	*addr;

	while ((addr = TAILQ_FIRST(&nbr->addr_list)) != NULL) {
		TAILQ_REMOVE(&nbr->addr_list, addr, entry);
		free(addr);
	}
}

void
lde_req_list_free(struct lde_nbr *nbr)
{
	struct lde_req_entry	*req;

	while ((req = TAILQ_FIRST(&nbr->req_list)) != NULL) {
		TAILQ_REMOVE(&nbr->req_list, req, entry);
		free(req);
	}
}

void
lde_map_list_free(struct lde_nbr *nbr)
{
	struct lde_map_entry	*map;

	while ((map = TAILQ_FIRST(&nbr->recv_map_list)) != NULL) {
		TAILQ_REMOVE(&nbr->recv_map_list, map, entry);
		free(map);
	}

	while ((map = TAILQ_FIRST(&nbr->sent_map_list)) != NULL) {
		TAILQ_REMOVE(&nbr->sent_map_list, map, entry);
		free(map);
	}
}

struct lde_nbr *
lde_find_address(struct in_addr address)
{
	struct iface	*iface;
	struct lde_nbr	*ln;

	LIST_FOREACH(iface, &ldeconf->iface_list, entry) {
		LIST_FOREACH(ln, &iface->lde_nbr_list, entry) {
			if (lde_address_find(ln, &address) != NULL)
				return (ln);
		}
	}

	return (NULL);
}
