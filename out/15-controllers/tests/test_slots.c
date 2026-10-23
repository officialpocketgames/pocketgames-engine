// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 clarification 8 (device report of TestFlight build 9): the XInput host slots as HID
// devices, with the pieces as they ship:
//   a  the registry plan of each slot device (ntdll/ios_hidpad_regplan.c);
//   b  the slot identity, names and descriptor (hidpad/ios_hidpad_xinput.c), the descriptor
//      parsed by Wine's own hidparse (server/ios_hidparse.c), the input report field by field,
//      and the request rules on a slot device (hidpad/ios_hidpad_request.c) in the order
//      win32u's raw input list, an hidapi-style reader and dinput use them;
//   c  the raw input handle per slot and the WM_INPUT schedule (ios_hidpad_rawinput_due);
//   f  the listing rule composed with the 1 s grace as the server composes them.
// Needs Wine 11.4's include/ for the preparsed data layout (wine/hid.h).

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
#include "ios_hidpad_request.h"
#include "ios_hidpad_regplan.h"
#include "test_common.h"

#define XA      0x1000u
#define XB      0x2000u
#define XX      0x4000u
#define XY      0x8000u
#define XLB     0x0100u
#define XRB     0x0200u
#define XBACK   0x0020u
#define XSTART  0x0010u
#define XLS     0x0040u
#define XRS     0x0080u
#define XGUIDE  0x0400u
#define XUP     0x0001u
#define XDOWN   0x0002u
#define XLEFT   0x0004u
#define XRIGHT  0x0008u

static unsigned int le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static void narrow(const uint8_t *w, uint32_t bytes, char *out, size_t size)
{
    size_t i;
    for (i = 0; i + 1 < size && 2 * i + 1 < bytes && w[2 * i]; i++) out[i] = (char)w[2 * i];
    out[i] = 0;
}

static void test_identity_and_names(void)
{
    struct ios_hidpad_identity id;
    char buf[512];
    unsigned int slot;

    CHECK_EQ(ios_hidpad_identity_slot(4, &id), 0);
    CHECK_EQ(id.kind, IOS_HIDPAD_NONE);
    CHECK_EQ(ios_hidpad_link(&id, buf, sizeof(buf)), 0);

    for (slot = 0; slot < IOS_HIDPAD_SLOTS; slot++)
    {
        char expect[256];

        CHECK_EQ(ios_hidpad_identity_slot(slot, &id), 1);
        CHECK_EQ(id.kind, IOS_HIDPAD_XINPUT);
        CHECK_EQ(id.slot, slot);
        CHECK_EQ(id.vendor_id, 0x045e);
        CHECK_EQ(id.product_id, 0x028e);
        CHECK_EQ(id.version, 0x0114);
        CHECK_STR(id.product, "Controller (XBOX 360 For Windows)");
        CHECK_STR(id.serial, "");
        CHECK_EQ(id.input_report_len, 15);

        snprintf(expect, sizeof(expect),
                 "\\\\?\\HID#VID_045E&PID_028E&IG_0%u#9&4d616465&1&000%u#{4d1e55b2-f16f-11cf-88cb-001111000030}", slot, slot);
        CHECK(ios_hidpad_user_path(&id, buf, sizeof(buf)) > 0);
        CHECK_STR(buf, expect);
        snprintf(expect, sizeof(expect),
                 "\\??\\HID#VID_045E&PID_028E&IG_0%u#9&4d616465&1&000%u#{4d1e55b2-f16f-11cf-88cb-001111000030}", slot, slot);
        ios_hidpad_symlink_path(&id, buf, sizeof(buf));
        CHECK_STR(buf, expect);
        snprintf(expect, sizeof(expect),
                 "##?#HID#VID_045E&PID_028E&IG_0%u#9&4d616465&1&000%u#{4d1e55b2-f16f-11cf-88cb-001111000030}", slot, slot);
        ios_hidpad_registry_link_key(&id, buf, sizeof(buf));
        CHECK_STR(buf, expect);
        snprintf(expect, sizeof(expect), "HID\\VID_045E&PID_028E&IG_0%u\\9&4d616465&1&000%u", slot, slot);
        ios_hidpad_instance_id(&id, buf, sizeof(buf));
        CHECK_STR(buf, expect);
        /* hardware ids as Windows gives the collection: REV before IG_ */
        snprintf(expect, sizeof(expect), "HID\\VID_045E&PID_028E&REV_0114&IG_0%u", slot);
        ios_hidpad_hardware_id(&id, 0, buf, sizeof(buf));
        CHECK_STR(buf, expect);
        snprintf(expect, sizeof(expect), "HID\\VID_045E&PID_028E&IG_0%u", slot);
        ios_hidpad_hardware_id(&id, 1, buf, sizeof(buf));
        CHECK_STR(buf, expect);
        ios_hidpad_hardware_id(&id, 2, buf, sizeof(buf));
        CHECK_STR(buf, "HID\\VID_045E&UP:0001_U:0005");
    }

    /* the DualSense's MI_ ids are unchanged by the IG_ rule */
    ios_hidpad_identity_get(IOS_HIDPAD_DUALSENSE, NULL, &id);
    ios_hidpad_hardware_id(&id, 0, buf, sizeof(buf));
    CHECK_STR(buf, "HID\\VID_054C&PID_0CE6&REV_0100&MI_03");
}

