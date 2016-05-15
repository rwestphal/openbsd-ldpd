/*	$OpenBSD$ */

/*
 * Copyright (c) 2009 Michele Marchetto <michele@openbsd.org>
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
#include <net/if.h>
#include <net/if_types.h>
#include <netinet/in.h>
#include <netmpls/mpls.h>
#include <arpa/inet.h>
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
#include "lde.h"

static int fec_compare(struct fec *, struct fec *);

void		 fec_free(void *);
struct fec_node	*fec_add(struct fec *fec);
struct fec_nh	*fec_nh_add(struct fec_node *, struct in_addr);
void		 fec_nh_del(struct fec_nh *);
int		 lde_nbr_is_nexthop(struct fec_node *, struct lde_nbr *);

RB_GENERATE(fec_tree, fec, entry, fec_compare)

extern struct nbr_tree	lde_nbrs;
RB_PROTOTYPE(nbr_tree, lde_nbr, entry, lde_nbr_compare)

extern struct ldpd_conf		*ldeconf;

struct fec_tree	ft = RB_INITIALIZER(&ft);
struct event gc_timer;

/* FEC tree functions */
void
fec_init(struct fec_tree *fh)
{
	RB_INIT(fh);
}

static int
fec_compare(struct fec *a, struct fec *b)
{
	if (a->type < b->type)
		return (-1);
	if (a->type > b->type)
		return (1);

	switch (a->type) {
	case FEC_TYPE_IPV4:
		if (ntohl(a->u.ipv4.prefix.s_addr) <
		    ntohl(b->u.ipv4.prefix.s_addr))
			return (-1);
		if (ntohl(a->u.ipv4.prefix.s_addr) >
		    ntohl(b->u.ipv4.prefix.s_addr))
			return (1);
		if (a->u.ipv4.prefixlen < b->u.ipv4.prefixlen)
			return (-1);
		if (a->u.ipv4.prefixlen > b->u.ipv4.prefixlen)
			return (1);
		return (0);
	case FEC_TYPE_PWID:
		if (a->u.pwid.type < b->u.pwid.type)
			return (-1);
		if (a->u.pwid.type > b->u.pwid.type)
			return (1);
		if (a->u.pwid.pwid < b->u.pwid.pwid)
			return (-1);
		if (a->u.pwid.pwid > b->u.pwid.pwid)
			return (1);
		if (ntohl(a->u.pwid.lsr_id.s_addr) <
		    ntohl(b->u.pwid.lsr_id.s_addr))
			return (-1);
		if (ntohl(a->u.pwid.lsr_id.s_addr) >
		    ntohl(b->u.pwid.lsr_id.s_addr))
			return (1);
		return (0);
	}

	return (-1);
}

struct fec *
fec_find(struct fec_tree *fh, struct fec *f)
{
	return (RB_FIND(fec_tree, fh, f));
}

int
fec_insert(struct fec_tree *fh, struct fec *f)
{
	if (RB_INSERT(fec_tree, fh, f) != NULL)
		return (-1);
	return (0);
}

int
fec_remove(struct fec_tree *fh, struct fec *f)
{
	if (RB_REMOVE(fec_tree, fh, f) == NULL) {
		log_warnx("%s failed for %s", __func__, log_fec(f));
		return (-1);
	}
	return (0);
}

void
fec_clear(struct fec_tree *fh, void (*free_cb)(void *))
{
	struct fec	*f;

	while ((f = RB_ROOT(fh)) != NULL) {
		fec_remove(fh, f);
		free_cb(f);
	}
}

/* routing table functions */
int
lde_nbr_is_nexthop(struct fec_node *fn, struct lde_nbr *ln)
{
	struct fec_nh		*fnh;

	LIST_FOREACH(fnh, &fn->nexthops, entry)
		if (lde_address_find(ln, &fnh->nexthop))
			return (1);

	return (0);
}

