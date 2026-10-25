// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part B: the HID pad, served by the wineserver itself; clarification 8: the XInput
// slots' HID devices the same way.
//
// Desktop Wine creates HID devices through winebus.sys, hidclass.sys and a device-manager
// process; none of them runs on iOS. Like \Device\NamedPipe or \Device\Afd, this device has no
// driver behind it: the server answers every request on its files directly, with the
// contract hidclass.sys gives its device files (Microsoft's IOCTL_HID_* documentation; the
// behaviour of refs/wine-11.4/dlls/hidclass.sys/device.c), so hid.dll, setupapi and dinput
// work unchanged.
//
//   \Device\MyiosdeckHidPad0          the device object (open: one file object per handle)
//   \??\HID#...#{4d1e55b2-...}        symlink to it, the name programs open (Part A2)
//   \Device\MyiosdeckXInput0..3       clarification 8: XInput slot n's device, symlink
//                                     \??\HID#VID_045E&PID_028E&IG_0n#9&4d616465&1&000n#{...}
//
// Compiled into the wineserver library (spec 08) with the server's own headers.

#include "config.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "winioctl.h"
#include "ddk/wdm.h"
#include "ddk/hidclass.h"

#include "object.h"
#include "file.h"
#include "handle.h"
#include "request.h"

#include "ios_hidpad.h"
#include "ios_hidparse.h"
#include "ios_hidpad_request.h"
#include "ios_hidpad_pnp.h"
#include "ios_hidpad_rawinput.h"
#include "ios_diag.h"

/* The request rules (hidpad/ios_hidpad_request.c) carry their own copies of these values so
 * they can be tested without Wine's headers; they must match Wine's. */
C_ASSERT( IOS_STATUS_SUCCESS == STATUS_SUCCESS );
C_ASSERT( IOS_STATUS_BUFFER_OVERFLOW == STATUS_BUFFER_OVERFLOW );
C_ASSERT( IOS_STATUS_INVALID_PARAMETER == STATUS_INVALID_PARAMETER );
C_ASSERT( IOS_STATUS_BUFFER_TOO_SMALL == STATUS_BUFFER_TOO_SMALL );
C_ASSERT( IOS_STATUS_NOT_SUPPORTED == STATUS_NOT_SUPPORTED );
C_ASSERT( IOS_STATUS_INVALID_USER_BUFFER == STATUS_INVALID_USER_BUFFER );
C_ASSERT( IOS_STATUS_INVALID_BUFFER_SIZE == STATUS_INVALID_BUFFER_SIZE );
C_ASSERT( IOS_STATUS_DELETE_PENDING == STATUS_DELETE_PENDING );
C_ASSERT( IOS_STATUS_DEVICE_NOT_CONNECTED == STATUS_DEVICE_NOT_CONNECTED );
C_ASSERT( IOS_IOCTL_HID_GET_COLLECTION_DESCRIPTOR == IOCTL_HID_GET_COLLECTION_DESCRIPTOR );
C_ASSERT( IOS_IOCTL_HID_FLUSH_QUEUE == IOCTL_HID_FLUSH_QUEUE );
C_ASSERT( IOS_IOCTL_HID_GET_COLLECTION_INFORMATION == IOCTL_HID_GET_COLLECTION_INFORMATION );
C_ASSERT( IOS_IOCTL_HID_GET_FEATURE == IOCTL_HID_GET_FEATURE );
C_ASSERT( IOS_IOCTL_HID_GET_INPUT_REPORT == IOCTL_HID_GET_INPUT_REPORT );
C_ASSERT( IOS_IOCTL_HID_GET_MANUFACTURER_STRING == IOCTL_HID_GET_MANUFACTURER_STRING );
C_ASSERT( IOS_IOCTL_HID_GET_PRODUCT_STRING == IOCTL_HID_GET_PRODUCT_STRING );
C_ASSERT( IOS_IOCTL_HID_GET_SERIALNUMBER_STRING == IOCTL_HID_GET_SERIALNUMBER_STRING );
C_ASSERT( IOS_IOCTL_HID_GET_INDEXED_STRING == IOCTL_HID_GET_INDEXED_STRING );
C_ASSERT( IOS_IOCTL_GET_NUM_DEVICE_INPUT_BUFFERS == IOCTL_GET_NUM_DEVICE_INPUT_BUFFERS );
C_ASSERT( IOS_IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS == IOCTL_SET_NUM_DEVICE_INPUT_BUFFERS );
C_ASSERT( IOS_IOCTL_HID_GET_POLL_FREQUENCY_MSEC == IOCTL_HID_GET_POLL_FREQUENCY_MSEC );
C_ASSERT( IOS_IOCTL_HID_SET_POLL_FREQUENCY_MSEC == IOCTL_HID_SET_POLL_FREQUENCY_MSEC );
C_ASSERT( IOS_IOCTL_HID_SET_FEATURE == IOCTL_HID_SET_FEATURE );
C_ASSERT( IOS_IOCTL_HID_SET_OUTPUT_REPORT == IOCTL_HID_SET_OUTPUT_REPORT );
/* Wine's private raw input handle query (include/wine/hid.h, not included: it needs winbase.h) */
C_ASSERT( IOS_IOCTL_HID_GET_WINE_RAWINPUT_HANDLE == HID_BUFFER_CTL_CODE(300) );
C_ASSERT( IOS_HIDPAD_COLLECTION_INFO_SIZE == sizeof(HID_COLLECTION_INFORMATION) );
C_ASSERT( offsetof(HID_COLLECTION_INFORMATION, Polled) == 4 );
C_ASSERT( offsetof(HID_COLLECTION_INFORMATION, VendorID) == 6 );
C_ASSERT( offsetof(HID_COLLECTION_INFORMATION, ProductID) == 8 );
C_ASSERT( offsetof(HID_COLLECTION_INFORMATION, VersionNumber) == 10 );

void ios_hidpad_server_init(void);

/* Clarification 4 (spec 21 Part B3): this server has the gate, so the app can create the HID
 * pad in every session and show it only by publishing it. The app links against this name in
 * builds that rely on the gate, so a server without it fails to link. */
int myiosdeck_hidpad_gate = 1;

/* ---- the devices ------------------------------------------------------------------------ */

