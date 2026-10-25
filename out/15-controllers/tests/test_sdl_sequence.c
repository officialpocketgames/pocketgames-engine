// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part F (device report of 2026-10-21, an SDL2 game): the calls a reader makes to the
// HID pad, in the order the readers in a session make them, run against the device's request
// rules (hidpad/ios_hidpad_request.c) and Wine's own hidparse output (server/ios_hidparse.c):
//   1. the gate (clarification 4): with no pad published every open is refused, so no reader
//      ever holds a half-working device;
//   2. win32u's raw input enumeration (dlls/win32u/rawinput.c add_device): raw input handle,
//      collection information, collection descriptor (exact size);
//   3. an hidapi-style enumeration and open (SetupDi path, HidD_GetAttributes, strings,
//      preparsed data and caps, HidD_SetNumInputBuffers);
//   4. a DualSense driver's start (feature reports 0x09, 0x20, 0x05 with the buffer passed as
//      input and output, as hidapi does), its first read (64 bytes = USB), pacing, an effects
//      write;
//   5. refusals as hidclass gives them; the generic identity.
// Needs Wine 11.4's include/ (for the IOCTL and status values the copies must match).

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "winioctl.h"
#include "ddk/hidclass.h"

#include "ios_hidpad.h"
#include "ios_hidparse.h"
#include "ios_hidpad_request.h"
#include "ios_hidpad_regplan.h"
#include "test_common.h"

/* the request rules' copies match Wine's values */
_Static_assert(IOS_IOCTL_HID_GET_FEATURE == IOCTL_HID_GET_FEATURE, "GET_FEATURE");
_Static_assert(IOS_IOCTL_HID_SET_FEATURE == IOCTL_HID_SET_FEATURE, "SET_FEATURE");
_Static_assert(IOS_IOCTL_HID_GET_INPUT_REPORT == IOCTL_HID_GET_INPUT_REPORT, "GET_INPUT_REPORT");
_Static_assert(IOS_IOCTL_HID_SET_OUTPUT_REPORT == IOCTL_HID_SET_OUTPUT_REPORT, "SET_OUTPUT_REPORT");
_Static_assert(IOS_IOCTL_HID_GET_COLLECTION_INFORMATION == IOCTL_HID_GET_COLLECTION_INFORMATION, "INFO");
_Static_assert(IOS_IOCTL_HID_GET_COLLECTION_DESCRIPTOR == IOCTL_HID_GET_COLLECTION_DESCRIPTOR, "DESC");
_Static_assert(IOS_IOCTL_HID_GET_PRODUCT_STRING == IOCTL_HID_GET_PRODUCT_STRING, "PRODUCT");
_Static_assert(IOS_IOCTL_HID_GET_MANUFACTURER_STRING == IOCTL_HID_GET_MANUFACTURER_STRING, "MANUFACTURER");
_Static_assert(IOS_IOCTL_HID_GET_SERIALNUMBER_STRING == IOCTL_HID_GET_SERIALNUMBER_STRING, "SERIAL");
_Static_assert(IOS_IOCTL_HID_GET_INDEXED_STRING == IOCTL_HID_GET_INDEXED_STRING, "INDEXED");
_Static_assert(IOS_IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS == IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS, "SET_NUM");
_Static_assert(IOS_IOCTL_GET_NUM_DEVICE_INPUT_BUFFERS == IOCTL_GET_NUM_DEVICE_INPUT_BUFFERS, "GET_NUM");
_Static_assert(IOS_IOCTL_HID_GET_POLL_FREQUENCY_MSEC == IOCTL_HID_GET_POLL_FREQUENCY_MSEC, "GET_POLL");
_Static_assert(IOS_IOCTL_HID_SET_POLL_FREQUENCY_MSEC == IOCTL_HID_SET_POLL_FREQUENCY_MSEC, "SET_POLL");
_Static_assert(IOS_IOCTL_HID_FLUSH_QUEUE == IOCTL_HID_FLUSH_QUEUE, "FLUSH");
_Static_assert(IOS_IOCTL_HID_GET_WINE_RAWINPUT_HANDLE == HID_BUFFER_CTL_CODE(300), "RAWINPUT");
_Static_assert(IOS_STATUS_INVALID_PARAMETER == (uint32_t)STATUS_INVALID_PARAMETER, "status");
_Static_assert(IOS_STATUS_INVALID_USER_BUFFER == (uint32_t)STATUS_INVALID_USER_BUFFER, "status");
_Static_assert(IOS_STATUS_BUFFER_TOO_SMALL == (uint32_t)STATUS_BUFFER_TOO_SMALL, "status");
_Static_assert(IOS_STATUS_BUFFER_OVERFLOW == (uint32_t)STATUS_BUFFER_OVERFLOW, "status");
_Static_assert(IOS_STATUS_INVALID_BUFFER_SIZE == (uint32_t)STATUS_INVALID_BUFFER_SIZE, "status");
_Static_assert(IOS_STATUS_NOT_SUPPORTED == (uint32_t)STATUS_NOT_SUPPORTED, "status");
_Static_assert(IOS_STATUS_DELETE_PENDING == (uint32_t)STATUS_DELETE_PENDING, "status");
_Static_assert(IOS_STATUS_DEVICE_NOT_CONNECTED == (uint32_t)STATUS_DEVICE_NOT_CONNECTED, "status");
_Static_assert(IOS_HIDPAD_COLLECTION_INFO_SIZE == sizeof(HID_COLLECTION_INFORMATION), "info size");