void
rt_dump(pid_t pid)
{
	struct fec		*f;
	struct fec_node		*fn;
	struct lde_map		*me;
	static struct ctl_rt	 rtctl;

	RB_FOREACH(f, fec_tree, &ft) {
		fn = (struct fec_node *)f;
		if (fn->fec.type != FEC_TYPE_IPV4)
			continue;

		if (fn->local_label == NO_LABEL &&
		    LIST_EMPTY(&fn->downstream))
			continue;

		rtctl.prefix = fn->fec.u.ipv4.prefix;
		rtctl.prefixlen = fn->fec.u.ipv4.prefixlen;
		rtctl.local_label = fn->local_label;

		LIST_FOREACH(me, &fn->downstream, entry) {
			rtctl.in_use = lde_nbr_is_nexthop(fn, me->nexthop);
			rtctl.nexthop = me->nexthop->id;
			rtctl.remote_label = me->map.label;

			lde_imsg_compose_ldpe(IMSG_CTL_SHOW_LIB, 0, pid,
			    &rtctl, sizeof(rtctl));
		}
		if (LIST_EMPTY(&fn->downstream)) {
			rtctl.in_use = 0;
			rtctl.nexthop.s_addr = INADDR_ANY;
			rtctl.remote_label = NO_LABEL;

			lde_imsg_compose_ldpe(IMSG_CTL_SHOW_LIB, 0, pid,
			    &rtctl, sizeof(rtctl));
		}
	}
}

void
fec_snap(struct lde_nbr *ln)
{
	struct fec	*f;
	struct fec_node	*fn;

	RB_FOREACH(f, fec_tree, &ft) {
		fn = (struct fec_node *)f;
		if (fn->local_label == NO_LABEL)
			continue;

		lde_send_labelmapping(ln, fn, 0);
	}

	lde_imsg_compose_ldpe(IMSG_MAPPING_ADD_END, ln->peerid, 0, NULL, 0);
}

void
fec_free(void *arg)
{
	struct fec_node	*fn = arg;
	struct fec_nh	*fnh;

	while ((fnh = LIST_FIRST(&fn->nexthops)))
		fec_nh_del(fnh);
	if (!LIST_EMPTY(&fn->downstream))
		log_warnx("%s: fec %s downstream list not empty", __func__,
		    log_fec(&fn->fec));
	if (!LIST_EMPTY(&fn->upstream))
		log_warnx("%s: fec %s upstream list not empty", __func__,
		    log_fec(&fn->fec));

	free(fn);
}

void
fec_tree_clear(void)
{
	fec_clear(&ft, fec_free);
}

struct fec_node *
fec_add(struct fec *fec)
{
	struct fec_node	*fn;

	fn = calloc(1, sizeof(*fn));
	if (fn == NULL)
		fatal(__func__);

	fn->fec = *fec;
	fn->local_label = NO_LABEL;
	LIST_INIT(&fn->upstream);
	LIST_INIT(&fn->downstream);
	LIST_INIT(&fn->nexthops);

	if (fec_insert(&ft, &fn->fec))
		log_warnx("failed to add %s to ft tree",
		    log_fec(&fn->fec));

	return (fn);
}

struct fec_nh *
fec_nh_find(struct fec_node *fn, struct in_addr nexthop)
{
	struct fec_nh	*fnh;

	LIST_FOREACH(fnh, &fn->nexthops, entry)
		if (fnh->nexthop.s_addr == nexthop.s_addr)
			return (fnh);
	return (NULL);
}

struct fec_nh *
fec_nh_add(struct fec_node *fn, struct in_addr nexthop)
{
	struct fec_nh	*fnh;

	fnh = calloc(1, sizeof(*fnh));
	if (fnh == NULL)
		fatal(__func__);

	fnh->nexthop = nexthop;
	fnh->remote_label = NO_LABEL;
	LIST_INSERT_HEAD(&fn->nexthops, fnh, entry);

	return (fnh);
}

void
fec_nh_del(struct fec_nh *fnh)
{
	LIST_REMOVE(fnh, entry);
	free(fnh);
}

uint32_t
egress_label(enum fec_type fec_type)
{
	if (!(ldeconf->flags & F_LDPD_EXPNULL))
		return (MPLS_LABEL_IMPLNULL);

	switch (fec_type) {
	case FEC_TYPE_IPV4:
		return (MPLS_LABEL_IPV4NULL);
	default:
		log_warnx("%s: unexpected fec type", __func__);
	}

	return (NO_LABEL);
}

