// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part B6: Wine's HID report-descriptor parser (hidparse.sys), compiled into the
// wineserver library, seen through plain C types so the device code needn't include the DDK.

#ifndef MYIOSDECK_IOS_HIDPARSE_H
#define MYIOSDECK_IOS_HIDPARSE_H

#include <stddef.h>
#include <stdint.h>

enum ios_hidparse_report_type
{
    IOS_HIDPARSE_INPUT = 0,     /* HidP_Input */
    IOS_HIDPARSE_OUTPUT = 1,    /* HidP_Output */
    IOS_HIDPARSE_FEATURE = 2,   /* HidP_Feature */
};

/* One HIDP_REPORT_IDS entry: lengths in bytes, with the report id byte. */
struct ios_hidparse_report
{
    uint8_t id;
    uint8_t collection;         /* 1-based */
    uint16_t input_len;
    uint16_t output_len;
    uint16_t feature_len;
};

struct ios_hidparse_desc
{
    void *preparsed;            /* the first collection's preparsed data, Wine's layout (malloc) */
    uint32_t preparsed_len;
    uint16_t usage_page;
    uint16_t usage;
    uint16_t input_len;         /* the collection's longest reports */
    uint16_t output_len;
    uint16_t feature_len;
    uint32_t report_count;
    struct ios_hidparse_report reports[256];
};

/* Parse `desc` with Wine's parser. Returns 1 on success (release with ios_hidparse_release),
 * 0 on failure (nothing to release). */
int ios_hidparse_parse(const uint8_t *desc, size_t len, struct ios_hidparse_desc *out);
void ios_hidparse_release(struct ios_hidparse_desc *desc);

/* hidclass's lookup (find_report_with_type_and_id, collection 1, exact id): the report of
 * `type` named by `id`, or one without an id; NULL if the descriptor has none. Pure: defined
 * in hidpad/ios_hidpad_request.c (libios_hidpad.a). */
const struct ios_hidparse_report *ios_hidparse_find_report(const struct ios_hidparse_desc *desc,
                                                           enum ios_hidparse_report_type type,
                                                           uint8_t id);

#endif
