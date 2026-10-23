// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 clarification 6 (device report of build 36, hot-plug): the path from the app's stores
// to a window's WM_DEVICECHANGE, with the pieces this part owns run as they ship:
//   server   ios_pnp_watch_update / ios_pnp_record_build (hidpad/ios_hidpad_pnp.c) sample the
//            stores; each channel handle's ios_pnp_queue holds what no read has taken;
//   sechost  ios_pnp_record_to_broadcast (pe/dlls/sechost/ios_pnp.h) turns a record into the
//            DEV_BROADCAST_DEVICEINTERFACE_W a notification receives;
// and the two upstream steps between them modelled after Wine 11.4: user32's
// RegisterDeviceNotificationW filter sizes (dlls/user32/input.c: DEVICE_NOTIFY_ALL_INTERFACE_CLASSES
// keeps only the header, so every class matches) and sechost's notification_filter_matches
// (dlls/sechost/service.c: a class filter matches its class GUID). A window's callback is
// SendMessageTimeoutW(hwnd, WM_DEVICECHANGE, code, broadcast).
// Needs Wine 11.4's include/ for dbt.h and ddk/hidclass.h.

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "dbt.h"
#include "initguid.h"
#include "ddk/hidclass.h"

#include "ios_hidpad.h"
#include "ios_hidpad_pnp.h"
#include "test_common.h"

_Static_assert(IOS_PNP_ARRIVAL == DBT_DEVICEARRIVAL, "arrival");
_Static_assert(IOS_PNP_REMOVECOMPLETE == DBT_DEVICEREMOVECOMPLETE, "removal");
_Static_assert(sizeof(WCHAR) == sizeof(uint16_t), "UTF-16 names");

/* {a5dcbf10-6530-11d2-901f-00c04fb951ed}: GUID_DEVINTERFACE_USB_DEVICE, a class we never announce */
static const GUID usb_device_class = { 0xa5dcbf10, 0x6530, 0x11d2, { 0x90, 0x1f, 0x00, 0xc0, 0x4f, 0xb9, 0x51, 0xed } };

/* ---- a window registered with RegisterDeviceNotification ---------------------------------- */

#define MAX_MESSAGES 16

struct window
{
    DEV_BROADCAST_DEVICEINTERFACE_W filter;   /* as user32 hands it to sechost */
    unsigned int count;
    struct { WPARAM code; GUID class_guid; DWORD size; char name[IOS_PNP_NAME_MAX]; } msg[MAX_MESSAGES];
};

static void register_window(struct window *w, const GUID *class_guid, DWORD flags)
{
    memset(w, 0, sizeof(*w));
    w->filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    w->filter.dbcc_classguid = *class_guid;
    if (flags & DEVICE_NOTIFY_ALL_INTERFACE_CLASSES)
        w->filter.dbcc_size = offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_classguid);
    else
        w->filter.dbcc_size = offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name);
}

/* sechost's notification_filter_matches for a device interface filter */
static int filter_matches(const DEV_BROADCAST_DEVICEINTERFACE_W *filter, const DEV_BROADCAST_DEVICEINTERFACE_W *event)
{
    if (filter->dbcc_devicetype != event->dbcc_devicetype) return 0;
    if (filter->dbcc_size == offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_classguid)) return 1;
    return !memcmp(&filter->dbcc_classguid, &event->dbcc_classguid, sizeof(GUID));
}

/* the window procedure's WM_DEVICECHANGE */
static void window_proc(struct window *w, WPARAM code, const DEV_BROADCAST_HDR *header)
{
    const DEV_BROADCAST_DEVICEINTERFACE_W *iface = (const DEV_BROADCAST_DEVICEINTERFACE_W *)header;
    size_t i;

    CHECK_EQ(header->dbch_devicetype, DBT_DEVTYP_DEVICEINTERFACE);
    if (w->count == MAX_MESSAGES) return;
    w->msg[w->count].code = code;
    w->msg[w->count].class_guid = iface->dbcc_classguid;
    w->msg[w->count].size = iface->dbcc_size;
    for (i = 0; iface->dbcc_name[i] && i < IOS_PNP_NAME_MAX - 1; i++)
        w->msg[w->count].name[i] = (char)iface->dbcc_name[i];
    w->msg[w->count].name[i] = 0;
    w->count++;
}

/* ---- the session: the server's poller, one channel handle, sechost's thread ---------------- */

static struct ios_pnp_watch watch;
static struct ios_pnp_queue channel;          /* sechost's handle on \??\MyiosdeckPnp */
static struct ios_hidpad_identity id;

static void server_poll(int hid_present, int s0, int s1, int s2, int s3)
{
    struct ios_pnp_sample sample = { hid_present, { s0, s1, s2, s3 } };
    struct ios_pnp_change changes[IOS_PNP_MAX_CHANGES];
    struct ios_pnp_record record;
    unsigned int i, n;

    n = ios_pnp_watch_update(&watch, &sample, changes);
    for (i = 0; i < n; i++)
    {
        CHECK_EQ(ios_pnp_record_build(&changes[i], &id, &record), 1);
        ios_pnp_queue_push(&channel, &record);
    }
}