static struct winios_hidpad slot_state(uint16_t buttons, int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                                       uint8_t lt, uint8_t rt)
{
    struct winios_gamepad g;
    struct winios_hidpad pad;

    memset(&g, 0, sizeof(g));
    g.packet = 7;
    g.buttons = buttons;
    g.lx = lx;
    g.ly = ly;
    g.rx = rx;
    g.ry = ry;
    g.left_trigger = lt;
    g.right_trigger = rt;
    g.connected = 1;
    ios_hidpad_from_gamepad(&g, 1, &pad);
    return pad;
}

static void test_input_report(void)
{
    struct winios_hidpad pad;
    struct winios_gamepad g;
    uint8_t r[IOS_HIDPAD_XINPUT_INPUT_LEN];

    /* at rest */
    pad = slot_state(0, 0, 0, 0, 0, 0, 0);
    CHECK_EQ(pad.packet, 7);
    CHECK_EQ(pad.connected, 1);
    CHECK_EQ(ios_hidpad_build_xinput_input(&pad, r), 15);
    CHECK_EQ(r[0], 0);                               /* no report ids */
    CHECK_EQ(le16(r + 1), 32768);
    CHECK_EQ(le16(r + 3), 32767);                    /* Y grows downwards */
    CHECK_EQ(le16(r + 5), 32768);
    CHECK_EQ(le16(r + 7), 32767);
    CHECK_EQ(le16(r + 9), 0x8000);
    CHECK_EQ(le16(r + 11), 0);                       /* no buttons, hat null */
    CHECK_EQ(le16(r + 13), 0);

    /* extremes */
    pad = slot_state(0, -32768, 32767, 32767, -32768, 255, 0);
    ios_hidpad_build_xinput_input(&pad, r);
    CHECK_EQ(le16(r + 1), 0);
    CHECK_EQ(le16(r + 3), 0);                        /* fully up */
    CHECK_EQ(le16(r + 5), 65535);
    CHECK_EQ(le16(r + 7), 65535);                    /* fully down */
    CHECK_EQ(le16(r + 9), 0x8000 + 255 * 128);       /* left trigger raises Z */
    pad = slot_state(0, 0, 0, 0, 0, 0, 255);
    ios_hidpad_build_xinput_input(&pad, r);
    CHECK_EQ(le16(r + 9), 0x8000 - 255 * 128);
    pad = slot_state(0, 0, 0, 0, 0, 255, 255);
    ios_hidpad_build_xinput_input(&pad, r);
    CHECK_EQ(le16(r + 9), 0x8000);                   /* both cancel, as on Windows */

    /* buttons 1-10 in order; the guide button isn't in the collection */
    pad = slot_state(XA, 0, 0, 0, 0, 0, 0);
    ios_hidpad_build_xinput_input(&pad, r);
    CHECK_EQ(le16(r + 11), 0x001);
    pad = slot_state(XB | XX | XY, 0, 0, 0, 0, 0, 0);
    ios_hidpad_build_xinput_input(&pad, r);
    CHECK_EQ(le16(r + 11), 0x00e);
    pad = slot_state(XLB | XRB | XBACK | XSTART | XLS | XRS | XGUIDE, 0, 0, 0, 0, 0, 0);
    ios_hidpad_build_xinput_input(&pad, r);
    CHECK_EQ(le16(r + 11), 0x3f0);

    /* hat: 1-8 clockwise from north in bits 10-13, opposite directions cancel */
    pad = slot_state(XUP, 0, 0, 0, 0, 0, 0);
    ios_hidpad_build_xinput_input(&pad, r);
    CHECK_EQ(le16(r + 11) >> 10, 1);
    pad = slot_state(XUP | XRIGHT, 0, 0, 0, 0, 0, 0);
    ios_hidpad_build_xinput_input(&pad, r);
    CHECK_EQ(le16(r + 11) >> 10, 2);
    pad = slot_state(XRIGHT, 0, 0, 0, 0, 0, 0);
    ios_hidpad_build_xinput_input(&pad, r);
    CHECK_EQ(le16(r + 11) >> 10, 3);
    pad = slot_state(XDOWN, 0, 0, 0, 0, 0, 0);
    ios_hidpad_build_xinput_input(&pad, r);
    CHECK_EQ(le16(r + 11) >> 10, 5);
    pad = slot_state(XDOWN | XLEFT, 0, 0, 0, 0, 0, 0);
    ios_hidpad_build_xinput_input(&pad, r);
    CHECK_EQ(le16(r + 11) >> 10, 6);
    pad = slot_state(XUP | XLEFT | XA, 0, 0, 0, 0, 0, 0);
    ios_hidpad_build_xinput_input(&pad, r);
    CHECK_EQ(le16(r + 11), (8u << 10) | 1u);
    pad = slot_state(XUP | XDOWN, 0, 0, 0, 0, 0, 0);
    ios_hidpad_build_xinput_input(&pad, r);
    CHECK_EQ(le16(r + 11) >> 10, 0);

    /* an unlisted slot is a pad at rest, its packet kept */
    memset(&g, 0, sizeof(g));
    g.packet = 9;
    g.buttons = XA;
    g.lx = 1000;
    ios_hidpad_from_gamepad(&g, 0, &pad);
    CHECK_EQ(pad.connected, 0);
    CHECK_EQ(pad.packet, 9);
    CHECK_EQ(pad.buttons, 0);
    ios_hidpad_build_xinput_input(&pad, r);
    CHECK_EQ(le16(r + 1), 32768);
    CHECK_EQ(le16(r + 11), 0);
}