static struct ios_hidpad_identity id;
static struct ios_hidparse_desc desc;
static struct ios_hidpad_report_state report_state;
static struct pgios_hidpad_output output;
static struct ios_hidpad_device dev;
static struct ios_hidpad_handle handle;
static struct pgios_hidpad pad;
static struct ios_hidpad_reply reply;
static uint64_t now = 1000;      /* the device clock the requests see, in microseconds */

static void open_device(const char *which, const char *name)
{
    memset(&dev, 0, sizeof(dev));
    ios_hidparse_release(&desc);
    ios_hidpad_identity_select(which, name, &id);
    CHECK_EQ(ios_hidparse_parse(id.descriptor, id.descriptor_len, &desc), 1);
    memset(&report_state, 0, sizeof(report_state));
    memset(&output, 0, sizeof(output));
    dev.id = &id;
    dev.desc = &desc;
    dev.report_state = &report_state;
    dev.output = &output;
    dev.poll_interval = 0;
    ios_hidpad_handle_init(&handle);
    memset(&pad, 0, sizeof(pad));
    pad.connected = 1;           /* the pad is published: the gate is open */
    now = 1000;
    CHECK_EQ(ios_hidpad_presence(&dev, pad.connected, now), 1);
}

/* DeviceIoControl as ntdll sends it: the input buffer, then (direct and neither methods) the
 * output buffer's contents, as the request data; the reply copied into `out`. */
static uint32_t device_io_control(uint32_t code, const void *in, uint32_t in_len, void *out, uint32_t out_len)
{
    static uint8_t data[8192];
    uint32_t size = 0, n;

    if (in && in_len) { memcpy(data, in, in_len); size = in_len; }
    if ((code & 3) != IOS_METHOD_BUFFERED && out && out_len) { memcpy(data + size, out, out_len); size += out_len; }
    ios_hidpad_ioctl(&dev, &handle, code, size ? data : NULL, size, out_len, &pad, now, &reply);
    if (reply.status == IOS_STATUS_SUCCESS && out && reply.size)
    {
        n = reply.size < out_len ? reply.size : out_len;
        memcpy(out, reply.data, n);
    }
    return reply.status;
}

static unsigned int le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* UTF-16 reply to ASCII, for the checks */
static void narrow(const uint8_t *w, uint32_t bytes, char *out, size_t size)
{
    size_t i;
    for (i = 0; i + 1 < size && 2 * i + 1 < bytes && w[2 * i]; i++) out[i] = (char)w[2 * i];
    out[i] = 0;
}

static void test_gate(void)
{
    uint32_t refusals = 0;
    int log_refusal;

    /* Xbox mode: the store reports no HID pad. Every open is refused, the raw input
     * enumeration's and hidapi's alike, so each reader leaves the device out entirely. */
    CHECK_EQ(ios_hidpad_open_allowed(0, &refusals, &log_refusal), 0);
    CHECK_EQ(ios_hidpad_open_allowed(0, &refusals, &log_refusal), 0);
    CHECK_EQ(refusals, 2);
    /* PlayStation mode: opens succeed */
    CHECK_EQ(ios_hidpad_open_allowed(1, &refusals, &log_refusal), 1);
    CHECK_EQ(log_refusal, 0);
}

