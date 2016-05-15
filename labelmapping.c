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
#include <netmpls/mpls.h>
#include <unistd.h>

#include <errno.h>
#include <event.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "ldpd.h"
#include "ldp.h"
#include "log.h"
#include "ldpe.h"

void		gen_label_tlv(struct ibuf *, uint32_t);
void		gen_reqid_tlv(struct ibuf *, uint32_t);

int	tlv_decode_label(struct nbr *, struct ldp_msg *, char *, uint16_t,
	    uint32_t *);

static void
enqueue_pdu(struct nbr *nbr, struct ibuf *buf, uint16_t size)
{
	struct ldp_hdr		*ldp_hdr;

	ldp_hdr = ibuf_seek(buf, 0, sizeof(struct ldp_hdr));
	ldp_hdr->length = htons(size);
	evbuf_enqueue(&nbr->tcp->wbuf, buf);
}

/* Generic function that handles all Label Message types */
void
send_labelmessage(struct nbr *nbr, uint16_t type, struct mapping_head *mh)
{
	struct ibuf		*buf = NULL;
	struct mapping_entry	*me;
	uint16_t		 msg_size, size = 0;
	int			 first = 1;

	while ((me = TAILQ_FIRST(mh)) != NULL) {
		/* generate pdu */
		if (first) {
			if ((buf = ibuf_open(nbr->max_pdu_len +
			    LDP_HDR_DEAD_LEN)) == NULL)
				fatal(__func__);

			/* real size will be set up later */
			gen_ldp_hdr(buf, 0);

			size = LDP_HDR_PDU_LEN;
			first = 0;
		}

		/* calculate size */
		msg_size = LDP_MSG_SIZE + TLV_HDR_LEN;

		switch (me->map.type) {
		case MAP_TYPE_WILDCARD:
			msg_size += FEC_ELM_WCARD_LEN;
			break;
		case MAP_TYPE_PREFIX:
			msg_size += FEC_ELM_PREFIX_MIN_LEN +
			    PREFIX_SIZE(me->map.fec.ipv4.prefixlen);
			break;
		case MAP_TYPE_PWID:
			msg_size += FEC_PWID_ELM_MIN_LEN;

			if (me->map.flags & F_MAP_PW_ID)
				msg_size += sizeof(uint32_t);
			if (me->map.flags & F_MAP_PW_IFMTU)
				msg_size += FEC_SUBTLV_IFMTU_LEN;
	    		if (me->map.flags & F_MAP_PW_STATUS)
				msg_size += PW_STATUS_TLV_LEN;
			break;
		}

		if (me->map.label != NO_LABEL)
			msg_size += LABEL_TLV_LEN;
		if (me->map.flags & F_MAP_REQ_ID)
			msg_size += REQID_TLV_LEN;

		/* maximum pdu length exceeded, we need a new ldp pdu */
		if (size + msg_size > nbr->max_pdu_len) {
			enqueue_pdu(nbr, buf, size);
			first = 1;
			continue;
		}

		size += msg_size;

		/* append message and tlvs */
		gen_msg_hdr(buf, type, msg_size);
		gen_fec_tlv(buf, &me->map);
		if (me->map.label != NO_LABEL)
			gen_label_tlv(buf, me->map.label);
		if (me->map.flags & F_MAP_REQ_ID)
			gen_reqid_tlv(buf, me->map.requestid);
	    	if (me->map.flags & F_MAP_PW_STATUS)
			gen_pw_status_tlv(buf, me->map.pw_status);

		TAILQ_REMOVE(mh, me, entry);
		free(me);
	}

	enqueue_pdu(nbr, buf, size);

	nbr_fsm(nbr, NBR_EVT_PDU_SENT);
}