static const struct hid_value_caps *find_caps(const struct hid_preparsed_data *p, USAGE page, USAGE usage)
{
    const struct hid_value_caps *caps = HID_INPUT_VALUE_CAPS(p);
    unsigned int i;

    for (i = 0; i < p->input_caps_count; i++)
        if (caps[i].usage_page == page && caps[i].usage_min <= usage && caps[i].usage_max >= usage)
            return caps + i;
    return NULL;
}

static void check_axis(const struct hid_preparsed_data *p, USAGE usage, unsigned int start_byte)
{
    const struct hid_value_caps *caps = find_caps(p, 0x01, usage);

    CHECK(caps != NULL);
    if (!caps) return;
    CHECK_EQ(caps->report_id, 0);
    CHECK_EQ(caps->start_byte, start_byte);
    CHECK_EQ(caps->start_bit, 0);
    CHECK_EQ(caps->bit_size, 16);
    CHECK_EQ(caps->logical_min, 0);
    CHECK_EQ(caps->logical_max, 65535);
    CHECK(!(caps->flags & HID_VALUE_CAPS_IS_BUTTON));
    CHECK(caps->flags & HID_VALUE_CAPS_IS_ABSOLUTE);
}

static void test_descriptor(struct ios_hidparse_desc *desc)
{
    const struct hid_preparsed_data *p;
    const struct hid_value_caps *caps;

    CHECK_EQ(ios_hidparse_parse(ios_hidpad_xinput_descriptor, ios_hidpad_xinput_descriptor_len, desc), 1);
    CHECK_EQ(desc->usage_page, 0x01);
    CHECK_EQ(desc->usage, 0x05);
    CHECK_EQ(desc->input_len, IOS_HIDPAD_XINPUT_INPUT_LEN);
    CHECK_EQ(desc->output_len, 0);                   /* no output reports: rumble is XInput's */
    CHECK_EQ(desc->feature_len, 0);
    CHECK_EQ(desc->report_count, 1);
    CHECK_EQ(desc->reports[0].id, 0);
    CHECK(ios_hidparse_find_report(desc, IOS_HIDPARSE_INPUT, 0) != NULL);
    CHECK(ios_hidparse_find_report(desc, IOS_HIDPARSE_OUTPUT, 0) == NULL);
    CHECK(ios_hidparse_find_report(desc, IOS_HIDPARSE_FEATURE, 0) == NULL);

    p = desc->preparsed;
    CHECK(!memcmp(p->magic, "HidP KDR", 8));
    CHECK_EQ(p->usage, 0x05);
    CHECK_EQ(p->usage_page, 0x01);
    CHECK_EQ(p->input_report_byte_length, 15);
    check_axis(p, 0x30, 1);                          /* X */
    check_axis(p, 0x31, 3);                          /* Y */
    check_axis(p, 0x33, 5);                          /* Rx */
    check_axis(p, 0x34, 7);                          /* Ry */
    check_axis(p, 0x32, 9);                          /* Z: both triggers */
    CHECK(find_caps(p, 0x01, 0x35) == NULL);         /* no Rz */

    caps = find_caps(p, 0x09, 1);                    /* buttons 1-10 */
    CHECK(caps != NULL);
    if (caps)
    {
        CHECK(caps->flags & HID_VALUE_CAPS_IS_BUTTON);
        CHECK(caps->flags & HID_VALUE_CAPS_IS_RANGE);
        CHECK_EQ(caps->usage_min, 1);
        CHECK_EQ(caps->usage_max, 10);
        CHECK_EQ(caps->start_byte, 11);
        CHECK_EQ(caps->start_bit, 0);
        CHECK_EQ(caps->bit_size, 1);
        CHECK_EQ(caps->report_count, 10);
    }
    CHECK(find_caps(p, 0x09, 11) == NULL);

    caps = find_caps(p, 0x01, 0x39);                 /* hat */
    CHECK(caps != NULL);
    if (caps)
    {
        CHECK_EQ(caps->start_byte, 12);
        CHECK_EQ(caps->start_bit, 2);
        CHECK_EQ(caps->bit_size, 4);
        CHECK_EQ(caps->logical_min, 1);
        CHECK_EQ(caps->logical_max, 8);
        CHECK(HID_VALUE_CAPS_HAS_NULL(caps));
    }
}