static void test_rawinput_enumeration(void)
{
    uint8_t buf[8192];
    uint32_t descriptor_size;

    open_device("dualsense", NULL);

    /* IOCTL_HID_GET_WINE_RAWINPUT_HANDLE, 4 bytes out */
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_WINE_RAWINPUT_HANDLE, NULL, 0, buf, 4), 0);
    CHECK_EQ(le32(buf), 0x100);
    CHECK_EQ(reply.information, 4);

    /* IOCTL_HID_GET_COLLECTION_INFORMATION */
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_COLLECTION_INFORMATION, NULL, 0, buf, 12), 0);
    descriptor_size = le32(buf);
    CHECK_EQ(descriptor_size, desc.preparsed_len);
    CHECK_EQ(buf[4], 0);                           /* not polled */
    CHECK_EQ(le16(buf + 6), 0x054c);
    CHECK_EQ(le16(buf + 8), 0x0ce6);
    CHECK_EQ(le16(buf + 10), 0x0100);

    /* IOCTL_HID_GET_COLLECTION_DESCRIPTOR into exactly DescriptorSize bytes */
    CHECK(descriptor_size <= sizeof(buf));
    memset(buf, 0, sizeof(buf));
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_COLLECTION_DESCRIPTOR, NULL, 0, buf, descriptor_size), 0);
    CHECK_EQ(reply.size, descriptor_size);
    CHECK(!memcmp(buf, desc.preparsed, descriptor_size));
    CHECK(!memcmp(buf, "HidP KDR", 8));
    CHECK_EQ(le16(buf + 8), 0x05);                 /* usage: Game Pad */
    CHECK_EQ(le16(buf + 10), 0x01);                /* usage page: Generic Desktop */

    /* one byte short */
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_COLLECTION_DESCRIPTOR, NULL, 0, buf, descriptor_size - 1),
             IOS_STATUS_INVALID_BUFFER_SIZE);
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_COLLECTION_INFORMATION, NULL, 0, buf, 8),
             IOS_STATUS_BUFFER_OVERFLOW);
}

static void test_hidapi_enumerate_and_open(void)
{
    struct ios_hidpad_reg_key keys[IOS_HIDPAD_REG_KEYS];
    uint8_t buf[4096];
    char text[256], path[256];
    uint32_t n;

    open_device("dualsense", NULL);

    /* the interface path SetupDi reports: the registry's SymbolicLink (Part C) */
    CHECK_EQ(ios_hidpad_registry_plan(&id, keys), 4);
    CHECK_STR(keys[2].values[0].name, "SymbolicLink");
    ios_hidpad_user_path(&id, path, sizeof(path));
    CHECK_STR(keys[2].values[0].text, path);
    CHECK(!strncmp(path, "\\\\?\\HID#VID_054C&PID_0CE6", 25));

    /* HidD_GetAttributes */
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_COLLECTION_INFORMATION, NULL, 0, buf, 12), 0);
    CHECK_EQ(le16(buf + 6), 0x054c);
    CHECK_EQ(le16(buf + 8), 0x0ce6);

    /* strings, into hidapi-sized buffers */
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_SERIALNUMBER_STRING, NULL, 0, buf, 512), 0);
    narrow(buf, reply.size, text, sizeof(text));
    CHECK_STR(text, "02a14d616465");
    CHECK_EQ(reply.size, 2 * 13);                  /* with the terminator */
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_MANUFACTURER_STRING, NULL, 0, buf, 512), 0);
    narrow(buf, reply.size, text, sizeof(text));
    CHECK_STR(text, "Sony Interactive Entertainment");
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_PRODUCT_STRING, NULL, 0, buf, 512), 0);
    narrow(buf, reply.size, text, sizeof(text));
    CHECK_STR(text, "DualSense Wireless Controller");
    /* HidD_GetIndexedString(2) with a language id in the high word */
    n = 0x04090002;
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_INDEXED_STRING, &n, 4, buf, 512), 0);
    narrow(buf, reply.size, text, sizeof(text));
    CHECK_STR(text, "DualSense Wireless Controller");
    n = 7;
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_INDEXED_STRING, &n, 4, buf, 512), IOS_STATUS_INVALID_PARAMETER);
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_PRODUCT_STRING, NULL, 0, buf, 4), IOS_STATUS_BUFFER_TOO_SMALL);

    /* HidD_GetPreparsedData + HidP_GetCaps: a Generic Desktop game pad, 64-byte input report */
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_COLLECTION_DESCRIPTOR, NULL, 0, buf, desc.preparsed_len), 0);
    CHECK_EQ(le16(buf + 10), 0x01);
    CHECK_EQ(le16(buf + 8), 0x05);
    CHECK_EQ(desc.input_len, 64);
    CHECK_EQ(desc.output_len, 48);

    /* hid_open_path: HidD_SetNumInputBuffers(64), read back */
    n = 64;
    CHECK_EQ(device_io_control(IOS_IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS, &n, 4, NULL, 0), 0);
    CHECK_EQ(device_io_control(IOS_IOCTL_GET_NUM_DEVICE_INPUT_BUFFERS, NULL, 0, buf, 4), 0);
    CHECK_EQ(le32(buf), 64);
    n = 1;
    CHECK_EQ(device_io_control(IOS_IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS, &n, 4, NULL, 0), IOS_STATUS_INVALID_PARAMETER);
    CHECK_EQ(handle.input_buffers, 64);
}

