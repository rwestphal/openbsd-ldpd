/*	$OpenBSD$ */

/*
 * Copyright (c) 2009 Michele Marchetto <michele@openbsd.org>
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
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <sys/tree.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <net/route.h>
#include <netmpls/mpls.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "ldpd.h"
#include "log.h"

struct {
	uint32_t		rtseq;
	pid_t			pid;
	int			fib_sync;
	int			fd;
	int			ioctl_fd;
	struct event		ev;
} kr_state;

struct kroute_node {
	TAILQ_ENTRY(kroute_node)	 entry;
	struct kroute_priority		*kprio;		/* back pointer */
	struct kroute			 r;
};

struct kroute_priority {
	TAILQ_ENTRY(kroute_priority)	 entry;
	struct kroute_prefix		*kp;		/* back pointer */
	uint8_t				 priority;
	TAILQ_HEAD(, kroute_node)	 nexthops;
};

struct kroute_prefix {
	RB_ENTRY(kroute_prefix)			 entry;
	struct in_addr				 prefix;
	uint8_t					 prefixlen;
	TAILQ_HEAD(plist, kroute_priority)	 priorities;
};

struct kif_addr {
	TAILQ_ENTRY(kif_addr)	 entry;
	struct kaddr		 addr;
};

struct kif_node {
	RB_ENTRY(kif_node)	 entry;
	TAILQ_HEAD(, kif_addr)	 addrs;
	struct kif		 k;
	struct kpw		*kpw;
};

void	kr_redist_remove(struct kroute *);
int	kr_redist_eval(struct kroute *);
void	kr_redistribute(struct kroute_prefix *);
int	kroute_compare(struct kroute_prefix *, struct kroute_prefix *);
int	kif_compare(struct kif_node *, struct kif_node *);

struct kroute_prefix	*kroute_find(in_addr_t, uint8_t);
struct kroute_priority	*kroute_find_prio(in_addr_t, uint8_t, uint8_t);
struct kroute_node	*kroute_find_gw(in_addr_t, uint8_t, uint8_t,
			    struct in_addr);
int			 kroute_insert(struct kroute *);
int			 kroute_uninstall(struct kroute_node *);
int			 kroute_remove(struct kroute *);
void			 kroute_clear(void);

struct kif_node		*kif_find(unsigned short);
struct kif_node		*kif_insert(unsigned short);
int			 kif_remove(struct kif_node *);
void			 kif_clear(void);
struct kif_node		*kif_update(unsigned short, int, struct if_data *,
			    struct sockaddr_dl *, int *);

struct kroute_priority	*kroute_match(in_addr_t);

uint8_t		prefixlen_classful(in_addr_t);
void		get_rtaddrs(int, struct sockaddr *, struct sockaddr **);
void		if_change(unsigned short, int, struct if_data *,
		    struct sockaddr_dl *);
void		if_newaddr(unsigned short, struct sockaddr_in *,
		    struct sockaddr_in *, struct sockaddr_in *);
void		if_deladdr(unsigned short, struct sockaddr_in *,
		    struct sockaddr_in *, struct sockaddr_in *);
void		if_announce(void *);

int		send_rtmsg(int, int, struct kroute *, uint32_t);
int		dispatch_rtmsg(void);
int		fetchtable(void);
int		fetchifs(unsigned short);
int		rtmsg_process(char *, size_t);

RB_HEAD(kroute_tree, kroute_prefix)	krt;
RB_PROTOTYPE(kroute_tree, kroute_prefix, entry, kroute_compare)
RB_GENERATE(kroute_tree, kroute_prefix, entry, kroute_compare)

RB_HEAD(kif_tree, kif_node)		kit;
RB_PROTOTYPE(kif_tree, kif_node, entry, kif_compare)
RB_GENERATE(kif_tree, kif_node, entry, kif_compare)

int
kif_init(void)
{
	RB_INIT(&kit);
	/* init also krt tree so that we can call kr_shutdown() */
	RB_INIT(&krt);
	kr_state.fib_sync = 0;	/* decoupled */

	if (fetchifs(0) == -1)
		return (-1);

	return (0);
}

void
kif_redistribute(void)
{
	struct kif_node		*kif;
	struct kif_addr		*ka;

	RB_FOREACH(kif, kif_tree, &kit) {
		TAILQ_FOREACH(ka, &kif->addrs, entry)
			main_imsg_compose_ldpe(IMSG_NEWADDR, 0, &ka->addr,
			    sizeof(struct kaddr));
	}
}