/* One served HID device: the HID pad (Part B), or an XInput slot's (clarification 8). */
struct pad_device
{
    int enabled;
    int slot;                               /* -1: the HID pad; 0-3: an XInput slot */
    struct ios_hidpad_identity id;
    struct ios_hidparse_desc desc;          /* B6: built once from the descriptor */
    struct ios_hidpad_report_state report_state;
    struct pgios_hidpad_output output;     /* the last output snapshot (A6; HID pad only) */
    struct list files;                      /* open file objects */
    uint32_t opens;                         /* clarification 8g: opens, the first few logged */
    uint32_t open_refusals;                 /* clarification 4: opens refused, no pad */
    uint32_t lost_handles;                  /* QUESTIONS 15-22: open handles lost the same way */
    struct timeout_user *tick;              /* B3: the device clock, only while reads wait */
    struct ios_hidpad_device requests;      /* B4/B5: what the request rules see */
    /* B7: effects-report log (HID pad) */
    uint32_t effects_count, rumble_count, no_rumble_count, trigger_count, lightbar_count, led_count;
    /* clarification 8c: raw input */
    int rawinput_have;
    uint32_t rawinput_packet;
    /* clarification 9a: what readers do with a slot device, summed per 5 s while it is open */
    uint64_t diag_start_ms;
    uint32_t diag_reads, diag_read_now, diag_read_waited, diag_read_lost;
    struct ios_hidpad_tally diag_ioctls;
    uint32_t diag_wm_input;                 /* WM_INPUT sent, this window */
    uint32_t diag_wm_input_total;           /* and since the start (the first 3 logged) */
};

static struct pad_device hidpad = { .slot = -1 };
static struct pad_device slots[IOS_HIDPAD_SLOTS] = { { .slot = 0 }, { .slot = 1 }, { .slot = 2 }, { .slot = 3 } };

/* The device object: which device it serves. */
struct pad_device_object
{
    struct object       obj;
    struct pad_device  *dev;
};

struct hidpad_file
{
    struct object       obj;
    struct fd          *fd;
    struct pad_device  *dev;
    struct list         entry;              /* in dev->files */
    struct async_queue  read_q;             /* reads waiting for the device clock */
    struct ios_hidpad_handle requests;      /* per-handle state of the request rules: input
                                               buffers, B3 pacing, lost (QUESTIONS 15-22) */
    int                 lost_logged;
    uint32_t            diag_ioctls;        /* clarification 9a: IOCTLs on this handle (first 8 logged) */
    uint32_t            diag_reads;         /* reports delivered on this handle (first 3 logged) */
};

/* Clarification 9a: the first IOCTLs of a handle and the first reports it gets, in full. */
#define DIAG_IOCTLS_LOGGED 8
#define DIAG_READS_LOGGED  3
#define DIAG_WM_INPUT_LOGGED 3

static uint64_t now_us(void)
{
    return (uint64_t)monotonic_time / 10;   /* 100 ns units */
}

static void get_store( struct pgios_hidpad *pad )
{
    memset( pad, 0, sizeof(*pad) );
    pgios_hidpad_get_state( pad );
}

static int slot_connected( unsigned int slot, struct pgios_gamepad *gamepad )
{
    memset( gamepad, 0, sizeof(*gamepad) );
    return pgios_gamepad_get_state( (int)slot, gamepad ) != 0;
}

/* Read a device's store into `pad` and feed the presence rule; returns whether the device is
 * present (clarification 5: published, or unpublished for less than the grace). The HID pad
 * reads the HID store; slot n reads XInput slot n, listed only while clarification 8f allows
 * (slot 0 isn't while the HID pad is present), so an unlisted slot reads as a pad at rest. */
static int sample_device( struct pad_device *dev, struct pgios_hidpad *pad )
{
    struct pgios_gamepad gamepad;
    struct pgios_hidpad hid;
    int connected, hid_present = 0;

    if (dev->slot < 0) get_store( pad );
    else
    {
        connected = slot_connected( (unsigned int)dev->slot, &gamepad );
        if (dev->slot == 0 && connected && hidpad.enabled) hid_present = sample_device( &hidpad, &hid );
        ios_hidpad_from_gamepad( &gamepad, ios_hidpad_slot_listed( (unsigned int)dev->slot, connected, hid_present ), pad );
    }
    return ios_hidpad_presence( &dev->requests, pad->connected, now_us() );
}

static const char *dev_tag( const struct pad_device *dev )
{
    static const char *const tags[IOS_HIDPAD_SLOTS] = { "xinput-slot0", "xinput-slot1", "xinput-slot2", "xinput-slot3" };
    return dev->slot < 0 ? "hid-pad" : tags[dev->slot];
}

/* ---- clarification 9a: diagnostics of the slot devices ----------------------------------- */

static uint64_t now_ms(void)
{
    return now_us() / 1000;
}

/* A report delivered on a slot device's handle (`waited`: by the device clock). */
static void diag_read( struct hidpad_file *file, const uint8_t *report, data_size_t len, int waited )
{
    struct pad_device *dev = file->dev;
    char hex[3 * 16 + 1];
    data_size_t i;

    if (dev->slot < 0) return;
    if (waited) dev->diag_read_waited++;
    else dev->diag_read_now++;
    if (++file->diag_reads > DIAG_READS_LOGGED) return;
    for (i = 0; i < len && i < 16; i++) snprintf( hex + 3 * i, 4, "%02x ", report[i] );
    hex[3 * i] = 0;
    ws_log( "[%s] handle %p read %u: %u bytes%s: %s", dev_tag( dev ), file, (unsigned int)file->diag_reads,
            (unsigned int)len, waited ? " (after a wait)" : "", hex );
}

/* An IOCTL answered on a slot device's handle. */
static void diag_ioctl( struct hidpad_file *file, ioctl_code_t code, data_size_t in_size,
                        data_size_t out_size, const struct ios_hidpad_reply *reply )
{
    struct pad_device *dev = file->dev;
    const char *name;

    if (dev->slot < 0) return;
    ios_hidpad_tally_add( &dev->diag_ioctls, code );
    if (++file->diag_ioctls > DIAG_IOCTLS_LOGGED) return;
    name = ios_hidpad_ioctl_name( code );
    ws_log( "[%s] handle %p ioctl %u: %s %#x, in %u, out %u: status %#x, %u bytes", dev_tag( dev ), file,
            (unsigned int)file->diag_ioctls, name ? name : "code", (unsigned int)code, (unsigned int)in_size,
            (unsigned int)out_size, (unsigned int)reply->status, (unsigned int)reply->information );
}

/* Every 5 s while a slot device has open handles (from the store poller), one line: how often
 * it was read and how, which IOCTLs it got, how many WM_INPUT it sent. Logged with zero reads
 * too: "opened but never read" is what this is for. */
