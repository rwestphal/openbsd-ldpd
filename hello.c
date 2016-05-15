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
#include <sys/socket.h>
#include <sys/uio.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
#include <net/if_dl.h>

#include <errno.h>
#include <event.h>
#include <stdlib.h>
#include <string.h>

#include "ldpd.h"
#include "ldp.h"
#include "log.h"
#include "ldpe.h"

extern struct ldpd_conf        *leconf;

int	tlv_decode_hello_prms(char *, uint16_t, uint16_t *, uint16_t *);
int	tlv_decode_opt_hello_prms(char *, uint16_t, struct in_addr *,
	    uint32_t *);
int	gen_hello_prms_tlv(struct ibuf *buf, uint16_t, uint16_t);
int	gen_opt4_hello_prms_tlv(struct ibuf *, uint16_t, uint32_t);

int
send_hello(enum hello_type type, struct iface *iface, struct tnbr *tnbr)
{
	struct sockaddr_in	 dst;
	struct ibuf		*buf;
	uint16_t		 size, holdtime = 0, flags = 0;
	int			 fd = 0;

	dst.sin_port = htons(LDP_PORT);
	dst.sin_family = AF_INET;
	dst.sin_len = sizeof(struct sockaddr_in);

	switch (type) {
	case HELLO_LINK:
		inet_aton(AllRouters, &dst.sin_addr);
		holdtime = iface->hello_holdtime;
		flags = 0;
		fd = iface->discovery_fd;
		break;
	case HELLO_TARGETED:
		dst.sin_addr.s_addr = tnbr->addr.s_addr;
		holdtime = tnbr->hello_holdtime;
		flags = TARGETED_HELLO;
		if ((tnbr->flags & F_TNBR_CONFIGURED) || tnbr->pw_count)
			flags |= REQUEST_TARG_HELLO;
		fd = tnbr->discovery_fd;
		break;
	}

	size = LDP_HDR_SIZE + LDP_MSG_SIZE + sizeof(struct hello_prms_tlv) +
	    sizeof(struct hello_prms_opt4_tlv);

	/* generate message */
	if ((buf = ibuf_open(size)) == NULL)
		fatal(__func__);

	gen_ldp_hdr(buf, size);
	size -= LDP_HDR_SIZE;
	gen_msg_hdr(buf, MSG_TYPE_HELLO, size);
	gen_hello_prms_tlv(buf, holdtime, flags);
	gen_opt4_hello_prms_tlv(buf, TLV_TYPE_IPV4TRANSADDR,
	    leconf->trans_addr.s_addr);

	send_packet(fd, iface, buf->buf, buf->wpos, &dst);
	ibuf_free(buf);

	return (0);
}

void
recv_hello(struct in_addr lsr_id, struct ldp_msg *lm, struct in_addr src,
    struct iface *iface, int multicast, char *buf, uint16_t len)
{
	struct adj		*adj;
	struct nbr		*nbr;
	uint16_t		 holdtime, flags;
	struct in_addr		 transport_addr;
	uint32_t		 conf_number;
	int			 r;
	struct hello_source	 source;
	struct tnbr		*tnbr = NULL;

	r = tlv_decode_hello_prms(buf, len, &holdtime, &flags);
	if (r == -1) {
		log_debug("%s: neighbor %s: failed to decode params", __func__,
		    inet_ntoa(lsr_id));
		return;
	}
	if (holdtime != 0 && holdtime < MIN_HOLDTIME) {
		log_debug("%s: neighbor %s: invalid hello holdtime (%u)",
		    __func__, inet_ntoa(lsr_id), holdtime);
		return;
	}
	buf += r;
	len -= r;

	/* safety checks */
	if (multicast && (flags & TARGETED_HELLO)) {
		log_debug("%s: neighbor %s: multicast targeted hello", __func__,
		    inet_ntoa(lsr_id));
		return;
	}
	if (!multicast && !((flags & TARGETED_HELLO))) {
		log_debug("%s: neighbor %s: unicast link hello", __func__,
		    inet_ntoa(lsr_id));
		return;
	}

	memset(&source, 0, sizeof(source));
	if (flags & TARGETED_HELLO) {
		tnbr = tnbr_find(leconf, src);

		/* remove the dynamic tnbr if the 'R' bit was cleared */
		if (tnbr && (tnbr->flags & F_TNBR_DYNAMIC) &&
		    !((flags & REQUEST_TARG_HELLO))) {
			tnbr->flags &= ~F_TNBR_DYNAMIC;
			tnbr = tnbr_check(tnbr);
		}

		if (!tnbr) {
			if (!((flags & REQUEST_TARG_HELLO) &&
			    leconf->flags & F_LDPD_TH_ACCEPT))
				return;

			tnbr = tnbr_new(leconf, src);
			tnbr->flags |= F_TNBR_DYNAMIC;
			tnbr_init(tnbr);
			LIST_INSERT_HEAD(&leconf->tnbr_list, tnbr, entry);
		}

		source.type = HELLO_TARGETED;
		source.target = tnbr;
	} else {
		source.type = HELLO_LINK;
		source.link.iface = iface;
		source.link.src_addr.s_addr = src.s_addr;
	}

