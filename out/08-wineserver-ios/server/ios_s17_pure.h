// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 17 Part H: the server's smaller rules, header-only so the host tests can include them.
//
//  - H1: Darwin's TCP state numbers. The iOS SDK has no netinet/tcp_fsm.h, so upstream's sock.c
//    falls back to Linux's numbering, while TCP_CONNECTION_INFO's tcpi_state uses the BSD one.
//    sock.c includes this (IOS_S17_TCP_STATES) on Apple targets without that header; upstream's
//    mapping to MIB_TCP_STATE stays as it is.
//  - H3: the screen size a virtual desktop's cursor clip covers.

#ifndef MYIOSDECK_SERVER_IOS_S17_PURE_H
#define MYIOSDECK_SERVER_IOS_S17_PURE_H

#ifdef IOS_S17_TCP_STATES
/* BSD numbering, as in the macOS SDK's netinet/tcp_fsm.h */
#define TCPS_CLOSED        0
#define TCPS_LISTEN        1
#define TCPS_SYN_SENT      2
#define TCPS_SYN_RECEIVED  3
#define TCPS_ESTABLISHED   4
#define TCPS_CLOSE_WAIT    5
#define TCPS_FIN_WAIT_1    6
#define TCPS_CLOSING       7
#define TCPS_LAST_ACK      8
#define TCPS_FIN_WAIT_2    9
#define TCPS_TIME_WAIT    10
#endif

/* H3: the size spec 08 clarification 3 knows (known != 0), else 1024 x 768; a size that isn't
 * positive counts as unknown */
static inline void ios_s17_virtual_desktop_size( int known, int *width, int *height )
{
    if (!known || *width <= 0 || *height <= 0)
    {
        *width = 1024;
        *height = 768;
    }
}

#endif /* MYIOSDECK_SERVER_IOS_S17_PURE_H */
