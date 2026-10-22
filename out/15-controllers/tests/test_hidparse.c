// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part F: the descriptors of Part A3 parsed by Wine's own hidparse code (compiled
// through server/ios_hidparse.c exactly as the server compiles it), and hidclass's report
// lookup that the server's checks use (B4, B5).
//
// Needs Wine 11.4's include/ (the build passes -I<wine>/include) to read the preparsed data.

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/hid.h"

#include "ios_hidpad.h"
#include "ios_hidparse.h"
#include "test_common.h"

static const struct hid_value_caps *find_caps(const struct hid_preparsed_data *p, USAGE page, USAGE usage,
                                              int *count)
{
    const struct hid_value_caps *caps = HID_INPUT_VALUE_CAPS(p), *found = NULL;
    unsigned int i;

    *count = 0;
    for (i = 0; i < p->input_caps_count; i++)
    {
        if (caps[i].usage_page != page) continue;
        if (caps[i].usage_min > usage || caps[i].usage_max < usage) continue;
        if (!found) found = caps + i;
        (*count)++;
    }
    return found;
}

static void check_axis(const struct hid_preparsed_data *p, USAGE usage, unsigned int start_byte)
{
    const struct hid_value_caps *caps;
    int count;

    caps = find_caps(p, 0x01, usage, &count);
    CHECK_EQ(count, 1);
    if (!caps) return;
    CHECK_EQ(caps->report_id, 1);
    CHECK_EQ(caps->start_byte, start_byte);
    CHECK_EQ(caps->start_bit, 0);
    CHECK_EQ(caps->bit_size, 16);
    CHECK_EQ(caps->report_count, 1);
    CHECK_EQ(caps->logical_min, -32767);
    CHECK_EQ(caps->logical_max, 32767);
    CHECK_EQ(caps->usage_min, usage);
    CHECK_EQ(caps->usage_max, usage);
    CHECK(!(caps->flags & HID_VALUE_CAPS_IS_BUTTON));
    CHECK(caps->flags & HID_VALUE_CAPS_IS_ABSOLUTE);
    CHECK(!(caps->flags & HID_VALUE_CAPS_IS_CONSTANT));
}

static void test_generic(void)
{
    struct ios_hidparse_desc desc;
    const struct hid_preparsed_data *p;
    const struct hid_value_caps *caps, *all;
    const struct ios_hidparse_report *report;
    unsigned int i, data_caps = 0;
    int count;

    CHECK_EQ(ios_hidparse_parse(ios_hidpad_generic_descriptor, ios_hidpad_generic_descriptor_len, &desc), 1);
    CHECK_EQ(desc.usage_page, 0x01);
    CHECK_EQ(desc.usage, 0x05);                     /* Game Pad */
    CHECK_EQ(desc.input_len, IOS_HIDPAD_GENERIC_INPUT_LEN);
    CHECK_EQ(desc.output_len, 0);
    CHECK_EQ(desc.feature_len, 0);
    CHECK_EQ(desc.report_count, 1);
    CHECK_EQ(desc.reports[0].id, 1);
    CHECK_EQ(desc.reports[0].collection, 1);
    CHECK_EQ(desc.reports[0].input_len, 14);

    p = desc.preparsed;
    CHECK(p != NULL);
    if (!p) return;
    CHECK(!memcmp(p->magic, "HidP KDR", 8));
    CHECK_EQ(desc.preparsed_len, p->caps_size + offsetof(struct hid_preparsed_data, value_caps[0]) +
                                 p->number_link_collection_nodes * sizeof(struct hid_collection_node));
    CHECK_EQ(p->usage_page, 0x01);
    CHECK_EQ(p->usage, 0x05);
    CHECK_EQ(p->input_report_byte_length, 14);
    CHECK_EQ(p->output_caps_count, 0);
    CHECK_EQ(p->feature_caps_count, 0);
    CHECK_EQ(p->number_link_collection_nodes, 1);

    /* axes in report order: X Y Rx Ry Z, 16-bit signed, symmetric */
    check_axis(p, 0x30, 1);
    check_axis(p, 0x31, 3);
    check_axis(p, 0x33, 5);
    check_axis(p, 0x34, 7);
    check_axis(p, 0x32, 9);
    caps = find_caps(p, 0x01, 0x35, &count);       /* no Rz */
    CHECK_EQ(count, 0);

    /* the hat: 4 bits, 1-8, null state */
    caps = find_caps(p, 0x01, 0x39, &count);
    CHECK_EQ(count, 1);
    if (caps)
    {
        CHECK_EQ(caps->start_byte, 11);
        CHECK_EQ(caps->start_bit, 0);
        CHECK_EQ(caps->bit_size, 4);
        CHECK_EQ(caps->logical_min, 1);
        CHECK_EQ(caps->logical_max, 8);
        CHECK_EQ(caps->physical_min, 0);
        CHECK_EQ(caps->physical_max, 315);
        CHECK(HID_VALUE_CAPS_HAS_NULL(caps));
        CHECK(!(caps->flags & HID_VALUE_CAPS_IS_BUTTON));
    }

    /* 11 buttons, one range */
    caps = find_caps(p, 0x09, 1, &count);
    CHECK_EQ(count, 1);
    if (caps)
    {
        CHECK(caps->flags & HID_VALUE_CAPS_IS_BUTTON);
        CHECK_EQ(caps->usage_min, 1);
        CHECK_EQ(caps->usage_max, 11);
        CHECK_EQ(caps->start_byte, 12);
        CHECK_EQ(caps->start_bit, 0);
        CHECK_EQ(caps->bit_size, 1);
        CHECK_EQ(caps->report_count, 11);
    }
    find_caps(p, 0x09, 12, &count);
    CHECK_EQ(count, 0);

    /* nothing else with a usage: 5 axes + hat + buttons */
    all = HID_INPUT_VALUE_CAPS(p);
    for (i = 0; i < p->input_caps_count; i++)
        if (all[i].usage_page && !(all[i].flags & HID_VALUE_CAPS_IS_CONSTANT)) data_caps++;
    CHECK_EQ(data_caps, 7);

    /* hidclass's lookups */
    report = ios_hidparse_find_report(&desc, IOS_HIDPARSE_INPUT, 1);
    CHECK(report && report->input_len == 14);
    CHECK(ios_hidparse_find_report(&desc, IOS_HIDPARSE_INPUT, 2) == NULL);
    CHECK(ios_hidparse_find_report(&desc, IOS_HIDPARSE_OUTPUT, 1) == NULL);
    CHECK(ios_hidparse_find_report(&desc, IOS_HIDPARSE_FEATURE, 1) == NULL);

    ios_hidparse_release(&desc);
    CHECK(desc.preparsed == NULL);
}

