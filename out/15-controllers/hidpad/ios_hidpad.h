// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part A: the HID pad's identity, names, report descriptors and reports.
//
// Pure C (C99, no Wine headers, no allocation, no I/O). Used by the wineserver's device
// (Part B, server/) and by ntdll's registry entries (Part C, ntdll/), so the device the server
// creates and the one the registry advertises come from the same table.

#ifndef MYIOSDECK_IOS_HIDPAD_H
#define MYIOSDECK_IOS_HIDPAD_H

#include <stddef.h>
#include <stdint.h>

#include "ios_hidpad_contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- A1: identities ------------------------------------------------------------------- */

enum ios_hidpad_kind
{
    IOS_HIDPAD_NONE = 0,
    IOS_HIDPAD_DUALSENSE = 1,
    IOS_HIDPAD_GENERIC = 2,
};

#define IOS_HIDPAD_STRING_MAX 127   /* product / manufacturer / serial, ASCII, without the NUL */
#define IOS_HIDPAD_HWID_COUNT 6

struct ios_hidpad_identity
{
    enum ios_hidpad_kind kind;
    const char *name;               /* the MADEIRA_HIDPAD value that selects it */
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t version;
    const char *device_id;          /* HID\VID_054C&PID_0CE6&MI_03 */
    const char *instance;           /* 9&4d616465&0&0000 */
    const char *container_id;       /* {...}, braces included */
    char product[IOS_HIDPAD_STRING_MAX + 1];
    char manufacturer[IOS_HIDPAD_STRING_MAX + 1];
    char serial[IOS_HIDPAD_STRING_MAX + 1];
    const uint8_t *descriptor;      /* A3 */
    size_t descriptor_len;
    size_t input_report_len;        /* A4, with the report id */
};

/* Fill `out` for the identity `hidpad_env` names ("dualsense" or "generic", exact match);
 * `name_env` is MADEIRA_HIDPAD_NAME (may be NULL). Returns 0 and sets out->kind to
 * IOS_HIDPAD_NONE for anything else, including NULL. */
int ios_hidpad_identity_select(const char *hidpad_env, const char *name_env,
                               struct ios_hidpad_identity *out);

/* Same, by kind (used when the caller already knows which one exists). */
int ios_hidpad_identity_get(enum ios_hidpad_kind kind, const char *name_env,
                            struct ios_hidpad_identity *out);

/* ---- A2: names (all ASCII; each returns the length written without the NUL, or 0 if
 * `size` is too small; the buffer is then an empty string) -------------------------------- */

#define IOS_HIDPAD_HID_INTERFACE_GUID "{4d1e55b2-f16f-11cf-88cb-001111000030}"
#define IOS_HIDPAD_HIDCLASS_GUID      "{745a17a0-74d3-11d0-b6fe-00a0c90f57da}"
#define IOS_HIDPAD_DEVICE_OBJECT      "\\Device\\MyiosdeckHidPad0"

/* <device id>\<instance>\{guid} with every '\' turned into '#'. */
size_t ios_hidpad_link(const struct ios_hidpad_identity *id, char *buf, size_t size);
/* \\?\<link>: what programs open (and what setupapi reports). */
size_t ios_hidpad_user_path(const struct ios_hidpad_identity *id, char *buf, size_t size);
/* \??\<link>: the server's symlink. */
size_t ios_hidpad_symlink_path(const struct ios_hidpad_identity *id, char *buf, size_t size);
/* ##?#<link>: the registry key under DeviceClasses\{guid}. */
size_t ios_hidpad_registry_link_key(const struct ios_hidpad_identity *id, char *buf, size_t size);
/* <device id>\<instance> */
size_t ios_hidpad_instance_id(const struct ios_hidpad_identity *id, char *buf, size_t size);
/* Hardware id number `index` (0..IOS_HIDPAD_HWID_COUNT-1), in the order of A1. */
size_t ios_hidpad_hardware_id(const struct ios_hidpad_identity *id, unsigned int index,
                              char *buf, size_t size);

/* ---- A3: descriptors ------------------------------------------------------------------ */

#define IOS_HIDPAD_DUALSENSE_DESCRIPTOR_LEN 273
#define IOS_HIDPAD_DUALSENSE_INPUT_LEN 64
#define IOS_HIDPAD_DUALSENSE_USB_OUTPUT_LEN 48
#define IOS_HIDPAD_DUALSENSE_BT_OUTPUT_LEN 78
#define IOS_HIDPAD_GENERIC_INPUT_LEN 14

extern const uint8_t ios_hidpad_dualsense_descriptor[IOS_HIDPAD_DUALSENSE_DESCRIPTOR_LEN];
extern const uint8_t ios_hidpad_generic_descriptor[];
extern const size_t ios_hidpad_generic_descriptor_len;

/* ---- A4: input reports ---------------------------------------------------------------- */

/* State that lives across reports of one device: the report counter and the touch ids. */
struct ios_hidpad_report_state
{
    uint8_t counter;            /* DualSense byte 7, incremented per report */
    uint8_t next_touch_id;      /* 7 bits */
    uint8_t touch_id[2];
    uint8_t touch_down[2];
};

