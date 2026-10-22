// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part C: the HID pad's registry entries as a plan (pure C, host-tested); the ntdll
// side (ios_hidpad_publish.c) writes the plan with NtCreateKey / NtSetValueKey.

#ifndef MYIOSDECK_IOS_HIDPAD_REGPLAN_H
#define MYIOSDECK_IOS_HIDPAD_REGPLAN_H

#include <stddef.h>
#include <stdint.h>

#include "ios_hidpad.h"

#define IOS_HIDPAD_REG_SZ        1u   /* REG_SZ */
#define IOS_HIDPAD_REG_DWORD     4u   /* REG_DWORD */
#define IOS_HIDPAD_REG_MULTI_SZ  7u   /* REG_MULTI_SZ */

#define IOS_HIDPAD_REG_KEYS      4
#define IOS_HIDPAD_REG_VALUES    7
#define IOS_HIDPAD_REG_TEXT      512
#define IOS_HIDPAD_REG_PATH      512

struct ios_hidpad_reg_value
{
    const char *name;
    unsigned int type;
    char text[IOS_HIDPAD_REG_TEXT];   /* REG_SZ: one string; REG_MULTI_SZ: strings, each NUL-ended,
                                         then one more NUL */
    size_t text_len;                  /* chars used in `text`, NULs included */
    uint32_t dword;                   /* REG_DWORD */
};

/* One key: `base` (under \Registry\Machine) is opened or created as an ordinary key; `tail`
 * is created below it level by level as volatile keys, so nothing of a pad survives into a
 * session that doesn't have it (QUESTIONS 15-13). */
struct ios_hidpad_reg_key
{
    char base[IOS_HIDPAD_REG_PATH];
    char tail[IOS_HIDPAD_REG_PATH];
    unsigned int value_count;
    struct ios_hidpad_reg_value values[IOS_HIDPAD_REG_VALUES];
};

/* Fill keys[0..3]: the Enum instance key, the DeviceClasses link key, its "#" subkey and
 * "#\Control". Returns the number of keys (4), or 0 for no identity or a name too long. */
unsigned int ios_hidpad_registry_plan(const struct ios_hidpad_identity *id,
                                      struct ios_hidpad_reg_key keys[IOS_HIDPAD_REG_KEYS]);

#endif
