// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 22 Part C5: the two Mach calls the end of a pseudo-process needs for a thread it left
// behind, on the app's own threads only (spec 00 B2): its run state, and suspending it for good
// (iOS can't end a thread from another thread). A thread is named by its Mach port name, the
// "unix tid" the server and the alert table know it by.

#if 0
#pragma makedep unix
#endif

#if defined(__APPLE__) && defined(WINE_IOS)

#include <mach/mach.h>
#include <mach/thread_info.h>

/* 1: blocked in the kernel (TH_STATE_WAITING), 0: any other state, -1: unknown (gone) */
int ios_mach_thread_waiting( unsigned int port )
{
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;

    if (!port) return -1;
    if (thread_info( (thread_act_t)port, THREAD_BASIC_INFO, (thread_info_t)&info, &count ) != KERN_SUCCESS)
        return -1;
    return info.run_state == TH_STATE_WAITING ? 1 : 0;
}

/* 0 when suspended */
int ios_mach_thread_suspend( unsigned int port )
{
    if (!port) return -1;
    return thread_suspend( (thread_act_t)port ) == KERN_SUCCESS ? 0 : -1;
}

#else

int ios_mach_thread_waiting( unsigned int port )
{
    (void)port;
    return -1;
}

int ios_mach_thread_suspend( unsigned int port )
{
    (void)port;
    return -1;
}

#endif