static void test_dualsense(void)
{
    struct ios_hidparse_desc desc;
    const struct hid_preparsed_data *p;
    const struct ios_hidparse_report *report;

    CHECK_EQ(ios_hidparse_parse(ios_hidpad_dualsense_descriptor, sizeof(ios_hidpad_dualsense_descriptor), &desc), 1);
    CHECK_EQ(desc.usage_page, 0x01);
    CHECK_EQ(desc.usage, 0x05);
    CHECK_EQ(desc.input_len, IOS_HIDPAD_DUALSENSE_INPUT_LEN);
    CHECK_EQ(desc.output_len, IOS_HIDPAD_DUALSENSE_USB_OUTPUT_LEN);
    CHECK_EQ(desc.feature_len, 64);
    p = desc.preparsed;
    CHECK(p && p->input_report_byte_length == 64);

    report = ios_hidparse_find_report(&desc, IOS_HIDPARSE_INPUT, 0x01);
    CHECK(report && report->input_len == 64);
    report = ios_hidparse_find_report(&desc, IOS_HIDPARSE_OUTPUT, 0x02);
    CHECK(report && report->output_len == 48);
    CHECK(ios_hidparse_find_report(&desc, IOS_HIDPARSE_OUTPUT, 0x31) == NULL);   /* Bluetooth: refused */
    CHECK(ios_hidparse_find_report(&desc, IOS_HIDPARSE_INPUT, 0x02) == NULL);
    report = ios_hidparse_find_report(&desc, IOS_HIDPARSE_FEATURE, 0x05);
    CHECK(report && report->feature_len == 41);
    report = ios_hidparse_find_report(&desc, IOS_HIDPARSE_FEATURE, 0x09);
    CHECK(report && report->feature_len == 20);
    report = ios_hidparse_find_report(&desc, IOS_HIDPARSE_FEATURE, 0x20);
    CHECK(report && report->feature_len == 64);
    report = ios_hidparse_find_report(&desc, IOS_HIDPARSE_FEATURE, 0xf5);
    CHECK(report && report->feature_len == 4);
    CHECK(ios_hidparse_find_report(&desc, IOS_HIDPARSE_FEATURE, 0x31) == NULL);
    CHECK(ios_hidparse_find_report(&desc, IOS_HIDPARSE_FEATURE, 0x01) == NULL);

    ios_hidparse_release(&desc);
}

static void test_bad(void)
{
    struct ios_hidparse_desc desc;
    static const uint8_t truncated[] = { 0x05, 0x01, 0x09, 0x05, 0xa1 };

    CHECK_EQ(ios_hidparse_parse(NULL, 0, &desc), 0);
    CHECK_EQ(ios_hidparse_parse(truncated, sizeof(truncated), &desc), 0);
    CHECK(desc.preparsed == NULL);
}

int main(void)
{
    test_generic();
    test_dualsense();
    test_bad();
    return test_report("test_hidparse");
}