int
kr_init(int fs)
{
	int		opt = 0, rcvbuf, default_rcvbuf;
	socklen_t	optlen;
	unsigned int	rtfilter;

	kr_state.fib_sync = fs;

	if ((kr_state.fd = socket(AF_ROUTE,
	    SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) == -1) {
		log_warn("kr_init: socket");
		return (-1);
	}

	/* not interested in my own messages */
	if (setsockopt(kr_state.fd, SOL_SOCKET, SO_USELOOPBACK,
	    &opt, sizeof(opt)) == -1)
		log_warn("kr_init: setsockopt(SO_USELOOPBACK)");

	/* filter out unwanted messages */
	rtfilter = ROUTE_FILTER(RTM_ADD) | ROUTE_FILTER(RTM_GET) |
	    ROUTE_FILTER(RTM_CHANGE) | ROUTE_FILTER(RTM_DELETE) |
	    ROUTE_FILTER(RTM_IFINFO) | ROUTE_FILTER(RTM_NEWADDR) |
	    ROUTE_FILTER(RTM_DELADDR) | ROUTE_FILTER(RTM_IFANNOUNCE);

	if (setsockopt(kr_state.fd, PF_ROUTE, ROUTE_MSGFILTER,
	    &rtfilter, sizeof(rtfilter)) == -1)
		log_warn("kr_init: setsockopt(ROUTE_MSGFILTER)");

	/* grow receive buffer, don't wanna miss messages */
	optlen = sizeof(default_rcvbuf);
	if (getsockopt(kr_state.fd, SOL_SOCKET, SO_RCVBUF,
	    &default_rcvbuf, &optlen) == -1)
		log_warn("kr_init getsockopt SOL_SOCKET SO_RCVBUF");
	else
		for (rcvbuf = MAX_RTSOCK_BUF;
		    rcvbuf > default_rcvbuf &&
		    setsockopt(kr_state.fd, SOL_SOCKET, SO_RCVBUF,
		    &rcvbuf, sizeof(rcvbuf)) == -1 && errno == ENOBUFS;
		    rcvbuf /= 2)
			;	/* nothing */

	kr_state.pid = getpid();
	kr_state.rtseq = 1;

	if (fetchtable() == -1)
		return (-1);

	event_set(&kr_state.ev, kr_state.fd, EV_READ | EV_PERSIST,
	    kr_dispatch_msg, NULL);
	event_add(&kr_state.ev, NULL);

	if ((kr_state.ioctl_fd = socket(AF_INET,
	    SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) == -1) {
		log_warn("kr_init: ioctl socket");
		return (-1);
	}

	return (0);
}

int
kr_change(struct kroute *kroute)
{
	struct kroute_node	*kn;
	int			 action = RTM_ADD;
	char			 buf[16];

	kn = kroute_find_gw(kroute->prefix.s_addr, kroute->prefixlen,
	    RTP_ANY, kroute->nexthop);
	if (kn == NULL) {
		log_warnx("kr_change: lost FEC %s/%d nexthop %s",
		    inet_ntoa(kroute->prefix), kroute->prefixlen,
		    inet_ntop(AF_INET, &kroute->nexthop, buf, sizeof(buf)));
		return (-1);
	}

	if (kn->r.flags & F_LDPD_INSERTED)
		action = RTM_CHANGE;

	kn->r.local_label = kroute->local_label;
	kn->r.remote_label = kroute->remote_label;
	kn->r.flags = kn->r.flags | F_LDPD_INSERTED;

	/* send update */
	if (send_rtmsg(kr_state.fd, action, &kn->r, AF_MPLS) == -1)
		return (-1);

	if (kn->r.nexthop.s_addr != INADDR_ANY &&
	    kn->r.remote_label != NO_LABEL) {
		if (send_rtmsg(kr_state.fd, RTM_CHANGE, &kn->r, AF_INET) == -1)
			return (-1);
	}

	return  (0);
}

int
kr_delete(struct kroute *kroute)
{
	struct kroute_node	*kn;
	int			 update = 0;

	kn = kroute_find_gw(kroute->prefix.s_addr, kroute->prefixlen,
	    RTP_ANY, kroute->nexthop);
	if (kn == NULL)
		return (0);

	if (!(kn->r.flags & F_LDPD_INSERTED))
		return (0);
	if (kn->r.nexthop.s_addr != INADDR_ANY &&
	    kn->r.remote_label != NO_LABEL)
		update = 1;

	/* kill MPLS LSP */
	if (send_rtmsg(kr_state.fd, RTM_DELETE, &kn->r, AF_MPLS) == -1)
		return (-1);

	kn->r.flags &= ~F_LDPD_INSERTED;
	kn->r.local_label = NO_LABEL;
	kn->r.remote_label = NO_LABEL;

	if (update &&
	    send_rtmsg(kr_state.fd, RTM_CHANGE, &kn->r, AF_INET) == -1)
		return (-1);

	return (0);
}

void
kr_shutdown(void)
{
	kr_fib_decouple();

	kroute_clear();
	kif_clear();
}

void
kr_fib_couple(void)
{
	struct kroute_prefix	*kp;
	struct kroute_priority	*kprio;
	struct kroute_node	*kn;
	struct kif_node		*kif;

	if (kr_state.fib_sync == 1)	/* already coupled */
		return;

	kr_state.fib_sync = 1;

	RB_FOREACH(kp, kroute_tree, &krt) {
		kprio = TAILQ_FIRST(&kp->priorities);
		if (kprio == NULL)
			continue;

		TAILQ_FOREACH(kn, &kprio->nexthops, entry) {
			if (!(kn->r.flags & F_LDPD_INSERTED))
				continue;

			send_rtmsg(kr_state.fd, RTM_ADD, &kn->r, AF_MPLS);

			if (kn->r.nexthop.s_addr != INADDR_ANY &&
			    kn->r.remote_label != NO_LABEL) {
				send_rtmsg(kr_state.fd, RTM_CHANGE,
				    &kn->r, AF_INET);
			}
		}
	}

	RB_FOREACH(kif, kif_tree, &kit)
		if (kif->kpw)
			kmpw_install(kif->k.ifname, kif->kpw);

	log_info("kernel routing table coupled");
}

void
kr_fib_decouple(void)
{
	struct kroute_prefix	*kp;
	struct kroute_priority	*kprio;
	struct kroute_node	*kn;
	uint32_t		 rl;
	struct kif_node		*kif;

	if (kr_state.fib_sync == 0)	/* already decoupled */
		return;

	RB_FOREACH(kp, kroute_tree, &krt) {
		kprio = TAILQ_FIRST(&kp->priorities);
		if (kprio == NULL)
			continue;

		TAILQ_FOREACH(kn, &kprio->nexthops, entry) {
			if (!(kn->r.flags & F_LDPD_INSERTED))
				continue;

			send_rtmsg(kr_state.fd, RTM_DELETE,
			    &kn->r, AF_MPLS);

			if (kn->r.nexthop.s_addr != INADDR_ANY &&
			    kn->r.remote_label != NO_LABEL) {
				rl = kn->r.remote_label;
				kn->r.remote_label = NO_LABEL;
				send_rtmsg(kr_state.fd, RTM_CHANGE,
				    &kn->r, AF_INET);
				kn->r.remote_label = rl;
			}
		}
	}

	RB_FOREACH(kif, kif_tree, &kit)
		if (kif->kpw)
			kmpw_uninstall(kif->k.ifname, kif->kpw);

	kr_state.fib_sync = 0;
	log_info("kernel routing table decoupled");
}

/* ARGSUSED */
void
kr_dispatch_msg(int fd, short event, void *bula)
{
	dispatch_rtmsg();
}

void
kr_show_route(struct imsg *imsg)
{
	struct kroute_prefix	*kp;
	struct kroute_priority	*kprio;
	struct kroute_node	*kn;
	int			 flags;
	struct in_addr		 addr;

	switch (imsg->hdr.type) {
	case IMSG_CTL_KROUTE:
		if (imsg->hdr.len != IMSG_HEADER_SIZE + sizeof(flags)) {
			log_warnx("kr_show_route: wrong imsg len");
			return;
		}
		memcpy(&flags, imsg->data, sizeof(flags));

		RB_FOREACH(kp, kroute_tree, &krt)
			TAILQ_FOREACH(kprio, &kp->priorities, entry)
				TAILQ_FOREACH(kn, &kprio->nexthops, entry) {
					if (!flags || kn->r.flags & flags)
						main_imsg_compose_ldpe(
						    IMSG_CTL_KROUTE,
						    imsg->hdr.pid,
						    &kn->r, sizeof(kn->r));
				}
		break;
	case IMSG_CTL_KROUTE_ADDR:
		if (imsg->hdr.len != IMSG_HEADER_SIZE +
		    sizeof(struct in_addr)) {
			log_warnx("kr_show_route: wrong imsg len");
			return;
		}
		memcpy(&addr, imsg->data, sizeof(addr));

		kprio = kroute_match(addr.s_addr);
		TAILQ_FOREACH(kn, &kprio->nexthops, entry)
			main_imsg_compose_ldpe(IMSG_CTL_KROUTE, imsg->hdr.pid,
			    &kn->r, sizeof(kn->r));
		break;
	default:
		log_debug("kr_show_route: error handling imsg");
		break;
	}
	main_imsg_compose_ldpe(IMSG_CTL_END, imsg->hdr.pid, NULL, 0);
}

void
kr_ifinfo(char *ifname, pid_t pid)
{
	struct kif_node	*kif;

	RB_FOREACH(kif, kif_tree, &kit)
		if (ifname == NULL || !strcmp(ifname, kif->k.ifname)) {
			main_imsg_compose_ldpe(IMSG_CTL_IFINFO,
			    pid, &kif->k, sizeof(kif->k));
		}

	main_imsg_compose_ldpe(IMSG_CTL_END, pid, NULL, 0);
}

void
kr_redist_remove(struct kroute *kr)
{
	/* was the route redistributed? */
	if ((kr->flags & F_REDISTRIBUTED) == 0)
		return;

	/* remove redistributed flag */
	kr->flags &= ~F_REDISTRIBUTED;
	main_imsg_compose_lde(IMSG_NETWORK_DEL, 0, kr,
	    sizeof(struct kroute));
}

int
kr_redist_eval(struct kroute *kr)
{
	uint32_t	 a;

	/* was the route redistributed? */
	if (kr->flags & F_REDISTRIBUTED)
		goto dont_redistribute;

	/* Dynamic routes are not redistributable. */
	if (kr->flags & F_DYNAMIC)
		goto dont_redistribute;

	/*
	 * We consider the loopback net, default route, multicast and
	 * experimental addresses as not redistributable.
	 */
	a = ntohl(kr->prefix.s_addr);
	if (IN_MULTICAST(a) || IN_BADCLASS(a) ||
	    (kr->prefixlen == 0) ||
	    (a >> IN_CLASSA_NSHIFT) == IN_LOOPBACKNET)
		goto dont_redistribute;
	/*
	 * Consider networks with nexthop loopback as not redistributable
	 * unless it is a reject or blackhole route.
	 */
	if (kr->nexthop.s_addr == htonl(INADDR_LOOPBACK) &&
	    !(kr->flags & (F_BLACKHOLE|F_REJECT)))
		goto dont_redistribute;

	/* prefix should be redistributed */
	kr->flags |= F_REDISTRIBUTED;
	main_imsg_compose_lde(IMSG_NETWORK_ADD, 0, kr, sizeof(struct kroute));
	return (1);

dont_redistribute:
	return (1);
}

void
kr_redistribute(struct kroute_prefix *kp)
{
	struct kroute_priority	*kprio;
	struct kroute_node	*kn;

	TAILQ_FOREACH_REVERSE(kprio, &kp->priorities, plist, entry) {
		if (kprio == TAILQ_FIRST(&kp->priorities)) {
			TAILQ_FOREACH(kn, &kprio->nexthops, entry)
				kr_redist_eval(&kn->r);
		} else {
			TAILQ_FOREACH(kn, &kprio->nexthops, entry)
				kr_redist_remove(&kn->r);
		}
	}
}

/* rb-tree compare */
int
kroute_compare(struct kroute_prefix *a, struct kroute_prefix *b)
{
	if (ntohl(a->prefix.s_addr) < ntohl(b->prefix.s_addr))
		return (-1);
	if (ntohl(a->prefix.s_addr) > ntohl(b->prefix.s_addr))
		return (1);
	if (a->prefixlen < b->prefixlen)
		return (-1);
	if (a->prefixlen > b->prefixlen)
		return (1);

	return (0);
}

int
kif_compare(struct kif_node *a, struct kif_node *b)
{
	return (b->k.ifindex - a->k.ifindex);
}

/* tree management */
struct kroute_prefix *
kroute_find(in_addr_t prefix, uint8_t prefixlen)
{
	struct kroute_prefix	 s;

	s.prefix.s_addr = prefix;
	s.prefixlen = prefixlen;

	return (RB_FIND(kroute_tree, &krt, &s));
}

struct kroute_priority *
kroute_find_prio(in_addr_t prefix, uint8_t prefixlen, uint8_t prio)
{
	struct kroute_prefix	*kp;
	struct kroute_priority	*kprio;

	if ((kp = kroute_find(prefix, prefixlen)) == NULL)
		return (NULL);

	/* RTP_ANY here picks the lowest priority node */
	if (prio == RTP_ANY)
		return (TAILQ_FIRST(&kp->priorities));

	TAILQ_FOREACH(kprio, &kp->priorities, entry)
		if (kprio->priority == prio)
			return (kprio);

	return (NULL);
}

struct kroute_node *
kroute_find_gw(in_addr_t prefix, uint8_t prefixlen, uint8_t prio,
    struct in_addr nh)
{
	struct kroute_priority	*kprio;
	struct kroute_node	*kn;

	if ((kprio = kroute_find_prio(prefix, prefixlen, prio)) == NULL)
		return (NULL);

	TAILQ_FOREACH(kn, &kprio->nexthops, entry)
		if (kn->r.nexthop.s_addr == nh.s_addr)
			return (kn);

	return (NULL);
}

int
kroute_insert(struct kroute *kr)
{
	struct kroute_prefix	*kp;
	struct kroute_priority	*kprio, *tmp = NULL;
	struct kroute_node	*kn;

	kp = kroute_find(kr->prefix.s_addr, kr->prefixlen);
	if (kp == NULL) {
		kp = calloc(1, sizeof(struct kroute_prefix));
		if (kp == NULL)
			fatal("kroute_insert");
		kp->prefix.s_addr = kr->prefix.s_addr;
		kp->prefixlen = kr->prefixlen;
		TAILQ_INIT(&kp->priorities);
		RB_INSERT(kroute_tree, &krt, kp);
	}

	kprio = kroute_find_prio(kr->prefix.s_addr, kr->prefixlen,
	    kr->priority);
	if (kprio == NULL) {
		kprio = calloc(1, sizeof(struct kroute_priority));
		if (kprio == NULL)
			fatal("kroute_insert");
		kprio->kp = kp;
		kprio->priority = kr->priority;
		TAILQ_INIT(&kprio->nexthops);

		/* lower priorities first */
		TAILQ_FOREACH(tmp, &kp->priorities, entry) {
			if (tmp->priority > kr->priority) {
				TAILQ_INSERT_BEFORE(tmp, kprio, entry);
				goto done;
			}
		}
		TAILQ_INSERT_TAIL(&kp->priorities, kprio, entry);
	}

done:
	kn = kroute_find_gw(kr->prefix.s_addr, kr->prefixlen, kr->priority,
	    kr->nexthop);
	if (kn == NULL) {
		kn = calloc(1, sizeof(struct kroute_node));
		if (kn == NULL)
			fatal("kroute_insert");
		kn->kprio = kprio;
		memcpy(&kn->r, kr, sizeof(struct kroute));
		TAILQ_INSERT_TAIL(&kprio->nexthops, kn, entry);
	}

	kr_redistribute(kp);
	return (0);
}

int
kroute_uninstall(struct kroute_node *kn)
{
	/* kill MPLS LSP if one was installed */
	if (kn->r.flags & F_LDPD_INSERTED)
		if (send_rtmsg(kr_state.fd, RTM_DELETE, &kn->r, AF_MPLS) ==
		    -1)
			return (-1);

	return (0);
}

int
kroute_remove(struct kroute *kr)
{
	struct kroute_prefix	*kp;
	struct kroute_priority	*kprio;
	struct kroute_node	*kn;

	kn = kroute_find_gw(kr->prefix.s_addr, kr->prefixlen, kr->priority,
	    kr->nexthop);
	if (kn == NULL) {
		log_warnx("kroute_remove failed to find %s/%u",
		    inet_ntoa(kr->prefix), kr->prefixlen);
		return (-1);
	}
	kprio = kn->kprio;
	kp = kprio->kp;

	kr_redist_remove(&kn->r);
	kroute_uninstall(kn);

	TAILQ_REMOVE(&kprio->nexthops, kn, entry);
	free(kn);

	if (TAILQ_EMPTY(&kprio->nexthops)) {
		TAILQ_REMOVE(&kp->priorities, kprio, entry);
		free(kprio);
	}

	if (TAILQ_EMPTY(&kp->priorities)) {
		if (RB_REMOVE(kroute_tree, &krt, kp) == NULL) {
			log_warnx("kroute_remove failed for %s/%u",
			    inet_ntoa(kp->prefix), kp->prefixlen);
			return (-1);
		}
		free(kp);
	} else
		kr_redistribute(kp);

	return (0);
}

void
kroute_clear(void)
{
	struct kroute_prefix	*kp;
	struct kroute_priority	*kprio;
	struct kroute_node	*kn;

	while ((kp = RB_MIN(kroute_tree, &krt)) != NULL) {
		while ((kprio = TAILQ_FIRST(&kp->priorities)) != NULL) {
			while ((kn = TAILQ_FIRST(&kprio->nexthops)) != NULL) {
				kr_redist_remove(&kn->r);
				kroute_uninstall(kn);
				TAILQ_REMOVE(&kprio->nexthops, kn, entry);
				free(kn);
			}
			TAILQ_REMOVE(&kp->priorities, kprio, entry);
			free(kprio);
		}
		RB_REMOVE(kroute_tree, &krt, kp);
		free(kp);
	}
}

struct kif_node *
kif_find(unsigned short ifindex)
{
	struct kif_node	s;

	memset(&s, 0, sizeof(s));
	s.k.ifindex = ifindex;

	return (RB_FIND(kif_tree, &kit, &s));
}

struct kif *
kif_findname(char *ifname)
{
	struct kif_node	*kif;

	RB_FOREACH(kif, kif_tree, &kit)
		if (!strcmp(ifname, kif->k.ifname))
			return (&kif->k);

	return (NULL);
}

struct kif_node *
kif_insert(unsigned short ifindex)
{
	struct kif_node	*kif;

	if ((kif = calloc(1, sizeof(struct kif_node))) == NULL)
		return (NULL);

	kif->k.ifindex = ifindex;
	TAILQ_INIT(&kif->addrs);

	if (RB_INSERT(kif_tree, &kit, kif) != NULL)
		fatalx("kif_insert: RB_INSERT");

	return (kif);
}

int
kif_remove(struct kif_node *kif)
{
	struct kif_addr	*ka;

	if (RB_REMOVE(kif_tree, &kit, kif) == NULL) {
		log_warnx("RB_REMOVE(kif_tree, &kit, kif)");
		return (-1);
	}

	while ((ka = TAILQ_FIRST(&kif->addrs)) != NULL) {
		TAILQ_REMOVE(&kif->addrs, ka, entry);
		free(ka);
	}
	free(kif);
	return (0);
}

void
kif_clear(void)
{
	struct kif_node	*kif;

	while ((kif = RB_MIN(kif_tree, &kit)) != NULL)
		kif_remove(kif);
}

struct kif_node *
kif_update(unsigned short ifindex, int flags, struct if_data *ifd,
    struct sockaddr_dl *sdl, int *link_old)
{
	struct kif_node		*kif;

	if ((kif = kif_find(ifindex)) == NULL) {
		if ((kif = kif_insert(ifindex)) == NULL)
			return (NULL);
	} else
		*link_old = (kif->k.flags & IFF_UP) &&
		    LINK_STATE_IS_UP(kif->k.link_state);

	kif->k.flags = flags;
	kif->k.link_state = ifd->ifi_link_state;
	kif->k.if_type = ifd->ifi_type;
	kif->k.baudrate = ifd->ifi_baudrate;
	kif->k.mtu = ifd->ifi_mtu;

	if (sdl && sdl->sdl_family == AF_LINK) {
		if (sdl->sdl_nlen >= sizeof(kif->k.ifname))
			memcpy(kif->k.ifname, sdl->sdl_data,
			    sizeof(kif->k.ifname) - 1);
		else if (sdl->sdl_nlen > 0)
			memcpy(kif->k.ifname, sdl->sdl_data,
			    sdl->sdl_nlen);
		/* string already terminated via calloc() */
	}

	return (kif);
}

struct kroute_priority *
kroute_match(in_addr_t key)
{
	int			 i;
	struct kroute_priority	*kprio;

	/* we will never match the default route */
	for (i = 32; i > 0; i--)
		if ((kprio = kroute_find_prio(key & prefixlen2mask(i), i,
		    RTP_ANY)) != NULL)
			return (kprio);

	/* if we don't have a match yet, try to find a default route */
	if ((kprio = kroute_find_prio(0, 0, RTP_ANY)) != NULL)
		return (kprio);

	return (NULL);
}

/* misc */
uint8_t
prefixlen_classful(in_addr_t ina)
{
	/* it hurt to write this. */

	if (ina >= 0xf0000000U)		/* class E */
		return (32);
	else if (ina >= 0xe0000000U)	/* class D */
		return (4);
	else if (ina >= 0xc0000000U)	/* class C */
		return (24);
	else if (ina >= 0x80000000U)	/* class B */
		return (16);
	else				/* class A */
		return (8);
}

uint8_t
mask2prefixlen(in_addr_t ina)
{
	if (ina == 0)
		return (0);
	else
		return (33 - ffs(ntohl(ina)));
}

in_addr_t
prefixlen2mask(uint8_t prefixlen)
{
	if (prefixlen == 0)
		return (0);

	return (htonl(0xffffffff << (32 - prefixlen)));
}

#define	ROUNDUP(a)	\
    (((a) & (sizeof(long) - 1)) ? (1 + ((a) | (sizeof(long) - 1))) : (a))

void
get_rtaddrs(int addrs, struct sockaddr *sa, struct sockaddr **rti_info)
{
	int	i;

	for (i = 0; i < RTAX_MAX; i++) {
		if (addrs & (1 << i)) {
			rti_info[i] = sa;
			sa = (struct sockaddr *)((char *)(sa) +
			    ROUNDUP(sa->sa_len));
		} else
			rti_info[i] = NULL;
	}
}

void
if_change(unsigned short ifindex, int flags, struct if_data *ifd,
    struct sockaddr_dl *sdl)
{
	struct kif_node		*kif;
	struct kif_addr		*ka;
	int			 link_old = 0, link_new;

	kif = kif_update(ifindex, flags, ifd, sdl, &link_old);
	if (!kif) {
		log_warn("if_change: kif_update(%u)", ifindex);
		return;
	}
	link_new = (kif->k.flags & IFF_UP) &&
	    LINK_STATE_IS_UP(kif->k.link_state);

	if (link_new == link_old)
		return;

	if (link_new) {
		main_imsg_compose_ldpe(IMSG_IFSTATUS, 0, &kif->k,
		    sizeof(struct kif));
		TAILQ_FOREACH(ka, &kif->addrs, entry)
			main_imsg_compose_ldpe(IMSG_NEWADDR, 0, &ka->addr,
			    sizeof(struct kaddr));
	} else {
		main_imsg_compose_ldpe(IMSG_IFSTATUS, 0, &kif->k,
		    sizeof(struct kif));
		TAILQ_FOREACH(ka, &kif->addrs, entry)
			main_imsg_compose_ldpe(IMSG_DELADDR, 0, &ka->addr,
			    sizeof(struct kaddr));
	}
}

void
if_newaddr(unsigned short ifindex, struct sockaddr_in *ifa,
    struct sockaddr_in *mask, struct sockaddr_in *brd)
{
	struct kif_node *kif;
	struct kif_addr *ka;
	uint32_t	 a;

	if (ifa == NULL || ifa->sin_family != AF_INET)
		return;
	if ((kif = kif_find(ifindex)) == NULL) {
		log_warnx("if_newaddr: corresponding if %d not found", ifindex);
		return;
	}
	a = ntohl(ifa->sin_addr.s_addr);
	if (IN_MULTICAST(a) || IN_BADCLASS(a) ||
	    (a >> IN_CLASSA_NSHIFT) == IN_LOOPBACKNET)
		return;

	if ((ka = calloc(1, sizeof(struct kif_addr))) == NULL)
		fatal("if_newaddr");
	ka->addr.ifindex = ifindex;
	ka->addr.addr.s_addr = ifa->sin_addr.s_addr;
	if (mask)
		ka->addr.mask.s_addr = mask->sin_addr.s_addr;
	else
		ka->addr.mask.s_addr = INADDR_NONE;
	if (brd)
		ka->addr.dstbrd.s_addr = brd->sin_addr.s_addr;
	else
		ka->addr.dstbrd.s_addr = INADDR_NONE;

	TAILQ_INSERT_TAIL(&kif->addrs, ka, entry);

	/* notify ldpe about new address */
	main_imsg_compose_ldpe(IMSG_NEWADDR, 0, &ka->addr,
	    sizeof(struct kaddr));
}

void
if_deladdr(unsigned short ifindex, struct sockaddr_in *ifa,
    struct sockaddr_in *mask, struct sockaddr_in *brd)
{
	struct kif_node *kif;
	struct kif_addr *ka, *nka;

	if (ifa == NULL || ifa->sin_family != AF_INET)
		return;
	if ((kif = kif_find(ifindex)) == NULL) {
		log_warnx("if_deladdr: corresponding if %d not found", ifindex);
		return;
	}

	for (ka = TAILQ_FIRST(&kif->addrs); ka != NULL; ka = nka) {
		nka = TAILQ_NEXT(ka, entry);

		if (ka->addr.addr.s_addr == ifa->sin_addr.s_addr) {
			TAILQ_REMOVE(&kif->addrs, ka, entry);

			/* notify ldpe about removed address */
			main_imsg_compose_ldpe(IMSG_DELADDR, 0, &ka->addr,
			    sizeof(struct kaddr));

			free(ka);
			return;
		}
	}
}

void
if_announce(void *msg)
{
	struct if_announcemsghdr	*ifan;
	struct kif_node			*kif;

	ifan = msg;

	switch (ifan->ifan_what) {
	case IFAN_ARRIVAL:
		kif = kif_insert(ifan->ifan_index);
		strlcpy(kif->k.ifname, ifan->ifan_name, sizeof(kif->k.ifname));
		break;
	case IFAN_DEPARTURE:
		kif = kif_find(ifan->ifan_index);
		kif_remove(kif);
		break;
	}
}

/* rtsock */
int
send_rtmsg(int fd, int action, struct kroute *kroute, uint32_t family)
{
	struct iovec		iov[5];
	struct rt_msghdr	hdr;
	struct sockaddr_mpls	label_in, label_out;
	struct sockaddr_in	dst, mask, nexthop;
	int			iovcnt = 0;

	if (kr_state.fib_sync == 0)
		return (0);

	/* Implicit NULL label should not be added to the FIB */
	if (family == AF_MPLS && kroute->local_label == MPLS_LABEL_IMPLNULL)
		return (0);

	/* initialize header */
	memset(&hdr, 0, sizeof(hdr));
	hdr.rtm_version = RTM_VERSION;

	hdr.rtm_type = action;
	hdr.rtm_flags = RTF_UP;
	hdr.rtm_fmask = RTF_MPLS;
	hdr.rtm_seq = kr_state.rtseq++;	/* overflow doesn't matter */
	hdr.rtm_msglen = sizeof(hdr);
	hdr.rtm_hdrlen = sizeof(struct rt_msghdr);
	hdr.rtm_priority = kroute->priority;
	/* adjust iovec */
	iov[iovcnt].iov_base = &hdr;
	iov[iovcnt++].iov_len = sizeof(hdr);

	if (family == AF_MPLS) {
		memset(&label_in, 0, sizeof(label_in));
		label_in.smpls_len = sizeof(label_in);
		label_in.smpls_family = AF_MPLS;
		label_in.smpls_label =
		    htonl(kroute->local_label << MPLS_LABEL_OFFSET);
		/* adjust header */
		hdr.rtm_flags |= RTF_MPLS | RTF_MPATH;
		hdr.rtm_addrs |= RTA_DST;
		hdr.rtm_msglen += sizeof(label_in);
		/* adjust iovec */
		iov[iovcnt].iov_base = &label_in;
		iov[iovcnt++].iov_len = sizeof(label_in);
	} else {
		memset(&dst, 0, sizeof(dst));
		dst.sin_len = sizeof(dst);
		dst.sin_family = AF_INET;
		dst.sin_addr.s_addr = kroute->prefix.s_addr;
		/* adjust header */
		hdr.rtm_addrs |= RTA_DST;
		hdr.rtm_msglen += sizeof(dst);
		/* adjust iovec */
		iov[iovcnt].iov_base = &dst;
		iov[iovcnt++].iov_len = sizeof(dst);
	}

	memset(&nexthop, 0, sizeof(nexthop));
	nexthop.sin_len = sizeof(nexthop);
	nexthop.sin_family = AF_INET;
	nexthop.sin_addr.s_addr = kroute->nexthop.s_addr;
	/* adjust header */
	hdr.rtm_flags |= RTF_GATEWAY;
	hdr.rtm_addrs |= RTA_GATEWAY;
	hdr.rtm_msglen += sizeof(nexthop);
	/* adjust iovec */
	iov[iovcnt].iov_base = &nexthop;
	iov[iovcnt++].iov_len = sizeof(nexthop);

	if (family == AF_INET) {
		memset(&mask, 0, sizeof(mask));
		mask.sin_len = sizeof(mask);
		mask.sin_family = AF_INET;
		mask.sin_addr.s_addr = prefixlen2mask(kroute->prefixlen);
		/* adjust header */
		hdr.rtm_addrs |= RTA_NETMASK;
		hdr.rtm_msglen += sizeof(mask);
		/* adjust iovec */
		iov[iovcnt].iov_base = &mask;
		iov[iovcnt++].iov_len = sizeof(mask);
	}

	/* If action is RTM_DELETE we have to get rid of MPLS infos */
	if (kroute->remote_label != NO_LABEL && action != RTM_DELETE) {
		memset(&label_out, 0, sizeof(label_out));
		label_out.smpls_len = sizeof(label_out);
		label_out.smpls_family = AF_MPLS;
		label_out.smpls_label =
		    htonl(kroute->remote_label << MPLS_LABEL_OFFSET);
		/* adjust header */
		hdr.rtm_addrs |= RTA_SRC;
		hdr.rtm_flags |= RTF_MPLS;
		hdr.rtm_msglen += sizeof(label_out);
		/* adjust iovec */
		iov[iovcnt].iov_base = &label_out;
		iov[iovcnt++].iov_len = sizeof(label_out);

		if (kroute->remote_label == MPLS_LABEL_IMPLNULL) {
			if (family == AF_MPLS)
				hdr.rtm_mpls = MPLS_OP_POP;
			else
				return (0);
		} else {
			if (family == AF_MPLS)
				hdr.rtm_mpls = MPLS_OP_SWAP;
			else
				hdr.rtm_mpls = MPLS_OP_PUSH;
		}
	}

retry:
	if (writev(fd, iov, iovcnt) == -1) {
		if (errno == ESRCH) {
			if (hdr.rtm_type == RTM_CHANGE && family == AF_MPLS) {
				hdr.rtm_type = RTM_ADD;
				goto retry;
			} else if (hdr.rtm_type == RTM_DELETE) {
				log_info("route %s/%u vanished before delete",
				    inet_ntoa(kroute->prefix),
				    kroute->prefixlen);
				return (0);
			}
		}
		log_warn("send_rtmsg: action %u, AF %d, prefix %s/%u",
		    hdr.rtm_type, family, inet_ntoa(kroute->prefix),
		    kroute->prefixlen);
		return (0);
	}

	return (0);
}

int
fetchtable(void)
{
	size_t			 len;
	int			 mib[7];
	char			*buf;
	int			 rv;

	mib[0] = CTL_NET;
	mib[1] = PF_ROUTE;
	mib[2] = 0;
	mib[3] = AF_INET;
	mib[4] = NET_RT_DUMP;
	mib[5] = 0;
	mib[6] = 0;	/* rtableid */

	if (sysctl(mib, 7, NULL, &len, NULL, 0) == -1) {
		log_warn("sysctl");
		return (-1);
	}
	if ((buf = malloc(len)) == NULL) {
		log_warn("fetchtable");
		return (-1);
	}
	if (sysctl(mib, 7, buf, &len, NULL, 0) == -1) {
		log_warn("sysctl");
		free(buf);
		return (-1);
	}

	rv = rtmsg_process(buf, len);
	free(buf);

	return (rv);
}

int
fetchifs(unsigned short ifindex)
{
	size_t			 len;
	int			 mib[6];
	char			*buf;
	int			 rv;

	mib[0] = CTL_NET;
	mib[1] = PF_ROUTE;
	mib[2] = 0;
	mib[3] = AF_INET;
	mib[4] = NET_RT_IFLIST;
	mib[5] = ifindex;

	if (sysctl(mib, 6, NULL, &len, NULL, 0) == -1) {
		log_warn("sysctl");
		return (-1);
	}
	if ((buf = malloc(len)) == NULL) {
		log_warn("fetchif");
		return (-1);
	}
	if (sysctl(mib, 6, buf, &len, NULL, 0) == -1) {
		log_warn("sysctl");
		free(buf);
		return (-1);
	}

	rv = rtmsg_process(buf, len);
	free(buf);

	return (rv);
}

int
dispatch_rtmsg(void)
{
	char			 buf[RT_BUF_SIZE];
	ssize_t			 n;

	if ((n = read(kr_state.fd, &buf, sizeof(buf))) == -1) {
		if (errno == EAGAIN || errno == EINTR)
			return (0);
		log_warn("dispatch_rtmsg: read error");
		return (-1);
	}

	if (n == 0) {
		log_warnx("routing socket closed");
		return (-1);
	}

	return (rtmsg_process(buf, n));
}

int
rtmsg_process(char *buf, size_t len)
{
	struct rt_msghdr	*rtm;
	struct if_msghdr	 ifm;
	struct ifa_msghdr	*ifam;
	struct sockaddr		*sa, *rti_info[RTAX_MAX];
	struct sockaddr_in	*sa_in;
	struct kroute_priority	*kprio;
	struct kroute_node	*kn;
	struct kroute		 kr;
	struct in_addr		 prefix, nexthop;
	uint8_t			 prefixlen, prio;
	int			 flags;
	unsigned short		 ifindex = 0;
	size_t			 offset;
	char			*next;

	for (offset = 0; offset < len; offset += rtm->rtm_msglen) {
		next = buf + offset;
		rtm = (struct rt_msghdr *)next;
		if (len < offset + sizeof(unsigned short) ||
		    len < offset + rtm->rtm_msglen)
			fatalx("rtmsg_process: partial rtm in buffer");
		if (rtm->rtm_version != RTM_VERSION)
			continue;
		log_rtmsg(rtm->rtm_type);

		prefix.s_addr = 0;
		prefixlen = 0;
		nexthop.s_addr = 0;
		prio = 0;
		flags = 0;

		sa = (struct sockaddr *)(next + rtm->rtm_hdrlen);
		get_rtaddrs(rtm->rtm_addrs, sa, rti_info);

		switch (rtm->rtm_type) {
		case RTM_ADD:
		case RTM_GET:
		case RTM_CHANGE:
		case RTM_DELETE:
			if (rtm->rtm_errno)		/* failed attempts... */
				continue;

			if (rtm->rtm_tableid != 0)
				continue;

			if ((sa = rti_info[RTAX_DST]) == NULL)
				continue;

			/* Skip ARP/ND cache and broadcast routes. */
			if (rtm->rtm_flags & (RTF_LLINFO|RTF_BROADCAST))
				continue;

			/* LDP should follow the IGP and ignore BGP routes */
			if (rtm->rtm_priority == RTP_BGP)
				continue;
			prio = rtm->rtm_priority;

			switch (sa->sa_family) {
			case AF_INET:
				prefix.s_addr =
				    ((struct sockaddr_in *)sa)->sin_addr.s_addr;
				sa_in = (struct sockaddr_in *)
				    rti_info[RTAX_NETMASK];
				if (sa_in != NULL) {
					if (sa_in->sin_len != 0)
						prefixlen = mask2prefixlen(
						    sa_in->sin_addr.s_addr);
				} else if (rtm->rtm_flags & RTF_HOST)
					prefixlen = 32;
				else
					prefixlen =
					    prefixlen_classful(prefix.s_addr);
				if (rtm->rtm_flags & RTF_STATIC)
					flags |= F_STATIC;
				if (rtm->rtm_flags & RTF_BLACKHOLE)
					flags |= F_BLACKHOLE;
				if (rtm->rtm_flags & RTF_REJECT)
					flags |= F_REJECT;
				if (rtm->rtm_flags & RTF_DYNAMIC)
					flags |= F_DYNAMIC;
				break;
			default:
				continue;
			}

			ifindex = rtm->rtm_index;
			if ((sa = rti_info[RTAX_GATEWAY]) != NULL) {
				switch (sa->sa_family) {
				case AF_INET:
					if (rtm->rtm_flags & RTF_CONNECTED) {
						flags |= F_CONNECTED;
						break;
					}
					nexthop.s_addr = ((struct
					    sockaddr_in *)sa)->sin_addr.s_addr;
					break;
				case AF_LINK:
					/*
					 * Traditional BSD connected routes have
					 * a gateway of type AF_LINK.
					 */
					flags |= F_CONNECTED;
					break;
				}
			}
		}

		switch (rtm->rtm_type) {
		case RTM_CHANGE:
			/*
			 * The kernel doesn't allow RTM_CHANGE for multipath
			 * routes. If we got this message we know that the
			 * route has only one nexthop and we should remove
			 * it before installing the same route with a new
			 * nexthop.
			 */
			if ((kprio = kroute_find_prio(prefix.s_addr,
			    prefixlen, prio)) == NULL) {
				log_warnx("dispatch_rtmsg route not found");
				return (-1);
			}
			kn = TAILQ_FIRST(&kprio->nexthops);
			if (kn && kroute_remove(&kn->r) == -1)
				return (-1);
			break;
		default:
			break;
		}

		switch (rtm->rtm_type) {
		case RTM_ADD:
		case RTM_GET:
		case RTM_CHANGE:
			if (nexthop.s_addr == 0 && !(flags & F_CONNECTED)) {
				log_warnx("no nexthop for %s/%u",
				    inet_ntoa(prefix), prefixlen);
				continue;
			}

			/* routes attached to loopback interfaces */
			if (prefix.s_addr == nexthop.s_addr)
				flags |= F_CONNECTED;

			if (kroute_find_gw(prefix.s_addr, prefixlen, prio,
			    nexthop) != NULL)
				break;

			memset(&kr, 0, sizeof(kr));
			kr.prefix.s_addr = prefix.s_addr;
			kr.prefixlen = prefixlen;
			kr.nexthop.s_addr = nexthop.s_addr;
			kr.flags = flags;
			kr.ifindex = ifindex;
			kr.priority = prio;
			kr.local_label = NO_LABEL;
			kr.remote_label = NO_LABEL;
			kroute_insert(&kr);
			break;
		case RTM_DELETE:
			/* get the correct route */
			if ((kn = kroute_find_gw(prefix.s_addr, prefixlen,
			    prio, nexthop)) == NULL) {
				log_warnx("dispatch_rtmsg route not found");
				return (-1);
			}
			if (kroute_remove(&kn->r) == -1)
				return (-1);
			break;
		case RTM_IFINFO:
			memcpy(&ifm, next, sizeof(ifm));
			if_change(ifm.ifm_index, ifm.ifm_flags, &ifm.ifm_data,
			    (struct sockaddr_dl *)rti_info[RTAX_IFP]);
			break;
		case RTM_NEWADDR:
			ifam = (struct ifa_msghdr *)rtm;
			if ((ifam->ifam_addrs & (RTA_NETMASK | RTA_IFA |
			    RTA_BRD)) == 0)
				break;

			if_newaddr(ifam->ifam_index,
			    (struct sockaddr_in *)rti_info[RTAX_IFA],
			    (struct sockaddr_in *)rti_info[RTAX_NETMASK],
			    (struct sockaddr_in *)rti_info[RTAX_BRD]);
			break;
		case RTM_DELADDR:
			ifam = (struct ifa_msghdr *)rtm;
			if ((ifam->ifam_addrs & (RTA_NETMASK | RTA_IFA |
			    RTA_BRD)) == 0)
				break;

			if_deladdr(ifam->ifam_index,
			    (struct sockaddr_in *)rti_info[RTAX_IFA],
			    (struct sockaddr_in *)rti_info[RTAX_NETMASK],
			    (struct sockaddr_in *)rti_info[RTAX_BRD]);
			break;
		case RTM_IFANNOUNCE:
			if_announce(next);
			break;
		default:
			/* ignore for now */
			break;
		}
	}

	return (offset);
}

void
kmpw_set(struct kpw *kpw)
{
	struct kif_node		*kif;

	kif = kif_find(kpw->ifindex);
	if (kif == NULL) {
		log_warn("kmpw_set: failed to find mpw by index (%u)",
		    kpw->ifindex);
		return;
	}

	if (kif->kpw == NULL)
		kif->kpw = malloc(sizeof(*kif->kpw));
	memcpy(kif->kpw, kpw, sizeof(*kif->kpw));

	kmpw_install(kif->k.ifname, kpw);
}

void
kmpw_unset(struct kpw *kpw)
{
	struct kif_node		*kif;

	kif = kif_find(kpw->ifindex);
	if (kif == NULL) {
		log_warn("kmpw_unset: failed to find mpw by index (%u)",
		    kpw->ifindex);
		return;
	}

	if (kif->kpw == NULL) {
		log_warn("kmpw_unset: %s is not set", kif->k.ifname);
		return;
	}

	free(kif->kpw);
	kif->kpw = NULL;
	kmpw_uninstall(kif->k.ifname, kpw);
}

void
kmpw_install(const char *ifname, struct kpw *kpw)
{
	struct sockaddr_in	*sin;
	struct ifreq		 ifr;
	struct ifmpwreq		 imr;

	memset(&imr, 0, sizeof(imr));
	switch (kpw->pw_type) {
	case PW_TYPE_ETHERNET:
		imr.imr_type = IMR_TYPE_ETHERNET;
		break;
	case PW_TYPE_ETHERNET_TAGGED:
		imr.imr_type = IMR_TYPE_ETHERNET_TAGGED;
		break;

	default:
		log_warn("kmpw_install: unhandled pseudowire type (%#X)",
		    kpw->pw_type);
	}

	if (kpw->flags & F_PW_CONTROLWORD)
		imr.imr_flags |= IMR_FLAG_CONTROLWORD;

	sin = (struct sockaddr_in *) &imr.imr_nexthop;
	sin->sin_family = AF_INET;
	sin->sin_addr.s_addr = kpw->nexthop.s_addr;
	sin->sin_len = sizeof(struct sockaddr_in);

	imr.imr_lshim.shim_label = kpw->local_label;
	imr.imr_rshim.shim_label = kpw->remote_label;

	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	ifr.ifr_data = (caddr_t) &imr;
	if (ioctl(kr_state.ioctl_fd, SIOCSETMPWCFG, &ifr))
		log_warn("ioctl SETMPWCFG");
}

void
kmpw_uninstall(const char *ifname, struct kpw *kpw)
{
	struct ifreq		 ifr;
	struct ifmpwreq		 imr;

	memset(&imr, 0, sizeof(imr));
	memset(&ifr, 0, sizeof(ifr));
	strlcpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name));
	ifr.ifr_data = (caddr_t) &imr;
	if (ioctl(kr_state.ioctl_fd, SIOCSETMPWCFG, &ifr))
		log_warn("ioctl SETMPWCFG");
}