static void diag_summary( struct pad_device *dev )
{
    char ioctls[256];
    struct hidpad_file *file;
    unsigned int handles = 0, i;
    size_t len = 0;

    LIST_FOR_EACH_ENTRY( file, &dev->files, struct hidpad_file, entry ) handles++;
    if (!handles)
    {
        dev->diag_start_ms = 0;
        return;
    }
    if (!ios_diag_period( &dev->diag_start_ms, now_ms() )) return;

    ioctls[0] = 0;
    for (i = 0; i < dev->diag_ioctls.used && len < sizeof(ioctls) - 64; i++)
    {
        const char *name = ios_hidpad_ioctl_name( dev->diag_ioctls.code[i] );
        len += snprintf( ioctls + len, sizeof(ioctls) - len, "%s%s%s%#x x%u", len ? ", " : "",
                         name ? name + 6 : "", name ? " " : "", (unsigned int)dev->diag_ioctls.code[i],
                         (unsigned int)dev->diag_ioctls.count[i] );
    }
    if (dev->diag_ioctls.other && len < sizeof(ioctls) - 32)
        snprintf( ioctls + len, sizeof(ioctls) - len, "%sother x%u", len ? ", " : "", (unsigned int)dev->diag_ioctls.other );

    ws_log( "[%s] 5 s: %u handles, %u reads (%u.%u/s): %u at once, %u after a wait, %u lost; IOCTLs: %s; WM_INPUT %u",
            dev_tag( dev ), handles, (unsigned int)dev->diag_reads, (unsigned int)(dev->diag_reads / 5),
            (unsigned int)(dev->diag_reads * 2 % 10), (unsigned int)dev->diag_read_now,
            (unsigned int)dev->diag_read_waited, (unsigned int)dev->diag_read_lost,
            ioctls[0] ? ioctls : "none", (unsigned int)dev->diag_wm_input );
    dev->diag_reads = dev->diag_read_now = dev->diag_read_waited = dev->diag_read_lost = 0;
    dev->diag_wm_input = 0;
    memset( &dev->diag_ioctls, 0, sizeof(dev->diag_ioctls) );
}

/* ---- the device objects ----------------------------------------------------------------- */

static void hidpad_device_dump( struct object *obj, int verbose )
{
    struct pad_device_object *device = (struct pad_device_object *)obj;
    fprintf( stderr, "MYIOSDECK %s device\n", device->dev ? dev_tag( device->dev ) : "pad" );
}

static struct object *hidpad_device_open_file( struct object *obj, unsigned int access,
                                               unsigned int sharing, unsigned int options );

static const struct object_ops hidpad_device_ops =
{
    sizeof(struct pad_device_object), /* size */
    &device_type,                     /* type */
    hidpad_device_dump,               /* dump */
    no_add_queue,                     /* add_queue */
    NULL,                             /* remove_queue */
    NULL,                             /* signaled */
    no_satisfied,                     /* satisfied */
    no_signal,                        /* signal */
    no_get_fd,                        /* get_fd */
    default_get_sync,                 /* get_sync */
    default_map_access,               /* map_access */
    default_get_sd,                   /* get_sd */
    default_set_sd,                   /* set_sd */
    default_get_full_name,            /* get_full_name */
    no_lookup_name,                   /* lookup_name */
    directory_link_name,              /* link_name */
    default_unlink_name,              /* unlink_name */
    hidpad_device_open_file,          /* open_file */
    no_kernel_obj_list,               /* get_kernel_obj_list */
    no_close_handle,                  /* close_handle */
    no_destroy                        /* destroy */
};

/* ---- file objects (one per open) -------------------------------------------------------- */

static void hidpad_file_dump( struct object *obj, int verbose );
static struct fd *hidpad_file_get_fd( struct object *obj );
static void hidpad_file_destroy( struct object *obj );

static const struct object_ops hidpad_file_ops =
{
    sizeof(struct hidpad_file),       /* size */
    &file_type,                       /* type */
    hidpad_file_dump,                 /* dump */
    NULL,                             /* add_queue */
    NULL,                             /* remove_queue */
    NULL,                             /* signaled */
    NULL,                             /* satisfied */
    no_signal,                        /* signal */
    hidpad_file_get_fd,               /* get_fd */
    default_fd_get_sync,              /* get_sync */
    default_map_access,               /* map_access */
    default_get_sd,                   /* get_sd */
    default_set_sd,                   /* set_sd */
    no_get_full_name,                 /* get_full_name */
    no_lookup_name,                   /* lookup_name */
    no_link_name,                     /* link_name */
    NULL,                             /* unlink_name */
    no_open_file,                     /* open_file */
    no_kernel_obj_list,               /* get_kernel_obj_list */
    no_close_handle,                  /* close_handle */
    hidpad_file_destroy               /* destroy */
};

static enum server_fd_type hidpad_file_get_fd_type( struct fd *fd );
static void hidpad_file_read( struct fd *fd, struct async *async, file_pos_t pos );
static void hidpad_file_write( struct fd *fd, struct async *async, file_pos_t pos );
static void hidpad_file_get_volume_info( struct fd *fd, struct async *async, unsigned int info_class );
static void hidpad_file_ioctl( struct fd *fd, ioctl_code_t code, struct async *async );

static const struct fd_ops hidpad_file_fd_ops =
{
    default_fd_get_poll_events,       /* get_poll_events */
    default_poll_event,               /* poll_event */
    hidpad_file_get_fd_type,          /* get_fd_type */
    hidpad_file_read,                 /* read */
    hidpad_file_write,                /* write */
    no_fd_flush,                      /* flush */
    default_fd_get_file_info,         /* get_file_info */
    hidpad_file_get_volume_info,      /* get_volume_info */
    hidpad_file_ioctl,                /* ioctl */
    default_fd_cancel_async,          /* cancel_async */
    no_fd_queue_async,                /* queue_async */
    default_fd_reselect_async         /* reselect_async */
};

/* Why an open was refused, for the log. */
static void log_refusal( const struct pad_device *dev )
{
    struct pgios_gamepad gamepad;

    if (dev->slot < 0)
        ws_log( "[hid-pad] open refused: no PlayStation pad published" );
    else if (dev->slot == 0 && slot_connected( 0, &gamepad ))
        ws_log( "[xinput-slot0] open refused: player 1 is the PlayStation pad" );
    else
        ws_log( "[%s] open refused: no pad in XInput slot %d", dev_tag( dev ), dev->slot );
}