/* Generic function that handles all Label Message types */
int
recv_labelmessage(struct nbr *nbr, char *buf, uint16_t len, uint16_t type)
{
	struct ldp_msg		 lm;
	struct tlv		 ft;
	uint32_t		 label = NO_LABEL, reqid = 0;
	uint32_t		 pw_status = 0;
	uint8_t			 flags = 0;
	int			 feclen, lbllen, tlen;
	struct mapping_entry	*me;
	struct mapping_head	 mh;
	struct map		 map;

	memcpy(&lm, buf, sizeof(lm));
	buf += LDP_MSG_SIZE;
	len -= LDP_MSG_SIZE;

	/* FEC TLV */
	if (len < sizeof(ft)) {
		session_shutdown(nbr, S_BAD_TLV_LEN, lm.msgid, lm.type);
		return (-1);
	}

	memcpy(&ft, buf, sizeof(ft));
	if (ntohs(ft.type) != TLV_TYPE_FEC) {
		send_notification_nbr(nbr, S_MISS_MSG, lm.msgid, lm.type);
		return (-1);
	}
	feclen = ntohs(ft.length);

	if (feclen > len - TLV_HDR_LEN) {
		session_shutdown(nbr, S_BAD_TLV_LEN, lm.msgid, lm.type);
		return (-1);
	}

	buf += TLV_HDR_LEN;	/* just advance to the end of the fec header */
	len -= TLV_HDR_LEN;

	TAILQ_INIT(&mh);
	do {
		memset(&map, 0, sizeof(map));
		map.messageid = lm.msgid;

		if ((tlen = tlv_decode_fec_elm(nbr, &lm, buf, feclen,
		    &map)) == -1)
			goto err;
		if (map.type == MAP_TYPE_PWID &&
		    type == MSG_TYPE_LABELMAPPING &&
		    !(map.flags & F_MAP_PW_ID)) {
			send_notification_nbr(nbr, S_MISS_MSG, lm.msgid,
			    lm.type);
			return (-1);
		}

		/*
		 * The Wildcard FEC Element can be used only in the
		 * Label Withdraw and Label Release messages.
		 */
		if (map.type == MAP_TYPE_WILDCARD) {
			switch (type) {
			case MSG_TYPE_LABELMAPPING:
			case MSG_TYPE_LABELREQUEST:
			case MSG_TYPE_LABELABORTREQ:
				session_shutdown(nbr, S_BAD_TLV_VAL, lm.msgid,
				    lm.type);
				goto err;
			default:
				break;
			}
		}

		/*
		 * LDP supports the use of multiple FEC Elements per
		 * FEC for the Label Mapping message only.
		 */
		if (type != MSG_TYPE_LABELMAPPING &&
		    tlen != feclen) {
			session_shutdown(nbr, S_BAD_TLV_VAL, lm.msgid,
			    lm.type);
			goto err;
		}

		mapping_list_add(&mh, &map);

		buf += tlen;
		len -= tlen;
		feclen -= tlen;
	} while (feclen > 0);

	/* Mandatory Label TLV */
	if (type == MSG_TYPE_LABELMAPPING) {
		lbllen = tlv_decode_label(nbr, &lm, buf, len, &label);
		if (lbllen == -1)
			goto err;

		buf += lbllen;
		len -= lbllen;
	}

	/* Optional Parameters */
	while (len > 0) {
		struct tlv 	tlv;
		uint32_t	reqbuf, labelbuf, statusbuf;

		if (len < sizeof(tlv)) {
			session_shutdown(nbr, S_BAD_TLV_LEN, lm.msgid,
			    lm.type);
			goto err;
		}

		memcpy(&tlv, buf, sizeof(tlv));
		if (ntohs(tlv.length) != len - TLV_HDR_LEN) {
			session_shutdown(nbr, S_BAD_TLV_LEN, lm.msgid,
			    lm.type);
			goto err;
		}
		buf += TLV_HDR_LEN;
		len -= TLV_HDR_LEN;

		switch (ntohs(tlv.type) & ~UNKNOWN_FLAG) {
		case TLV_TYPE_LABELREQUEST:
			switch (type) {
			case MSG_TYPE_LABELMAPPING:
			case MSG_TYPE_LABELREQUEST:
				if (ntohs(tlv.length) != 4) {
					session_shutdown(nbr, S_BAD_TLV_LEN,
					    lm.msgid, lm.type);
					goto err;
				}

				flags |= F_MAP_REQ_ID;
				memcpy(&reqbuf, buf, sizeof(reqbuf));
				reqid = ntohl(reqbuf);
				break;
			default:
				/* ignore */
				break;
			}
			break;
		case TLV_TYPE_HOPCOUNT:
		case TLV_TYPE_PATHVECTOR:
			/* TODO just ignore for now */
			break;
		case TLV_TYPE_GENERICLABEL:
			switch (type) {
			case MSG_TYPE_LABELWITHDRAW:
			case MSG_TYPE_LABELRELEASE:
				if (ntohs(tlv.length) != 4) {
					session_shutdown(nbr, S_BAD_TLV_LEN,
					    lm.msgid, lm.type);
					goto err;
				}

				memcpy(&labelbuf, buf, sizeof(labelbuf));
				label = ntohl(labelbuf);
				break;
			default:
				/* ignore */
				break;
			}
			break;
		case TLV_TYPE_ATMLABEL:
		case TLV_TYPE_FRLABEL:
			switch (type) {
			case MSG_TYPE_LABELWITHDRAW:
			case MSG_TYPE_LABELRELEASE:
				/* unsupported */
				session_shutdown(nbr, S_BAD_TLV_VAL, lm.msgid,
				    lm.type);
				goto err;
				break;
			default:
				/* ignore */
				break;
			}
			break;
		case TLV_TYPE_PW_STATUS:
			switch (type) {
			case MSG_TYPE_LABELMAPPING:
				if (ntohs(tlv.length) != 4) {
					session_shutdown(nbr, S_BAD_TLV_LEN,
					    lm.msgid, lm.type);
					goto err;
				}

				flags |= F_MAP_PW_STATUS;
				memcpy(&statusbuf, buf, sizeof(statusbuf));
				pw_status = ntohl(statusbuf);
				break;
			default:
				/* ignore */
				break;
			}
			break;
		default:
			if (!(ntohs(tlv.type) & UNKNOWN_FLAG)) {
				send_notification_nbr(nbr, S_UNKNOWN_TLV,
				    lm.msgid, lm.type);
			}
			/* ignore unknown tlv */
			break;
		}
		buf += ntohs(tlv.length);
		len -= ntohs(tlv.length);
	}

	/* notify lde about the received message. */
	while ((me = TAILQ_FIRST(&mh)) != NULL) {
		int imsg_type = IMSG_NONE;

		me->map.flags |= flags;
		me->map.label = label;
		if (me->map.flags & F_MAP_REQ_ID)
			me->map.requestid = reqid;
		if (me->map.flags & F_MAP_PW_STATUS)
			me->map.pw_status = pw_status;

		switch (type) {
		case MSG_TYPE_LABELMAPPING:
			log_debug("label mapping from nbr %s, FEC %s, "
			    "label %u", inet_ntoa(nbr->id),
			    log_map(&me->map), me->map.label);
			imsg_type = IMSG_LABEL_MAPPING;
			break;
		case MSG_TYPE_LABELREQUEST:
			log_debug("label request from nbr %s, FEC %s",
			    inet_ntoa(nbr->id), log_map(&me->map));
			imsg_type = IMSG_LABEL_REQUEST;
			break;
		case MSG_TYPE_LABELWITHDRAW:
			log_debug("label withdraw from nbr %s, FEC %s",
			    inet_ntoa(nbr->id), log_map(&me->map));
			imsg_type = IMSG_LABEL_WITHDRAW;
			break;
		case MSG_TYPE_LABELRELEASE:
			log_debug("label release from nbr %s, FEC %s",
			    inet_ntoa(nbr->id), log_map(&me->map));
			imsg_type = IMSG_LABEL_RELEASE;
			break;
		case MSG_TYPE_LABELABORTREQ:
			log_debug("label abort from nbr %s, FEC %s",
			    inet_ntoa(nbr->id), log_map(&me->map));
			imsg_type = IMSG_LABEL_ABORT;
			break;
		default:
			break;
		}

		ldpe_imsg_compose_lde(imsg_type, nbr->peerid, 0, &me->map,
		    sizeof(struct map));

		TAILQ_REMOVE(&mh, me, entry);
		free(me);
	}

	return (0);

err:
	mapping_list_clr(&mh);

	return (-1);
}

