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
#include <unistd.h>

#include <errno.h>
#include <event.h>
#include <stdlib.h>
#include <string.h>

#include "ldpd.h"
#include "ldp.h"
#include "log.h"
#include "ldpe.h"

void		gen_fec_tlv(struct buf *, u_int32_t, u_int8_t);
void		gen_label_tlv(struct buf *, u_int32_t);

u_int32_t	tlv_decode_label(struct label_tlv *);
u_int32_t	decode_fec_elm(char *);
u_int8_t	decode_fec_len_elm(char *);
int		validate_fec_elm(char *);

/* Label Mapping Message */
int
send_labelmapping(struct nbr *nbr)
{
	struct buf		*buf;
	struct mapping_entry	*me;
	struct ldp_hdr		*ldp_hdr;
	u_int16_t		 tlv_size, size;

	if (nbr->iface->passive)
		return (0);

	log_debug("send_labelmapping: neighbor ID %s", inet_ntoa(nbr->id));

	if ((buf = buf_open(LDP_MAX_LEN)) == NULL)
		fatal("send_labelmapping");

	/* real size will be set up later */
	gen_ldp_hdr(buf, nbr->iface, 0);

	size = LDP_HDR_SIZE - TLV_HDR_LEN;

	TAILQ_FOREACH(me, &nbr->mapping_list, entry) {
		tlv_size = BASIC_LABEL_MAP_LEN + me->prefixlen/8;
		size += tlv_size;

		gen_msg_tlv(buf, MSG_TYPE_LABELMAPPING, tlv_size);
		gen_fec_tlv(buf, me->prefix, me->prefixlen);
		gen_label_tlv(buf, me->label);
	}

	/* XXX: should we remove them first? */
	nbr_mapping_list_clr(nbr, &nbr->mapping_list);

	ldp_hdr = buf_seek(buf, 0, sizeof(struct ldp_hdr));
	ldp_hdr->length = htons(size);

	bufferevent_write(nbr->bev, buf->buf, buf->wpos);
	buf_free(buf);

	return (0);
}

int
recv_labelmapping(struct nbr *nbr, char *buf, u_int16_t len)
{
	struct ldp_msg		*lm;
	struct fec_tlv		*ft;
	struct label_tlv	*lt;
	struct map		 map;
	int			 feclen;

	log_debug("recv_labelmapping: neighbor ID %s", inet_ntoa(nbr->id));

	if (nbr->state != NBR_STA_OPER)
		return (-1);

	bzero(&map, sizeof(map));
	lm = (struct ldp_msg *)buf;

	if ((len - TLV_HDR_LEN) < ntohs(lm->length)) {
		session_shutdown(nbr, S_BAD_MSG_LEN, lm->msgid, lm->type);
		return (-1);
	}

	buf += sizeof(struct ldp_msg);
	len -= sizeof(struct ldp_msg);

	ft = (struct fec_tlv *)buf;
	lt = (struct label_tlv *)(buf + TLV_HDR_LEN + ntohs(ft->length));

	if (len < (sizeof(*ft) + LABEL_TLV_LEN)) {
		session_shutdown(nbr, S_BAD_TLV_LEN, lm->msgid, lm->type);
		return (-1);
	}

	feclen = ntohs(ft->length);
	if (len - TLV_HDR_LEN < feclen) {
		session_shutdown(nbr, S_BAD_TLV_LEN, lm->msgid, lm->type);
		return (-1);
	}

	map.label = tlv_decode_label(lt);
	if (map.label == NO_LABEL) {
		session_shutdown(nbr, S_BAD_TLV_VAL, lm->msgid, lm->type);
		return (-1);
	}

	buf += sizeof(struct fec_tlv);
	len -= sizeof(struct fec_tlv);

	while (feclen >= FEC_ELM_MIN_LEN) {
		if (validate_fec_elm(buf) < 0) {
			session_shutdown(nbr, S_BAD_TLV_VAL, lm->msgid,
			    lm->type);
			return (-1);
		}

		map.prefix = decode_fec_elm(buf);
		map.prefixlen = decode_fec_len_elm(buf);
		map.prefix &= prefixlen2mask(map.prefixlen);

		ldpe_imsg_compose_lde(IMSG_LABEL_MAPPING, nbr->peerid, 0, &map,
		    sizeof(map));

		buf += FEC_ELM_MIN_LEN + map.prefixlen/8;
		feclen -= (FEC_ELM_MIN_LEN + map.prefixlen/8);
	}

	nbr_fsm(nbr, NBR_EVT_PDU_RCVD);

	return (ntohs(lm->length));
}

