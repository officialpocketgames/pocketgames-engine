// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 16 Part E5: stands in for Linux's <linux/ntsync.h> on iOS, where there is no ntsync
// driver. The argument structures carry the field names of the kernel's ntsync documentation
// (https://docs.kernel.org/userspace-api/ntsync.html); the request codes are this library's own.
// The "driver" is src/ios_usync.c, a library that the server thread and every Windows thread of
// the app call directly: descriptors are pseudo descriptors (0x70000000 | index), the device is
// IOS_USYNC_DEVICE. The two files that use this header (the server's inproc_sync.c and ntdll's
// sync.c) send ioctl() and close() to ios_usync_ioctl() / ios_usync_close(), which pass every
// descriptor that isn't theirs to libc.

#ifndef MYIOSDECK_LINUX_NTSYNC_H
#define MYIOSDECK_LINUX_NTSYNC_H

#include <stdint.h>

#ifdef __linux__
# include <linux/types.h>
#else
typedef uint32_t __u32;
typedef uint64_t __u64;
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct ntsync_sem_args
{
    __u32 count;
    __u32 max;
};

struct ntsync_mutex_args
{
    __u32 owner;
    __u32 count;
};

struct ntsync_event_args
{
    __u32 manual;
    __u32 signaled;
};

#define NTSYNC_WAIT_REALTIME   0x1
#define NTSYNC_MAX_WAIT_COUNT  64

struct ntsync_wait_args
{
    __u64 timeout;     /* absolute, ns; CLOCK_MONOTONIC, or CLOCK_REALTIME with NTSYNC_WAIT_REALTIME; ~0: infinite */
    __u64 objs;        /* pointer to an array of count int descriptors */
    __u32 count;
    __u32 index;       /* out: which object; count when the alert fired */
    __u32 flags;
    __u32 owner;       /* mutex owner id of the waiter */
    __u32 alert;       /* an event descriptor, 0: none */
    __u32 pad;
};

/* request codes: this library's own */
#define NTSYNC_IOC_CREATE_SEM    0x4e530001ul   /* device: struct ntsync_sem_args *, returns a descriptor */
#define NTSYNC_IOC_CREATE_MUTEX  0x4e530002ul   /* device: struct ntsync_mutex_args * */
#define NTSYNC_IOC_CREATE_EVENT  0x4e530003ul   /* device: struct ntsync_event_args * */
#define NTSYNC_IOC_WAIT_ANY      0x4e530004ul   /* device: struct ntsync_wait_args * */
#define NTSYNC_IOC_WAIT_ALL      0x4e530005ul   /* device: struct ntsync_wait_args * */
#define NTSYNC_IOC_SEM_RELEASE   0x4e530010ul   /* __u32 *: in the count, out the previous count */
#define NTSYNC_IOC_SEM_READ      0x4e530011ul   /* struct ntsync_sem_args * */
#define NTSYNC_IOC_MUTEX_UNLOCK  0x4e530020ul   /* struct ntsync_mutex_args *: in owner, out the previous count */
#define NTSYNC_IOC_MUTEX_KILL    0x4e530021ul   /* __u32 *: the owner */
#define NTSYNC_IOC_MUTEX_READ    0x4e530022ul   /* struct ntsync_mutex_args * */
#define NTSYNC_IOC_EVENT_SET     0x4e530030ul   /* __u32 *: out the previous state */
#define NTSYNC_IOC_EVENT_RESET   0x4e530031ul
#define NTSYNC_IOC_EVENT_PULSE   0x4e530032ul
#define NTSYNC_IOC_EVENT_READ    0x4e530033ul   /* struct ntsync_event_args * */

/* the userspace library (src/ios_usync.c) */
#define IOS_USYNC_DEVICE    0x6fffffff
#define IOS_USYNC_FD_TAG    0x70000000

extern int ios_usync_ioctl( int fd, unsigned long request, void *arg );
extern int ios_usync_close( int fd );
/* E4: the server posts {process id, token, descriptor} with a reference instead of sending a
 * descriptor; 0, or -1 when it can't be stored */
extern int ios_usync_post( unsigned int pid, unsigned int token, int fd );
/* E4: the client takes it after the reply: the descriptor, or -1 when there is none */
extern int ios_usync_take( unsigned int pid, unsigned int token );

#ifdef __cplusplus
}
#endif

#endif /* MYIOSDECK_LINUX_NTSYNC_H */