/* Other TLV related functions */
void
gen_label_tlv(struct ibuf *buf, uint32_t label)
{
	struct label_tlv	lt;

	lt.type = htons(TLV_TYPE_GENERICLABEL);
	lt.length = htons(sizeof(label));
	lt.label = htonl(label);

	ibuf_add(buf, &lt, sizeof(lt));
}

int
tlv_decode_label(struct nbr *nbr, struct ldp_msg *lm, char *buf,
    uint16_t len, uint32_t *label)
{
	struct label_tlv lt;

	if (len < sizeof(lt)) {
		session_shutdown(nbr, S_BAD_TLV_LEN, lm->msgid, lm->type);
		return (-1);
	}
	memcpy(&lt, buf, sizeof(lt));

	if (!(ntohs(lt.type) & TLV_TYPE_GENERICLABEL)) {
		send_notification_nbr(nbr, S_MISS_MSG, lm->msgid, lm->type);
		return (-1);
	}

	switch (htons(lt.type)) {
	case TLV_TYPE_GENERICLABEL:
		if (ntohs(lt.length) != sizeof(lt) - TLV_HDR_LEN) {
			session_shutdown(nbr, S_BAD_TLV_LEN, lm->msgid,
			    lm->type);
			return (-1);
		}

		*label = ntohl(lt.label);
		if (*label > MPLS_LABEL_MAX ||
		    (*label <= MPLS_LABEL_RESERVED_MAX &&
		     *label != MPLS_LABEL_IPV4NULL &&
		     *label != MPLS_LABEL_IMPLNULL)) {
			session_shutdown(nbr, S_BAD_TLV_VAL, lm->msgid,
			    lm->type);
			return (-1);
		}
		break;
	case TLV_TYPE_ATMLABEL:
	case TLV_TYPE_FRLABEL:
	default:
		/* unsupported */
		session_shutdown(nbr, S_BAD_TLV_VAL, lm->msgid, lm->type);
		return (-1);
	}

	return (sizeof(lt));
}

