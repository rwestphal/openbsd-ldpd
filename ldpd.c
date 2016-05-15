/*	$OpenBSD$ */

/*
 * Copyright (c) 2005 Claudio Jeker <claudio@openbsd.org>
 * Copyright (c) 2004, 2008 Esben Norby <norby@openbsd.org>
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
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netmpls/mpls.h>

#include <event.h>
#include <err.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "ldpd.h"
#include "ldp.h"
#include "ldpe.h"
#include "control.h"
#include "log.h"
#include "lde.h"

void		main_sig_handler(int, short, void *);
__dead void	usage(void);
void		ldpd_shutdown(void);
int		check_child(pid_t, const char *);

void	main_dispatch_ldpe(int, short, void *);
void	main_dispatch_lde(int, short, void *);

int	main_imsg_compose_both(enum imsg_type, void *, uint16_t);
void	main_imsg_send_net_sockets(void);
void	main_imsg_send_net_socket(enum socket_type);
int	ldp_reload(void);
void	merge_global(struct ldpd_conf *, struct ldpd_conf *);
void	merge_ifaces(struct ldpd_conf *, struct ldpd_conf *);
void	merge_tnbrs(struct ldpd_conf *, struct ldpd_conf *);
void	merge_nbrps(struct ldpd_conf *, struct ldpd_conf *);
void	merge_l2vpns(struct ldpd_conf *, struct ldpd_conf *);
void	merge_l2vpn(struct ldpd_conf *, struct l2vpn *, struct l2vpn *);

int	pipe_parent2ldpe[2];
int	pipe_parent2lde[2];
int	pipe_ldpe2lde[2];

struct ldpd_conf	*ldpd_conf = NULL;
struct imsgev		*iev_ldpe;
struct imsgev		*iev_lde;
char			*conffile;

pid_t			 ldpe_pid = 0;
pid_t			 lde_pid = 0;

extern struct ldpd_conf	*leconf;

/* ARGSUSED */
void
main_sig_handler(int sig, short event, void *arg)
{
	/*
	 * signal handler rules don't apply, libevent decouples for us
	 */

	int	die = 0;

	switch (sig) {
	case SIGTERM:
	case SIGINT:
		die = 1;
		/* FALLTHROUGH */
	case SIGCHLD:
		if (check_child(ldpe_pid, "ldp engine")) {
			ldpe_pid = 0;
			die = 1;
		}
		if (check_child(lde_pid, "label decision engine")) {
			lde_pid = 0;
			die = 1;
		}
		if (die)
			ldpd_shutdown();
		break;
	case SIGHUP:
		if (ldp_reload() == -1)
			log_warnx("configuration reload failed");
		else
			log_debug("configuration reloaded");
		break;
	default:
		fatalx("unexpected signal");
		/* NOTREACHED */
	}
}

__dead void
usage(void)
{
	extern char *__progname;

	fprintf(stderr, "usage: %s [-dnv] [-D macro=value] [-f file]\n",
	    __progname);
	exit(1);
}

struct ldpd_global global;