static void test_dualsense_driver(void)
{
    uint8_t in[64], out[64], report[64], fx[48], bt[78];
    char serial[32];
    struct pgios_hidpad store;

    open_device("dualsense", NULL);

    /* feature reports, the buffer passed as input and output (hidapi's hid_get_feature_report) */
    memset(in, 0, sizeof(in));
    in[0] = 0x09;
    memcpy(out, in, sizeof(out));
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_FEATURE, in, 64, out, 64), 0);
    CHECK_EQ(reply.information, 20);
    CHECK(reply.information >= 7);
    snprintf(serial, sizeof(serial), "%.2x-%.2x-%.2x-%.2x-%.2x-%.2x", out[6], out[5], out[4], out[3], out[2], out[1]);
    CHECK_STR(serial, "02-a1-4d-61-64-65");

    in[0] = 0x20;
    memcpy(out, in, sizeof(out));
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_FEATURE, in, 64, out, 64), 0);
    CHECK_EQ(reply.information, 64);
    CHECK(reply.information >= 46);
    CHECK_EQ(le16(out + 44), 0x0224);

    /* the calibration report, the id only in the output buffer (hid.dll's HidD_GetFeature) */
    memset(out, 0, sizeof(out));
    out[0] = 0x05;
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_FEATURE, NULL, 0, out, 64), 0);
    CHECK_EQ(reply.information, 41);
    CHECK(reply.information >= 35);
    CHECK_EQ((int16_t)le16(out + 7), 8640);
    /* a buffer shorter than the report: refused */
    memset(out, 0, sizeof(out));
    out[0] = 0x05;
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_FEATURE, NULL, 0, out, 16), IOS_STATUS_INVALID_PARAMETER);
    /* an id the descriptor doesn't have */
    out[0] = 0x31;
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_FEATURE, NULL, 0, out, 64), IOS_STATUS_INVALID_PARAMETER);

    /* the first read of the handle completes at once with 64 bytes: the driver's "USB" test */
    memset(&store, 0, sizeof(store));
    store.connected = 1;
    store.packet = 3;
    store.battery = 80;
    CHECK_EQ(ios_hidpad_read_ready(10000, 0, 0, 0, store.packet), 1);
    CHECK_EQ(ios_hidpad_build_input(&id, &store, &report_state, 10000, report), 64);
    CHECK_EQ(report[0], 0x01);
    /* the next read 1 ms later with nothing new waits for the device clock; 4 ms later it doesn't */
    CHECK_EQ(ios_hidpad_read_ready(11000, 1, 10000, 3, store.packet), 0);
    CHECK_EQ(ios_hidpad_read_ready(14000, 1, 10000, 3, store.packet), 1);
    /* a change is delivered after 1 ms */
    store.packet = 4;
    CHECK_EQ(ios_hidpad_read_ready(11000, 1, 10000, 3, store.packet), 1);

    /* IOCTL_HID_GET_INPUT_REPORT */
    memset(out, 0, sizeof(out));
    out[0] = 0x01;
    pad = store;
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_INPUT_REPORT, NULL, 0, out, 64), 0);
    CHECK_EQ(reply.information, 64);
    CHECK_EQ(out[0], 0x01);
    CHECK_EQ(out[53], 0x08);                       /* 80 %, discharging */

    /* an effects report (hid_write, 48 bytes): light bar and rumble */
    memset(fx, 0, sizeof(fx));
    fx[0] = 0x02;
    fx[1] = 0x01 | 0x02;                           /* compatible vibration, haptics select */
    fx[2] = 0x04;                                  /* light bar */
    fx[3] = 0x20;
    fx[4] = 0x40;
    fx[45] = 0xff;
    ios_hidpad_write(&dev, &handle, &pad, now, fx, sizeof(fx), &reply);
    CHECK_EQ(reply.status, 0);
    CHECK_EQ(reply.information, 48);
    CHECK_EQ(reply.output_report, 1);
    CHECK_EQ(reply.output_result, IOS_HIDPAD_OUTPUT_FOLDED);
    CHECK_EQ(reply.output_changed, 1);
    CHECK_EQ(output.rumble_valid, 1);
    CHECK_EQ(output.rumble_right, 0x20);
    CHECK_EQ(output.rumble_left, 0x40);
    CHECK_EQ(output.lightbar_valid, 1);
    CHECK_EQ(output.red, 0xff);
    /* the same report again changes nothing */
    ios_hidpad_write(&dev, &handle, &pad, now, fx, sizeof(fx), &reply);
    CHECK_EQ(reply.output_changed, 0);
    /* the same through IOCTL_HID_SET_OUTPUT_REPORT, with a new colour */
    fx[46] = 0x10;
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_SET_OUTPUT_REPORT, fx, 48, NULL, 0), 0);
    CHECK_EQ(reply.information, 48);
    CHECK_EQ(reply.output_changed, 1);
    CHECK_EQ(output.green, 0x10);

    /* refusals as hidclass gives them */
    memset(bt, 0, sizeof(bt));
    bt[0] = 0x31;
    ios_hidpad_write(&dev, &handle, &pad, now, bt, sizeof(bt), &reply);
    CHECK_EQ(reply.status, IOS_STATUS_INVALID_PARAMETER);    /* no output report 0x31 */
    CHECK_EQ(reply.output_report, 0);
    ios_hidpad_write(&dev, &handle, &pad, now, fx, 10, &reply);
    CHECK_EQ(reply.status, IOS_STATUS_INVALID_PARAMETER);    /* shorter than the report */
    ios_hidpad_write(&dev, &handle, &pad, now, NULL, 0, &reply);
    CHECK_EQ(reply.status, IOS_STATUS_INVALID_USER_BUFFER);
    CHECK_EQ(device_io_control(0x0022c0ffu, NULL, 0, out, 4), IOS_STATUS_NOT_SUPPORTED);

    /* features can be set (accepted) */
    memset(in, 0, sizeof(in));
    in[0] = 0x08;
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_SET_FEATURE, in, 48, NULL, 0), 0);
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_SET_FEATURE, in, 10, NULL, 0), IOS_STATUS_INVALID_PARAMETER);

    /* poll frequency and flush */
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_POLL_FREQUENCY_MSEC, NULL, 0, out, 4), 0);
    CHECK_EQ(le32(out), 0);
    {
        uint32_t ms = 20000;
        CHECK_EQ(device_io_control(IOS_IOCTL_HID_SET_POLL_FREQUENCY_MSEC, &ms, 4, NULL, 0), 0);
        CHECK_EQ(dev.poll_interval, 10000);
    }
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_FLUSH_QUEUE, NULL, 0, NULL, 0), 0);
}