/* The request rules on slot 1's device, in the order win32u's raw input list, an hidapi-style
 * reader and dinput use them. */
static void test_requests(const struct ios_hidparse_desc *desc)
{
    struct ios_hidpad_identity id;
    struct ios_hidpad_report_state state;
    struct winios_hidpad_output output;
    struct ios_hidpad_device dev;
    struct ios_hidpad_handle handle;
    struct ios_hidpad_reply reply;
    struct winios_hidpad pad;
    uint8_t out[512], report[IOS_HIDPAD_XINPUT_INPUT_LEN], fx[8];
    char text[128];
    uint64_t now = 1000;

    ios_hidpad_identity_slot(1, &id);
    memset(&dev, 0, sizeof(dev));
    memset(&state, 0, sizeof(state));
    memset(&output, 0, sizeof(output));
    dev.id = &id;
    dev.desc = desc;
    dev.report_state = &state;
    dev.output = &output;
    dev.rawinput_handle = IOS_HIDPAD_SLOT_RAWINPUT_HANDLE(1);
    ios_hidpad_handle_init(&handle);
    pad = slot_state(XA, 100, -200, 300, -400, 10, 20);
    CHECK_EQ(ios_hidpad_presence(&dev, pad.connected, now), 1);

    /* win32u's add_device: raw input handle, collection information, descriptor */
    ios_hidpad_ioctl(&dev, &handle, IOS_IOCTL_HID_GET_WINE_RAWINPUT_HANDLE, NULL, 0, 4, &pad, now, &reply);
    CHECK_EQ(reply.status, 0);
    CHECK_EQ(le32(reply.data), 0x102);               /* one handle per slot, not the HID pad's 0x100 */
    CHECK_EQ(IOS_HIDPAD_SLOT_RAWINPUT_HANDLE(0), 0x101);
    CHECK_EQ(IOS_HIDPAD_SLOT_RAWINPUT_HANDLE(3), 0x104);
    ios_hidpad_ioctl(&dev, &handle, IOS_IOCTL_HID_GET_COLLECTION_INFORMATION, NULL, 0, 12, &pad, now, &reply);
    CHECK_EQ(reply.status, 0);
    CHECK_EQ(le32(reply.data), desc->preparsed_len);
    CHECK_EQ(le16(reply.data + 6), 0x045e);
    CHECK_EQ(le16(reply.data + 8), 0x028e);
    CHECK_EQ(le16(reply.data + 10), 0x0114);
    ios_hidpad_ioctl(&dev, &handle, IOS_IOCTL_HID_GET_COLLECTION_DESCRIPTOR, NULL, 0, desc->preparsed_len, &pad, now, &reply);
    CHECK_EQ(reply.status, 0);
    CHECK_EQ(reply.size, desc->preparsed_len);

    /* strings */
    ios_hidpad_ioctl(&dev, &handle, IOS_IOCTL_HID_GET_PRODUCT_STRING, NULL, 0, sizeof(out), &pad, now, &reply);
    CHECK_EQ(reply.status, 0);
    narrow(reply.data, reply.size, text, sizeof(text));
    CHECK_STR(text, "Controller (XBOX 360 For Windows)");
    ios_hidpad_ioctl(&dev, &handle, IOS_IOCTL_HID_GET_SERIALNUMBER_STRING, NULL, 0, sizeof(out), &pad, now, &reply);
    CHECK_EQ(reply.size, 2);

    /* the first read completes at once with the slot's state */
    CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 0, now), IOS_HIDPAD_READ_NOW);
    CHECK_EQ(ios_hidpad_read_report(&dev, &handle, &pad, now, report), 15);
    CHECK_EQ(report[0], 0);
    CHECK_EQ(le16(report + 1), 32868);
    CHECK_EQ(le16(report + 3), 32967);
    CHECK_EQ(le16(report + 9), 0x8000 - 10 * 128);
    CHECK_EQ(le16(report + 11), 1);
    /* nothing new 1 ms later: wait for the clock; 4 ms later a report */
    CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 0, now + 1000), IOS_HIDPAD_READ_WAIT);
    CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 0, now + 4000), IOS_HIDPAD_READ_NOW);

    /* IOCTL_HID_GET_INPUT_REPORT (id 0) */
    memset(out, 0, sizeof(out));
    ios_hidpad_ioctl(&dev, &handle, IOS_IOCTL_HID_GET_INPUT_REPORT, out, 15, 15, &pad, now, &reply);
    CHECK_EQ(reply.status, 0);
    CHECK_EQ(reply.information, 15);

    /* no output or feature reports: rumble stays with XInput */
    memset(fx, 0, sizeof(fx));
    ios_hidpad_write(&dev, &handle, &pad, now, fx, sizeof(fx), &reply);
    CHECK_EQ(reply.status, IOS_STATUS_INVALID_PARAMETER);
    CHECK_EQ(reply.output_report, 0);
    ios_hidpad_ioctl(&dev, &handle, IOS_IOCTL_HID_SET_OUTPUT_REPORT, fx, sizeof(fx), 0, &pad, now, &reply);
    CHECK_EQ(reply.status, IOS_STATUS_INVALID_PARAMETER);
    ios_hidpad_ioctl(&dev, &handle, IOS_IOCTL_HID_GET_FEATURE, out, 8, 8, &pad, now, &reply);
    CHECK_EQ(reply.status, IOS_STATUS_INVALID_PARAMETER);
    CHECK_EQ(output.rumble_valid, 0);

    /* the pad leaves the slot: within a second the handle works, then it is lost */
    memset(&pad, 0, sizeof(pad));
    now = 2000000;
    CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 0, now), IOS_HIDPAD_READ_NOW);
    CHECK_EQ(ios_hidpad_read_report(&dev, &handle, &pad, now, report), 15);
    CHECK_EQ(le16(report + 1), 32768);               /* at rest during the gap */
    now = 3000000;
    CHECK_EQ(ios_hidpad_read_decide(&dev, &handle, &pad, 0, now), IOS_HIDPAD_READ_LOST);
    ios_hidpad_ioctl(&dev, &handle, IOS_IOCTL_HID_GET_COLLECTION_INFORMATION, NULL, 0, 12, &pad, now, &reply);
    CHECK_EQ(reply.status, IOS_STATUS_DELETE_PENDING);
}