void
gen_reqid_tlv(struct ibuf *buf, uint32_t reqid)
{
	struct reqid_tlv	rt;

	rt.type = htons(TLV_TYPE_LABELREQUEST);
	rt.length = htons(sizeof(reqid));
	rt.reqid = htonl(reqid);

	ibuf_add(buf, &rt, sizeof(rt));
}

void
gen_pw_status_tlv(struct ibuf *buf, uint32_t status)
{
	struct pw_status_tlv	st;

	st.type = htons(TLV_TYPE_PW_STATUS);
	st.length = htons(sizeof(status));
	st.value = htonl(status);

	ibuf_add(buf, &st, sizeof(st));
}

void
gen_fec_tlv(struct ibuf *buf, struct map *map)
{
	struct tlv	ft;
	uint16_t	family, len, pw_type, ifmtu;
	uint8_t		pw_len = 0;
	uint32_t	group_id, pwid;

	ft.type = htons(TLV_TYPE_FEC);

	switch (map->type) {
	case MAP_TYPE_WILDCARD:
		ft.length = htons(sizeof(uint8_t));
		ibuf_add(buf, &ft, sizeof(ft));
		ibuf_add(buf, &map->type, sizeof(map->type));
		break;
	case MAP_TYPE_PREFIX:
		len = PREFIX_SIZE(map->fec.ipv4.prefixlen);
		ft.length = htons(sizeof(map->type) + sizeof(family) +
		    sizeof(map->fec.ipv4.prefixlen) + len);
		ibuf_add(buf, &ft, sizeof(ft));

		ibuf_add(buf, &map->type, sizeof(map->type));
		family = htons(AF_IPV4);
		ibuf_add(buf, &family, sizeof(family));
		ibuf_add(buf, &map->fec.ipv4.prefixlen,
		    sizeof(map->fec.ipv4.prefixlen));
		if (len)
			ibuf_add(buf, &map->fec.ipv4.prefix, len);
		break;
	case MAP_TYPE_PWID:
		if (map->flags & F_MAP_PW_ID)
			pw_len += sizeof(uint32_t);
		if (map->flags & F_MAP_PW_IFMTU)
			pw_len += FEC_SUBTLV_IFMTU_LEN;

		len = FEC_PWID_ELM_MIN_LEN + pw_len;

		ft.length = htons(len);
		ibuf_add(buf, &ft, sizeof(ft));

		ibuf_add(buf, &map->type, sizeof(uint8_t));
		pw_type = map->fec.pwid.type;
		if (map->flags & F_MAP_PW_CWORD)
			pw_type |= CONTROL_WORD_FLAG;
		pw_type = htons(pw_type);
		ibuf_add(buf, &pw_type, sizeof(uint16_t));
		ibuf_add(buf, &pw_len, sizeof(uint8_t));
		group_id = htonl(map->fec.pwid.group_id);
		ibuf_add(buf, &group_id, sizeof(uint32_t));
		if (map->flags & F_MAP_PW_ID) {
			pwid = htonl(map->fec.pwid.pwid);
			ibuf_add(buf, &pwid, sizeof(uint32_t));
		}
		if (map->flags & F_MAP_PW_IFMTU) {
			struct subtlv 	stlv;

			stlv.type = SUBTLV_IFMTU;
			stlv.length = FEC_SUBTLV_IFMTU_LEN;
			ibuf_add(buf, &stlv, sizeof(uint16_t));

			ifmtu = htons(map->fec.pwid.ifmtu);
			ibuf_add(buf, &ifmtu, sizeof(uint16_t));
		}
		break;
	default:
		break;
	}
}