/* Label Request Message */
int
send_labelrequest(struct nbr *nbr)
{
	struct buf		*buf;
	struct mapping_entry	*me;
	struct ldp_hdr		*ldp_hdr;
	u_int16_t		 tlv_size, size;

	if (nbr->iface->passive)
		return (0);

	log_debug("send_labelrequest: neighbor ID %s", inet_ntoa(nbr->id));

	if ((buf = buf_open(LDP_MAX_LEN)) == NULL)
		fatal("send_labelrequest");

	/* real size will be set up later */
	gen_ldp_hdr(buf, nbr->iface, 0);

	size = LDP_HDR_SIZE - TLV_HDR_LEN;

	TAILQ_FOREACH(me, &nbr->request_list, entry) {
		tlv_size = me->prefixlen/8;
		size += tlv_size;

		gen_msg_tlv(buf, MSG_TYPE_LABELREQUEST, tlv_size);
		gen_fec_tlv(buf, me->prefix, me->prefixlen);
	}

	/* XXX: should we remove them first? */
	nbr_mapping_list_clr(nbr, &nbr->request_list);

	ldp_hdr = buf_seek(buf, 0, sizeof(struct ldp_hdr));
	ldp_hdr->length = htons(size);

	bufferevent_write(nbr->bev, buf->buf, buf->wpos);
	buf_free(buf);

	return (0);
}

int
recv_labelrequest(struct nbr *nbr, char *buf, u_int16_t len)
{
	struct ldp_msg	*lr;
	struct fec_tlv	*ft;
	struct map	 map;
	int		 feclen;

	log_debug("recv_labelrequest: neighbor ID %s", inet_ntoa(nbr->id));

	if (nbr->state != NBR_STA_OPER)
		return (-1);

	bzero(&map, sizeof(map));
	lr = (struct ldp_msg *)buf;

	if ((len - TLV_HDR_LEN) < ntohs(lr->length)) {
		session_shutdown(nbr, S_BAD_MSG_LEN, lr->msgid, lr->type);
		return (-1);
	}

	buf += sizeof(struct ldp_msg);
	len -= sizeof(struct ldp_msg);

	ft = (struct fec_tlv *)buf;

	if (len < sizeof(*ft) ||
	    (len - TLV_HDR_LEN) < ntohs(ft->length)) {
		session_shutdown(nbr, S_BAD_TLV_LEN, lr->msgid, lr->type);
		return (-1);
	}

	feclen = ntohs(ft->length);

	buf += sizeof(struct fec_tlv);
	len -= sizeof(struct fec_tlv);

	while (feclen >= FEC_ELM_MIN_LEN) {
		if (validate_fec_elm(buf) < 0) {
			session_shutdown(nbr, S_BAD_TLV_VAL, lr->msgid,
			    lr->type);
			return (-1);
		}

		map.prefix = decode_fec_elm(buf);
		map.prefixlen = decode_fec_len_elm(buf);
		map.prefix &= prefixlen2mask(map.prefixlen);
		map.messageid = lr->msgid;

		ldpe_imsg_compose_lde(IMSG_LABEL_REQUEST, nbr->peerid, 0, &map,
		    sizeof(map));

		buf += FEC_ELM_MIN_LEN + map.prefixlen/8;
		feclen -= (FEC_ELM_MIN_LEN + map.prefixlen/8);
	}

	nbr_fsm(nbr, NBR_EVT_PDU_RCVD);

	return (0);
}

