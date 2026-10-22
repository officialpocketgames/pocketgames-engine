// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 clarification 6: which arrivals and removals the wineserver announces, from samples
// of the app's stores. Plain C; the server (server/ios_hidpad_server.c) feeds it from a timer.

#ifndef MYIOSDECK_IOS_HIDPAD_PNP_H
#define MYIOSDECK_IOS_HIDPAD_PNP_H

#include <stdint.h>

#include "ios_hidpad.h"
#include "ios_pnp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IOS_PNP_SLOTS 4                      /* the XInput slots */
#define IOS_PNP_POLL_US 100000u              /* how often the server samples the stores */

/* What one sample saw: the HID pad present (ios_hidpad_presence; 0 when the server has no HID
 * pad) and each XInput slot connected (winios_gamepad_get_state). */
struct ios_pnp_sample
{
    int hid_present;
    int slot[IOS_PNP_SLOTS];
};

struct ios_pnp_watch
{
    int started;                             /* the first sample is the baseline: no events */
    struct ios_pnp_sample last;
};

/* -1: the HID pad, 0-3: an XInput slot */
struct ios_pnp_change
{
    uint32_t code;                           /* IOS_PNP_ARRIVAL or IOS_PNP_REMOVECOMPLETE */
    int which;
};

#define IOS_PNP_MAX_CHANGES (1 + IOS_PNP_SLOTS)

/* Compare `now` with the last sample; write the changes (the HID pad first, then slots 0-3) to
 * `out` and return their number. The first call only records the baseline. */
unsigned int ios_pnp_watch_update(struct ios_pnp_watch *watch, const struct ios_pnp_sample *now,
                                  struct ios_pnp_change out[IOS_PNP_MAX_CHANGES]);

/* The record announcing `change`. The HID pad's interface is `id`'s \\?\ path; an XInput slot
 * n is announced as the HID interface of an XInput pad, as Windows names one:
 * \\?\HID#VID_045E&PID_028E&IG_0n#9&4d616465&1&000n#{4d1e55b2-f16f-11cf-88cb-001111000030}.
 * Both under GUID_DEVINTERFACE_HID. Returns 0 if the name doesn't fit. */
int ios_pnp_record_build(const struct ios_pnp_change *change, const struct ios_hidpad_identity *id,
                         struct ios_pnp_record *record);

/* What one channel handle holds while no read waits: the records since it was opened, in
 * order. When full, a new record pushes out the oldest. */
#define IOS_PNP_QUEUE_MAX 64

struct ios_pnp_queue
{
    unsigned int head, count;
    struct ios_pnp_record records[IOS_PNP_QUEUE_MAX];
};

void ios_pnp_queue_push(struct ios_pnp_queue *queue, const struct ios_pnp_record *record);
/* Take the oldest record into `record`; returns 0 when the queue is empty. */
int ios_pnp_queue_pop(struct ios_pnp_queue *queue, struct ios_pnp_record *record);

#ifdef __cplusplus
}
#endif

#endif