/* f: the server's composition: slot n is listed while it has a pad and (slot 0) no HID pad is
 * present; presence then adds clarification 5's grace; opens follow presence. */
struct slot_session
{
    struct ios_hidpad_device hid, slot0, slot1;
    uint32_t refusals;
};

static int slot_present(struct slot_session *s, struct ios_hidpad_device *dev, unsigned int slot,
                        int slot_connected, int hid_published, uint64_t now)
{
    int hid_present = ios_hidpad_presence(&s->hid, hid_published, now);
    return ios_hidpad_presence(dev, ios_hidpad_slot_listed(slot, slot_connected, hid_present), now);
}

static void test_listing(void)
{
    struct slot_session s;
    int log;

    CHECK_EQ(ios_hidpad_slot_listed(0, 0, 0), 0);
    CHECK_EQ(ios_hidpad_slot_listed(0, 1, 0), 1);
    CHECK_EQ(ios_hidpad_slot_listed(0, 1, 1), 0);    /* player 1 is the DualSense */
    CHECK_EQ(ios_hidpad_slot_listed(1, 1, 1), 1);    /* player 2's XInput pad stays */
    CHECK_EQ(ios_hidpad_slot_listed(3, 1, 0), 1);
    CHECK_EQ(ios_hidpad_slot_listed(4, 1, 0), 0);

    memset(&s, 0, sizeof(s));
    /* Xbox look: slot 0 listed, opens allowed */
    CHECK_EQ(slot_present(&s, &s.slot0, 0, 1, 0, 1000), 1);
    CHECK_EQ(ios_hidpad_open_allowed(slot_present(&s, &s.slot0, 0, 1, 0, 2000), &s.refusals, &log), 1);
    /* the player switches to the PlayStation look: slot 0 stays present for the grace, then
     * isn't; slot 1 is unaffected */
    CHECK_EQ(slot_present(&s, &s.slot0, 0, 1, 1, 10000000), 1);
    CHECK_EQ(slot_present(&s, &s.slot0, 0, 1, 1, 10999999), 1);
    CHECK_EQ(slot_present(&s, &s.slot0, 0, 1, 1, 11000000), 0);
    CHECK_EQ(ios_hidpad_open_allowed(slot_present(&s, &s.slot0, 0, 1, 1, 11000001), &s.refusals, &log), 0);
    CHECK_EQ(log, 1);
    CHECK_EQ(slot_present(&s, &s.slot1, 1, 1, 1, 11000002), 1);
    /* back to the Xbox look: the HID pad is present for its own grace, then slot 0 returns */
    CHECK_EQ(slot_present(&s, &s.slot0, 0, 1, 0, 20000000), 0);
    CHECK_EQ(slot_present(&s, &s.slot0, 0, 1, 0, 21000000), 1);
    /* no pad in the slot: never listed */
    memset(&s, 0, sizeof(s));
    CHECK_EQ(slot_present(&s, &s.slot1, 1, 0, 0, 1000), 0);
}