/* Label Withdraw Message */
int
send_labelwithdraw(struct nbr *nbr)
{
	struct buf		*buf;
	struct mapping_entry	*me;
	struct ldp_hdr		*ldp_hdr;
	u_int16_t		 tlv_size, size;

	if (nbr->iface->passive)
		return (0);

	log_debug("send_labelwithdraw: neighbor ID %s", inet_ntoa(nbr->id));

	if ((buf = buf_open(LDP_MAX_LEN)) == NULL)
		fatal("send_labelwithdraw");

	/* real size will be set up later */
	gen_ldp_hdr(buf, nbr->iface, 0);

	size = LDP_HDR_SIZE - TLV_HDR_LEN;

	TAILQ_FOREACH(me, &nbr->withdraw_list, entry) {
		if (me->label == NO_LABEL)
			tlv_size = me->prefixlen/8;
		else
			tlv_size = BASIC_LABEL_MAP_LEN + me->prefixlen/8;

		size += tlv_size;

		gen_msg_tlv(buf, MSG_TYPE_LABELWITHDRAW, tlv_size);
		gen_fec_tlv(buf, me->prefix, me->prefixlen);

		if (me->label != NO_LABEL)
			gen_label_tlv(buf, me->label);
	}

	/* XXX: should we remove them first? */
	nbr_mapping_list_clr(nbr, &nbr->withdraw_list);

	ldp_hdr = buf_seek(buf, 0, sizeof(struct ldp_hdr));
	ldp_hdr->length = htons(size);

	bufferevent_write(nbr->bev, buf->buf, buf->wpos);

	buf_free(buf);

	return (0);
}

int
recv_labelwithdraw(struct nbr *nbr, char *buf, u_int16_t len)
{
	struct ldp_msg		*lw;

	log_debug("recv_labelwithdraw: neighbor ID %s", inet_ntoa(nbr->id));

	if (nbr->state != NBR_STA_OPER)
		return (-1);

	lw = (struct ldp_msg *)buf;

	if ((len - TLV_HDR_LEN) < ntohs(lw->length)) {
		session_shutdown(nbr, S_BAD_MSG_LEN, lw->msgid, lw->type);
		return (-1);
	}

	buf += sizeof(struct ldp_msg);
	len -= sizeof(struct ldp_msg);

	nbr_fsm(nbr, NBR_EVT_PDU_RCVD);

	return (0);
}

/* Label Release Message */
int
send_labelrelease(struct nbr *nbr)
{
	struct buf		*buf;
	struct mapping_entry	*me;
	struct ldp_hdr		*ldp_hdr;
	u_int16_t		 tlv_size, size;

	if (nbr->iface->passive)
		return (0);

	log_debug("send_labelrelease: neighbor ID %s", inet_ntoa(nbr->id));

	if ((buf = buf_open(LDP_MAX_LEN)) == NULL)
		fatal("send_labelrelease");

	/* real size will be set up later */
	gen_ldp_hdr(buf, nbr->iface, 0);

	size = LDP_HDR_SIZE - TLV_HDR_LEN;

	TAILQ_FOREACH(me, &nbr->release_list, entry) {
		if (me->label == NO_LABEL)
			tlv_size = me->prefixlen/8;
		else
			tlv_size = BASIC_LABEL_MAP_LEN + me->prefixlen/8;

		size += tlv_size;

		gen_msg_tlv(buf, MSG_TYPE_LABELRELEASE, tlv_size);
		gen_fec_tlv(buf, me->prefix, me->prefixlen);

		if (me->label != NO_LABEL)
			gen_label_tlv(buf, me->label);
	}

	/* XXX: should we remove them first? */
	nbr_mapping_list_clr(nbr, &nbr->release_list);

	ldp_hdr = buf_seek(buf, 0, sizeof(struct ldp_hdr));
	ldp_hdr->length = htons(size);

	bufferevent_write(nbr->bev, buf->buf, buf->wpos);
	buf_free(buf);

	return (0);
}

int
recv_labelrelease(struct nbr *nbr, char *buf, u_int16_t len)
{
	struct ldp_msg		*lr;

	log_debug("recv_labelrelease: neighbor ID %s", inet_ntoa(nbr->id));

	if (nbr->state != NBR_STA_OPER)
		return (-1);

	lr = (struct ldp_msg *)buf;

	if ((len - TLV_HDR_LEN) < ntohs(lr->length)) {
		session_shutdown(nbr, S_BAD_MSG_LEN, lr->msgid, lr->type);
		return (-1);
	}

	buf += sizeof(struct ldp_msg);
	len -= sizeof(struct ldp_msg);

	nbr_fsm(nbr, NBR_EVT_PDU_RCVD);

	return (0);
}

