/*	$OpenBSD$ */

/*
 * Copyright (c) 2005 Claudio Jeker <claudio@openbsd.org>
 * Copyright (c) 2004, 2005, 2008 Esben Norby <norby@openbsd.org>
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
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_types.h>
#include <fcntl.h>
#include <ctype.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <event.h>

#include "ldpd.h"
#include "ldp.h"
#include "log.h"
#include "ldpe.h"

extern struct ldpd_conf        *leconf;

int		 if_act_start(struct iface *);
int		 if_act_reset(struct iface *);
int		 if_act_update(struct iface *);
void		 if_hello_timer(int, short, void *);
void		 if_start_hello_timer(struct iface *);
void		 if_stop_hello_timer(struct iface *);
struct nbr	*if_elect(struct nbr *, struct nbr *);

struct {
	int			state;
	enum iface_event	event;
	enum iface_action	action;
	int			new_state;
} iface_fsm[] = {
    /* current state	event that happened	action to take	resulting state */
    {IF_STA_DOWN,	IF_EVT_DOWN,		IF_ACT_NOTHING,	0},
    {IF_STA_DOWN,	IF_EVT_UP,		IF_ACT_UPDATE,	0},
    {IF_STA_DOWN,	IF_EVT_NEWADDR,		IF_ACT_UPDATE,	0},
    {IF_STA_DOWN,	IF_EVT_DELADDR,		IF_ACT_NOTHING,	0},
    {IF_STA_ACTIVE,	IF_EVT_DOWN,		IF_ACT_RST,	IF_STA_DOWN},
    {IF_STA_ACTIVE,	IF_EVT_NEWADDR,		IF_ACT_NOTHING,	0},
    {IF_STA_ACTIVE,	IF_EVT_DELADDR,		IF_ACT_UPDATE,	0},
    {-1,		IF_EVT_NOTHING,		IF_ACT_NOTHING,	0},
};

const char * const if_event_names[] = {
	"NOTHING",
	"UP",
	"DOWN",
	"NEWADDR",
	"DELADDR"
};

const char * const if_action_names[] = {
	"NOTHING",
	"UPDATE",
	"RESET"
};

int
if_fsm(struct iface *iface, enum iface_event event)
{
	int	old_state;
	int	new_state = 0;
	int	i, ret = 0;

	old_state = iface->state;

	for (i = 0; iface_fsm[i].state != -1; i++)
		if ((iface_fsm[i].state & old_state) &&
		    (iface_fsm[i].event == event)) {
			new_state = iface_fsm[i].new_state;
			break;
		}

	if (iface_fsm[i].state == -1) {
		/* event outside of the defined fsm, ignore it. */
		log_debug("if_fsm: interface %s, "
		    "event %s not expected in state %s", iface->name,
		    if_event_names[event], if_state_name(old_state));
		return (0);
	}

	switch (iface_fsm[i].action) {
	case IF_ACT_UPDATE:
		ret = if_act_update(iface);
		break;
	case IF_ACT_RST:
		ret = if_act_reset(iface);
		break;
	case IF_ACT_NOTHING:
		/* do nothing */
		break;
	}

	if (ret) {
		log_debug("if_fsm: error changing state for interface %s, "
		    "event %s, state %s", iface->name, if_event_names[event],
		    if_state_name(old_state));
		return (-1);
	}

	if (new_state != 0)
		iface->state = new_state;

	log_debug("if_fsm: event %s resulted in action %s and changing "
	    "state for interface %s from %s to %s",
	    if_event_names[event], if_action_names[iface_fsm[i].action],
	    iface->name, if_state_name(old_state), if_state_name(iface->state));

	return (ret);
}

struct iface *
if_new(struct kif *kif)
{
	struct iface		*iface;

	if ((iface = calloc(1, sizeof(*iface))) == NULL)
		err(1, "if_new: calloc");

	iface->state = IF_STA_DOWN;

	LIST_INIT(&iface->lde_nbr_list);

	strlcpy(iface->name, kif->ifname, sizeof(iface->name));

	/* get type */
	if (kif->flags & IFF_POINTOPOINT)
		iface->type = IF_TYPE_POINTOPOINT;
	if (kif->flags & IFF_BROADCAST &&
	    kif->flags & IFF_MULTICAST)
		iface->type = IF_TYPE_BROADCAST;

	/* get mtu, index and flags */
	iface->mtu = kif->mtu;
	iface->ifindex = kif->ifindex;
	iface->flags = kif->flags;
	iface->linkstate = kif->link_state;
	iface->media_type = kif->media_type;
	iface->baudrate = kif->baudrate;

	return (iface);
}

