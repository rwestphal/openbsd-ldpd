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

int	gen_status_tlv(struct buf *, u_int32_t, u_int32_t, u_int32_t);

int
send_notification_nbr(struct nbr *nbr, u_int32_t status, u_int32_t msgid,
    u_int32_t type)
{
	if (nbr->iface->passive)
		return (0);

	log_debug("send_notification: neighbor ID %s", inet_ntoa(nbr->id));

	return (send_notification(status, nbr->iface, nbr->fd, msgid, type));
}

int
send_notification(u_int32_t status, struct iface *iface, int fd,
    u_int32_t msgid, u_int32_t type)
{
	struct buf	*buf;
	u_int16_t	 size;

	if ((buf = buf_open(LDP_MAX_LEN)) == NULL)
		fatal("send_notification");

	size = LDP_HDR_SIZE + sizeof(struct ldp_msg) + STATUS_SIZE;

	gen_ldp_hdr(buf, iface, size);

	size -= LDP_HDR_SIZE;

	gen_msg_tlv(buf, MSG_TYPE_NOTIFICATION, size);

	size -= sizeof(struct ldp_msg);

	gen_status_tlv(buf, status, msgid, type);

	write(fd, buf->buf, buf->wpos);
	buf_free(buf);

	return (0);
}

int
recv_notification(struct nbr *nbr, char *buf, u_int16_t len)
{
	struct ldp_msg		*not;
	struct status_tlv	*st;

	log_debug("recv_notification: neighbor ID %s", inet_ntoa(nbr->id));

	not = (struct ldp_msg *)buf;

	if ((len - TLV_HDR_LEN) < ntohs(not->length)) {
		session_shutdown(nbr, S_BAD_MSG_LEN, not->msgid, not->type);
		return (-1);
	}

	buf += sizeof(struct ldp_msg);
	len -= sizeof(struct ldp_msg);

	st = (struct status_tlv *)buf;

	if (len < STATUS_SIZE ||
	    (STATUS_SIZE - TLV_HDR_LEN) != ntohs(st->length)) {
		session_shutdown(nbr, S_BAD_TLV_LEN, not->msgid, not->type);
		return (-1);
	}

	if (st->status_code & htonl(STATUS_FATAL)) {
		nbr_fsm(nbr, NBR_EVT_CLOSE_SESSION);
		return (-1);
	}

	return (ntohs(not->length));
}

int
gen_status_tlv(struct buf *buf, u_int32_t status, u_int32_t msgid,
    u_int32_t type)
{
	struct status_tlv	st;

	bzero(&st, sizeof(st));

	st.type = htons(TLV_TYPE_STATUS);
	st.length = htons(STATUS_TLV_LEN);
	st.status_code = htonl(status);

	st.msg_id = msgid;
	st.msg_type = type;

	return (buf_add(buf, &st, STATUS_SIZE));
}