int
tlv_decode_fec_elm(struct nbr *nbr, struct ldp_msg *lm, char *buf,
    uint16_t len, struct map *map)
{
	uint16_t	family, off = 0;
	uint8_t		pw_len;

	map->type = *buf;
	off += sizeof(uint8_t);

	switch (map->type) {
	case MAP_TYPE_WILDCARD:
		if (len == FEC_ELM_WCARD_LEN)
			return (off);
		else {
			session_shutdown(nbr, S_BAD_TLV_VAL, lm->msgid,
			    lm->type);
			return (-1);
		}
		break;
	case MAP_TYPE_PREFIX:
		if (len < FEC_ELM_PREFIX_MIN_LEN) {
			session_shutdown(nbr, S_BAD_TLV_LEN, lm->msgid,
			    lm->type);
			return (-1);
		}

		/* Address Family */
		memcpy(&family, buf + off, sizeof(family));
		off += sizeof(family);

		if (family != htons(AF_IPV4)) {
			send_notification_nbr(nbr, S_UNSUP_ADDR, lm->msgid,
			    lm->type);
			return (-1);
		}

		/* PreLen */
		map->fec.ipv4.prefixlen = buf[off];
		off += sizeof(uint8_t);

		if (len < off + PREFIX_SIZE(map->fec.ipv4.prefixlen)) {
			session_shutdown(nbr, S_BAD_TLV_LEN, lm->msgid,
			    lm->type);
			return (-1);
		}

		/* Prefix */
		map->fec.ipv4.prefix.s_addr = 0;
		memcpy(&map->fec.ipv4.prefix, buf + off,
		    PREFIX_SIZE(map->fec.ipv4.prefixlen));

		return (off + PREFIX_SIZE(map->fec.ipv4.prefixlen));
	case MAP_TYPE_PWID:
		if (len < FEC_PWID_ELM_MIN_LEN) {
			session_shutdown(nbr, S_BAD_TLV_LEN, lm->msgid,
			    lm->type);
			return (-1);
		}

		/* PW type */
		memcpy(&map->fec.pwid.type, buf + off, sizeof(uint16_t));
		map->fec.pwid.type = ntohs(map->fec.pwid.type);
		if (map->fec.pwid.type & CONTROL_WORD_FLAG) {
			map->flags |= F_MAP_PW_CWORD;
			map->fec.pwid.type &= ~CONTROL_WORD_FLAG;
		}
		off += sizeof(uint16_t);

		/* PW info Length */
		pw_len = buf[off];
		off += sizeof(uint8_t);

		if (len != FEC_PWID_ELM_MIN_LEN + pw_len) {
			session_shutdown(nbr, S_BAD_TLV_LEN, lm->msgid,
			    lm->type);
			return (-1);
		}

		/* Group ID */
		memcpy(&map->fec.pwid.group_id, buf + off, sizeof(uint32_t));
		map->fec.pwid.group_id = ntohl(map->fec.pwid.group_id);
		off += sizeof(uint32_t);

		/* PW ID */
		if (pw_len == 0)
			return (off);

		if (pw_len < sizeof(uint32_t)) {
			session_shutdown(nbr, S_BAD_TLV_LEN, lm->msgid,
			    lm->type);
			return (-1);
		}

		memcpy(&map->fec.pwid.pwid, buf + off, sizeof(uint32_t));
		map->fec.pwid.pwid = ntohl(map->fec.pwid.pwid);
		map->flags |= F_MAP_PW_ID;
		off += sizeof(uint32_t);
		pw_len -= sizeof(uint32_t);

		/* Optional Interface Parameter Sub-TLVs */
		while (pw_len > 0) {
			struct subtlv 	stlv;

			if (pw_len < sizeof(stlv)) {
				session_shutdown(nbr, S_BAD_TLV_LEN,
				    lm->msgid, lm->type);
				return (-1);
			}

			memcpy(&stlv, buf + off, sizeof(stlv));
			off += SUBTLV_HDR_LEN;
			pw_len -= SUBTLV_HDR_LEN;

			switch (stlv.type) {
			case SUBTLV_IFMTU:
				if (stlv.length != FEC_SUBTLV_IFMTU_LEN) {
					session_shutdown(nbr, S_BAD_TLV_LEN,
					    lm->msgid, lm->type);
					return (-1);
				}
				memcpy(&map->fec.pwid.ifmtu, buf + off,
				    sizeof(uint16_t));
				map->fec.pwid.ifmtu = ntohs(map->fec.pwid.ifmtu);
				map->flags |= F_MAP_PW_IFMTU;
				break;
			default:
				/* ignore */
				break;
			}
			off += stlv.length - SUBTLV_HDR_LEN;
			pw_len -= stlv.length - SUBTLV_HDR_LEN;
		}

		return (off);
	default:
		send_notification_nbr(nbr, S_UNKNOWN_FEC, lm->msgid, lm->type);
		break;
	}

	return (-1);
}