static struct object *hidpad_device_open_file( struct object *obj, unsigned int access,
                                               unsigned int sharing, unsigned int options )
{
    struct pad_device *dev = ((struct pad_device_object *)obj)->dev;
    struct hidpad_file *file;
    struct pgios_hidpad pad;
    int log;

    if (!dev || !dev->enabled)
    {
        set_error( STATUS_DEVICE_NOT_CONNECTED );
        return NULL;
    }
    /* Clarification 4 (and 8b for the slots): while there is no pad the device exists but
     * won't open, as a pad that isn't plugged in; every reader skips a device it can't open.
     * Handles opened before are lost as an unplugged pad's are (QUESTIONS 15-22). */
    if (!ios_hidpad_open_allowed( sample_device( dev, &pad ), &dev->open_refusals, &log ))
    {
        if (log) log_refusal( dev );
        set_error( STATUS_NO_SUCH_DEVICE );
        return NULL;
    }
    if (!(file = alloc_object( &hidpad_file_ops ))) return NULL;

    file->dev = dev;
    file->lost_logged = 0;
    file->diag_ioctls = 0;
    file->diag_reads = 0;
    ios_hidpad_handle_init( &file->requests );
    init_async_queue( &file->read_q );
    list_add_tail( &dev->files, &file->entry );

    if (!(file->fd = alloc_pseudo_fd( &hidpad_file_fd_ops, &file->obj, options )))
    {
        release_object( file );
        return NULL;
    }
    allow_fd_caching( file->fd );
    if (dev->slot >= 0 && ++dev->opens <= IOS_HIDPAD_REFUSALS_LOGGED)
        ws_log( "[%s] opened (open %u)", dev_tag( dev ), (unsigned int)dev->opens );
    return &file->obj;
}

static void hidpad_file_dump( struct object *obj, int verbose )
{
    struct hidpad_file *file = (struct hidpad_file *)obj;
    fprintf( stderr, "MYIOSDECK %s file\n", dev_tag( file->dev ) );
}

static struct fd *hidpad_file_get_fd( struct object *obj )
{
    struct hidpad_file *file = (struct hidpad_file *)obj;
    return (struct fd *)grab_object( file->fd );
}

static void log_output( const char *why )
{
    const struct pgios_hidpad_output *o = &hidpad.output;

    ws_log( "[hidpad] %s: rumble %u L%u R%u, lightbar %u #%02x%02x%02x, player LEDs %u %#x, mute LED %u %u, "
            "triggers L%u R%u, power %u %#x", why,
            o->rumble_valid, o->rumble_left, o->rumble_right,
            o->lightbar_valid, o->red, o->green, o->blue,
            o->player_leds_valid, o->player_leds, o->mute_led_valid, o->mute_led,
            o->trigger_valid[0], o->trigger_valid[1], o->power_valid, o->power_reduction );
}

/* B2: the HID pad's last handle is gone (the game quit or crashed). A real pad keeps its
 * motors and trigger effects going; this one must not stay rumbling for the rest of the
 * session. (A slot device has no output reports: its rumble goes through XInput.) */
static void hidpad_file_destroy( struct object *obj )
{
    struct hidpad_file *file = (struct hidpad_file *)obj;
    struct pad_device *dev = file->dev;

    list_remove( &file->entry );
    free_async_queue( &file->read_q );
    if (file->fd) release_object( file->fd );

    if (dev->slot < 0 && list_empty( &dev->files ) && ios_hidpad_output_release( &dev->output ))
    {
        pgios_hidpad_set_output( &dev->output );
        log_output( "last handle closed, motors stopped and trigger effects released" );
    }
}

static enum server_fd_type hidpad_file_get_fd_type( struct fd *fd )
{
    return FD_TYPE_DEVICE;
}

/* Complete `async` now with `status`, Information = `result` and `size` bytes of output. */
static void complete( struct async *async, unsigned int status, data_size_t result,
                      const void *data, data_size_t size )
{
    async_request_complete_alloc( async, status, result, size, data );
    set_error( STATUS_PENDING );
}

/* ---- B3: reads ---------------------------------------------------------------------------- */

/* QUESTIONS 15-22: the pad went away while `file` was open. Fail its waiting reads as
 * hidclass fails a removed device's (the first losses are logged). */
static void lose_handle( struct hidpad_file *file )
{
    struct pad_device *dev = file->dev;
    struct async *async;

    if (!file->lost_logged)
    {
        file->lost_logged = 1;
        if (++dev->lost_handles <= IOS_HIDPAD_REFUSALS_LOGGED)
        {
            if (dev->slot < 0) ws_log( "[hid-pad] open handle lost: no PlayStation pad published any more" );
            else ws_log( "[%s] open handle lost: XInput slot %d's device is gone", dev_tag( dev ), dev->slot );
        }
    }
    while ((async = find_pending_async( &file->read_q )))
    {
        async_request_complete( async, STATUS_DEVICE_NOT_CONNECTED, 0, 0, NULL );
        release_object( async );
    }
}

/* Lose every open handle of a device that is no longer present (the store poller). */
static void lose_handles( struct pad_device *dev )
{
    struct hidpad_file *file;

    LIST_FOR_EACH_ENTRY( file, &dev->files, struct hidpad_file, entry )
        if (!file->requests.lost && ios_hidpad_handle_lost( &file->requests, 0 )) lose_handle( file );
}

static void hidpad_tick( void *private );

static void start_tick( struct pad_device *dev )
{
    if (!dev->tick) dev->tick = add_timeout_user( -(timeout_t)IOS_HIDPAD_TICK_US * 10, hidpad_tick, dev );
}

static int has_waiting_read( struct hidpad_file *file )
{
    struct async *async = find_pending_async( &file->read_q );
    if (!async) return 0;
    release_object( async );
    return 1;
}

/* The device clock: one report for each handle with a read waiting. It is armed only while
 * some handle has a read waiting, and re-armed after a tick only if one still does. */
static void hidpad_tick( void *private )
{
    struct pad_device *dev = private;
    uint8_t report[IOS_HIDPAD_DUALSENSE_INPUT_LEN];
    struct pgios_hidpad pad;
    struct hidpad_file *file;
    struct async *async;
    data_size_t len;
    int waiting = 0, present;

    dev->tick = NULL;
    present = sample_device( dev, &pad );
    LIST_FOR_EACH_ENTRY( file, &dev->files, struct hidpad_file, entry )
    {
        if (!has_waiting_read( file )) continue;
        if (ios_hidpad_handle_lost( &file->requests, present ))
        {
            lose_handle( file );
            continue;
        }
        if (!(async = find_pending_async( &file->read_q ))) continue;
        len = ios_hidpad_read_report( &dev->requests, &file->requests, &pad, now_us(), report );
        async_request_complete_alloc( async, STATUS_SUCCESS, len, len, report );
        release_object( async );
        diag_read( file, report, len, 1 );
        if (has_waiting_read( file )) waiting = 1;
    }
    if (waiting) start_tick( dev );
}