void
if_del(struct iface *iface)
{
	struct if_addr		*if_addr;

	log_debug("if_del: interface %s", iface->name);

	if_stop_hello_timer(iface);

	while ((if_addr = LIST_FIRST(&iface->addr_list)) != NULL)
		LIST_REMOVE(if_addr, iface_entry);

	free(iface);
}

void
if_init(struct ldpd_conf *xconf, struct iface *iface)
{
	/* set event handlers for interface */
	evtimer_set(&iface->hello_timer, if_hello_timer, iface);

	iface->discovery_fd = xconf->ldp_discovery_socket;
}

struct iface *
if_lookup(u_short ifindex)
{
	struct iface *iface;

	LIST_FOREACH(iface, &leconf->iface_list, entry)
		if (iface->ifindex == ifindex)
			return (iface);

	return (NULL);
}

/* timers */
/* ARGSUSED */
void
if_hello_timer(int fd, short event, void *arg)
{
	struct iface *iface = arg;
	struct timeval tv;

	send_hello(iface);

	/* reschedule hello_timer */
	timerclear(&tv);
	tv.tv_sec = iface->hello_interval;
	if (evtimer_add(&iface->hello_timer, &tv) == -1)
		fatal("if_hello_timer");
}

void
if_start_hello_timer(struct iface *iface)
{
	struct timeval tv;

	timerclear(&tv);
	tv.tv_sec = iface->hello_interval;
	if (evtimer_add(&iface->hello_timer, &tv) == -1)
		fatal("if_start_hello_timer");
}

void
if_stop_hello_timer(struct iface *iface)
{
	if (evtimer_pending(&iface->hello_timer, NULL) &&
	    evtimer_del(&iface->hello_timer) == -1)
		fatal("if_stop_hello_timer");
}

/* actions */
int
if_act_start(struct iface *iface)
{
	struct in_addr		 addr;
	struct timeval		 now;

	gettimeofday(&now, NULL);
	iface->uptime = now.tv_sec;

	inet_aton(AllRouters, &addr);
	if (if_join_group(iface, &addr))
		return (-1);

	/* hello timer needs to be started in any case */
	if_start_hello_timer(iface);
	return (0);
}

int
if_act_reset(struct iface *iface)
{
	struct in_addr		 addr;

	if_stop_hello_timer(iface);

	/* try to cleanup */
	inet_aton(AllRouters, &addr);
	if_leave_group(iface, &addr);

	return (0);
}

int
if_act_update(struct iface *iface)
{
	int ret;

	if (iface->state == IF_STA_DOWN) {
		if (!((iface->flags & IFF_UP) &&
		    LINK_STATE_IS_UP(iface->linkstate)))
			return (0);

		if (LIST_EMPTY(&iface->addr_list))
			return (0);

		iface->state = IF_STA_ACTIVE;
		ret = if_act_start(iface);
	} else {
		if (!LIST_EMPTY(&iface->addr_list))
			return (0);

		iface->state = IF_STA_DOWN;
		ret = if_act_reset(iface);
	}

	return (ret);
}

struct ctl_iface *
if_to_ctl(struct iface *iface)
{
	static struct ctl_iface	 ictl;
	struct timeval		 tv, now, res;

	memcpy(ictl.name, iface->name, sizeof(ictl.name));
	ictl.rtr_id.s_addr = ldpe_router_id();
	ictl.ifindex = iface->ifindex;
	ictl.state = iface->state;
	ictl.mtu = iface->mtu;
	ictl.baudrate = iface->baudrate;
	ictl.holdtime = iface->holdtime;
	ictl.hello_interval = iface->hello_interval;
	ictl.flags = iface->flags;
	ictl.type = iface->type;
	ictl.linkstate = iface->linkstate;
	ictl.mediatype = iface->media_type;
	ictl.priority = iface->priority;

	gettimeofday(&now, NULL);
	if (evtimer_pending(&iface->hello_timer, &tv)) {
		timersub(&tv, &now, &res);
		ictl.hello_timer = res.tv_sec;
	} else
		ictl.hello_timer = -1;

	if (iface->state != IF_STA_DOWN &&
	    iface->uptime != 0) {
		ictl.uptime = now.tv_sec - iface->uptime;
	} else
		ictl.uptime = 0;

	return (&ictl);
}

/* misc */
int
if_set_mcast_ttl(int fd, u_int8_t ttl)
{
	if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL,
	    (char *)&ttl, sizeof(ttl)) < 0) {
		log_warn("if_set_mcast_ttl: error setting "
		    "IP_MULTICAST_TTL to %d", ttl);
		return (-1);
	}

	return (0);
}

int
if_set_tos(int fd, int tos)
{
	if (setsockopt(fd, IPPROTO_IP, IP_TOS, (int *)&tos, sizeof(tos)) < 0) {
		log_warn("if_set_tos: error setting IP_TOS to 0x%x", tos);
		return (-1);
	}

	return (0);
}