/* Label Abort Req Message */
int
send_labelabortreq(struct nbr *nbr)
{
	struct buf	*buf;
	u_int16_t	 size;

	if (nbr->iface->passive)
		return (0);

	log_debug("send_labelabortreq: neighbor ID %s", inet_ntoa(nbr->id));

	if ((buf = buf_open(LDP_MAX_LEN)) == NULL)
		fatal("send_labelabortreq");

	size = LDP_HDR_SIZE + sizeof(struct ldp_msg);

	gen_ldp_hdr(buf, nbr->iface, size);

	size -= LDP_HDR_SIZE;

	gen_msg_tlv(buf, MSG_TYPE_LABELABORTREQ, size);

	bufferevent_write(nbr->bev, buf->buf, buf->wpos);

	buf_free(buf);

	return (0);
}

int
recv_labelabortreq(struct nbr *nbr, char *buf, u_int16_t len)
{
	struct ldp_msg	*la;

	log_debug("recv_labelabortreq: neighbor ID %s", inet_ntoa(nbr->id));

	if (nbr->state != NBR_STA_OPER)
		return (-1);

	la = (struct ldp_msg *)buf;

	if ((len - TLV_HDR_LEN) < ntohs(la->length)) {
		session_shutdown(nbr, S_BAD_MSG_LEN, la->msgid, la->type);
		return (-1);
	}

	buf += sizeof(struct ldp_msg);
	len -= sizeof(struct ldp_msg);

	nbr_fsm(nbr, NBR_EVT_PDU_RCVD);

	return (0);
}

/* Other TLV related functions */
void
gen_fec_tlv(struct buf *buf, u_int32_t prefix, u_int8_t prefixlen)
{
	struct fec_tlv	ft;
	u_int8_t	type;
	u_int16_t	family;
	u_int8_t	len;
	u_int32_t	addr;

	ft.type = htons(TLV_TYPE_FEC);
	ft.length = htons(sizeof(ft) + (int)(prefixlen/8));

	buf_add(buf, &ft, sizeof(ft));

	if (prefixlen == 32) {
		type = FEC_ADDRESS;
		len = prefixlen/8;
	} else {
		type = FEC_PREFIX;
		len = prefixlen;
	}
	family = htons(FEC_IPV4);
	addr = prefix;

	buf_add(buf, &type, sizeof(type));
	buf_add(buf, &family, sizeof(family));
	buf_add(buf, &len, sizeof(len));
	buf_add(buf, &addr, (int)(prefixlen/8));
}

void
gen_label_tlv(struct buf *buf, u_int32_t label)
{
	struct label_tlv	lt;

	lt.type = htons(TLV_TYPE_GENERICLABEL);
	lt.length = htons(sizeof(label));
	lt.label = htonl(label);

	buf_add(buf, &lt, sizeof(lt));
}

u_int32_t
tlv_decode_label(struct label_tlv *lt)
{
	if (lt->type != htons(TLV_TYPE_GENERICLABEL))
		return (NO_LABEL);

	if (ntohs(lt->length) != sizeof(lt->label))
		return (NO_LABEL);

	return (ntohl(lt->label));
}

int
validate_fec_elm(char *buf)
{
	u_int16_t	*family;

	if (*buf != FEC_WILDCARD && *buf != FEC_PREFIX && *buf !=
	    FEC_ADDRESS)
		return (-1);

	buf += sizeof(u_int8_t);
	family = (u_int16_t *)buf;

	if (*family != htons(FEC_IPV4))
		return (-1);

	return (0);
}

u_int32_t
decode_fec_elm(char *buf)
{
	struct fec_elm *fe = (struct fec_elm *)buf;

	return (fe->addr);
}

u_int8_t
decode_fec_len_elm(char *buf)
{
	/* Skip type and family */
	buf += sizeof(u_int8_t);
	buf += sizeof(u_int16_t);

	return (*buf);
}
