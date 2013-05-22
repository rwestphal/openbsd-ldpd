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
#include <netinet/in_systm.h>
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

int	tlv_decode_hello_prms(char *, u_int16_t, u_int16_t *, u_int16_t *);
int	tlv_decode_opt_hello_prms(char *, u_int16_t, struct in_addr *,
	    u_int32_t *);
int	gen_hello_prms_tlv(struct iface *, struct ibuf *);
int	gen_opt4_hello_prms_tlv(struct iface *, struct ibuf *);

int
send_hello(struct iface *iface)
{
	struct sockaddr_in	 dst;
	struct ibuf		*buf;
	u_int16_t		 size;

	dst.sin_port = htons(LDP_PORT);
	dst.sin_family = AF_INET;
	dst.sin_len = sizeof(struct sockaddr_in);
	inet_aton(AllRouters, &dst.sin_addr);

	if ((buf = ibuf_open(LDP_MAX_LEN)) == NULL)
		fatal("send_hello");

	size = LDP_HDR_SIZE + sizeof(struct ldp_msg) +
	    sizeof(struct hello_prms_tlv) +
	    sizeof(struct hello_prms_opt4_tlv);

	gen_ldp_hdr(buf, size);

	size -= LDP_HDR_SIZE;

	gen_msg_tlv(buf, MSG_TYPE_HELLO, size);

	gen_hello_prms_tlv(iface, buf);
	gen_opt4_hello_prms_tlv(iface, buf);

	send_packet(iface, buf->buf, buf->wpos, &dst);
	ibuf_free(buf);

	return (0);
}

void
recv_hello(struct iface *iface, struct in_addr src, char *buf, u_int16_t len)
{
	struct ldp_msg		 hello;
	struct ldp_hdr		 ldp;
	struct nbr		*nbr = NULL;
	struct in_addr		 address;
	u_int32_t		 conf_number;
	u_int16_t		 holdtime, flags;
	int			 r;

	bcopy(buf, &ldp, sizeof(ldp));
	buf += LDP_HDR_SIZE;
	len -= LDP_HDR_SIZE;

	bcopy(buf, &hello, sizeof(hello));
	buf += sizeof(struct ldp_msg);
	len -= sizeof(struct ldp_msg);

	r = tlv_decode_hello_prms(buf, len, &holdtime, &flags);
	if (r == -1) {
		address.s_addr = ldp.lsr_id;
		log_debug("recv_hello: neighbor %s: failed to decode params",
		    inet_ntoa(address));
		return;
	}

	buf += r;
	len -= r;

	r = tlv_decode_opt_hello_prms(buf, len, &address, &conf_number);
	if (r == -1) {
		address.s_addr = ldp.lsr_id;
		log_debug("recv_hello: neighbor %s: failed to decode "
		    "optional params", inet_ntoa(address));
		return;
	}
	if (r != len) {
		address.s_addr = ldp.lsr_id;
		log_debug("recv_hello: neighbor %s: unexpected data in message",
		    inet_ntoa(address));
		return;
	}

	nbr = nbr_find_ldpid(ldp.lsr_id);
	if (!nbr) {
		struct in_addr	a;

		if (address.s_addr == INADDR_ANY)
			a = src;
		else
			a = address;

		nbr = nbr_new(ldp.lsr_id, iface, a);

		/* set neighbor parameters */
		nbr->hello_type = flags;

		/* XXX: lacks support for targeted hellos */
		if (holdtime == 0)
			holdtime = LINK_DFLT_HOLDTIME;

		if (iface->holdtime < holdtime)
			nbr->holdtime = iface->holdtime;
		else
			nbr->holdtime = holdtime;
	}

	nbr_fsm(nbr, NBR_EVT_HELLO_RCVD);

	if (nbr->state == NBR_STA_PRESENT && nbr_session_active_role(nbr) &&
	    !nbr_pending_connect(nbr) && !nbr_pending_idtimer(nbr))
		nbr_establish_connection(nbr);
}

int
gen_hello_prms_tlv(struct iface *iface, struct ibuf *buf)
{
	struct hello_prms_tlv	parms;

	bzero(&parms, sizeof(parms));
	parms.type = htons(TLV_TYPE_COMMONHELLO);
	parms.length = htons(sizeof(parms.holdtime) + sizeof(parms.flags));
	/* XXX */
	parms.holdtime = htons(iface->holdtime);
	parms.flags = 0;

	return (ibuf_add(buf, &parms, sizeof(parms)));
}

int
gen_opt4_hello_prms_tlv(struct iface *iface, struct ibuf *buf)
{
	struct hello_prms_opt4_tlv	parms;

	bzero(&parms, sizeof(parms));
	parms.type = htons(TLV_TYPE_IPV4TRANSADDR);
	parms.length = htons(4);
	parms.value = ldpe_router_id();

	return (ibuf_add(buf, &parms, sizeof(parms)));
}

int
tlv_decode_hello_prms(char *buf, u_int16_t len, u_int16_t *holdtime,
    u_int16_t *flags)
{
	struct hello_prms_tlv	tlv;

	if (len < sizeof(tlv))
		return (-1);
	bcopy(buf, &tlv, sizeof(tlv));

	if (ntohs(tlv.length) != sizeof(tlv) - TLV_HDR_LEN)
		return (-1);

	if (tlv.type != htons(TLV_TYPE_COMMONHELLO))
		return (-1);

	*holdtime = ntohs(tlv.holdtime);
	*flags = ntohs(tlv.flags);

	return (sizeof(tlv));
}

int
tlv_decode_opt_hello_prms(char *buf, u_int16_t len, struct in_addr *addr,
    u_int32_t *conf_number)
{
	struct tlv	tlv;
	int		cons = 0;
	u_int16_t	tlv_len;

	bzero(addr, sizeof(*addr));
	*conf_number = 0;

	while (len >= sizeof(tlv)) {
		bcopy(buf, &tlv, sizeof(tlv));
		tlv_len = ntohs(tlv.length);
		switch (ntohs(tlv.type)) {
		case TLV_TYPE_IPV4TRANSADDR:
			if (tlv_len != sizeof(u_int32_t))
				return (-1);
			bcopy(buf + TLV_HDR_LEN, addr, sizeof(u_int32_t));
			break;
		case TLV_TYPE_CONFIG:
			if (tlv_len != sizeof(u_int32_t))
				return (-1);
			bcopy(buf + TLV_HDR_LEN, conf_number,
			    sizeof(u_int32_t));
			break;
		default:
			/* if unknown flag set, ignore TLV */
			if (!(ntohs(tlv.type) & UNKNOWN_FLAG))
				return (-1);
			break;
		}
		buf += TLV_HDR_LEN + tlv_len;
		len -= TLV_HDR_LEN + tlv_len;
		cons += TLV_HDR_LEN + tlv_len;
	}

	return (cons);
}