int
main(int argc, char *argv[])
{
	struct event		 ev_sigint, ev_sigterm, ev_sigchld, ev_sighup;
	int			 ch;
	int			 debug = 0;

	conffile = CONF_FILE;
	ldpd_process = PROC_MAIN;

	log_init(1);	/* log to stderr until daemonized */
	log_verbose(1);

	while ((ch = getopt(argc, argv, "dD:f:nv")) != -1) {
		switch (ch) {
		case 'd':
			debug = 1;
			break;
		case 'D':
			if (cmdline_symset(optarg) < 0)
				log_warnx("could not parse macro definition %s",
				    optarg);
			break;
		case 'f':
			conffile = optarg;
			break;
		case 'n':
			global.cmd_opts |= LDPD_OPT_NOACTION;
			break;
		case 'v':
			if (global.cmd_opts & LDPD_OPT_VERBOSE)
				global.cmd_opts |= LDPD_OPT_VERBOSE2;
			global.cmd_opts |= LDPD_OPT_VERBOSE;
			break;
		default:
			usage();
			/* NOTREACHED */
		}
	}

	/* fetch interfaces early */
	kif_init();

	/* parse config file */
	if ((ldpd_conf = parse_config(conffile)) == NULL ) {
		kif_clear();
		exit(1);
	}

	if (global.cmd_opts & LDPD_OPT_NOACTION) {
		if (global.cmd_opts & LDPD_OPT_VERBOSE)
			print_config(ldpd_conf);
		else
			fprintf(stderr, "configuration OK\n");
		kif_clear();
		exit(0);
	}

	/* check for root privileges  */
	if (geteuid())
		errx(1, "need root privileges");

	/* check for ldpd user */
	if (getpwnam(LDPD_USER) == NULL)
		errx(1, "unknown user %s", LDPD_USER);

	log_init(debug);
	log_verbose(global.cmd_opts & (LDPD_OPT_VERBOSE | LDPD_OPT_VERBOSE2));

	if (!debug)
		daemon(1, 0);

	log_info("startup");

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
	    PF_UNSPEC, pipe_parent2ldpe) == -1)
		fatal("socketpair");
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
	    PF_UNSPEC, pipe_parent2lde) == -1)
		fatal("socketpair");
	if (socketpair(AF_UNIX, SOCK_STREAM |SOCK_NONBLOCK | SOCK_CLOEXEC,
	    PF_UNSPEC, pipe_ldpe2lde) == -1)
		fatal("socketpair");

	/* start children */
	lde_pid = lde(ldpd_conf, pipe_parent2lde, pipe_ldpe2lde,
	    pipe_parent2ldpe);
	ldpe_pid = ldpe(ldpd_conf, pipe_parent2ldpe, pipe_ldpe2lde,
	    pipe_parent2lde);

	/* show who we are */
	setproctitle("parent");

	event_init();

	/* setup signal handler */
	signal_set(&ev_sigint, SIGINT, main_sig_handler, NULL);
	signal_set(&ev_sigterm, SIGTERM, main_sig_handler, NULL);
	signal_set(&ev_sigchld, SIGCHLD, main_sig_handler, NULL);
	signal_set(&ev_sighup, SIGHUP, main_sig_handler, NULL);
	signal_add(&ev_sigint, NULL);
	signal_add(&ev_sigterm, NULL);
	signal_add(&ev_sigchld, NULL);
	signal_add(&ev_sighup, NULL);
	signal(SIGPIPE, SIG_IGN);

	/* setup pipes to children */
	close(pipe_parent2ldpe[1]);
	close(pipe_parent2lde[1]);
	close(pipe_ldpe2lde[0]);
	close(pipe_ldpe2lde[1]);

	if ((iev_ldpe = malloc(sizeof(struct imsgev))) == NULL ||
	    (iev_lde = malloc(sizeof(struct imsgev))) == NULL)
		fatal(NULL);
	imsg_init(&iev_ldpe->ibuf, pipe_parent2ldpe[0]);
	iev_ldpe->handler = main_dispatch_ldpe;
	imsg_init(&iev_lde->ibuf, pipe_parent2lde[0]);
	iev_lde->handler = main_dispatch_lde;

	/* setup event handler */
	iev_ldpe->events = EV_READ;
	event_set(&iev_ldpe->ev, iev_ldpe->ibuf.fd, iev_ldpe->events,
	    iev_ldpe->handler, iev_ldpe);
	event_add(&iev_ldpe->ev, NULL);

	iev_lde->events = EV_READ;
	event_set(&iev_lde->ev, iev_lde->ibuf.fd, iev_lde->events,
	    iev_lde->handler, iev_lde);
	event_add(&iev_lde->ev, NULL);

	/* notify ldpe about existing interfaces and addresses */
	kif_redistribute(NULL);

	if (kr_init(!(ldpd_conf->flags & F_LDPD_NO_FIB_UPDATE)) == -1)
		fatalx("kr_init failed");

	main_imsg_send_net_sockets();

	/* remove unneded stuff from config */
		/* ... */

	event_dispatch();

	ldpd_shutdown();
	/* NOTREACHED */
	return (0);
}

void
ldpd_shutdown(void)
{
	pid_t		 pid;

	if (ldpe_pid)
		kill(ldpe_pid, SIGTERM);

	if (lde_pid)
		kill(lde_pid, SIGTERM);

	kr_shutdown();

	do {
		if ((pid = wait(NULL)) == -1 &&
		    errno != EINTR && errno != ECHILD)
			fatal("wait");
	} while (pid != -1 || (pid == -1 && errno == EINTR));

	config_clear(ldpd_conf);

	msgbuf_clear(&iev_ldpe->ibuf.w);
	free(iev_ldpe);
	msgbuf_clear(&iev_lde->ibuf.w);
	free(iev_lde);

	log_info("terminating");
	exit(0);
}