/* ReadFile. A buffer shorter than one input report is refused as hidclass refuses it. The
 * read completes at once when this handle has had no report for 4 ms, or when the store
 * changed and 1 ms has passed; otherwise it waits for the device clock. Reports are built
 * when they are delivered, so a slow reader gets the current state, never a backlog. */
static void hidpad_file_read( struct fd *fd, struct async *async, file_pos_t pos )
{
    struct hidpad_file *file = get_fd_user( fd );
    struct pad_device *dev = file->dev;
    uint8_t report[IOS_HIDPAD_DUALSENSE_INPUT_LEN];
    struct iosb *iosb = async_get_iosb( async );
    data_size_t size = iosb->out_size, len;
    struct pgios_hidpad pad;

    release_object( iosb );
    if (dev->slot >= 0) dev->diag_reads++;
    if (size < dev->desc.input_len)
    {
        if (dev->slot >= 0 && file->diag_reads < DIAG_READS_LOGGED)
            ws_log( "[%s] handle %p read refused: buffer %u bytes, a report is %u", dev_tag( dev ), file,
                    (unsigned int)size, (unsigned int)dev->desc.input_len );
        set_error( STATUS_INVALID_BUFFER_SIZE );
        return;
    }

    sample_device( dev, &pad );
    switch (ios_hidpad_read_decide( &dev->requests, &file->requests, &pad, has_waiting_read( file ), now_us() ))
    {
    case IOS_HIDPAD_READ_LOST:
        if (dev->slot >= 0) dev->diag_read_lost++;
        lose_handle( file );
        set_error( STATUS_DEVICE_NOT_CONNECTED );
        return;
    case IOS_HIDPAD_READ_NOW:
        len = ios_hidpad_read_report( &dev->requests, &file->requests, &pad, now_us(), report );
        complete( async, STATUS_SUCCESS, len, report, len );
        diag_read( file, report, len, 0 );
        return;
    case IOS_HIDPAD_READ_WAIT:
        break;
    }

    queue_async( &file->read_q, async );
    start_tick( dev );
    set_error( STATUS_PENDING );
}

/* ---- B4/B5: output reports and IOCTLs ----------------------------------------------------- */

/* B7: log what an output report did; called after the request rules folded it. */
static void log_output_report( const uint8_t *data, const struct ios_hidpad_reply *reply )
{
    const struct ios_hidpad_output_flags *flags = &reply->output_flags;
    char hex[3 * 80 + 1];
    uint32_t i, len = reply->information;
    int kind;

    if (reply->output_result == IOS_HIDPAD_OUTPUT_BAD_CRC)
    {
        ws_log( "[hidpad] Bluetooth effects report with a bad CRC ignored" );
        return;
    }
    if (reply->output_result != IOS_HIDPAD_OUTPUT_FOLDED) return;

    hidpad.effects_count++;
    if (flags->rumble) hidpad.rumble_count++;
    else hidpad.no_rumble_count++;
    if (flags->triggers) hidpad.trigger_count++;
    if (flags->lightbar) hidpad.lightbar_count++;
    if (flags->leds) hidpad.led_count++;

    kind = ios_hidpad_effects_log_kind( hidpad.effects_count );
    if (kind == 1)
    {
        for (i = 0; i < len && i < 80; i++) snprintf( hex + 3 * i, 4, "%02x ", data[i] );
        hex[3 * i] = 0;
        ws_log( "[hidpad] effects report %u (%u bytes): %s", (unsigned int)hidpad.effects_count,
                (unsigned int)len, hex );
        log_output( "snapshot" );
    }
    else if (kind == 2)
    {
        ws_log( "[hidpad] %u effects reports: rumble %u, no rumble bit %u, triggers %u, light bar %u, LEDs %u",
                (unsigned int)hidpad.effects_count, (unsigned int)hidpad.rumble_count,
                (unsigned int)hidpad.no_rumble_count, (unsigned int)hidpad.trigger_count,
                (unsigned int)hidpad.lightbar_count, (unsigned int)hidpad.led_count );
    }
}

/* Hand a request's answer back: logs and the store for output reports (only the HID pad has
 * them), then the reply. */
static void finish_request( struct pad_device *dev, struct async *async, const uint8_t *data,
                            const struct ios_hidpad_reply *reply )
{
    if (reply->output_report && dev->slot < 0)
    {
        log_output_report( data, reply );
        if (reply->output_changed) pgios_hidpad_set_output( &dev->output );
    }
    if (reply->status != STATUS_SUCCESS) set_error( reply->status );
    else complete( async, STATUS_SUCCESS, reply->information, reply->data, reply->size );
}

/* WriteFile: hidclass's IOCTL_HID_WRITE_REPORT checks, then A6. */
static void hidpad_file_write( struct fd *fd, struct async *async, file_pos_t pos )
{
    struct hidpad_file *file = get_fd_user( fd );
    struct iosb *iosb = async_get_iosb( async );
    struct ios_hidpad_reply reply;
    struct pgios_hidpad pad;

    sample_device( file->dev, &pad );
    ios_hidpad_write( &file->dev->requests, &file->requests, &pad, now_us(), iosb->in_data, iosb->in_size, &reply );
    if (file->requests.lost) lose_handle( file );
    finish_request( file->dev, async, iosb->in_data, &reply );
    release_object( iosb );
}

static void hidpad_file_get_volume_info( struct fd *fd, struct async *async, unsigned int info_class )
{
    switch (info_class)
    {
    case FileFsDeviceInformation:
    {
        static const FILE_FS_DEVICE_INFORMATION device_info = { FILE_DEVICE_UNKNOWN, 0 };
        if (get_reply_max_size() >= sizeof(device_info))
            set_reply_data( &device_info, sizeof(device_info) );
        else
            set_error( STATUS_BUFFER_TOO_SMALL );
        break;
    }
    default:
        set_error( STATUS_NOT_IMPLEMENTED );
        break;
    }
}

static void hidpad_file_ioctl( struct fd *fd, ioctl_code_t code, struct async *async )
{
    struct hidpad_file *file = get_fd_user( fd );
    struct iosb *iosb = async_get_iosb( async );
    struct ios_hidpad_reply reply;
    struct pgios_hidpad pad;

    sample_device( file->dev, &pad );
    ios_hidpad_ioctl( &file->dev->requests, &file->requests, code, iosb->in_data, iosb->in_size,
                      iosb->out_size, &pad, now_us(), &reply );
    diag_ioctl( file, code, iosb->in_size, iosb->out_size, &reply );
    if (file->requests.lost) lose_handle( file );
    finish_request( file->dev, async, iosb->in_data, &reply );
    release_object( iosb );
}