int
if_set_recvif(int fd, int enable)
{
	if (setsockopt(fd, IPPROTO_IP, IP_RECVIF, &enable,
	    sizeof(enable)) < 0) {
		log_warn("if_set_recvif: error setting IP_RECVIF");
		return (-1);
	}
	return (0);
}

void
if_set_recvbuf(int fd)
{
	int	bsize;

	bsize = 65535;
	while (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bsize,
	    sizeof(bsize)) == -1)
		bsize /= 2;
}

int
if_set_reuse(int fd, int enable)
{
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable,
	    sizeof(int)) < 0) {
		log_warn("if_set_reuse: error setting SO_REUSEADDR");
		return (-1);
	}

	return (0);
}

/*
 * only one JOIN or DROP per interface and address is allowed so we need
 * to keep track of what is added and removed.
 */
struct if_group_count {
	LIST_ENTRY(if_group_count)	entry;
	struct in_addr			addr;
	unsigned int			ifindex;
	int				count;
};

LIST_HEAD(,if_group_count) ifglist = LIST_HEAD_INITIALIZER(ifglist);

int
if_join_group(struct iface *iface, struct in_addr *addr)
{
	struct ip_mreq		 mreq;
	struct if_group_count	*ifg;
	struct if_addr		*if_addr;

	LIST_FOREACH(ifg, &ifglist, entry)
		if (iface->ifindex == ifg->ifindex &&
		    addr->s_addr == ifg->addr.s_addr)
			break;
	if (ifg == NULL) {
		if ((ifg = calloc(1, sizeof(*ifg))) == NULL)
			fatal("if_join_group");
		ifg->addr.s_addr = addr->s_addr;
		ifg->ifindex = iface->ifindex;
		LIST_INSERT_HEAD(&ifglist, ifg, entry);
	}

	if (ifg->count++ != 0)
		/* already joined */
		return (0);

	if_addr = LIST_FIRST(&iface->addr_list);
	mreq.imr_multiaddr.s_addr = addr->s_addr;
	mreq.imr_interface.s_addr = if_addr->addr.s_addr;

	if (setsockopt(iface->discovery_fd, IPPROTO_IP,
	    IP_ADD_MEMBERSHIP, (void *)&mreq, sizeof(mreq)) < 0) {
		log_warn("if_join_group: error IP_ADD_MEMBERSHIP, "
		    "interface %s address %s", iface->name,
		    inet_ntoa(*addr));
		LIST_REMOVE(ifg, entry);
		free(ifg);
		return (-1);
	}
	return (0);
}

int
if_leave_group(struct iface *iface, struct in_addr *addr)
{
	struct ip_mreq		 mreq;
	struct if_group_count	*ifg;
	struct if_addr		*if_addr;

	LIST_FOREACH(ifg, &ifglist, entry)
		if (iface->ifindex == ifg->ifindex &&
		    addr->s_addr == ifg->addr.s_addr)
			break;

	/* if interface is not found just try to drop membership */
	if (ifg) {
		if (--ifg->count != 0)
			/* others still joined */
			return (0);

		LIST_REMOVE(ifg, entry);
		free(ifg);
	}

	if_addr = LIST_FIRST(&iface->addr_list);
	if (!if_addr)
		return (0);

	mreq.imr_multiaddr.s_addr = addr->s_addr;
	mreq.imr_interface.s_addr = if_addr->addr.s_addr;

	if (setsockopt(iface->discovery_fd, IPPROTO_IP,
	    IP_DROP_MEMBERSHIP, (void *)&mreq, sizeof(mreq)) < 0) {
		log_warn("if_leave_group: error IP_DROP_MEMBERSHIP, "
		    "interface %s address %s", iface->name,
		    inet_ntoa(*addr));
		return (-1);
	}

	return (0);
}

int
if_set_mcast(struct iface *iface)
{
	struct if_addr		*if_addr;

	if_addr = LIST_FIRST(&iface->addr_list);

	if (setsockopt(iface->discovery_fd, IPPROTO_IP, IP_MULTICAST_IF,
	    &if_addr->addr.s_addr, sizeof(if_addr->addr.s_addr)) < 0) {
		log_debug("if_set_mcast: error setting "
		    "IP_MULTICAST_IF, interface %s", iface->name);
		return (-1);
	}

	return (0);
}

int
if_set_mcast_loop(int fd)
{
	u_int8_t	loop = 0;

	if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP,
	    (char *)&loop, sizeof(loop)) < 0) {
		log_warn("if_set_mcast_loop: error setting IP_MULTICAST_LOOP");
		return (-1);
	}

	return (0);
}
