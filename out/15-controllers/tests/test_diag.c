// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 clarification 9f: the diagnostic counters every reader path uses
// (pe/include/ios_diag.h: the first few events in full, then a count per 5 s window; the
// server's summary period), and the server's IOCTL names and tally
// (hidpad/ios_hidpad_request.c).

#include <stdio.h>
#include <string.h>

#include "ios_diag.h"
#include "ios_hidpad_request.h"
#include "test_common.h"

static void test_events(void)
{
    struct ios_diag d;
    uint64_t elapsed = 0;

    memset(&d, 0, sizeof(d));
    CHECK_EQ(ios_diag_due(&d, 100000, &elapsed), 0);        /* nothing counted yet */
    CHECK_EQ(ios_diag_event(&d, 1000), 1);                  /* logged in full */
    CHECK_EQ(ios_diag_event(&d, 1500), 2);
    CHECK_EQ(ios_diag_event(&d, 2000), 3);
    CHECK_EQ(ios_diag_event(&d, 2500), 4);                  /* past "the first 3": counted only */
    CHECK_EQ(ios_diag_due(&d, 5999, &elapsed), 0);          /* the window began with the first event */
    CHECK_EQ(ios_diag_due(&d, 6000, &elapsed), 4);
    CHECK_EQ(elapsed, 5000);
    CHECK_EQ(ios_diag_due(&d, 6001, NULL), 0);              /* reset */
    CHECK_EQ(ios_diag_due(&d, 20000, NULL), 0);             /* no events: no line */
    CHECK_EQ(ios_diag_event(&d, 20000), 5);                 /* numbering goes on */
    CHECK_EQ(ios_diag_due(&d, 20000, &elapsed), 1);         /* the quiet window ends with it */
    CHECK_EQ(elapsed, 14000);
    CHECK_EQ(ios_diag_event(&d, 21000), 6);
    CHECK_EQ(ios_diag_due(&d, 24999, NULL), 0);
    CHECK_EQ(ios_diag_due(&d, 25000, NULL), 1);
    CHECK_EQ(ios_diag_due(&d, 1000, NULL), 0);              /* a clock before the window */
    CHECK_EQ(d.total, 6);
}

static void test_period(void)
{
    uint64_t start = 0;

    CHECK_EQ(ios_diag_period(&start, 7000), 0);             /* the first call starts it */
    CHECK_EQ(start, 7000);
    CHECK_EQ(ios_diag_period(&start, 11999), 0);
    CHECK_EQ(ios_diag_period(&start, 12000), 1);            /* every 5 s, with or without events */
    CHECK_EQ(ios_diag_period(&start, 12001), 0);
    CHECK_EQ(ios_diag_period(&start, 17000), 1);
    CHECK_EQ(ios_diag_period(&start, 3000), 0);             /* a clock that went back restarts it */
    CHECK_EQ(start, 3000);
}

static void test_ioctls(void)
{
    struct ios_hidpad_tally t;
    unsigned int i;

    CHECK_STR(ios_hidpad_ioctl_name(IOS_IOCTL_HID_GET_COLLECTION_INFORMATION), "IOCTL_HID_GET_COLLECTION_INFORMATION");
    CHECK_STR(ios_hidpad_ioctl_name(IOS_IOCTL_HID_GET_WINE_RAWINPUT_HANDLE), "IOCTL_HID_GET_WINE_RAWINPUT_HANDLE");
    CHECK_STR(ios_hidpad_ioctl_name(IOS_IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS), "IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS");
    CHECK_STR(ios_hidpad_ioctl_name(IOS_IOCTL_HID_GET_INPUT_REPORT), "IOCTL_HID_GET_INPUT_REPORT");
    CHECK(ios_hidpad_ioctl_name(0x00090000u) == NULL);       /* an FSCTL: printed in hex */
    CHECK(!strncmp(ios_hidpad_ioctl_name(IOS_IOCTL_HID_FLUSH_QUEUE), "IOCTL_", 6));   /* the summary drops it */

    memset(&t, 0, sizeof(t));
    ios_hidpad_tally_add(&t, 1);
    ios_hidpad_tally_add(&t, 2);
    ios_hidpad_tally_add(&t, 1);
    CHECK_EQ(t.used, 2);
    CHECK_EQ(t.code[0], 1);
    CHECK_EQ(t.count[0], 2);
    CHECK_EQ(t.count[1], 1);
    for (i = 3; i <= IOS_HIDPAD_TALLY_CODES + 2; i++) ios_hidpad_tally_add(&t, i);
    CHECK_EQ(t.used, IOS_HIDPAD_TALLY_CODES);
    CHECK_EQ(t.other, 2);                                    /* codes past the table */
    ios_hidpad_tally_add(&t, 2);                             /* a known code still counts as itself */
    CHECK_EQ(t.count[1], 2);
    CHECK_EQ(t.other, 2);
}

int main(void)
{
    test_events();
    test_period();
    test_ioctls();
    return test_report("test_diag");
}
