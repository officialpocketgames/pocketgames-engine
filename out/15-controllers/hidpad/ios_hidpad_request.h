// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Parts B4/B5: what the HID pad's device answers to WriteFile and to each IOCTL, as
// plain C without the wineserver's API, so the server (server/ios_hidpad_server.c) and the
// host tests run the same rules. The server keeps what needs its API: handles, asyncs, the
// read clock (B3), logging (B7) and handing a changed output snapshot to the store.

#ifndef MYIOSDECK_IOS_HIDPAD_REQUEST_H
#define MYIOSDECK_IOS_HIDPAD_REQUEST_H

#include <stddef.h>
#include <stdint.h>

#include "ios_hidpad.h"
#include "ios_hidparse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* NTSTATUS values (Microsoft's ntstatus.h); the server asserts they match Wine's. */
#define IOS_STATUS_SUCCESS              0x00000000u
#define IOS_STATUS_BUFFER_OVERFLOW      0x80000005u
#define IOS_STATUS_INVALID_PARAMETER    0xC000000Du
#define IOS_STATUS_BUFFER_TOO_SMALL     0xC0000023u
#define IOS_STATUS_NOT_SUPPORTED        0xC00000BBu
#define IOS_STATUS_INVALID_USER_BUFFER  0xC00000E8u
#define IOS_STATUS_INVALID_BUFFER_SIZE  0xC0000206u
#define IOS_STATUS_DELETE_PENDING       0xC0000056u
#define IOS_STATUS_DEVICE_NOT_CONNECTED 0xC000009Du

/* IOCTL codes (Microsoft's hidclass.h: CTL_CODE(FILE_DEVICE_KEYBOARD, id, method, any)),
 * plus Wine's raw input handle query (include/wine/hid.h); the server asserts they match. */
#define IOS_HID_CTL(id, method)  ((0x0bu << 16) | ((uint32_t)(id) << 2) | (uint32_t)(method))
#define IOS_METHOD_BUFFERED   0u
#define IOS_METHOD_IN_DIRECT  1u
#define IOS_METHOD_OUT_DIRECT 2u
#define IOS_METHOD_NEITHER    3u

#define IOS_IOCTL_HID_GET_COLLECTION_DESCRIPTOR   IOS_HID_CTL(100, IOS_METHOD_NEITHER)
#define IOS_IOCTL_HID_FLUSH_QUEUE                 IOS_HID_CTL(101, IOS_METHOD_NEITHER)
#define IOS_IOCTL_HID_GET_COLLECTION_INFORMATION  IOS_HID_CTL(106, IOS_METHOD_BUFFERED)
#define IOS_IOCTL_HID_GET_FEATURE                 IOS_HID_CTL(100, IOS_METHOD_OUT_DIRECT)
#define IOS_IOCTL_HID_GET_INPUT_REPORT            IOS_HID_CTL(104, IOS_METHOD_OUT_DIRECT)
#define IOS_IOCTL_HID_GET_MANUFACTURER_STRING     IOS_HID_CTL(110, IOS_METHOD_OUT_DIRECT)
#define IOS_IOCTL_HID_GET_PRODUCT_STRING          IOS_HID_CTL(111, IOS_METHOD_OUT_DIRECT)
#define IOS_IOCTL_HID_GET_SERIALNUMBER_STRING     IOS_HID_CTL(112, IOS_METHOD_OUT_DIRECT)
#define IOS_IOCTL_HID_GET_INDEXED_STRING          IOS_HID_CTL(120, IOS_METHOD_OUT_DIRECT)
#define IOS_IOCTL_GET_NUM_DEVICE_INPUT_BUFFERS    IOS_HID_CTL(104, IOS_METHOD_BUFFERED)
#define IOS_IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS    IOS_HID_CTL(105, IOS_METHOD_BUFFERED)
#define IOS_IOCTL_HID_GET_POLL_FREQUENCY_MSEC     IOS_HID_CTL(102, IOS_METHOD_BUFFERED)
#define IOS_IOCTL_HID_SET_POLL_FREQUENCY_MSEC     IOS_HID_CTL(103, IOS_METHOD_BUFFERED)
#define IOS_IOCTL_HID_SET_FEATURE                 IOS_HID_CTL(100, IOS_METHOD_IN_DIRECT)
#define IOS_IOCTL_HID_SET_OUTPUT_REPORT           IOS_HID_CTL(101, IOS_METHOD_IN_DIRECT)
#define IOS_IOCTL_HID_GET_WINE_RAWINPUT_HANDLE    IOS_HID_CTL(300, IOS_METHOD_BUFFERED)

/* B5: Wine's virtual mouse and keyboard use raw input handles 1 and 2. */
#define IOS_HIDPAD_RAWINPUT_HANDLE      0x100u
/* Clarification 8: XInput slot n's device is 0x101 + n (win32u keeps one device per handle). */
#define IOS_HIDPAD_SLOT_RAWINPUT_HANDLE(n) (0x101u + (n))
#define IOS_HIDPAD_MAX_POLL_INTERVAL    10000u   /* hidclass's limit for a set poll frequency */
#define IOS_HIDPAD_DEFAULT_INPUT_BUFFERS 32u     /* hidclass's queue length for a new handle */

/* HID_COLLECTION_INFORMATION (Microsoft's hidclass.h), 12 bytes. */
#define IOS_HIDPAD_COLLECTION_INFO_SIZE 12u

