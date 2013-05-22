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

#ifndef _LDPD_H_
#define _LDPD_H_

#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/tree.h>
#include <md5.h>
#include <net/if.h>
#include <netinet/in.h>
#include <event.h>

#include <imsg.h>
#include "ldp.h"

#define CONF_FILE		"/etc/ldpd.conf"
#define	LDPD_SOCKET		"/var/run/ldpd.sock"
#define LDPD_USER		"_ldpd"

#define NBR_IDSELF		1
#define NBR_CNTSTART		(NBR_IDSELF + 1)

#define	RT_BUF_SIZE		16384
#define	MAX_RTSOCK_BUF		128 * 1024
#define	LDP_BACKLOG		128

#define	LDPD_FLAG_NO_FIB_UPDATE	0x0001

#define	F_LDPD_INSERTED		0x0001
#define	F_CONNECTED		0x0002
#define	F_STATIC		0x0004
#define	F_DYNAMIC		0x0008
#define	F_REJECT		0x0010
#define	F_BLACKHOLE		0x0020
#define	F_REDISTRIBUTED		0x0040

struct evbuf {
	struct msgbuf		wbuf;
	struct event		ev;
};

struct imsgev {
	struct imsgbuf		 ibuf;
	void			(*handler)(int, short, void *);
	struct event		 ev;
	void			*data;
	short			 events;
};

enum imsg_type {
	IMSG_NONE,
	IMSG_CTL_RELOAD,
	IMSG_CTL_SHOW_INTERFACE,
	IMSG_CTL_SHOW_NBR,
	IMSG_CTL_SHOW_LIB,
	IMSG_CTL_FIB_COUPLE,
	IMSG_CTL_FIB_DECOUPLE,
	IMSG_CTL_KROUTE,
	IMSG_CTL_KROUTE_ADDR,
	IMSG_CTL_IFINFO,
	IMSG_CTL_END,
	IMSG_CTL_LOG_VERBOSE,
	IMSG_KLABEL_CHANGE,
	IMSG_KLABEL_DELETE,
	IMSG_IFINFO,
	IMSG_LABEL_MAPPING,
	IMSG_LABEL_MAPPING_FULL,
	IMSG_LABEL_REQUEST,
	IMSG_LABEL_RELEASE,
	IMSG_LABEL_WITHDRAW,
	IMSG_LABEL_ABORT,
	IMSG_REQUEST_ADD,
	IMSG_REQUEST_ADD_END,
	IMSG_MAPPING_ADD,
	IMSG_MAPPING_ADD_END,
	IMSG_RELEASE_ADD,
	IMSG_RELEASE_ADD_END,
	IMSG_ADDRESS_ADD,
	IMSG_ADDRESS_DEL,
	IMSG_NOTIFICATION_SEND,
	IMSG_NEIGHBOR_UP,
	IMSG_NEIGHBOR_DOWN,
	IMSG_NEIGHBOR_CHANGE,
	IMSG_NETWORK_ADD,
	IMSG_NETWORK_DEL,
	IMSG_RECONF_CONF,
	IMSG_RECONF_IFACE,
	IMSG_RECONF_END
};

/* interface states */
#define	IF_STA_NEW		0x00	/* dummy state for reload */
#define	IF_STA_DOWN		0x01
#define	IF_STA_ACTIVE		0x02
#define	IF_STA_ANY		(IF_STA_DOWN | IF_STA_ACTIVE)

/* interface events */
enum iface_event {
	IF_EVT_NOTHING,
	IF_EVT_UP,
	IF_EVT_DOWN
};

/* interface actions */
enum iface_action {
	IF_ACT_NOTHING,
	IF_ACT_STRT,
	IF_ACT_RST
};

/* interface types */
enum iface_type {
	IF_TYPE_POINTOPOINT,
	IF_TYPE_BROADCAST
};

/* neighbor states */
#define	NBR_STA_DOWN		0x0001
#define	NBR_STA_PRESENT		0x0002
#define	NBR_STA_INITIAL		0x0004
#define	NBR_STA_OPENREC		0x0008
#define	NBR_STA_OPENSENT	0x0010
#define	NBR_STA_OPER		0x0020
#define	NBR_STA_SESSION		(NBR_STA_PRESENT | NBR_STA_INITIAL | \
				NBR_STA_OPENREC | NBR_STA_OPENSENT | \
				NBR_STA_OPER)