/* ---- clarification 8c: raw input for the slot devices ------------------------------------ */

static struct
{
    const struct ios_rawinput_sink *sink;   /* spec 08's queue; NULL: no raw input messages */
    struct timeout_user *timer;             /* the 4 ms raw input clock, only while wanted */
} rawinput;

void ios_hidpad_set_rawinput_sink( const struct ios_rawinput_sink *sink )
{
    rawinput.sink = sink;
    ws_log( "[xinput-slots] raw input sink %s", sink ? "installed: WM_INPUT for the slot devices" : "removed" );
}

static int rawinput_wanted(void)
{
    return rawinput.sink && rawinput.sink->wanted && rawinput.sink->send &&
           rawinput.sink->wanted( 0x01, 0x05 );    /* Generic Desktop, Game Pad */
}

static void rawinput_tick( void *private );

static void rawinput_start(void)
{
    if (!rawinput.timer && rawinput_wanted())
        rawinput.timer = add_timeout_user( -(timeout_t)IOS_HIDPAD_TICK_US * 10, rawinput_tick, NULL );
}

/* Every 4 ms while some process wants game pad raw input: one WM_INPUT per listed slot device
 * whose store changed (and one when it is first listed). Stops when no one wants it; the
 * store poller starts it again. */
static void rawinput_tick( void *private )
{
    uint8_t report[IOS_HIDPAD_XINPUT_INPUT_LEN];
    struct pgios_hidpad pad;
    unsigned int i;
    size_t len;

    rawinput.timer = NULL;
    if (!rawinput_wanted()) return;
    for (i = 0; i < IOS_HIDPAD_SLOTS; i++)
    {
        struct pad_device *dev = &slots[i];
        if (!dev->enabled) continue;
        sample_device( dev, &pad );
        if (!ios_hidpad_rawinput_due( &dev->rawinput_have, &dev->rawinput_packet, pad.connected, pad.packet ))
            continue;
        len = ios_hidpad_build_xinput_input( &pad, report );
        rawinput.sink->send( IOS_RAWINPUT_WM_INPUT, 0, dev->requests.rawinput_handle, 0x01, 0x05,
                             report, (unsigned int)len );
        /* clarification 9b: the first WM_INPUT of each slot, then the 5 s summary's count */
        dev->diag_wm_input++;
        if (++dev->diag_wm_input_total <= DIAG_WM_INPUT_LOGGED)
            ws_log( "[%s] WM_INPUT %u sent: device %#x, %u bytes, packet %u", dev_tag( dev ),
                    (unsigned int)dev->diag_wm_input_total, (unsigned int)dev->requests.rawinput_handle,
                    (unsigned int)len, (unsigned int)pad.packet );
    }
    rawinput_start();
}

static void rawinput_device_change( struct pad_device *dev, int arrival )
{
    if (!rawinput.sink || !rawinput.sink->send) return;
    dev->rawinput_have = 0;
    rawinput.sink->send( IOS_RAWINPUT_WM_INPUT_DEVICE_CHANGE,
                         arrival ? IOS_RAWINPUT_GIDC_ARRIVAL : IOS_RAWINPUT_GIDC_REMOVAL,
                         dev->requests.rawinput_handle, 0x01, 0x05, NULL, 0 );
}

/* ---- clarification 6: device arrival and removal (\??\MyiosdeckPnp) ---------------------- */

static struct
{
    struct list listeners;                  /* open pnp_listener objects */
    struct ios_pnp_watch watch;             /* what the last sample saw */
    struct timeout_user *timer;             /* the store poller */
} pnp;

/* One handle on the channel: the events since it was opened, in order. */
struct pnp_listener
{
    struct object        obj;
    struct fd           *fd;
    struct list          entry;             /* in pnp.listeners */
    struct async_queue   read_q;            /* reads waiting for an event */
    struct ios_pnp_queue queue;             /* events no read has taken yet */
};

static void pnp_device_dump( struct object *obj, int verbose )
{
    fputs( "MYIOSDECK device-change channel\n", stderr );
}

static struct object *pnp_device_open_file( struct object *obj, unsigned int access,
                                            unsigned int sharing, unsigned int options );

static const struct object_ops pnp_device_ops =
{
    sizeof(struct object),            /* size */
    &device_type,                     /* type */
    pnp_device_dump,                  /* dump */
    no_add_queue,                     /* add_queue */
    NULL,                             /* remove_queue */
    NULL,                             /* signaled */
    no_satisfied,                     /* satisfied */
    no_signal,                        /* signal */
    no_get_fd,                        /* get_fd */
    default_get_sync,                 /* get_sync */
    default_map_access,               /* map_access */
    default_get_sd,                   /* get_sd */
    default_set_sd,                   /* set_sd */
    default_get_full_name,            /* get_full_name */
    no_lookup_name,                   /* lookup_name */
    directory_link_name,              /* link_name */
    default_unlink_name,              /* unlink_name */
    pnp_device_open_file,             /* open_file */
    no_kernel_obj_list,               /* get_kernel_obj_list */
    no_close_handle,                  /* close_handle */
    no_destroy                        /* destroy */
};

static void pnp_listener_dump( struct object *obj, int verbose )
{
    fputs( "MYIOSDECK device-change listener\n", stderr );
}

static struct fd *pnp_listener_get_fd( struct object *obj )
{
    struct pnp_listener *listener = (struct pnp_listener *)obj;
    return (struct fd *)grab_object( listener->fd );
}

static void pnp_listener_destroy( struct object *obj )
{
    struct pnp_listener *listener = (struct pnp_listener *)obj;

    list_remove( &listener->entry );
    free_async_queue( &listener->read_q );
    if (listener->fd) release_object( listener->fd );
}

static const struct object_ops pnp_listener_ops =
{
    sizeof(struct pnp_listener),      /* size */
    &file_type,                       /* type */
    pnp_listener_dump,                /* dump */
    NULL,                             /* add_queue */
    NULL,                             /* remove_queue */
    NULL,                             /* signaled */
    NULL,                             /* satisfied */
    no_signal,                        /* signal */
    pnp_listener_get_fd,              /* get_fd */
    default_fd_get_sync,              /* get_sync */
    default_map_access,               /* map_access */
    default_get_sd,                   /* get_sd */
    default_set_sd,                   /* set_sd */
    no_get_full_name,                 /* get_full_name */
    no_lookup_name,                   /* lookup_name */
    no_link_name,                     /* link_name */
    NULL,                             /* unlink_name */
    no_open_file,                     /* open_file */
    no_kernel_obj_list,               /* get_kernel_obj_list */
    no_close_handle,                  /* close_handle */
    pnp_listener_destroy              /* destroy */
};