static void test_generic(void)
{
    uint8_t buf[512];
    char text[128];

    open_device("generic", "Backbone One");
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_COLLECTION_INFORMATION, NULL, 0, buf, 12), 0);
    CHECK_EQ(le16(buf + 6), 0x1209);
    CHECK_EQ(le16(buf + 8), 0x4d47);
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_PRODUCT_STRING, NULL, 0, buf, sizeof(buf)), 0);
    narrow(buf, reply.size, text, sizeof(text));
    CHECK_STR(text, "Backbone One");
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_SERIALNUMBER_STRING, NULL, 0, buf, sizeof(buf)), 0);
    CHECK_EQ(reply.size, 2);                       /* an empty string: the terminator */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x01;
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_INPUT_REPORT, NULL, 0, buf, 14), 0);
    CHECK_EQ(reply.information, 14);
    CHECK_EQ(buf[0], 0x01);
    /* no output or feature reports */
    memset(buf, 0, sizeof(buf));
    buf[0] = 0x01;
    ios_hidpad_write(&dev, &handle, &pad, now, buf, 14, &reply);
    CHECK_EQ(reply.status, IOS_STATUS_INVALID_PARAMETER);
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_FEATURE, NULL, 0, buf, 64), IOS_STATUS_INVALID_PARAMETER);
}

