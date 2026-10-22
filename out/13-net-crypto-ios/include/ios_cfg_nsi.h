// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part I5: per-file configuration for the NSI providers (nsi/ndis.c, upstream
// nsiproxy.sys/ip.c, nsi/nsi_ip6scope.c, nsi/nsi_tcp.c, nsi/nsi_unix.c), force-included after
// the wine build dir's config.h and spec 09's no-exit header.
//
// The iPhoneOS SDK lacks <net/route.h>, <net/if_arp.h>, <netinet/if_ether.h>,
// <netinet/ip_var.h> and <netinet/icmp_var.h>, which the macOS config.h claims. <net/route.h>
// is provided by include/net/route.h (Darwin's routing-message ABI, which the kernel still
// answers through sysctl); the others are turned off.

#ifndef MYIOSDECK_IOS_CFG_NSI_H
#define MYIOSDECK_IOS_CFG_NSI_H

#undef HAVE_NET_IF_ARP_H
#undef HAVE_NETINET_IF_ETHER_H
#undef HAVE_NETINET_IP_VAR_H
#undef HAVE_NETINET_ICMP_VAR_H

#undef  HAVE_NET_ROUTE_H
#define HAVE_NET_ROUTE_H 1

#include "ios_nsi_rename.h"

#ifdef IOS_NSI_IP_C
/* ip.c only: IP statistics it can't read stay unimplemented, as upstream on such systems */
#undef HAVE_STRUCT_IPSTAT_IPS_TOTAL
#undef HAVE_STRUCT_IP_STATS_IPS_TOTAL
#endif

#endif /* MYIOSDECK_IOS_CFG_NSI_H */
