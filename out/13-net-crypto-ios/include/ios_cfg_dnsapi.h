// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part H1: per-file configuration for dnsapi/libresolv.c (force-included after the wine
// build dir's config.h and spec 09's no-exit header).
//
// The resolver is reached through ios_resolv.c (Part H2, clarification 3: libresolv linked with
// -lresolv, called through res_ninit / res_nquery / res_ndestroy on a state per thread). With
// HAVE_RES_GETSERVERS and HAVE_STRUCT___RES_STATE__U__EXT_NSCOUNT6 undefined, the server list
// comes from the resolver state's IPv4 servers (nscount, nsaddr_list).

#ifndef MYIOSDECK_IOS_CFG_DNSAPI_H
#define MYIOSDECK_IOS_CFG_DNSAPI_H

#undef  HAVE_RESOLV
#define HAVE_RESOLV 1
#undef  HAVE_RESOLV_H
#define HAVE_RESOLV_H 1
#undef  HAVE_NETINET_IN_H
#define HAVE_NETINET_IN_H 1
#undef  HAVE_ARPA_NAMESER_H
#define HAVE_ARPA_NAMESER_H 1
#undef  HAVE_NETDB_H
#define HAVE_NETDB_H 1

#undef HAVE_RES_GETSERVERS
#undef HAVE_STRUCT___RES_STATE__U__EXT_NSCOUNT6

#endif /* MYIOSDECK_IOS_CFG_DNSAPI_H */