static int has_waiting_event_read( struct pnp_listener *listener )
{
    struct async *async = find_pending_async( &listener->read_q );
    if (!async) return 0;
    release_object( async );
    return 1;
}

/* ReadFile: one record, at once if one is queued, else when the next event happens. */
static void pnp_listener_read( struct fd *fd, struct async *async, file_pos_t pos )
{
    struct pnp_listener *listener = get_fd_user( fd );
    struct iosb *iosb = async_get_iosb( async );
    data_size_t size = iosb->out_size;
    struct ios_pnp_record record;

    release_object( iosb );
    if (size < sizeof(struct ios_pnp_record))
    {
        set_error( STATUS_INVALID_BUFFER_SIZE );
        return;
    }
    if (!has_waiting_event_read( listener ) && ios_pnp_queue_pop( &listener->queue, &record ))
    {
        complete( async, STATUS_SUCCESS, sizeof(record), &record, sizeof(record) );
        return;
    }
    queue_async( &listener->read_q, async );
    set_error( STATUS_PENDING );
}

static enum server_fd_type pnp_listener_get_fd_type( struct fd *fd )
{
    return FD_TYPE_DEVICE;
}

static const struct fd_ops pnp_listener_fd_ops =
{
    default_fd_get_poll_events,       /* get_poll_events */
    default_poll_event,               /* poll_event */
    pnp_listener_get_fd_type,         /* get_fd_type */
    pnp_listener_read,                /* read */
    no_fd_write,                      /* write */
    no_fd_flush,                      /* flush */
    default_fd_get_file_info,         /* get_file_info */
    no_fd_get_volume_info,            /* get_volume_info */
    no_fd_ioctl,                      /* ioctl */
    default_fd_cancel_async,          /* cancel_async */
    no_fd_queue_async,                /* queue_async */
    default_fd_reselect_async         /* reselect_async */
};

static struct object *pnp_device_open_file( struct object *obj, unsigned int access,
                                            unsigned int sharing, unsigned int options )
{
    struct pnp_listener *listener;

    if (!(listener = alloc_object( &pnp_listener_ops ))) return NULL;
    memset( &listener->queue, 0, sizeof(listener->queue) );
    init_async_queue( &listener->read_q );
    list_add_tail( &pnp.listeners, &listener->entry );
    if (!(listener->fd = alloc_pseudo_fd( &pnp_listener_fd_ops, &listener->obj, options )))
    {
        release_object( listener );
        return NULL;
    }
    allow_fd_caching( listener->fd );
    return &listener->obj;
}

/* Hand `record` to one listener: to a waiting read, else to its queue (the oldest record goes
 * when the queue is full). */
static void pnp_deliver( struct pnp_listener *listener, const struct ios_pnp_record *record )
{
    struct async *async;

    if ((async = find_pending_async( &listener->read_q )))
    {
        async_request_complete_alloc( async, STATUS_SUCCESS, sizeof(*record), sizeof(*record), record );
        release_object( async );
        return;
    }
    ios_pnp_queue_push( &listener->queue, record );
}

/* The store poller: every IOS_PNP_POLL_US, sample the HID pad's presence and each slot
 * device's (clarification 8: listed, with the same grace), announce what changed to every
 * listener (and to raw input readers), and lose the open handles of a device gone past the
 * grace (clarification 5), even when no read waits on them. */
static void pnp_poll( void *private )
{
    struct ios_pnp_change changes[IOS_PNP_MAX_CHANGES];
    struct ios_pnp_sample sample;
    struct ios_pnp_record record;
    struct pgios_hidpad pad;
    struct pgios_gamepad gamepad;
    struct pnp_listener *listener;
    unsigned int i, n;

    pnp.timer = NULL;
    memset( &sample, 0, sizeof(sample) );
    if (hidpad.enabled) sample.hid_present = sample_device( &hidpad, &pad );
    for (i = 0; i < IOS_PNP_SLOTS; i++)
    {
        if (slots[i].enabled) sample.slot[i] = sample_device( &slots[i], &pad );
        else sample.slot[i] = ios_hidpad_slot_listed( i, slot_connected( i, &gamepad ), sample.hid_present );
    }

    n = ios_pnp_watch_update( &pnp.watch, &sample, changes );
    for (i = 0; i < n; i++)
    {
        if (!ios_pnp_record_build( &changes[i], hidpad.enabled ? &hidpad.id : NULL, &record )) continue;
        if (changes[i].which < 0)
            ws_log( "[pnp] %s: HID pad", changes[i].code == IOS_PNP_ARRIVAL ? "arrival" : "removal" );
        else
        {
            ws_log( "[pnp] %s: XInput slot %d", changes[i].code == IOS_PNP_ARRIVAL ? "arrival" : "removal",
                    changes[i].which );
            if (slots[changes[i].which].enabled)
                rawinput_device_change( &slots[changes[i].which], changes[i].code == IOS_PNP_ARRIVAL );
        }
        LIST_FOR_EACH_ENTRY( listener, &pnp.listeners, struct pnp_listener, entry )
            pnp_deliver( listener, &record );
    }

    if (hidpad.enabled && !sample.hid_present) lose_handles( &hidpad );
    for (i = 0; i < IOS_PNP_SLOTS; i++)
        if (slots[i].enabled && !sample.slot[i]) lose_handles( &slots[i] );

    /* clarification 9: the 5 s summaries, and once whether raw input can be sent at all */
    for (i = 0; i < IOS_PNP_SLOTS; i++)
    {
        static int no_sink_logged;
        if (!slots[i].enabled) continue;
        diag_summary( &slots[i] );
        if (sample.slot[i] && !rawinput.sink && !no_sink_logged)
        {
            no_sink_logged = 1;
            ws_log( "[xinput-slots] no raw input sink installed: no WM_INPUT for the slot devices (QUESTIONS 15-24)" );
        }
    }

    rawinput_start();
    pnp.timer = add_timeout_user( -(timeout_t)IOS_PNP_POLL_US * 10, pnp_poll, NULL );
}

static int make_unicode( const char *ascii, WCHAR *buffer, size_t count, struct unicode_str *str );