void
lde_kernel_insert(struct fec *fec, struct in_addr nexthop, int connected,
    void *data)
{
	struct fec_node		*fn;
	struct fec_nh		*fnh;
	struct lde_map		*me;
	struct lde_nbr		*ln;

	log_debug("lde add fec %s nexthop %s",
	    log_fec(fec), inet_ntoa(nexthop));

	fn = (struct fec_node *)fec_find(&ft, fec);
	if (fn == NULL)
		fn = fec_add(fec);

	if (fec_nh_find(fn, nexthop) != NULL)
		return;

	if (fn->fec.type == FEC_TYPE_PWID)
		fn->data = data;

	if (fn->local_label == NO_LABEL) {
		if (connected)
			fn->local_label = egress_label(fn->fec.type);
		else
			fn->local_label = lde_assign_label();

		/* FEC.1: perform lsr label distribution procedure */
		RB_FOREACH(ln, nbr_tree, &lde_nbrs)
			lde_send_labelmapping(ln, fn, 1);
	}

	fnh = fec_nh_add(fn, nexthop);
	lde_send_change_klabel(fn, fnh);

	switch (fn->fec.type) {
	case FEC_TYPE_IPV4:
		ln = lde_nbr_find_by_addr(fnh->nexthop);
		break;
	case FEC_TYPE_PWID:
		ln = lde_nbr_find_by_lsrid(fn->fec.u.pwid.lsr_id);
		break;
	default:
		ln = NULL;
		break;
	}

	if (ln) {
		/* FEC.2  */
		me = (struct lde_map *)fec_find(&ln->recv_map, &fn->fec);
		if (me)
			/* FEC.5 */
			lde_check_mapping(&me->map, ln);
	}
}

void
lde_kernel_remove(struct fec *fec, struct in_addr nexthop)
{
	struct fec_node		*fn;
	struct fec_nh		*fnh;

	log_debug("lde remove fec %s nexthop %s",
	    log_fec(fec), inet_ntoa(nexthop));

	fn = (struct fec_node *)fec_find(&ft, fec);
	if (fn == NULL)
		/* route lost */
		return;

	fnh = fec_nh_find(fn, nexthop);
	if (fnh == NULL)
		/* route lost */
		return;

	lde_send_delete_klabel(fn, fnh);
	fec_nh_del(fnh);
	if (LIST_EMPTY(&fn->nexthops)) {
		lde_send_labelwithdraw_all(fn, NO_LABEL);
		fn->local_label = NO_LABEL;
		if (fn->fec.type == FEC_TYPE_PWID)
			fn->data = NULL;
	}
}

void
lde_check_mapping(struct map *map, struct lde_nbr *ln)
{
	struct fec		 fec;
	struct fec_node		*fn;
	struct fec_nh		*fnh;
	struct lde_req		*lre;
	struct lde_map		*me;
	struct l2vpn_pw		*pw;
	int			 msgsource = 0;

	lde_map2fec(map, ln->id, &fec);
	fn = (struct fec_node *)fec_find(&ft, &fec);
	if (fn == NULL)
		fn = fec_add(&fec);

	/* LMp.1: first check if we have a pending request running */
	lre = (struct lde_req *)fec_find(&ln->sent_req, &fn->fec);
	if (lre)
		/* LMp.2: delete record of outstanding label request */
		lde_req_del(ln, lre, 1);

	/* RFC 4447 control word and status tlv negotiation */
	if (map->type == MAP_TYPE_PWID && l2vpn_pw_negotiate(ln, fn, map))
		return;

	/*
	 * LMp.3 - LMp.8: Loop detection LMp.3 - unecessary for frame-mode
	 * mpls networks
	 */

	/* LMp.9 */
	me = (struct lde_map *)fec_find(&ln->recv_map, &fn->fec);
	if (me) {
		/* LMp.10 */
		if (me->map.label != map->label && lre == NULL) {
			/* LMp.10a */
			lde_send_labelrelease(ln, fn, me->map.label);

			/*
			 * Can not use lde_nbr_find_by_addr() because there's
			 * the possibility of multipath.
			 */
			LIST_FOREACH(fnh, &fn->nexthops, entry) {
				if (lde_address_find(ln, &fnh->nexthop) == NULL)
					continue;

				lde_send_delete_klabel(fn, fnh);
				fnh->remote_label = NO_LABEL;
			}
		}
	}

	/*
	 * LMp.11 - 12: consider multiple nexthops in order to
	 * support multipath
	 */
	LIST_FOREACH(fnh, &fn->nexthops, entry) {
		/* LMp.15: install FEC in FIB */
		switch (fec.type) {
		case FEC_TYPE_IPV4:
			if (!lde_address_find(ln, &fnh->nexthop))
				continue;

			fnh->remote_label = map->label;
			lde_send_change_klabel(fn, fnh);
			break;
		case FEC_TYPE_PWID:
			pw = (struct l2vpn_pw *) fn->data;
			if (pw == NULL)
				continue;

			pw->remote_group = map->fec.pwid.group_id;
			if (map->flags & F_MAP_PW_IFMTU)
				pw->remote_mtu = map->fec.pwid.ifmtu;
			if (map->flags & F_MAP_PW_STATUS)
				pw->remote_status = map->pw_status;
			fnh->remote_label = map->label;
			if (l2vpn_pw_ok(pw, fnh))
				lde_send_change_klabel(fn, fnh);
			break;
		default:
			break;
		}

		msgsource = 1;
	}
	/* LMp.13 & LMp.16: Record the mapping from this peer */
	if (me == NULL)
		me = lde_map_add(ln, fn, 0);
	me->map = *map;

	if (msgsource == 0)
		/* LMp.13: just return since we use liberal lbl retention */
		return;

	/*
	 * LMp.17 - LMp.27 are unnecessary since we don't need to implement
	 * loop detection. LMp.28 - LMp.30 are unnecessary because we are
	 * merging capable.
	 */
}