/* What the device holds for all handles (owned by the server). */
struct ios_hidpad_device
{
    const struct ios_hidpad_identity *id;
    const struct ios_hidparse_desc *desc;
    struct ios_hidpad_report_state *report_state;
    struct winios_hidpad_output *output;    /* the last output snapshot (A6) */
    uint32_t poll_interval;
    uint32_t rawinput_handle;               /* win32u's handle for the device; 0: the HID pad's */
    /* clarification 5: presence, from the store's samples */
    int published;                          /* the last sample saw the pad published */
    int was_published;                      /* some sample since start saw it published */
    uint64_t unpublished_since_us;          /* when the samples stopped seeing it */
};

/* Clarification 5: the pad stays present until the store has reported it unpublished for this
 * long, so a short gap in the app's publishing never drops a handle or announces a removal. */
#define IOS_HIDPAD_UNPUBLISH_GRACE_US 1000000u

/* Feed one store sample (`connected` = winios_hidpad_get_state's answer at `now_us`) and
 * return whether the pad is present: published now, or unpublished for less than the grace.
 * A pad never published since the server started is not present. */
int ios_hidpad_presence(struct ios_hidpad_device *dev, int connected, uint64_t now_us);

/* What one open handle holds. */
struct ios_hidpad_handle
{
    uint32_t input_buffers;                 /* 2-512, stored only */
    int lost;                               /* the pad stopped being published (QUESTIONS 15-22) */
    int have_last;                          /* B3 pacing: this handle had a report */
    uint64_t last_report_us;
    uint32_t last_packet;
};

void ios_hidpad_handle_init(struct ios_hidpad_handle *handle);

/* Clarification 4 (as amended) and 5: a handle is lost for good once the pad is no longer
 * present (`present` = ios_hidpad_presence), as a pad that was unplugged: hidclass then fails
 * pending and new reads with STATUS_DEVICE_NOT_CONNECTED and every other request with
 * STATUS_DELETE_PENDING, and a reader drops the device instead of keeping an idle one.
 * Publishing the pad again doesn't revive the handle; a new open works. An idle handle of a
 * present pad is never lost. Returns 1 when the handle is lost. */
int ios_hidpad_handle_lost(struct ios_hidpad_handle *handle, int present);

enum ios_hidpad_read_outcome
{
    IOS_HIDPAD_READ_NOW,                    /* complete with a report built from `pad` */
    IOS_HIDPAD_READ_WAIT,                   /* wait for the device clock (B3) */
    IOS_HIDPAD_READ_LOST,                   /* fail with STATUS_DEVICE_NOT_CONNECTED */
};

/* A read on `handle` with the store's newest snapshot `pad`: B3's pacing after the lost-handle
 * rule. `queued` is 1 when older reads of the handle still wait (a new read queues behind).
 * While the pad is present every read gets a report, at least one per 4 ms clock tick, built
 * from the current state (a pad at rest while unpublished within the grace), with the counter
 * and sensor timestamp advancing as on hardware: a reader never sees silence. */
enum ios_hidpad_read_outcome ios_hidpad_read_decide(struct ios_hidpad_device *dev,
                                                    struct ios_hidpad_handle *handle,
                                                    const struct winios_hidpad *pad,
                                                    int queued, uint64_t now_us);

/* Build the report a read delivers and record it as the handle's last (B3). Returns its
 * length; `out` holds dev->id->input_report_len bytes. */
uint32_t ios_hidpad_read_report(struct ios_hidpad_device *dev, struct ios_hidpad_handle *handle,
                                const struct winios_hidpad *pad, uint64_t now_us, uint8_t *out);

#define IOS_HIDPAD_REPLY_MAX 256

struct ios_hidpad_reply
{
    uint32_t status;                        /* an NTSTATUS */
    uint32_t information;                   /* bytes transferred / returned */
    const void *data;                       /* `size` bytes to return (points into `buffer` or
                                               the device's preparsed data) */
    uint32_t size;
    uint8_t buffer[IOS_HIDPAD_REPLY_MAX];
    /* an output report went to the decoder (B4) */
    int output_report;                      /* 1 when an output report passed the checks */
    enum ios_hidpad_output_result output_result;
    struct ios_hidpad_output_flags output_flags;
    int output_changed;                     /* the snapshot changed: hand it to the store */
};

/* The number of bytes of the caller's input buffer in a request whose data is `data_size`
 * bytes and whose output buffer is `out_size`: for direct and neither-method codes the
 * request data is the input buffer followed by the output buffer. */
uint32_t ios_hidpad_input_size(uint32_t code, uint32_t data_size, uint32_t out_size);

/* WriteFile (B4): the lost-handle rule, hidclass's IOCTL_HID_WRITE_REPORT checks, then A6.
 * `pad` is the store's newest snapshot. */
void ios_hidpad_write(struct ios_hidpad_device *dev, struct ios_hidpad_handle *handle,
                      const struct winios_hidpad *pad, uint64_t now_us, const uint8_t *data,
                      uint32_t size, struct ios_hidpad_reply *reply);

/* An IOCTL (B4, B5). `data`/`data_size` is the request data, `out_size` the output buffer's
 * length, `pad` the store's newest snapshot (the lost-handle rule; IOCTL_HID_GET_INPUT_REPORT's
 * report), `now_us` the sensor clock. */
void ios_hidpad_ioctl(struct ios_hidpad_device *dev, struct ios_hidpad_handle *handle, uint32_t code,
                      const uint8_t *data, uint32_t data_size, uint32_t out_size,
                      const struct winios_hidpad *pad, uint64_t now_us, struct ios_hidpad_reply *reply);

#ifdef __cplusplus
}
#endif

#endif