/* neighbor events */
enum nbr_event {
	NBR_EVT_NOTHING,
	NBR_EVT_HELLO_RCVD,
	NBR_EVT_CONNECT_UP,
	NBR_EVT_CLOSE_SESSION,
	NBR_EVT_INIT_RCVD,
	NBR_EVT_KEEPALIVE_RCVD,
	NBR_EVT_PDU_RCVD,
	NBR_EVT_INIT_SENT,
	NBR_EVT_DOWN
};

/* neighbor actions */
enum nbr_action {
	NBR_ACT_NOTHING,
	NBR_ACT_STRT_ITIMER,
	NBR_ACT_RST_ITIMER,
	NBR_ACT_RST_KTIMEOUT,
	NBR_ACT_SESSION_EST,
	NBR_ACT_RST_KTIMER,
	NBR_ACT_CONNECT_SETUP,
	NBR_ACT_PASSIVE_INIT,
	NBR_ACT_KEEPALIVE_SEND,
	NBR_ACT_CLOSE_SESSION
};

TAILQ_HEAD(mapping_head, mapping_entry);

struct map {
	struct in_addr	prefix;
	u_int32_t	label;
	u_int32_t	messageid;
	u_int32_t	requestid;
	u_int8_t	prefixlen;
	u_int8_t	flags;
};
#define F_MAP_WILDCARD	0x01	/* wildcard FEC */
#define F_MAP_OPTLABEL	0x02	/* optional label present */
#define F_MAP_REQ_ID	0x04	/* optional request message id present */

struct notify_msg {
	u_int32_t	messageid;
	u_int32_t	status;
	u_int32_t	type;
};

struct iface {
	LIST_ENTRY(iface)	 entry;
	struct event		 hello_timer;

	LIST_HEAD(, lde_nbr)	 lde_nbr_list;

	char			 name[IF_NAMESIZE];
	struct in_addr		 addr;
	struct in_addr		 dst;
	struct in_addr		 mask;

	u_int16_t		 lspace_id;

	u_int64_t		 baudrate;
	time_t			 uptime;
	unsigned int		 ifindex;
	int			 discovery_fd;
	int			 session_fd;
	int			 state;
	int			 mtu;
	u_int16_t		 holdtime;
	u_int16_t		 keepalive;
	u_int16_t		 hello_interval;
	u_int16_t		 flags;
	enum iface_type		 type;
	u_int8_t		 media_type;
	u_int8_t		 linkstate;
	u_int8_t		 priority;
	u_int8_t		 passive;
};

/* ldp_conf */
enum {
	PROC_MAIN,
	PROC_LDP_ENGINE,
	PROC_LDE_ENGINE
} ldpd_process;

enum blockmodes {
	BM_NORMAL,
	BM_NONBLOCK
};

#define	MODE_DIST_INDEPENDENT	0x01
#define	MODE_DIST_ORDERED	0x02
#define	MODE_RET_LIBERAL	0x04
#define	MODE_RET_CONSERVATIVE	0x08
#define	MODE_ADV_ONDEMAND	0x10
#define	MODE_ADV_UNSOLICITED	0x20

struct ldpd_conf {
	struct event		disc_ev;
	struct in_addr		rtr_id;
	LIST_HEAD(, iface)	iface_list;

	u_int32_t		opts;
#define LDPD_OPT_VERBOSE	0x00000001
#define LDPD_OPT_VERBOSE2	0x00000002
#define LDPD_OPT_NOACTION	0x00000004
	time_t			uptime;
	int			ldp_discovery_socket;
	int			ldp_session_socket;
	int			flags;
	u_int8_t		mode;
};

/* kroute */
struct kroute {
	struct in_addr	prefix;
	struct in_addr	nexthop;
	u_int32_t	local_label;
	u_int32_t	remote_label;
	u_int16_t	flags;
	u_short		ifindex;
	u_int8_t	prefixlen;
	u_int8_t	priority;
};