	r = tlv_decode_opt_hello_prms(buf, len, &transport_addr,
	    &conf_number);
	if (r == -1) {
		log_debug("%s: neighbor %s: failed to decode optional params",
		    __func__, inet_ntoa(lsr_id));
		return;
	}
	if (r != len) {
		log_debug("%s: neighbor %s: unexpected data in message",
		    __func__, inet_ntoa(lsr_id));
		return;
	}

	/* implicit transport address */
	if (transport_addr.s_addr == INADDR_ANY)
		transport_addr.s_addr = src.s_addr;
	if (bad_ip_addr(transport_addr)) {
		log_debug("%s: neighbor %s: invalid transport address %s",
		    __func__, inet_ntoa(lsr_id), inet_ntoa(transport_addr));
		return;
	}

	nbr = nbr_find_ldpid(lsr_id.s_addr);
	if (!nbr) {
		/* create new adjacency and new neighbor */
		nbr = nbr_new(lsr_id, transport_addr);
		adj = adj_new(nbr, &source, transport_addr);
	} else {
		adj = adj_find(nbr, &source);
		if (!adj) {
			/* create new adjacency for existing neighbor */
			adj = adj_new(nbr, &source, transport_addr);

			if (nbr->raddr.s_addr != transport_addr.s_addr)
				log_warnx("%s: neighbor %s: multiple "
				    "adjacencies advertising different "
				    "transport addresses", __func__,
				    inet_ntoa(lsr_id));
		}
	}

	/* always update the holdtime to properly handle runtime changes */
	switch (source.type) {
	case HELLO_LINK:
		if (holdtime == 0)
			holdtime = LINK_DFLT_HOLDTIME;

		adj->holdtime = min(iface->hello_holdtime, holdtime);
		break;
	case HELLO_TARGETED:
		if (holdtime == 0)
			holdtime = TARGETED_DFLT_HOLDTIME;

		adj->holdtime = min(tnbr->hello_holdtime, holdtime);
	}
	if (adj->holdtime != INFINITE_HOLDTIME)
		adj_start_itimer(adj);
	else
		adj_stop_itimer(adj);

	if (nbr->state == NBR_STA_PRESENT && nbr_session_active_role(nbr) &&
	    !nbr_pending_connect(nbr) && !nbr_pending_idtimer(nbr))
		nbr_establish_connection(nbr);
}

int
gen_hello_prms_tlv(struct ibuf *buf, uint16_t holdtime, uint16_t flags)
{
	struct hello_prms_tlv	parms;

	memset(&parms, 0, sizeof(parms));
	parms.type = htons(TLV_TYPE_COMMONHELLO);
	parms.length = htons(sizeof(parms.holdtime) + sizeof(parms.flags));
	parms.holdtime = htons(holdtime);
	parms.flags = htons(flags);

	return (ibuf_add(buf, &parms, sizeof(parms)));
}

int
gen_opt4_hello_prms_tlv(struct ibuf *buf, uint16_t type, uint32_t value)
{
	struct hello_prms_opt4_tlv	parms;

	memset(&parms, 0, sizeof(parms));
	parms.type = htons(type);
	parms.length = htons(4);
	parms.value = value;

	return (ibuf_add(buf, &parms, sizeof(parms)));
}

int
tlv_decode_hello_prms(char *buf, uint16_t len, uint16_t *holdtime,
    uint16_t *flags)
{
	struct hello_prms_tlv	tlv;

	if (len < sizeof(tlv))
		return (-1);
	memcpy(&tlv, buf, sizeof(tlv));

	if (tlv.type != htons(TLV_TYPE_COMMONHELLO))
		return (-1);
	if (ntohs(tlv.length) != sizeof(tlv) - TLV_HDR_LEN)
		return (-1);

	*holdtime = ntohs(tlv.holdtime);
	*flags = ntohs(tlv.flags);

	return (sizeof(tlv));
}

int
tlv_decode_opt_hello_prms(char *buf, uint16_t len, struct in_addr *addr,
    uint32_t *conf_number)
{
	struct tlv	tlv;
	uint16_t	tlv_len;
	int		total = 0;

	memset(addr, 0, sizeof(*addr));
	*conf_number = 0;

	while (len >= sizeof(tlv)) {
		memcpy(&tlv, buf, sizeof(tlv));
		tlv_len = ntohs(tlv.length);
		switch (ntohs(tlv.type)) {
		case TLV_TYPE_IPV4TRANSADDR:
			if (tlv_len != sizeof(uint32_t))
				return (-1);
			memcpy(addr, buf + TLV_HDR_LEN, sizeof(uint32_t));
			break;
		case TLV_TYPE_CONFIG:
			if (tlv_len != sizeof(uint32_t))
				return (-1);
			memcpy(conf_number, buf + TLV_HDR_LEN,
			    sizeof(uint32_t));
			break;
		default:
			/* if unknown flag set, ignore TLV */
			if (!(ntohs(tlv.type) & UNKNOWN_FLAG))
				return (-1);
			break;
		}
		buf += TLV_HDR_LEN + tlv_len;
		len -= TLV_HDR_LEN + tlv_len;
		total += TLV_HDR_LEN + tlv_len;
	}

	return (total);
}