/* Clarification 5 (device report of build 36, the PlayStation look): the pad counts as present
 * until the store has reported it unpublished for a full second. A never-published pad isn't
 * present; a short gap is no removal. */
static void test_presence_grace(void)
{
    struct ios_hidpad_device d;

    memset(&d, 0, sizeof(d));
    CHECK_EQ(ios_hidpad_presence(&d, 0, 0), 0);                   /* never published */
    CHECK_EQ(ios_hidpad_presence(&d, 0, 5000000), 0);
    CHECK_EQ(ios_hidpad_presence(&d, 1, 6000000), 1);
    CHECK_EQ(ios_hidpad_presence(&d, 0, 6100000), 1);             /* the gap starts */
    CHECK_EQ(d.unpublished_since_us, 6100000);
    CHECK_EQ(ios_hidpad_presence(&d, 0, 7099999), 1);             /* still within a second */
    CHECK_EQ(ios_hidpad_presence(&d, 0, 6000000), 1);             /* an older sample: within */
    CHECK_EQ(ios_hidpad_presence(&d, 1, 7099999), 1);             /* back: the gap is forgotten */
    CHECK_EQ(ios_hidpad_presence(&d, 0, 8000000), 1);             /* a new gap starts here */
    CHECK_EQ(d.unpublished_since_us, 8000000);
    CHECK_EQ(ios_hidpad_presence(&d, 0, 8999999), 1);
    CHECK_EQ(ios_hidpad_presence(&d, 0, 9000000), 0);             /* a full second: gone */
    CHECK_EQ(ios_hidpad_presence(&d, 0, 60000000), 0);
    CHECK_EQ(ios_hidpad_presence(&d, 1, 61000000), 1);            /* published again */
}

/* The device reports' case (QUESTIONS 15-22, clarification 5): an SDL-like reader opened the
 * DualSense while the on-screen pad published it. While the pad is present every read gets a
 * report, also when the handle sat idle and also across a short gap in publishing (a game
 * loading), so the reader never drops a live pad. Once the app has stopped publishing it for a
 * second (back to the Xbox look), the handle acts as an unplugged pad's, so the reader drops the
 * DualSense and the XInput slot is the only controller left. */