void
lde_check_request(struct map *map, struct lde_nbr *ln)
{
	struct fec	 fec;
	struct lde_req	*lre;
	struct fec_node	*fn;
	struct fec_nh	*fnh;

	/* TODO LRq.1: loop detection */

	/* LRq.2: is there a next hop for fec? */
	lde_map2fec(map, ln->id, &fec);
	fn = (struct fec_node *)fec_find(&ft, &fec);
	if (fn == NULL || LIST_EMPTY(&fn->nexthops)) {
		lde_send_notification(ln->peerid, S_NO_ROUTE, map->messageid,
		    htons(MSG_TYPE_LABELREQUEST));
		return;
	}

	/* LRq.3: is MsgSource the next hop? */
	LIST_FOREACH(fnh, &fn->nexthops, entry) {
		switch (fec.type) {
		case FEC_TYPE_IPV4:
			if (!lde_address_find(ln, &fnh->nexthop))
				continue;

			lde_send_notification(ln->peerid, S_LOOP_DETECTED,
			    map->messageid, htons(MSG_TYPE_LABELREQUEST));
			return;
		default:
			break;
		}
	}

	/* LRq.6: first check if we have a pending request running */
	lre = (struct lde_req *)fec_find(&ln->recv_req, &fn->fec);
	if (lre != NULL)
		/* LRq.7: duplicate request */
		return;

	/* LRq.8: record label request */
	lre = lde_req_add(ln, &fn->fec, 0);
	if (lre != NULL)
		lre->msgid = map->messageid;

	/* LRq.9: perform LSR label distribution */
	lde_send_labelmapping(ln, fn, 1);

	/*
	 * LRq.10: do nothing (Request Never) since we use liberal
	 * label retention.
	 * LRq.11 - 12 are unnecessary since we are merging capable.
	 */
}

void
lde_check_release(struct map *map, struct lde_nbr *ln)
{
	struct fec		 fec;
	struct fec_node		*fn;
	struct lde_wdraw	*lw;
	struct lde_map		*me;

	/* TODO group wildcard */
	if (map->type == MAP_TYPE_PWID && !(map->flags & F_MAP_PW_ID))
		return;

	lde_map2fec(map, ln->id, &fec);
	fn = (struct fec_node *)fec_find(&ft, &fec);
	/* LRl.1: does FEC match a known FEC? */
	if (fn == NULL)
		return;

	/* LRl.3: first check if we have a pending withdraw running */
	lw = (struct lde_wdraw *)fec_find(&ln->sent_wdraw, &fn->fec);
	if (lw && (map->label == NO_LABEL ||
	    (lw->label != NO_LABEL && map->label == lw->label))) {
		/* LRl.4: delete record of outstanding label withdraw */
		lde_wdraw_del(ln, lw);
	}

	/* LRl.6: check sent map list and remove it if available */
	me = (struct lde_map *)fec_find(&ln->sent_map, &fn->fec);
	if (me && (map->label == NO_LABEL || map->label == me->map.label))
		lde_map_del(ln, me, 1);

	/*
	 * LRl.11 - 13 are unnecessary since we remove the label from
	 * forwarding/switching as soon as the FEC is unreachable.
	 */
}

void
lde_check_release_wcard(struct map *map, struct lde_nbr *ln)
{
	struct fec		*f;
	struct fec_node		*fn;
	struct lde_wdraw	*lw;
	struct lde_map		*me;

	RB_FOREACH(f, fec_tree, &ft) {
		fn = (struct fec_node *)f;

		/* LRl.3: first check if we have a pending withdraw running */
		lw = (struct lde_wdraw *)fec_find(&ln->sent_wdraw, &fn->fec);
		if (lw && (map->label == NO_LABEL ||
		    (lw->label != NO_LABEL && map->label == lw->label))) {
			/* LRl.4: delete record of outstanding lbl withdraw */
			lde_wdraw_del(ln, lw);
		}

		/* LRl.6: check sent map list and remove it if available */
		me = (struct lde_map *)fec_find(&ln->sent_map, &fn->fec);
		if (me &&
		    (map->label == NO_LABEL || map->label == me->map.label)) {
			lde_map_del(ln, me, 1);
		}

		/*
		 * LRl.11 - 13 are unnecessary since we remove the label from
		 * forwarding/switching as soon as the FEC is unreachable.
		 */
	}
}