static void sechost_dispatch(struct window **windows, unsigned int nwindows)
{
    union { DEV_BROADCAST_DEVICEINTERFACE_W iface; char bytes[IOS_PNP_BROADCAST_MAX]; } buffer;
    struct ios_pnp_record record;
    unsigned int i;

    while (ios_pnp_queue_pop(&channel, &record))
    {
        ios_pnp_record_to_broadcast(&record, &buffer.iface);
        for (i = 0; i < nwindows; i++)
            if (filter_matches(&windows[i]->filter, &buffer.iface))
                window_proc(windows[i], record.code, (DEV_BROADCAST_HDR *)&buffer.iface);
    }
}

static void test_watch(void)
{
    struct ios_pnp_watch w;
    struct ios_pnp_sample s;
    struct ios_pnp_change c[IOS_PNP_MAX_CHANGES];

    memset(&w, 0, sizeof(w));
    memset(&s, 0, sizeof(s));
    s.slot[0] = 1;
    CHECK_EQ(ios_pnp_watch_update(&w, &s, c), 0);    /* the baseline: what was there at start */
    CHECK_EQ(ios_pnp_watch_update(&w, &s, c), 0);
    s.hid_present = 1;
    s.slot[3] = 7;                                   /* any non-zero is connected */
    s.slot[0] = 0;
    CHECK_EQ(ios_pnp_watch_update(&w, &s, c), 3);
    CHECK_EQ(c[0].code, IOS_PNP_ARRIVAL);            /* the HID pad first */
    CHECK_EQ(c[0].which, -1);
    CHECK_EQ(c[1].code, IOS_PNP_REMOVECOMPLETE);
    CHECK_EQ(c[1].which, 0);
    CHECK_EQ(c[2].code, IOS_PNP_ARRIVAL);
    CHECK_EQ(c[2].which, 3);
    s.slot[3] = 1;
    CHECK_EQ(ios_pnp_watch_update(&w, &s, c), 0);
    s.hid_present = 0;
    s.slot[0] = s.slot[1] = s.slot[2] = 1;
    s.slot[3] = 0;
    CHECK_EQ(ios_pnp_watch_update(&w, &s, c), IOS_PNP_MAX_CHANGES);
}

static void test_records(void)
{
    struct ios_pnp_change change;
    struct ios_pnp_record record;
    char name[IOS_PNP_NAME_MAX], path[IOS_PNP_NAME_MAX];
    size_t i;

    ios_hidpad_identity_select("dualsense", NULL, &id);
    change.code = IOS_PNP_ARRIVAL;
    change.which = -1;
    CHECK_EQ(ios_pnp_record_build(&change, &id, &record), 1);
    CHECK_EQ(record.code, DBT_DEVICEARRIVAL);
    CHECK(!memcmp(record.class_guid, &GUID_DEVINTERFACE_HID, 16));
    for (i = 0; record.name[i]; i++) name[i] = (char)record.name[i];
    name[i] = 0;
    ios_hidpad_user_path(&id, path, sizeof(path));
    CHECK_STR(name, path);                           /* the path SetupDi and hidapi report */
    CHECK_EQ(ios_pnp_record_build(&change, NULL, &record), 0);   /* no HID pad configured */

    change.code = IOS_PNP_REMOVECOMPLETE;
    change.which = 2;
    CHECK_EQ(ios_pnp_record_build(&change, NULL, &record), 1);
    CHECK_EQ(record.code, DBT_DEVICEREMOVECOMPLETE);
    CHECK(!memcmp(record.class_guid, &GUID_DEVINTERFACE_HID, 16));
    for (i = 0; record.name[i]; i++) name[i] = (char)record.name[i];
    name[i] = 0;
    CHECK_STR(name, "\\\\?\\HID#VID_045E&PID_028E&IG_02#9&4d616465&1&0002#{4d1e55b2-f16f-11cf-88cb-001111000030}");
}

static void test_queue(void)
{
    struct ios_pnp_queue q;
    struct ios_pnp_record r;
    unsigned int i;

    memset(&q, 0, sizeof(q));
    CHECK_EQ(ios_pnp_queue_pop(&q, &r), 0);
    for (i = 0; i < IOS_PNP_QUEUE_MAX + 6; i++)
    {
        memset(&r, 0, sizeof(r));
        r.code = i;
        ios_pnp_queue_push(&q, &r);
    }
    CHECK_EQ(q.count, IOS_PNP_QUEUE_MAX);
    for (i = 6; i < IOS_PNP_QUEUE_MAX + 6; i++)      /* the oldest six went; the rest in order */
    {
        CHECK_EQ(ios_pnp_queue_pop(&q, &r), 1);
        CHECK_EQ(r.code, i);
    }
    CHECK_EQ(ios_pnp_queue_pop(&q, &r), 0);
}