struct kif_addr {
	TAILQ_ENTRY(kif_addr)	 entry;
	struct in_addr		 addr;
	struct in_addr		 mask;
	struct in_addr		 dstbrd;
};

struct kif {
	char			 ifname[IF_NAMESIZE];
	u_int64_t		 baudrate;
	int			 flags;
	int			 mtu;
	u_short			 ifindex;
	u_int8_t		 media_type;
	u_int8_t		 link_state;
};

/* control data structures */
struct ctl_iface {
	char			 name[IF_NAMESIZE];
	struct in_addr		 addr;
	struct in_addr		 mask;
	struct in_addr		 lspace;
	struct in_addr		 rtr_id;
	struct in_addr		 dr_id;
	struct in_addr		 dr_addr;
	struct in_addr		 bdr_id;
	struct in_addr		 bdr_addr;
	time_t			 hello_timer;
	time_t			 uptime;
	u_int64_t		 baudrate;
	unsigned int		 ifindex;
	int			 state;
	int			 mtu;
	int			 nbr_cnt;
	int			 adj_cnt;
	u_int16_t		 flags;
	u_int16_t		 holdtime;
	u_int16_t		 hello_interval;
	enum iface_type		 type;
	u_int8_t		 linkstate;
	u_int8_t		 mediatype;
	u_int8_t		 priority;
	u_int8_t		 passive;
};

struct ctl_nbr {
	char			 name[IF_NAMESIZE];
	struct in_addr		 id;
	struct in_addr		 addr;
	struct in_addr		 dr;
	struct in_addr		 bdr;
	struct in_addr		 lspace;
	time_t			 dead_timer;
	time_t			 uptime;
	u_int32_t		 db_sum_lst_cnt;
	u_int32_t		 ls_req_lst_cnt;
	u_int32_t		 ls_retrans_lst_cnt;
	u_int32_t		 state_chng_cnt;
	int			 nbr_state;
	int			 iface_state;
	u_int8_t		 priority;
	u_int8_t		 options;
};

struct ctl_rt {
	struct in_addr		 prefix;
	struct in_addr		 nexthop;
	struct in_addr		 lspace;
	struct in_addr		 adv_rtr;
	time_t			 uptime;
	u_int32_t		 local_label;
	u_int32_t		 remote_label;
	u_int8_t		 flags;
	u_int8_t		 prefixlen;
	u_int8_t		 connected;
	u_int8_t		 in_use;
};

/* parse.y */
struct ldpd_conf	*parse_config(char *, int);
int			 cmdline_symset(char *);

/* control.c */
void	session_socket_blockmode(int, enum blockmodes);

/* kroute.c */
int		 kif_init(void);
int		 kr_init(int);
int		 kr_change(struct kroute *);
int		 kr_delete(struct kroute *);
void		 kr_shutdown(void);
void		 kr_fib_couple(void);
void		 kr_fib_decouple(void);
void		 kr_dispatch_msg(int, short, void *);
void		 kr_show_route(struct imsg *);
void		 kr_ifinfo(char *, pid_t);
struct kif	*kif_findname(char *, struct in_addr, struct kif_addr **);
void		 kr_reload(void);

u_int8_t	mask2prefixlen(in_addr_t);
in_addr_t	prefixlen2mask(u_int8_t);

/* log.h */
const char	*nbr_state_name(int);
const char	*if_state_name(int);
const char	*if_type_name(enum iface_type);
const char	*notification_name(u_int32_t);

/* ldpd.c */
void	main_imsg_compose_ldpe(int, pid_t, void *, u_int16_t);
void	main_imsg_compose_lde(int, pid_t, void *, u_int16_t);
void	merge_config(struct ldpd_conf *, struct ldpd_conf *);
int	imsg_compose_event(struct imsgev *, u_int16_t, u_int32_t, pid_t,
	    int, void *, u_int16_t);
void	imsg_event_add(struct imsgev *);
void	evbuf_enqueue(struct evbuf *, struct ibuf *);
void	evbuf_event_add(struct evbuf *);
void	evbuf_init(struct evbuf *, int, void (*)(int, short, void *), void *);
void	evbuf_clear(struct evbuf *);

/* printconf.c */
void	print_config(struct ldpd_conf *);

#endif	/* _LDPD_H_ */
