/*	$OpenBSD$ */

/*
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

#include <sys/queue.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdio.h>

#include "ldp.h"
#include "ldpd.h"
#include "ldpe.h"

void	print_mainconf(struct ldpd_conf *);
void	print_iface(struct iface *);
void	print_tnbr(struct tnbr *);
void	print_nbrp(struct nbr_params *);
void	print_l2vpn(struct l2vpn *);
void	print_pw(struct l2vpn_pw *);

void
print_mainconf(struct ldpd_conf *conf)
{
	printf("router-id %s\n\n", inet_ntoa(conf->rtr_id));

	if (conf->flags & F_LDPD_NO_FIB_UPDATE)
		printf("fib-update no\n");
	else
		printf("fib-update yes\n");

	if (conf->flags & F_LDPD_TH_ACCEPT)
		printf("targeted-hello-accept yes\n");
	else
		printf("targeted-hello-accept no\n");

	if (conf->flags & F_LDPD_EXPNULL)
		printf("explicit-null yes\n");
	else
		printf("explicit-null no\n");

	printf("keepalive %u\n", conf->keepalive);
	printf("transport-address %s\n", inet_ntoa(conf->trans_addr));
}

void
print_iface(struct iface *iface)
{
	printf("\ninterface %s {\n", iface->name);
	printf("\tlink-hello-holdtime %u\n", iface->hello_holdtime);
	printf("\tlink-hello-interval %u\n", iface->hello_interval);
	printf("}\n");
}

void
print_tnbr(struct tnbr *tnbr)
{
	printf("\ntargeted-neighbor %s {\n", inet_ntoa(tnbr->addr));
	printf("\ttargeted-hello-holdtime %u\n", tnbr->hello_holdtime);
	printf("\ttargeted-hello-interval %u\n", tnbr->hello_interval);
	printf("}\n");
}

void
print_nbrp(struct nbr_params *nbrp)
{
	printf("\nneighbor %s {\n", inet_ntoa(nbrp->lsr_id));
	if (nbrp->flags & F_NBRP_KEEPALIVE)
		printf("\tkeepalive %u\n", nbrp->keepalive);
	if (nbrp->auth.method == AUTH_MD5SIG)
		printf("\tpassword XXXXXX\n");
	printf("}\n");
}

void
print_l2vpn(struct l2vpn *l2vpn)
{
	struct l2vpn_if	*lif;
	struct l2vpn_pw	*pw;

	printf("l2vpn %s type vpls {\n", l2vpn->name);
	if (l2vpn->pw_type == PW_TYPE_ETHERNET)
		printf("\tpw-type ethernet\n");
	else
		printf("\tpw-type ethernet-tagged\n");
	printf("\tmtu %u\n", l2vpn->mtu);
	printf("\n");
	if (l2vpn->br_ifindex != 0)
		printf("\tbridge %s\n", l2vpn->br_ifname);
	LIST_FOREACH(lif, &l2vpn->if_list, entry)
		printf("\tinterface %s\n", lif->ifname);
	LIST_FOREACH(pw, &l2vpn->pw_list, entry)
		print_pw(pw);
	printf("}\n");
}

void
print_pw(struct l2vpn_pw *pw)
{
	printf("\tpseudowire %s {\n", pw->ifname);
	printf("\t\tneighbor %s\n", inet_ntoa(pw->lsr_id));
	printf("\t\tpw-id %u\n", pw->pwid);
	if (pw->flags & F_PW_STATUSTLV_CONF)
		printf("\t\tstatus-tlv yes\n");
	else
		printf("\t\tstatus-tlv no\n");
	if (pw->flags & F_PW_CWORD_CONF)
		printf("\t\tcontrol-word yes\n");
	else
		printf("\t\tcontrol-word no\n");
	printf("\t}\n");
}

void
print_config(struct ldpd_conf *conf)
{
	struct iface		*iface;
	struct tnbr		*tnbr;
	struct nbr_params	*nbrp;
	struct l2vpn		*l2vpn;

	print_mainconf(conf);
	printf("\n");

	LIST_FOREACH(iface, &conf->iface_list, entry)
		print_iface(iface);
	printf("\n");
	LIST_FOREACH(tnbr, &conf->tnbr_list, entry)
		if (tnbr->flags & F_TNBR_CONFIGURED)
			print_tnbr(tnbr);
	printf("\n");
	LIST_FOREACH(nbrp, &conf->nbrp_list, entry)
		print_nbrp(nbrp);
	printf("\n");
	LIST_FOREACH(l2vpn, &conf->l2vpn_list, entry)
		print_l2vpn(l2vpn);
}
