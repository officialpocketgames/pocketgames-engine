// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 08, device bug of build 9 (a sent message never woke a thread waiting for QS_SENDMESSAGE):
// the wake rules of the server's message queues as plain functions, so queue.c and the host test
// (tests/test_queue_mask.c) use the same code. The fields mirror queue_shm_t.

#ifndef MYIOSDECK_SERVER_IOS_QUEUE_PURE_H
#define MYIOSDECK_SERVER_IOS_QUEUE_PURE_H

struct ios_queue_bits
{
    unsigned int wake_bits;
    unsigned int wake_mask;
    unsigned int changed_bits;
    unsigned int changed_mask;
    unsigned int internal_bits;
};

/* upstream queue.c get_queue_status(): is the queue's wait object signalled? */
static inline int ios_queue_status( const struct ios_queue_bits *q )
{
    return (q->wake_bits & q->wake_mask) || (q->changed_bits & q->changed_mask) || q->internal_bits;
}

/* Does a set_queue_mask request store its wake and changed masks?
 *
 * Upstream drops the whole request when the client asks to poll driver events (poll_events)
 * and the queue has no fd; the X11 driver always gives its queue one. The iOS user driver has
 * none, and spec 12's win32u still sends poll_events = 1 whenever ProcessEvents delivered
 * something just before a wait. The thread then waited with its previous masks (0 on a new
 * queue), and a message sent to it set QS_SENDMESSAGE without signalling the queue: both
 * threads waited forever. In the app (ios != 0) the masks are always stored; only the fd part
 * of the request needs an fd. */
static inline int ios_queue_mask_stored( int poll_events, int has_fd, int ios )
{
    if (ios) return 1;
    return !poll_events || has_fd;
}

/* The mask part of set_queue_mask on the model: -1 if the request is dropped (the wait object is
 * left as it was), else the new status, which decides signal (1) or reset (0). */
static inline int ios_queue_set_mask( struct ios_queue_bits *q, int poll_events, int has_fd, int ios,
                                      unsigned int wake_mask, unsigned int changed_mask )
{
    if (!ios_queue_mask_stored( poll_events, has_fd, ios )) return -1;
    q->wake_mask = wake_mask;
    q->changed_mask = changed_mask;
    return ios_queue_status( q );
}

/* upstream set_queue_bits() / clear_queue_bits() on the model (internal bits not split off) */
static inline int ios_queue_set_bits( struct ios_queue_bits *q, unsigned int bits )
{
    q->wake_bits |= bits;
    q->changed_bits |= bits;
    return ios_queue_status( q );
}

static inline int ios_queue_clear_bits( struct ios_queue_bits *q, unsigned int bits )
{
    q->wake_bits &= ~bits;
    q->changed_bits &= ~bits;
    return ios_queue_status( q );
}

/* Spec 15 clarification 10b (QUESTIONS 15-24): the raw input sink of spec 15's slot devices.
 * A registration's usage key, as upstream stores it (MAKELONG(usage, usage page)). */
static inline unsigned int ios_rawinput_usage_key( unsigned short usage_page, unsigned short usage )
{
    return (unsigned int)usage | ((unsigned int)usage_page << 16);
}

/* The RAWHID shape of a message: one report of report_len bytes for WM_INPUT, none for a device
 * change (report_len 0). */
static inline void ios_rawinput_hid_shape( unsigned int report_len, unsigned int *count, unsigned int *size )
{
    *count = report_len ? 1 : 0;
    *size = report_len;
}

#endif /* MYIOSDECK_SERVER_IOS_QUEUE_PURE_H */