int
check_child(pid_t pid, const char *pname)
{
	int	status;

	if (waitpid(pid, &status, WNOHANG) > 0) {
		if (WIFEXITED(status)) {
			log_warnx("lost child: %s exited", pname);
			return (1);
		}
		if (WIFSIGNALED(status)) {
			log_warnx("lost child: %s terminated; signal %d",
			    pname, WTERMSIG(status));
			return (1);
		}
	}

	return (0);
}

/* imsg handling */
/* ARGSUSED */
void
main_dispatch_ldpe(int fd, short event, void *bula)
{
	struct imsgev		*iev = bula;
	struct imsgbuf		*ibuf = &iev->ibuf;
	struct imsg		 imsg;
	ssize_t			 n;
	int			 shut = 0, verbose;

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read error");
		if (n == 0)	/* connection closed */
			shut = 1;
	}
	if (event & EV_WRITE) {
		if ((n = msgbuf_write(&ibuf->w)) == -1 && errno != EAGAIN)
			fatal("msgbuf_write");
		if (n == 0)
			shut = 1;
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("imsg_get");

		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_REQUEST_SOCKETS:
			main_imsg_send_net_sockets();
			break;
		case IMSG_CTL_RELOAD:
			if (ldp_reload() == -1)
				log_warnx("configuration reload failed");
			else
				log_debug("configuration reloaded");
			break;
		case IMSG_CTL_FIB_COUPLE:
			kr_fib_couple();
			break;
		case IMSG_CTL_FIB_DECOUPLE:
			kr_fib_decouple();
			break;
		case IMSG_CTL_KROUTE:
		case IMSG_CTL_KROUTE_ADDR:
			kr_show_route(&imsg);
			break;
		case IMSG_CTL_IFINFO:
			if (imsg.hdr.len == IMSG_HEADER_SIZE)
				kr_ifinfo(NULL, imsg.hdr.pid);
			else if (imsg.hdr.len == IMSG_HEADER_SIZE + IFNAMSIZ)
				kr_ifinfo(imsg.data, imsg.hdr.pid);
			else
				log_warnx("IFINFO request with wrong len");
			break;
		case IMSG_CTL_LOG_VERBOSE:
			/* already checked by ldpe */
			memcpy(&verbose, imsg.data, sizeof(verbose));
			log_verbose(verbose);
			break;
		default:
			log_debug("%s: error handling imsg %d", __func__,
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
main_dispatch_lde(int fd, short event, void *bula)
{
	struct imsgev  *iev = bula;
	struct imsgbuf *ibuf = &iev->ibuf;
	struct imsg	 imsg;
	ssize_t		 n;
	int		 shut = 0;
	struct kpw	*kpw;

	if (event & EV_READ) {
		if ((n = imsg_read(ibuf)) == -1 && errno != EAGAIN)
			fatal("imsg_read error");
		if (n == 0)	/* connection closed */
			shut = 1;
	}
	if (event & EV_WRITE) {
		if ((n = msgbuf_write(&ibuf->w)) == -1 && errno != EAGAIN)
			fatal("msgbuf_write");
		if (n == 0)
			shut = 1;
	}

	for (;;) {
		if ((n = imsg_get(ibuf, &imsg)) == -1)
			fatal("imsg_get");

		if (n == 0)
			break;

		switch (imsg.hdr.type) {
		case IMSG_KLABEL_CHANGE:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct kroute))
				fatalx("invalid size of IMSG_KLABEL_CHANGE");
			if (kr_change(imsg.data))
				log_warn("%s: error changing route", __func__);
			break;
		case IMSG_KLABEL_DELETE:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct kroute))
				fatalx("invalid size of IMSG_KLABEL_DELETE");
			if (kr_delete(imsg.data))
				log_warn("%s: error deleting route", __func__);
			break;
		case IMSG_KPWLABEL_CHANGE:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct kpw))
				fatalx("invalid size of IMSG_KPWLABEL_CHANGE");

			kpw = imsg.data;
			kmpw_set(kpw);
			break;
		case IMSG_KPWLABEL_DELETE:
			if (imsg.hdr.len - IMSG_HEADER_SIZE !=
			    sizeof(struct kpw))
				fatalx("invalid size of IMSG_KPWLABEL_DELETE");

			kpw = imsg.data;
			kmpw_unset(kpw);
			break;
		default:
			log_debug("%s: error handling imsg %d", __func__,
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

void
main_imsg_compose_ldpe(int type, pid_t pid, void *data, uint16_t datalen)
{
	if (iev_ldpe == NULL)
		return;
	imsg_compose_event(iev_ldpe, type, 0, pid, -1, data, datalen);
}

void
main_imsg_compose_lde(int type, pid_t pid, void *data, uint16_t datalen)
{
	imsg_compose_event(iev_lde, type, 0, pid, -1, data, datalen);
}

int
main_imsg_compose_both(enum imsg_type type, void *buf, uint16_t len)
{
	if (imsg_compose_event(iev_ldpe, type, 0, 0, -1, buf, len) == -1)
		return (-1);
	if (imsg_compose_event(iev_lde, type, 0, 0, -1, buf, len) == -1)
		return (-1);
	return (0);
}

void
imsg_event_add(struct imsgev *iev)
{
	iev->events = EV_READ;
	if (iev->ibuf.w.queued)
		iev->events |= EV_WRITE;

	event_del(&iev->ev);
	event_set(&iev->ev, iev->ibuf.fd, iev->events, iev->handler, iev);
	event_add(&iev->ev, NULL);
}

int
imsg_compose_event(struct imsgev *iev, uint16_t type, uint32_t peerid,
    pid_t pid, int fd, void *data, uint16_t datalen)
{
	int	ret;

	if ((ret = imsg_compose(&iev->ibuf, type, peerid,
	    pid, fd, data, datalen)) != -1)
		imsg_event_add(iev);
	return (ret);
}

void
evbuf_enqueue(struct evbuf *eb, struct ibuf *buf)
{
	ibuf_close(&eb->wbuf, buf);
	evbuf_event_add(eb);
}

void
evbuf_event_add(struct evbuf *eb)
{
	if (eb->wbuf.queued)
		event_add(&eb->ev, NULL);
}

void
evbuf_init(struct evbuf *eb, int fd, void (*handler)(int, short, void *),
    void *arg)
{
	msgbuf_init(&eb->wbuf);
	eb->wbuf.fd = fd;
	event_set(&eb->ev, eb->wbuf.fd, EV_WRITE, handler, arg);
}

void
evbuf_clear(struct evbuf *eb)
{
	event_del(&eb->ev);
	msgbuf_clear(&eb->wbuf);
	eb->wbuf.fd = -1;
}

void
main_imsg_send_net_sockets(void)
{
	main_imsg_send_net_socket(LDP_SOCKET_DISC);
	main_imsg_send_net_socket(LDP_SOCKET_EDISC);
	main_imsg_send_net_socket(LDP_SOCKET_SESSION);
	main_imsg_compose_ldpe(IMSG_SETUP_SOCKETS, 0, NULL, 0);
}

void
main_imsg_send_net_socket(enum socket_type type)
{
	int			 fd;

	fd = ldp_create_socket(type);
	if (fd == -1) {
		log_warnx("%s: failed to create %s socket", __func__,
		    socket_name(type));
		return;
	}

	imsg_compose_event(iev_ldpe, IMSG_SOCKET_NET, 0, 0, fd, &type,
	    sizeof(type));
}

int
ldp_reload(void)
{
	struct iface		*iface;
	struct tnbr		*tnbr;
	struct nbr_params	*nbrp;
	struct l2vpn		*l2vpn;
	struct l2vpn_if		*lif;
	struct l2vpn_pw		*pw;
	struct ldpd_conf	*xconf;

	if ((xconf = parse_config(conffile)) == NULL)
		return (-1);

	if (main_imsg_compose_both(IMSG_RECONF_CONF, xconf,
	    sizeof(*xconf)) == -1)
		return (-1);

	LIST_FOREACH(iface, &xconf->iface_list, entry) {
		if (main_imsg_compose_both(IMSG_RECONF_IFACE, iface,
		    sizeof(*iface)) == -1)
			return (-1);
	}

	LIST_FOREACH(tnbr, &xconf->tnbr_list, entry) {
		if (main_imsg_compose_both(IMSG_RECONF_TNBR, tnbr,
		    sizeof(*tnbr)) == -1)
			return (-1);
	}

	LIST_FOREACH(nbrp, &xconf->nbrp_list, entry) {
		if (main_imsg_compose_both(IMSG_RECONF_NBRP, nbrp,
		    sizeof(*nbrp)) == -1)
			return (-1);
	}

	LIST_FOREACH(l2vpn, &xconf->l2vpn_list, entry) {
		if (main_imsg_compose_both(IMSG_RECONF_L2VPN, l2vpn,
		    sizeof(*l2vpn)) == -1)
			return (-1);

		LIST_FOREACH(lif, &l2vpn->if_list, entry) {
			if (main_imsg_compose_both(IMSG_RECONF_L2VPN_IF, lif,
			    sizeof(*lif)) == -1)
				return (-1);
		}
		LIST_FOREACH(pw, &l2vpn->pw_list, entry) {
			if (main_imsg_compose_both(IMSG_RECONF_L2VPN_PW, pw,
			    sizeof(*pw)) == -1)
				return (-1);
		}
	}

	if (main_imsg_compose_both(IMSG_RECONF_END, NULL, 0) == -1)
		return (-1);

	merge_config(ldpd_conf, xconf);

	return (0);
}

void
merge_config(struct ldpd_conf *conf, struct ldpd_conf *xconf)
{
	merge_global(conf, xconf);
	merge_ifaces(conf, xconf);
	merge_tnbrs(conf, xconf);
	merge_nbrps(conf, xconf);
	merge_l2vpns(conf, xconf);
	free(xconf);
}

void
merge_global(struct ldpd_conf *conf, struct ldpd_conf *xconf)
{
	struct nbr		*nbr;
	struct nbr_params	*nbrp;
	int			 egress_label_changed = 0;

	/* change of rtr_id needs a restart */
	if (conf->keepalive != xconf->keepalive) {
		conf->keepalive = xconf->keepalive;
		if (ldpd_process == PROC_LDP_ENGINE)
			ldpe_stop_init_backoff();
	}

	conf->thello_holdtime = xconf->thello_holdtime;
	conf->thello_interval = xconf->thello_interval;

	/* update flags */
	if (ldpd_process == PROC_LDP_ENGINE &&
	    (conf->flags & F_LDPD_TH_ACCEPT) &&
	    !(xconf->flags & F_LDPD_TH_ACCEPT))
		ldpe_remove_dynamic_tnbrs();

	if ((conf->flags & F_LDPD_EXPNULL) !=
	    (xconf->flags & F_LDPD_EXPNULL))
		egress_label_changed = 1;

	conf->flags = xconf->flags;

	if (egress_label_changed) {
		switch (ldpd_process) {
		case PROC_LDE_ENGINE:
			lde_change_egress_label(conf->flags & F_LDPD_EXPNULL);
			break;
		case PROC_MAIN:
			kr_change_egress_label(conf->flags & F_LDPD_EXPNULL);
			break;
		default:
			break;
		}
	}

	if (conf->trans_addr.s_addr != xconf->trans_addr.s_addr) {
		conf->trans_addr = xconf->trans_addr;
		if (ldpd_process == PROC_MAIN)
			imsg_compose_event(iev_ldpe, IMSG_CLOSE_SOCKETS, 0,
			    0, -1, NULL, 0);
		if (ldpd_process == PROC_LDP_ENGINE) {
			RB_FOREACH(nbr, nbr_id_head, &nbrs_by_id) {
				session_shutdown(nbr, S_SHUTDOWN, 0, 0);

				pfkey_remove(nbr);
				nbr->laddr = conf->trans_addr;
				nbrp = nbr_params_find(leconf, nbr->id);
				if (nbrp && pfkey_establish(nbr, nbrp) == -1)
					fatalx("pfkey setup failed");
			}
		}
	}
}

void
merge_ifaces(struct ldpd_conf *conf, struct ldpd_conf *xconf)
{
	struct iface		*iface, *itmp, *xi;

	LIST_FOREACH_SAFE(iface, &conf->iface_list, entry, itmp) {
		/* find deleted interfaces */
		if ((xi = if_lookup(xconf, iface->ifindex)) == NULL) {
			LIST_REMOVE(iface, entry);
			if (ldpd_process == PROC_LDP_ENGINE)
				if_del(iface);
			else
				free(iface);
		}
	}
	LIST_FOREACH_SAFE(xi, &xconf->iface_list, entry, itmp) {
		/* find new interfaces */
		if ((iface = if_lookup(conf, xi->ifindex)) == NULL) {
			LIST_REMOVE(xi, entry);
			LIST_INSERT_HEAD(&conf->iface_list, xi, entry);

			/* resend addresses to activate new interfaces */
			if (ldpd_process == PROC_MAIN)
				kif_redistribute(xi->name);
			continue;
		}

		/* update existing interfaces */
		iface->hello_holdtime = xi->hello_holdtime;
		iface->hello_interval = xi->hello_interval;
		LIST_REMOVE(xi, entry);
		free(xi);
	}
}

void
merge_tnbrs(struct ldpd_conf *conf, struct ldpd_conf *xconf)
{
	struct tnbr		*tnbr, *ttmp, *xt;

	LIST_FOREACH_SAFE(tnbr, &conf->tnbr_list, entry, ttmp) {
		if (!(tnbr->flags & F_TNBR_CONFIGURED))
			continue;

		/* find deleted tnbrs */
		if ((xt = tnbr_find(xconf, tnbr->addr)) == NULL) {
			if (ldpd_process == PROC_LDP_ENGINE) {
				tnbr->flags &= ~F_TNBR_CONFIGURED;
				tnbr_check(tnbr);
			} else {
				LIST_REMOVE(tnbr, entry);
				free(tnbr);
			}
		}
	}
	LIST_FOREACH_SAFE(xt, &xconf->tnbr_list, entry, ttmp) {
		/* find new tnbrs */
		if ((tnbr = tnbr_find(conf, xt->addr)) == NULL) {
			LIST_REMOVE(xt, entry);
			LIST_INSERT_HEAD(&conf->tnbr_list, xt, entry);

			if (ldpd_process == PROC_LDP_ENGINE)
				tnbr_update(xt);
			continue;
		}

		/* update existing tnbrs */
		if (!(tnbr->flags & F_TNBR_CONFIGURED))
			tnbr->flags |= F_TNBR_CONFIGURED;
		tnbr->hello_holdtime = xt->hello_holdtime;
		tnbr->hello_interval = xt->hello_interval;
		LIST_REMOVE(xt, entry);
		free(xt);
	}
}

void
merge_nbrps(struct ldpd_conf *conf, struct ldpd_conf *xconf)
{
	struct nbr_params	*nbrp, *ntmp, *xn;
	struct nbr		*nbr;
	int			 nbrp_changed;

	LIST_FOREACH_SAFE(nbrp, &conf->nbrp_list, entry, ntmp) {
		/* find deleted nbrps */
		if ((xn = nbr_params_find(xconf, nbrp->lsr_id)) == NULL) {
			if (ldpd_process == PROC_LDP_ENGINE) {
				nbr = nbr_find_ldpid(nbrp->lsr_id.s_addr);
				if (nbr) {
					session_shutdown(nbr, S_SHUTDOWN, 0, 0);
					pfkey_remove(nbr);
				}
			}
			LIST_REMOVE(nbrp, entry);
			free(nbrp);
		}
	}
	LIST_FOREACH_SAFE(xn, &xconf->nbrp_list, entry, ntmp) {
		/* find new nbrps */
		if ((nbrp = nbr_params_find(conf, xn->lsr_id)) == NULL) {
			LIST_REMOVE(xn, entry);
			LIST_INSERT_HEAD(&conf->nbrp_list, xn, entry);

			if (ldpd_process == PROC_LDP_ENGINE) {
				nbr = nbr_find_ldpid(xn->lsr_id.s_addr);
				if (nbr) {
					session_shutdown(nbr, S_SHUTDOWN, 0, 0);
					if (pfkey_establish(nbr, xn) == -1)
						fatalx("pfkey setup failed");
				}
			}
			continue;
		}

		/* update existing nbrps */
		if (nbrp->keepalive != xn->keepalive ||
		    nbrp->auth.method != xn->auth.method ||
		    strcmp(nbrp->auth.md5key, xn->auth.md5key) != 0)
			nbrp_changed = 1;
		else
			nbrp_changed = 0;

		nbrp->keepalive = xn->keepalive;
		nbrp->auth.method = xn->auth.method;
		strlcpy(nbrp->auth.md5key, xn->auth.md5key,
		    sizeof(nbrp->auth.md5key));
		nbrp->auth.md5key_len = xn->auth.md5key_len;
		nbrp->flags = xn->flags;

		if (ldpd_process == PROC_LDP_ENGINE) {
			nbr = nbr_find_ldpid(nbrp->lsr_id.s_addr);
			if (nbr && nbrp_changed) {
				session_shutdown(nbr, S_SHUTDOWN, 0, 0);
				pfkey_remove(nbr);
				if (pfkey_establish(nbr, nbrp) == -1)
					fatalx("pfkey setup failed");
			}
		}
		LIST_REMOVE(xn, entry);
		free(xn);
	}
}

void
merge_l2vpns(struct ldpd_conf *conf, struct ldpd_conf *xconf)
{
	struct l2vpn		*l2vpn, *ltmp, *xl;

	LIST_FOREACH_SAFE(l2vpn, &conf->l2vpn_list, entry, ltmp) {
		/* find deleted l2vpns */
		if ((xl = l2vpn_find(xconf, l2vpn->name)) == NULL) {
			LIST_REMOVE(l2vpn, entry);

			switch (ldpd_process) {
			case PROC_LDE_ENGINE:
				l2vpn_del(l2vpn);
				break;
			case PROC_LDP_ENGINE:
				ldpe_l2vpn_exit(l2vpn);
				free(l2vpn);
				break;
			case PROC_MAIN:
				free(l2vpn);
				break;
			}
		}
	}
	LIST_FOREACH_SAFE(xl, &xconf->l2vpn_list, entry, ltmp) {
		/* find new l2vpns */
		if ((l2vpn = l2vpn_find(conf, xl->name)) == NULL) {
			LIST_REMOVE(xl, entry);
			LIST_INSERT_HEAD(&conf->l2vpn_list, xl, entry);

			switch (ldpd_process) {
			case PROC_LDE_ENGINE:
				l2vpn_init(xl);
				break;
			case PROC_LDP_ENGINE:
				ldpe_l2vpn_init(xl);
				break;
			case PROC_MAIN:
				break;
			}
			continue;
		}

		/* update existing l2vpns */
		merge_l2vpn(conf, l2vpn, xl);
		LIST_REMOVE(xl, entry);
		free(xl);
	}
}

void
merge_l2vpn(struct ldpd_conf *xconf, struct l2vpn *l2vpn, struct l2vpn *xl)
{
	struct l2vpn_if		*lif, *ftmp, *xf;
	struct l2vpn_pw		*pw, *ptmp, *xp;
	struct nbr		*nbr;
	int			 reset_nbr, reinstall_pwfec, reinstall_tnbr;
	int			 previous_pw_type, previous_mtu;

	previous_pw_type = l2vpn->pw_type;
	previous_mtu = l2vpn->mtu;

	/* merge intefaces */
	LIST_FOREACH_SAFE(lif, &l2vpn->if_list, entry, ftmp) {
		/* find deleted interfaces */
		if ((xf = l2vpn_if_find(xl, lif->ifindex)) == NULL) {
			LIST_REMOVE(lif, entry);
			free(lif);
		}
	}
	LIST_FOREACH_SAFE(xf, &xl->if_list, entry, ftmp) {
		/* find new interfaces */
		if ((lif = l2vpn_if_find(l2vpn, xf->ifindex)) == NULL) {
			LIST_REMOVE(xf, entry);
			LIST_INSERT_HEAD(&l2vpn->if_list, xf, entry);
			xf->l2vpn = l2vpn;
			continue;
		}

		LIST_REMOVE(xf, entry);
		free(xf);
	}

	/* merge pseudowires */
	LIST_FOREACH_SAFE(pw, &l2vpn->pw_list, entry, ptmp) {
		/* find deleted pseudowires */
		if ((xp = l2vpn_pw_find(xl, pw->ifindex)) == NULL) {
			switch (ldpd_process) {
			case PROC_LDE_ENGINE:
				l2vpn_pw_exit(pw);
				break;
			case PROC_LDP_ENGINE:
				ldpe_l2vpn_pw_exit(pw);
				break;
			case PROC_MAIN:
				break;
			}

			LIST_REMOVE(pw, entry);
			free(pw);
		}
	}
	LIST_FOREACH_SAFE(xp, &xl->pw_list, entry, ptmp) {
		/* find new pseudowires */
		if ((pw = l2vpn_pw_find(l2vpn, xp->ifindex)) == NULL) {
			LIST_REMOVE(xp, entry);
			LIST_INSERT_HEAD(&l2vpn->pw_list, xp, entry);
			xp->l2vpn = l2vpn;

			switch (ldpd_process) {
			case PROC_LDE_ENGINE:
				l2vpn_pw_init(xp);
				break;
			case PROC_LDP_ENGINE:
				ldpe_l2vpn_pw_init(xp);
				break;
			case PROC_MAIN:
				break;
			}
			continue;
		}

		/* update existing pseudowire */
    		if (pw->lsr_id.s_addr != xp->lsr_id.s_addr)
			reinstall_tnbr = 1;
		else
			reinstall_tnbr = 0;

		/* changes that require a session restart */
		if ((pw->flags & (F_PW_STATUSTLV_CONF|F_PW_CWORD_CONF)) !=
		    (xp->flags & (F_PW_STATUSTLV_CONF|F_PW_CWORD_CONF)))
			reset_nbr = 1;
		else
			reset_nbr = 0;

		if (l2vpn->pw_type != xl->pw_type || l2vpn->mtu != xl->mtu ||
		    pw->pwid != xp->pwid || reinstall_tnbr || reset_nbr ||
		    pw->lsr_id.s_addr != xp->lsr_id.s_addr)
			reinstall_pwfec = 1;
		else
			reinstall_pwfec = 0;

		if (ldpd_process == PROC_LDP_ENGINE) {
			if (reinstall_tnbr)
				ldpe_l2vpn_pw_exit(pw);
			if (reset_nbr) {
				nbr = nbr_find_ldpid(pw->lsr_id.s_addr);
				if (nbr && nbr->state == NBR_STA_OPER)
					session_shutdown(nbr, S_SHUTDOWN, 0, 0);
			}
		}
		if (ldpd_process == PROC_LDE_ENGINE &&
		    !reset_nbr && reinstall_pwfec)
			l2vpn_pw_exit(pw);
		pw->lsr_id = xp->lsr_id;
		pw->pwid = xp->pwid;
		strlcpy(pw->ifname, xp->ifname, sizeof(pw->ifname));
		pw->ifindex = xp->ifindex;
		if (xp->flags & F_PW_CWORD_CONF)
			pw->flags |= F_PW_CWORD_CONF;
		else
			pw->flags &= ~F_PW_CWORD_CONF;
		if (xp->flags & F_PW_STATUSTLV_CONF)
			pw->flags |= F_PW_STATUSTLV_CONF;
		else
			pw->flags &= ~F_PW_STATUSTLV_CONF;
		if (ldpd_process == PROC_LDP_ENGINE && reinstall_tnbr)
			ldpe_l2vpn_pw_init(pw);
		if (ldpd_process == PROC_LDE_ENGINE &&
		    !reset_nbr && reinstall_pwfec) {
			l2vpn->pw_type = xl->pw_type;
			l2vpn->mtu = xl->mtu;
			l2vpn_pw_init(pw);
			l2vpn->pw_type = previous_pw_type;
			l2vpn->mtu = previous_mtu;
		}

		LIST_REMOVE(xp, entry);
		free(xp);
	}

	l2vpn->pw_type = xl->pw_type;
	l2vpn->mtu = xl->mtu;
	strlcpy(l2vpn->br_ifname, xl->br_ifname, sizeof(l2vpn->br_ifname));
	l2vpn->br_ifindex = xl->br_ifindex;
}

void
config_clear(struct ldpd_conf *conf)
{
	struct ldpd_conf	*xconf;

	/* merge current config with an empty config */
	xconf = malloc(sizeof(*xconf));
	*xconf = *conf;
	LIST_INIT(&xconf->iface_list);
	LIST_INIT(&xconf->tnbr_list);
	LIST_INIT(&xconf->nbrp_list);
	LIST_INIT(&xconf->l2vpn_list);
	merge_config(conf, xconf);

	free(conf);
}
