// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 08, device bug of build 9: a launcher's main thread SendMessage()s to dinput's input
// thread right after starting it; the input thread waits in MsgWaitForMultipleObjectsEx
// (QS_ALLINPUT) on its new queue. On the device the message never woke it. Cause: spec 12's
// win32u sends set_queue_mask with poll_events = 1 when ProcessEvents delivered something, the
// iOS driver's queues have no fd, and upstream's server then dropped the whole request, so the
// thread waited with the masks of a new queue (0).
//
// This test drives the queue rules of server/ios_queue_pure.h (the code queue.c uses) through
// that pattern: first the upstream rules, which show the hang, then the iOS rules, many times
// over with both orders and with and without a delivering ProcessEvents. The whole pattern
// (two threads, a window, the real server) is an on-device check in TESTS.md.

#include <stdio.h>

#include "ios_queue_pure.h"

/* Win32 QS_* values (winuser.h) */
#define QS_KEY          0x0001
#define QS_MOUSEMOVE    0x0002
#define QS_MOUSEBUTTON  0x0004
#define QS_POSTMESSAGE  0x0008
#define QS_TIMER        0x0010
#define QS_PAINT        0x0020
#define QS_SENDMESSAGE  0x0040
#define QS_HOTKEY       0x0080
#define QS_SMRESULT     0x8000   /* Wine's internal "send result" bit */
#define WAIT_MASK       0x1cff   /* the input thread's wake mask in the device dump */

static int failures;

#define CHECK(cond) do { if (!(cond)) { failures++; \
    fprintf( stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond ); } } while (0)

static const struct ios_queue_bits new_queue;  /* all zero, as create_msg_queue leaves it */

/* the wait object of the model: signalled or not, changed only where queue.c changes it */
struct model
{
    struct ios_queue_bits q;
    int signaled;
};

/* set_queue_mask as queue.c handles it: signal or reset from the new status, nothing if the
 * request was dropped */
static void mask_request( struct model *m, int poll_events, int has_fd, int ios )
{
    int status = ios_queue_set_mask( &m->q, poll_events, has_fd, ios, WAIT_MASK, 0 );
    if (status != -1) m->signaled = status;
}

/* SendMessage from another thread: set_queue_bits( QS_SENDMESSAGE ) signals if the status is up */
static void send_message( struct model *m )
{
    if (ios_queue_set_bits( &m->q, QS_SENDMESSAGE )) m->signaled = 1;
}

static void test_upstream_rules_show_the_hang(void)
{
    struct model m = { new_queue, 0 };

    /* ProcessEvents delivered something: poll_events = 1, and the queue has no fd */
    mask_request( &m, 1, 0, 0 );
    CHECK( m.q.wake_mask == 0 );    /* the request was dropped */
    send_message( &m );
    CHECK( !m.signaled );           /* the receiver is never woken: the device hang */

    /* the same with a queue fd (X11) works */
    m.q = new_queue;
    m.signaled = 0;
    mask_request( &m, 1, 1, 0 );
    send_message( &m );
    CHECK( m.signaled );
}

static void test_ios_rules_store_the_masks(void)
{
    struct model m = { new_queue, 0 };

    CHECK( ios_queue_mask_stored( 1, 0, 1 ) );
    CHECK( ios_queue_mask_stored( 0, 0, 1 ) );
    CHECK( ios_queue_mask_stored( 1, 1, 1 ) );
    CHECK( !ios_queue_mask_stored( 1, 0, 0 ) );
    CHECK( ios_queue_mask_stored( 0, 0, 0 ) );

    mask_request( &m, 1, 0, 1 );
    CHECK( m.q.wake_mask == WAIT_MASK );
    CHECK( !m.signaled );           /* nothing pending yet: reset */
    send_message( &m );
    CHECK( m.signaled );            /* the sent message wakes the waiter */
}

/* a small deterministic generator, so a failure repeats */
static unsigned int rng = 12345;
static unsigned int next_rand(void)
{
    rng = rng * 1103515245u + 12345u;
    return rng >> 16;
}

/* the launcher's pattern over and over, each time as the first activity of a new queue:
 * the receiver's last mask request before its wait and the sender's message come in either
 * order; ProcessEvents delivered something or not; then the receiver handles the message */
static void test_pattern_many_times(void)
{
    unsigned int i, woken = 0;

    for (i = 0; i < 100000; i++)
    {
        struct model m = { new_queue, 0 };
        int drained = next_rand() & 1;
        int send_first = next_rand() & 1;

        if (send_first)
        {
            send_message( &m );     /* bits set while the mask is still 0: not signalled yet */
            CHECK( !m.signaled );
            mask_request( &m, drained, 0, 1 );
        }
        else
        {
            mask_request( &m, drained, 0, 1 );
            send_message( &m );
        }
        CHECK( m.signaled );
        if (m.signaled) woken++;

        /* the receiver takes the message: QS_SENDMESSAGE goes, nothing else is pending */
        if (!ios_queue_clear_bits( &m.q, QS_SENDMESSAGE )) m.signaled = 0;
        CHECK( !m.signaled );

        /* the next wait with nothing pending stays asleep; the reply (QS_SMRESULT) goes to
         * the sender's queue, not this one */
        mask_request( &m, next_rand() & 1, 0, 1 );
        CHECK( !m.signaled );
    }
    CHECK( woken == 100000 );
}

/* a pure poll (no wake or changed mask) and a later wait's mask */
static void test_masks_follow_the_last_request(void)
{
    struct model m = { new_queue, 0 };

    mask_request( &m, 1, 0, 1 );                    /* the wait's mask */
    CHECK( ios_queue_set_mask( &m.q, 1, 0, 1, 0, 0 ) == 0 );   /* a later pure poll: masks 0 */
    CHECK( m.q.wake_mask == 0 );
    m.signaled = 0;
    mask_request( &m, 0, 0, 1 );                    /* the next wait sets QS_ALLINPUT again */
    send_message( &m );
    CHECK( m.signaled );

    /* internal bits signal whatever the masks are */
    m.q = new_queue;
    m.q.internal_bits = 1;
    CHECK( ios_queue_status( &m.q ) );
}

int main(void)
{
    test_upstream_rules_show_the_hang();
    test_ios_rules_store_the_masks();
    test_pattern_many_times();
    test_masks_follow_the_last_request();

    if (failures)
    {
        fprintf( stderr, "test_queue_mask: %d failure(s)\n", failures );
        return 1;
    }
    printf( "test_queue_mask: all passed\n" );
    return 0;
}
