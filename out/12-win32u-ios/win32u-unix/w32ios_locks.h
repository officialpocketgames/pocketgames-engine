// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 round 4 (1.2-1.3): every pthread mutex that sysparams.c and gdiobj.c lock (win32u's
// user lock user_mutex, its display lock display_lock, its GDI lock gdi_lock, and the other
// static mutexes of those files) is taken through ntdll's owner tracking (ios_lock.c in
// out/09-ntdll-processes): a wait of more than 3 s says which thread of which process holds it,
// and a lock whose holder's process has ended is recovered. These locks are shared by every
// pseudo-process. Each lock is named after its expression in the source ("win32u &gdi_lock").
// Included by the patched sysparams.c and gdiobj.c after their headers (patch S9R4-locks).

#ifndef W32IOS_LOCKS_H
#define W32IOS_LOCKS_H

#include <pthread.h>

/* ntdll's (out/09-ntdll-processes/ntdll-unix/ios_lock.h); weak, so a build without it locks plainly */
extern void ios_lock_take( pthread_mutex_t *mutex, const char *name ) __attribute__((weak));
extern void ios_lock_give( pthread_mutex_t *mutex ) __attribute__((weak));

static inline int w32ios_mutex_lock( pthread_mutex_t *mutex, const char *name )
{
    if (!ios_lock_take) return (pthread_mutex_lock)( mutex );
    ios_lock_take( mutex, name );
    return 0;
}

static inline int w32ios_mutex_unlock( pthread_mutex_t *mutex )
{
    if (!ios_lock_give) return (pthread_mutex_unlock)( mutex );
    ios_lock_give( mutex );
    return 0;
}

#define pthread_mutex_lock(m)   w32ios_mutex_lock( (m), "win32u " #m )
#define pthread_mutex_unlock(m) w32ios_mutex_unlock( (m) )

#endif /* W32IOS_LOCKS_H */