/* The DualSense hat from XINPUT_GAMEPAD_DPAD_* bits: 0-7 clockwise from north, 8 centred.
 * Opposite directions cancel. */
uint8_t ios_hidpad_hat_from_buttons(uint32_t buttons);

/* Store Y (+up) to HID/DirectInput Y (+down): -32768 becomes 32767. */
int16_t ios_hidpad_flip_y(int16_t v);

/* Build DualSense USB report 0x01 into out[IOS_HIDPAD_DUALSENSE_INPUT_LEN]. `pad` may be a
 * disconnected snapshot (connected == 0): the report is then a pad at rest. `now_us` is the
 * sensor clock in microseconds. Returns the report length. */
size_t ios_hidpad_build_dualsense_input(const struct winios_hidpad *pad,
                                        struct ios_hidpad_report_state *state,
                                        uint64_t now_us, uint8_t *out);

/* Build the generic report 1 into out[IOS_HIDPAD_GENERIC_INPUT_LEN]. */
size_t ios_hidpad_build_generic_input(const struct winios_hidpad *pad, uint8_t *out);

/* Either of the above, by identity. `out` must hold id->input_report_len bytes. */
size_t ios_hidpad_build_input(const struct ios_hidpad_identity *id, const struct winios_hidpad *pad,
                              struct ios_hidpad_report_state *state, uint64_t now_us, uint8_t *out);

/* ---- A5: feature reports (DualSense) -------------------------------------------------- */

/* Fill out[0..len-1] for feature report `report_id` (out[0] = report_id, the rest as A5;
 * ids not listed in A5 are zero-filled). `len` is the feature report's length with the id. */
void ios_hidpad_build_feature(const struct ios_hidpad_identity *id, uint8_t report_id,
                              uint8_t *out, size_t len);

/* ---- A6: output reports --------------------------------------------------------------- */

/* CRC-32 (IEEE 802.3, reflected, init and final xor 0xffffffff), continuing from `crc`
 * (pass 0 to start). */
uint32_t ios_hidpad_crc32(uint32_t crc, const uint8_t *data, size_t len);

enum ios_hidpad_output_result
{
    IOS_HIDPAD_OUTPUT_IGNORED = 0,   /* not an effects report (accepted, nothing to do) */
    IOS_HIDPAD_OUTPUT_FOLDED = 1,    /* an effects report: flagged fields folded into `out` */
    IOS_HIDPAD_OUTPUT_BAD_CRC = 2,   /* a Bluetooth effects report whose CRC doesn't match */
};

/* What one effects report asked for, for the log (B7). */
struct ios_hidpad_output_flags
{
    uint8_t rumble;      /* a rumble flag was set */
    uint8_t triggers;    /* either trigger block was flagged */
    uint8_t lightbar;
    uint8_t leds;        /* player LEDs or the mic LED */
    uint8_t power;
};

/* Decode `data[0..len-1]` (byte 0 the report id) and fold every flagged field into `out`
 * (which holds the last output snapshot; its serial is left alone). `flags` may be NULL. */
enum ios_hidpad_output_result ios_hidpad_decode_output(const uint8_t *data, size_t len,
                                                       struct winios_hidpad_output *out,
                                                       struct ios_hidpad_output_flags *flags);

/* B2: what a real pad would keep but a closed game must not: motors stopped, trigger effects
 * released (both blocks all zero, i.e. no effect). Fields a game never set stay unset.
 * Returns 1 if `out` changed. */
int ios_hidpad_output_release(struct winios_hidpad_output *out);

/* Bytewise comparison ignoring `serial`: 1 if the two snapshots ask for the same thing. */
int ios_hidpad_output_same(const struct winios_hidpad_output *a, const struct winios_hidpad_output *b);

/* ---- B3 / B7 helpers (pure, host-tested) ---------------------------------------------- */

#define IOS_HIDPAD_TICK_US 4000u        /* a wired DualSense's report interval */
#define IOS_HIDPAD_FRESH_US 1000u       /* minimum gap for a report caused by a store change */

/* Whether a read may complete at once with a new report. `have_last` is 0 when this handle
 * has never had a report. */
int ios_hidpad_read_ready(uint64_t now_us, int have_last, uint64_t last_report_us,
                          uint32_t last_packet, uint32_t packet);

/* Effects-report log: returns 1 when report number `count` (1-based) is logged in full
 * (the first 8), 2 when a tally is due (after 100, 1000, 10000 reports), else 0. */
int ios_hidpad_effects_log_kind(uint32_t count);

/* ---- clarification 4 (spec 21 Part B): the gate --------------------------------------- */

#define IOS_HIDPAD_REFUSALS_LOGGED 4

/* Whether a new handle may open the device: only while the HID store reports the pad
 * connected (`connected` = winios_hidpad_get_state's result), as a pad that isn't plugged in.
 * A refusal adds one to `*refusals`; `*log_refusal` (may be NULL) is set to 1 for the first
 * IOS_HIDPAD_REFUSALS_LOGGED refusals, else 0. Handles already open are not affected. */
int ios_hidpad_open_allowed(int connected, uint32_t *refusals, int *log_refusal);

#ifdef __cplusplus
}
#endif

#endif