/* c: one WM_INPUT when a slot is first listed, then one per store change; unlisting forgets. */
static void test_rawinput_schedule(void)
{
    int have = 0;
    uint32_t last = 0;

    CHECK_EQ(ios_hidpad_rawinput_due(&have, &last, 0, 5), 0);
    CHECK_EQ(ios_hidpad_rawinput_due(&have, &last, 1, 5), 1);
    CHECK_EQ(ios_hidpad_rawinput_due(&have, &last, 1, 5), 0);
    CHECK_EQ(ios_hidpad_rawinput_due(&have, &last, 1, 5), 0);
    CHECK_EQ(ios_hidpad_rawinput_due(&have, &last, 1, 6), 1);
    CHECK_EQ(ios_hidpad_rawinput_due(&have, &last, 0, 6), 0);
    CHECK_EQ(have, 0);
    CHECK_EQ(ios_hidpad_rawinput_due(&have, &last, 1, 6), 1);  /* listed again: a fresh report */
}

/* a: each slot's registry entries, as the HID pad's, volatile below the fixed bases */
static void test_registry(void)
{
    struct ios_hidpad_reg_key keys[IOS_HIDPAD_REG_KEYS];
    struct ios_hidpad_identity id;
    unsigned int i;

    ios_hidpad_identity_slot(2, &id);
    CHECK_EQ(ios_hidpad_registry_plan(&id, keys), IOS_HIDPAD_REG_KEYS);
    CHECK_STR(keys[0].tail, "VID_045E&PID_028E&IG_02\\9&4d616465&1&0002");
    CHECK(strstr(keys[0].base, "\\Enum\\HID") != NULL);
    for (i = 0; i < keys[0].value_count; i++)
    {
        if (!strcmp(keys[0].values[i].name, "HardwareID"))
            CHECK_STR(keys[0].values[i].text, "HID\\VID_045E&PID_028E&REV_0114&IG_02");
        if (!strcmp(keys[0].values[i].name, "ContainerID"))
            CHECK_STR(keys[0].values[i].text, "{6d796f73-6465-636b-0003-0002045e028e}");
    }
    CHECK_STR(keys[1].tail, "##?#HID#VID_045E&PID_028E&IG_02#9&4d616465&1&0002#{4d1e55b2-f16f-11cf-88cb-001111000030}");
    CHECK_STR(keys[1].values[0].text, "HID\\VID_045E&PID_028E&IG_02\\9&4d616465&1&0002");
    CHECK_STR(keys[2].values[0].text,
              "\\\\?\\HID#VID_045E&PID_028E&IG_02#9&4d616465&1&0002#{4d1e55b2-f16f-11cf-88cb-001111000030}");
    CHECK_STR(keys[3].values[0].name, "Linked");
    CHECK_EQ(keys[3].values[0].dword, 1);
}

int main(void)
{
    struct ios_hidparse_desc desc;

    memset(&desc, 0, sizeof(desc));
    test_identity_and_names();
    test_input_report();
    test_descriptor(&desc);
    test_requests(&desc);
    test_listing();
    test_rawinput_schedule();
    test_registry();
    ios_hidparse_release(&desc);
    return test_report("test_slots");
}