static void test_broadcast(void)
{
    union { DEV_BROADCAST_DEVICEINTERFACE_W iface; char bytes[IOS_PNP_BROADCAST_MAX]; } buffer;
    struct ios_pnp_record record;
    size_t i;

    memset(&record, 0, sizeof(record));
    record.code = IOS_PNP_ARRIVAL;
    memcpy(record.class_guid, &GUID_DEVINTERFACE_HID, 16);
    for (i = 0; i < 5; i++) record.name[i] = (uint16_t)"\\\\?\\X"[i];
    memset(&buffer, 0xcc, sizeof(buffer));
    ios_pnp_record_to_broadcast(&record, &buffer.iface);
    CHECK_EQ(buffer.iface.dbcc_size, offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name[6]));
    CHECK_EQ(buffer.iface.dbcc_devicetype, DBT_DEVTYP_DEVICEINTERFACE);
    CHECK_EQ(buffer.iface.dbcc_reserved, 0);
    CHECK(!memcmp(&buffer.iface.dbcc_classguid, &GUID_DEVINTERFACE_HID, sizeof(GUID)));
    CHECK_EQ(buffer.iface.dbcc_name[4], 'X');
    CHECK_EQ(buffer.iface.dbcc_name[5], 0);

    /* a name without a terminator is cut at the last unit */
    for (i = 0; i < IOS_PNP_NAME_MAX; i++) record.name[i] = 'a';
    ios_pnp_record_to_broadcast(&record, &buffer.iface);
    CHECK_EQ(buffer.iface.dbcc_size, offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name[IOS_PNP_NAME_MAX]));
    CHECK(buffer.iface.dbcc_size <= sizeof(buffer));
    CHECK_EQ(buffer.iface.dbcc_name[IOS_PNP_NAME_MAX - 1], 0);
}

/* The report's case: a game window registered for HID interface arrivals (as SDL's HIDAPI and
 * hidapi-based readers do), one registered with DEVICE_NOTIFY_ALL_INTERFACE_CLASSES (as
 * windows.gaming.input and dinput do), one for USB devices only. */
static void test_window_hotplug(void)
{
    struct window hid_window, all_window, usb_window;
    struct window *windows[] = { &hid_window, &all_window, &usb_window };
    char path[IOS_PNP_NAME_MAX];

    memset(&watch, 0, sizeof(watch));
    memset(&channel, 0, sizeof(channel));
    ios_hidpad_identity_select("dualsense", NULL, &id);
    ios_hidpad_user_path(&id, path, sizeof(path));
    register_window(&hid_window, &GUID_DEVINTERFACE_HID, 0);
    register_window(&all_window, &usb_device_class, DEVICE_NOTIFY_ALL_INTERFACE_CLASSES);
    register_window(&usb_window, &usb_device_class, 0);

    /* the session starts with the on-screen pad on slot 0: that is the baseline, no message */
    server_poll(0, 1, 0, 0, 0);
    sechost_dispatch(windows, 3);
    CHECK_EQ(hid_window.count, 0);

    /* the player switches to the PlayStation look: the HID pad arrives */
    server_poll(1, 1, 0, 0, 0);
    sechost_dispatch(windows, 3);
    CHECK_EQ(hid_window.count, 1);
    CHECK_EQ(hid_window.msg[0].code, DBT_DEVICEARRIVAL);
    CHECK(!memcmp(&hid_window.msg[0].class_guid, &GUID_DEVINTERFACE_HID, sizeof(GUID)));
    CHECK_STR(hid_window.msg[0].name, path);
    CHECK_EQ(hid_window.msg[0].size, offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name[strlen(path) + 1]));
    CHECK_EQ(all_window.count, 1);
    CHECK_EQ(usb_window.count, 0);

    /* a physical pad joins on slot 1, then the player goes back to the Xbox look */
    server_poll(1, 1, 1, 0, 0);
    server_poll(0, 1, 1, 0, 0);
    sechost_dispatch(windows, 3);                    /* the thread was busy: both queued, in order */
    CHECK_EQ(hid_window.count, 3);
    CHECK_EQ(hid_window.msg[1].code, DBT_DEVICEARRIVAL);
    CHECK_STR(hid_window.msg[1].name, "\\\\?\\HID#VID_045E&PID_028E&IG_01#9&4d616465&1&0001#{4d1e55b2-f16f-11cf-88cb-001111000030}");
    CHECK_EQ(hid_window.msg[2].code, DBT_DEVICEREMOVECOMPLETE);
    CHECK_STR(hid_window.msg[2].name, path);
    CHECK_EQ(all_window.count, 3);
    CHECK_EQ(usb_window.count, 0);

    /* the physical pad leaves */
    server_poll(0, 1, 0, 0, 0);
    sechost_dispatch(windows, 3);
    CHECK_EQ(hid_window.count, 4);
    CHECK_EQ(hid_window.msg[3].code, DBT_DEVICEREMOVECOMPLETE);
    CHECK_STR(hid_window.msg[3].name, "\\\\?\\HID#VID_045E&PID_028E&IG_01#9&4d616465&1&0001#{4d1e55b2-f16f-11cf-88cb-001111000030}");
}

int main(void)
{
    test_watch();
    test_records();
    test_queue();
    test_broadcast();
    test_window_hotplug();
    return test_report("test_pnp");
}