/* Create \Device\MyiosdeckPnp, its \??\ symlink, and start the poller. */
static void pnp_init(void)
{
    WCHAR device_buf[64], link_buf[64], target_buf[64];
    struct unicode_str device_name, link_name, target;
    struct object *device, *symlink;

    list_init( &pnp.listeners );
    if (!make_unicode( IOS_PNP_DEVICE_OBJECT, device_buf, 64, &device_name ) ||
        !make_unicode( IOS_PNP_SYMLINK, link_buf, 64, &link_name ) ||
        !make_unicode( IOS_PNP_DEVICE_OBJECT, target_buf, 64, &target ))
        return;
    if (!(device = create_named_object( NULL, &pnp_device_ops, &device_name, OBJ_PERMANENT, NULL )))
    {
        ws_log( "[pnp] creating %s failed, status %#x", IOS_PNP_DEVICE_OBJECT, get_error() );
        return;
    }
    release_object( device );
    if (!(symlink = create_symlink( NULL, &link_name, OBJ_PERMANENT, &target, NULL )))
    {
        ws_log( "[pnp] creating %s failed, status %#x", IOS_PNP_SYMLINK, get_error() );
        return;
    }
    release_object( symlink );
    clear_error();
    pnp.timer = add_timeout_user( -(timeout_t)IOS_PNP_POLL_US * 10, pnp_poll, NULL );
    ws_log( "[pnp] device-change channel %s -> %s, polling the stores every %u ms",
            IOS_PNP_SYMLINK, IOS_PNP_DEVICE_OBJECT, IOS_PNP_POLL_US / 1000 );
}

/* ---- B1: start-up ------------------------------------------------------------------------ */

static int make_unicode( const char *ascii, WCHAR *buffer, size_t count, struct unicode_str *str )
{
    size_t i, n = strlen( ascii );

    if (n >= count) return 0;
    for (i = 0; i < n; i++) buffer[i] = (WCHAR)(unsigned char)ascii[i];
    buffer[n] = 0;
    str->str = buffer;
    str->len = (data_size_t)(n * sizeof(WCHAR));
    return 1;
}

/* Parse the device's descriptor, create its device object `object_name` and the symlink its
 * identity names, and set up what the request rules see. Returns 1 when the device is served. */
static int create_device( struct pad_device *dev, const char *object_name, uint32_t rawinput_handle )
{
    WCHAR device_buf[64], link_buf[256], target_buf[64];
    struct unicode_str device_name, link_name, target;
    struct pad_device_object *device;
    struct object *symlink;
    char link[256];

    list_init( &dev->files );

    /* B6: the preparsed data hid.dll reads, built once from the descriptor */
    if (!ios_hidparse_parse( dev->id.descriptor, dev->id.descriptor_len, &dev->desc ))
    {
        ws_log( "[%s] %s: the report descriptor didn't parse, no device", dev_tag( dev ), dev->id.name );
        return 0;
    }

    if (!ios_hidpad_symlink_path( &dev->id, link, sizeof(link) ) ||
        !make_unicode( link, link_buf, sizeof(link_buf) / sizeof(WCHAR), &link_name ) ||
        !make_unicode( object_name, device_buf, sizeof(device_buf) / sizeof(WCHAR), &device_name ) ||
        !make_unicode( object_name, target_buf, sizeof(target_buf) / sizeof(WCHAR), &target ))
    {
        ws_log( "[%s] %s: names too long, no device", dev_tag( dev ), dev->id.name );
        return 0;
    }

    if (!(device = create_named_object( NULL, &hidpad_device_ops, &device_name, OBJ_PERMANENT, NULL )))
    {
        ws_log( "[%s] %s: creating %s failed, status %#x", dev_tag( dev ), dev->id.name, object_name, get_error() );
        return 0;
    }
    device->dev = dev;
    release_object( device );

    if (!(symlink = create_symlink( NULL, &link_name, OBJ_PERMANENT, &target, NULL )))
    {
        ws_log( "[%s] %s: creating %s failed, status %#x", dev_tag( dev ), dev->id.name, link, get_error() );
        return 0;
    }
    release_object( symlink );
    clear_error();

    dev->requests.id = &dev->id;
    dev->requests.desc = &dev->desc;
    dev->requests.report_state = &dev->report_state;
    dev->requests.output = &dev->output;
    dev->requests.poll_interval = 0;        /* not polled */
    dev->requests.rawinput_handle = rawinput_handle;
    dev->enabled = 1;
    ws_log( "[%s] %s %04X:%04X v%04x \"%s\" / \"%s\" / \"%s\", descriptor %u bytes, input report %u, "
            "output %u, feature %u, preparsed %u bytes, raw input handle %#x; %s -> %s",
            dev_tag( dev ), dev->id.name, dev->id.vendor_id, dev->id.product_id, dev->id.version,
            dev->id.product, dev->id.manufacturer, dev->id.serial,
            (unsigned int)dev->id.descriptor_len, (unsigned int)dev->desc.input_len,
            (unsigned int)dev->desc.output_len, (unsigned int)dev->desc.feature_len,
            (unsigned int)dev->desc.preparsed_len,
            rawinput_handle ? (unsigned int)rawinput_handle : IOS_HIDPAD_RAWINPUT_HANDLE, link, object_name );
    return 1;
}

/* Called once by the wineserver after its object namespace and the registry exist (spec 08
 * clarification 6). The device-change channel and the four XInput slot devices exist in every
 * session (clarifications 6 and 8); the HID pad only with an identity. */
void ios_hidpad_server_init(void)
{
    static int done;
    char name[64];
    unsigned int i;

    if (done) return;
    done = 1;

    list_init( &hidpad.files );
    for (i = 0; i < IOS_HIDPAD_SLOTS; i++) list_init( &slots[i].files );
    pnp_init();

    /* clarification 8: the XInput slot devices */
    for (i = 0; i < IOS_HIDPAD_SLOTS; i++)
    {
        ios_hidpad_identity_slot( i, &slots[i].id );
        snprintf( name, sizeof(name), IOS_HIDPAD_SLOT_DEVICE_OBJECT_FMT, i );
        create_device( &slots[i], name, IOS_HIDPAD_SLOT_RAWINPUT_HANDLE( i ) );
    }

    if (!ios_hidpad_identity_select( getenv( "PG_HIDPAD" ), getenv( "PG_HIDPAD_NAME" ), &hidpad.id ))
        return;
    create_device( &hidpad, IOS_HIDPAD_DEVICE_OBJECT, 0 );
}
