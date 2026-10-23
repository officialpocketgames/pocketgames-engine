// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 Part I5: the iPhoneOS SDK has no <net/route.h>, but the kernel answers the routing
// sysctls (NET_RT_DUMP, NET_RT_FLAGS, NET_RT_IFLIST) as on macOS. This header, first on the
// NSI files' include path, provides the part of Darwin's routing-message ABI that nsi/ndis.c and
// upstream nsiproxy.sys/ip.c read, written from the layout spec 13 gives, and, for ip.c's
// neighbour table, struct sockaddr_inarp (normally from <netinet/if_ether.h>, also missing).
// Every size and offset is checked below.

#ifndef MYIOSDECK_NET_ROUTE_H
#define MYIOSDECK_NET_ROUTE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <netinet/in.h>

struct rt_metrics
{
    uint32_t rmx_locks;
    uint32_t rmx_mtu;
    uint32_t rmx_hopcount;
    int32_t  rmx_expire;
    uint32_t rmx_recvpipe;
    uint32_t rmx_sendpipe;
    uint32_t rmx_ssthresh;
    uint32_t rmx_rtt;
    uint32_t rmx_rttvar;
    uint32_t rmx_pksent;
    uint32_t rmx_filler[4];
};

/* a routing message; its socket addresses follow the header directly */
struct rt_msghdr
{
    uint16_t rtm_msglen;
    uint8_t  rtm_version;
    uint8_t  rtm_type;
    uint16_t rtm_index;
    int32_t  rtm_flags;
    int32_t  rtm_addrs;
    pid_t    rtm_pid;
    int32_t  rtm_seq;
    int32_t  rtm_errno;
    int32_t  rtm_use;
    uint32_t rtm_inits;
    struct rt_metrics rtm_rmx;
};

/* message types */
#define RTM_GET        0x4
#define RTM_IFINFO     0xe

/* route flags */
#define RTF_GATEWAY    0x2
#define RTF_LLINFO     0x400
#define RTF_MULTICAST  0x800000

/* bits of rtm_addrs */
#define RTA_DST        0x1
#define RTA_GATEWAY    0x2
#define RTA_NETMASK    0x4
#define RTA_IFP        0x10

/* an ARP entry's address in NET_RT_FLAGS / RTF_LLINFO replies */
struct sockaddr_inarp
{
    uint8_t        sin_len;
    uint8_t        sin_family;
    uint16_t       sin_port;
    struct in_addr sin_addr;
    struct in_addr sin_srcaddr;
    uint16_t       sin_tos;
    uint16_t       sin_other;
};

_Static_assert( sizeof(struct rt_metrics) == 56, "rt_metrics is 56 bytes" );
_Static_assert( offsetof(struct rt_metrics, rmx_expire) == 12, "rmx_expire at 12" );
_Static_assert( offsetof(struct rt_metrics, rmx_filler) == 40, "rmx_filler at 40" );
_Static_assert( sizeof(pid_t) == 4, "pid_t is 32 bits" );
_Static_assert( offsetof(struct rt_msghdr, rtm_msglen) == 0, "rtm_msglen at 0" );
_Static_assert( offsetof(struct rt_msghdr, rtm_version) == 2, "rtm_version at 2" );
_Static_assert( offsetof(struct rt_msghdr, rtm_type) == 3, "rtm_type at 3" );
_Static_assert( offsetof(struct rt_msghdr, rtm_index) == 4, "rtm_index at 4" );
_Static_assert( offsetof(struct rt_msghdr, rtm_flags) == 8, "rtm_flags at 8" );
_Static_assert( offsetof(struct rt_msghdr, rtm_addrs) == 12, "rtm_addrs at 12" );
_Static_assert( offsetof(struct rt_msghdr, rtm_pid) == 16, "rtm_pid at 16" );
_Static_assert( offsetof(struct rt_msghdr, rtm_seq) == 20, "rtm_seq at 20" );
_Static_assert( offsetof(struct rt_msghdr, rtm_errno) == 24, "rtm_errno at 24" );
_Static_assert( offsetof(struct rt_msghdr, rtm_use) == 28, "rtm_use at 28" );
_Static_assert( offsetof(struct rt_msghdr, rtm_inits) == 32, "rtm_inits at 32" );
_Static_assert( offsetof(struct rt_msghdr, rtm_rmx) == 36, "rtm_rmx at 36" );
_Static_assert( sizeof(struct rt_msghdr) == 92, "rt_msghdr is 92 bytes" );
_Static_assert( offsetof(struct sockaddr_inarp, sin_addr) == 4, "sin_addr at 4" );
_Static_assert( offsetof(struct sockaddr_inarp, sin_srcaddr) == 8, "sin_srcaddr at 8" );
_Static_assert( offsetof(struct sockaddr_inarp, sin_tos) == 12, "sin_tos at 12" );
_Static_assert( offsetof(struct sockaddr_inarp, sin_other) == 14, "sin_other at 14" );
_Static_assert( sizeof(struct sockaddr_inarp) == 16, "sockaddr_inarp is 16 bytes" );

#endif /* MYIOSDECK_NET_ROUTE_H */