static void test_gate_closes_on_open_handle(void)
{
    uint8_t report[64], out[64], fx[48];
    struct ios_hidpad_handle second;
    uint32_t refusals = 0;

    open_device("dualsense", NULL);
    CHECK_EQ(ios_hidpad_open_allowed(ios_hidpad_presence(&dev, pad.connected, now), &refusals, NULL), 1);

    /* published: the first read completes, the next waits for the clock */
    pad.packet = 10;
    CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 0, 1000), IOS_HIDPAD_READ_NOW);
    CHECK_EQ(ios_hidpad_read_report(&dev, &handle, &pad, 1000, report), 64);
    CHECK_EQ(handle.have_last, 1);
    CHECK_EQ(handle.last_packet, 10);
    CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 0, 1500), IOS_HIDPAD_READ_WAIT);
    CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 1, 9000), IOS_HIDPAD_READ_WAIT);   /* queued */
    CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 0, 5000), IOS_HIDPAD_READ_NOW);
    CHECK_EQ(ios_hidpad_read_report(&dev, &handle, &pad, 5000, report), 64);

    /* steady input with nothing pressed: one report per 4 ms clock tick, the packet unchanged */
    for (now = 9000; now < 9000 + 4000 * 50; now += 4000)
    {
        CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 0, now), IOS_HIDPAD_READ_NOW);
        CHECK_EQ(ios_hidpad_read_report(&dev, &handle, &pad, now, report), 64);
        CHECK_EQ(report[0], 0x01);
    }

    /* the handle sits idle for a minute (no reads): it isn't lost, the next read completes */
    now = 60000000;
    CHECK_EQ(ios_hidpad_presence(&dev, pad.connected, now), 1);
    CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 0, now), IOS_HIDPAD_READ_NOW);
    CHECK_EQ(handle.lost, 0);
    memset(out, 0, sizeof(out));
    out[0] = 0x05;
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_FEATURE, NULL, 0, out, 64), 0);

    /* a gap shorter than a second (the app re-publishing while a game loads): reads keep
     * completing with a pad at rest, nothing is lost, the gap is forgotten */
    memset(&pad, 0, sizeof(pad));
    now = 61000000;
    CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 0, now), IOS_HIDPAD_READ_NOW);
    CHECK_EQ(ios_hidpad_read_report(&dev, &handle, &pad, now, report), 64);
    CHECK_EQ(report[0], 0x01);
    now = 61900000;
    CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 0, now), IOS_HIDPAD_READ_NOW);
    CHECK_EQ(ios_hidpad_read_report(&dev, &handle, &pad, now, report), 64);
    out[0] = 0x05;
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_FEATURE, NULL, 0, out, 64), 0);
    pad.connected = 1;
    pad.packet = 20;
    now = 62000000;
    CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 0, now), IOS_HIDPAD_READ_NOW);
    CHECK_EQ(handle.lost, 0);

    /* the app stops publishing the pad for good: within the second the handle still works */
    memset(&pad, 0, sizeof(pad));
    pad.packet = 21;
    now = 70000000;
    CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 0, now), IOS_HIDPAD_READ_NOW);
    now = 70999999;
    CHECK_EQ(ios_hidpad_open_allowed(ios_hidpad_presence(&dev, pad.connected, now), &refusals, NULL), 1);
    CHECK_EQ(handle.lost, 0);
    /* a second later it is lost */
    now = 71000000;
    CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 0, now), IOS_HIDPAD_READ_LOST);
    CHECK_EQ(handle.lost, 1);
    memset(out, 0, sizeof(out));
    out[0] = 0x05;
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_FEATURE, NULL, 0, out, 64), IOS_STATUS_DELETE_PENDING);
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_COLLECTION_INFORMATION, NULL, 0, out, 12), IOS_STATUS_DELETE_PENDING);
    memset(fx, 0, sizeof(fx));
    fx[0] = 0x02;
    fx[1] = 0x01;
    fx[3] = 0x80;
    ios_hidpad_write(&dev, &handle, &pad, now, fx, sizeof(fx), &reply);
    CHECK_EQ(reply.status, IOS_STATUS_DELETE_PENDING);
    CHECK_EQ(reply.output_report, 0);
    CHECK_EQ(output.rumble_valid, 0);                /* nothing reached the snapshot */
    /* and new opens are refused (clarification 4) */
    CHECK_EQ(ios_hidpad_open_allowed(ios_hidpad_presence(&dev, pad.connected, now), &refusals, NULL), 0);

    /* published again: the old handle stays lost (a reader reopens, as after a replug) */
    pad.connected = 1;
    pad.packet = 22;
    now = 72000000;
    CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 0, now), IOS_HIDPAD_READ_LOST);
    CHECK_EQ(device_io_control(IOS_IOCTL_HID_GET_WINE_RAWINPUT_HANDLE, NULL, 0, out, 4), IOS_STATUS_DELETE_PENDING);
    /* a new handle works */
    CHECK_EQ(ios_hidpad_open_allowed(ios_hidpad_presence(&dev, pad.connected, now), &refusals, NULL), 1);
    ios_hidpad_handle_init(&second);
    CHECK_EQ(second.lost, 0);
    CHECK_EQ(second.input_buffers, IOS_HIDPAD_DEFAULT_INPUT_BUFFERS);
    CHECK_EQ(ios_hidpad_read_decide(&dev, &second, &pad, 0, now), IOS_HIDPAD_READ_NOW);
    CHECK_EQ(ios_hidpad_read_report(&dev, &second, &pad, now, report), 64);
    CHECK_EQ(report[0], 0x01);
}

int main(void)
{
    test_gate();
    test_rawinput_enumeration();
    test_hidapi_enumerate_and_open();
    test_dualsense_driver();
    test_generic();
    test_presence_grace();
    test_gate_closes_on_open_handle();
    ios_hidparse_release(&desc);
    return test_report("test_sdl_sequence");
}