void
lde_check_withdraw(struct map *map, struct lde_nbr *ln)
{
	struct fec		 fec;
	struct fec_node		*fn;
	struct fec_nh		*fnh;
	struct lde_map		*me;
	struct l2vpn_pw		*pw;

	/* TODO group wildcard */
	if (map->type == MAP_TYPE_PWID && !(map->flags & F_MAP_PW_ID))
		return;

	lde_map2fec(map, ln->id, &fec);
	fn = (struct fec_node *)fec_find(&ft, &fec);
	if (fn == NULL)
		fn = fec_add(&fec);

	/* LWd.1: remove label from forwarding/switching use */
	LIST_FOREACH(fnh, &fn->nexthops, entry) {
		switch (fec.type) {
		case FEC_TYPE_IPV4:
			if (!lde_address_find(ln, &fnh->nexthop))
				continue;
			break;
		case FEC_TYPE_PWID:
			pw = (struct l2vpn_pw *) fn->data;
			if (pw == NULL)
				continue;
			break;
		default:
			break;
		}
		lde_send_delete_klabel(fn, fnh);
		fnh->remote_label = NO_LABEL;
	}

	/* LWd.2: send label release */
	lde_send_labelrelease(ln, fn, map->label);

	/* LWd.3: check previously received label mapping */
	me = (struct lde_map *)fec_find(&ln->recv_map, &fn->fec);
	if (me && (map->label == NO_LABEL || map->label == me->map.label))
		/* LWd.4: remove record of previously received lbl mapping */
		lde_map_del(ln, me, 0);
}

void
lde_check_withdraw_wcard(struct map *map, struct lde_nbr *ln)
{
	struct fec	*f;
	struct fec_node	*fn;
	struct fec_nh	*fnh;
	struct lde_map	*me;

	/* LWd.2: send label release */
	lde_send_labelrelease(ln, NULL, map->label);

	RB_FOREACH(f, fec_tree, &ft) {
		fn = (struct fec_node *)f;

		/* LWd.1: remove label from forwarding/switching use */
		LIST_FOREACH(fnh, &fn->nexthops, entry) {
			switch (f->type) {
			case FEC_TYPE_IPV4:
				if (!lde_address_find(ln, &fnh->nexthop))
					continue;
				break;
			case FEC_TYPE_PWID:
				if (f->u.pwid.lsr_id.s_addr != ln->id.s_addr)
					continue;
				break;
			default:
				break;
			}
			lde_send_delete_klabel(fn, fnh);
			fnh->remote_label = NO_LABEL;
		}

		/* LWd.3: check previously received label mapping */
		me = (struct lde_map *)fec_find(&ln->recv_map, &fn->fec);
		if (me && (map->label == NO_LABEL ||
		    map->label == me->map.label))
			/*
			 * LWd.4: remove record of previously received
			 * label mapping
			 */
			lde_map_del(ln, me, 0);
	}
}

/* gabage collector timer: timer to remove dead entries from the LIB */

/* ARGSUSED */
void
lde_gc_timer(int fd, short event, void *arg)
{
	struct fec	*fec, *safe;
	struct fec_node	*fn;
	int		 count = 0;

	RB_FOREACH_SAFE(fec, fec_tree, &ft, safe) {
		fn = (struct fec_node *) fec;

		if (!LIST_EMPTY(&fn->nexthops) ||
		    !LIST_EMPTY(&fn->downstream) ||
		    !LIST_EMPTY(&fn->upstream))
			continue;

		fec_remove(&ft, &fn->fec);
		free(fn);
		count++;
	}

	if (count > 0)
		log_debug("%s: %u entries removed", __func__, count);

	lde_gc_start_timer();
}

void
lde_gc_start_timer(void)
{
	struct timeval	 tv;

	timerclear(&tv);
	tv.tv_sec = LDE_GC_INTERVAL;
	if (evtimer_add(&gc_timer, &tv) == -1)
		fatal(__func__);
}

void
lde_gc_stop_timer(void)
{
	if (evtimer_pending(&gc_timer, NULL) &&
	    evtimer_del(&gc_timer) == -1)
		fatal(__func__);
}
