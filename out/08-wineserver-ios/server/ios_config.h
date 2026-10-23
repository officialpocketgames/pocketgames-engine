// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 08 Part A: included before anything else in every server file (build-ios.sh passes it
// with -include). It pulls in the host (macOS) config.h first, so the include guard
// __WINE_CONFIG_H makes every later #include "config.h" a no-op, and then switches off the
// configure-detected features that iOS lacks or that must not be used inside the app.

#ifndef MYIOSDECK_SERVER_IOS_CONFIG_H
#define MYIOSDECK_SERVER_IOS_CONFIG_H

#include "config.h"

#ifndef WINE_IOS
#define WINE_IOS 1
#endif

/* kqueue: poll() and kqueue don't report readiness for the app's own sockets and pipes
 * (Part D), so the server never uses kqueue. */
#undef HAVE_KQUEUE
#undef HAVE_SYS_EVENT_H

/* epoll and Solaris event ports (never present on Apple systems; undefined for safety) */
#undef HAVE_SYS_EPOLL_H
#undef HAVE_EPOLL_CREATE
#undef HAVE_EPOLL_PWAIT2
#undef HAVE_PORT_H
#undef HAVE_PORT_CREATE

/* inotify (directory change notifications) */
#undef HAVE_SYS_INOTIFY_H

/* spec 17 Part H1: the iPhoneOS SDK has no netinet/tcp_fsm.h (the macOS host configure finds
 * it); sock.c then takes Darwin's BSD TCP state numbers from ios_s17_pure.h */
#undef HAVE_NETINET_TCP_FSM_H

/* ptrace and the /proc process interfaces */
#undef HAVE_SYS_PTRACE_H
#undef HAVE_SYS_USER_H
#undef HAVE_ASM_USER_H
#undef HAVE_LIBPROCSTAT
#undef HAVE_PROCESS_VM_READV
#undef HAVE_PROCESS_VM_WRITEV
#undef HAVE_SYS_THR_H
#undef HAVE_THR_KILL2

/* Linux-only interfaces */
#undef HAVE_MEMFD_CREATE
#undef HAVE_LINUX_MEMFD_H
/* spec 16 Part E: build-ios.sh defines HAVE_LINUX_NTSYNC_H=1 and IOS_USYNC=1; the header is
 * out/16-sync-ios/include/linux/ntsync.h and the "driver" a library of the app */
#ifndef IOS_USYNC
#undef HAVE_LINUX_NTSYNC_H
#endif
#undef HAVE_LINUX_MAJOR_H
#undef HAVE_LINUX_RTNETLINK_H
#undef HAVE_LINUX_FILTER_H
#undef HAVE_SCHED_SETAFFINITY

/* SIGIO with si_fd: the server installs no signal handlers on iOS (Part B2) */
#undef HAVE_SIGINFO_T_SI_FD

/* no valgrind inside the app */
#undef HAVE_VALGRIND_MEMCHECK_H

#endif /* MYIOSDECK_SERVER_IOS_CONFIG_H */
