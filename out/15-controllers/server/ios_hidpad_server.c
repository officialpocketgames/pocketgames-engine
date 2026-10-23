// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part B: the HID pad, served by the wineserver itself.
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

/* ---- the device ------------------------------------------------------------------------- */

static struct
{
    int enabled;
    struct ios_hidpad_identity id;
    struct ios_hidparse_desc desc;          /* B6: built once from A3's descriptor */
    struct ios_hidpad_report_state report_state;
    struct winios_hidpad_output output;     /* the last output snapshot (A6) */
    struct list files;                      /* open file objects */
    uint32_t open_refusals;                 /* clarification 4: opens refused, no pad published */
    uint32_t lost_handles;                  /* QUESTIONS 15-22: open handles lost the same way */
    struct timeout_user *tick;              /* B3: the device clock, only while reads wait */
    struct ios_hidpad_device requests;      /* B4/B5: what the request rules see */
    /* B7: effects-report log */
    uint32_t effects_count, rumble_count, no_rumble_count, trigger_count, lightbar_count, led_count;
} hidpad;

struct hidpad_file
{
    struct object       obj;
    struct fd          *fd;
    struct list         entry;              /* in hidpad.files */
    struct async_queue  read_q;             /* reads waiting for the device clock */
    struct ios_hidpad_handle requests;      /* per-handle state of the request rules: input
                                               buffers, B3 pacing, lost (QUESTIONS 15-22) */
    int                 lost_logged;
};

static uint64_t now_us(void)
{
    return (uint64_t)monotonic_time / 10;   /* 100 ns units */
}

static void get_store( struct winios_hidpad *pad )
{
    memset( pad, 0, sizeof(*pad) );
    winios_hidpad_get_state( pad );
}

/* Clarification 5: read the HID store and feed the presence rule; returns whether the pad is
 * present (published, or unpublished for less than the grace). */
static int store_present( struct winios_hidpad *pad )
{
    get_store( pad );
    return ios_hidpad_presence( &hidpad.requests, pad->connected, now_us() );
}

/* ---- the device object ------------------------------------------------------------------ */

static void hidpad_device_dump( struct object *obj, int verbose )
{
    fputs( "MYIOSDECK HID pad device\n", stderr );
}

static struct object *hidpad_device_open_file( struct object *obj, unsigned int access,
                                               unsigned int sharing, unsigned int options );

static const struct object_ops hidpad_device_ops =
{
    sizeof(struct object),            /* size */
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

static struct object *hidpad_device_open_file( struct object *obj, unsigned int access,
                                               unsigned int sharing, unsigned int options )
{
    struct hidpad_file *file;
    struct winios_hidpad pad;
    int log_refusal;

    if (!hidpad.enabled)
    {
        set_error( STATUS_DEVICE_NOT_CONNECTED );
        return NULL;
    }
    /* Clarification 4: while the app publishes no HID pad (Xbox mode) the device exists but
     * won't open, as a pad that isn't plugged in; every reader skips a device it can't open.
     * Handles opened before are lost as an unplugged pad's are (QUESTIONS 15-22). */
    memset( &pad, 0, sizeof(pad) );
    if (!ios_hidpad_open_allowed( store_present( &pad ), &hidpad.open_refusals, &log_refusal ))
    {
        if (log_refusal) ws_log( "[hid-pad] open refused: no PlayStation pad published" );
        set_error( STATUS_NO_SUCH_DEVICE );
        return NULL;
    }
    if (!(file = alloc_object( &hidpad_file_ops ))) return NULL;

    ios_hidpad_handle_init( &file->requests );
    init_async_queue( &file->read_q );
    list_add_tail( &hidpad.files, &file->entry );

    if (!(file->fd = alloc_pseudo_fd( &hidpad_file_fd_ops, &file->obj, options )))
    {
        release_object( file );
        return NULL;
    }
    allow_fd_caching( file->fd );
    return &file->obj;
}

static void hidpad_file_dump( struct object *obj, int verbose )
{
    fputs( "MYIOSDECK HID pad file\n", stderr );
}

static struct fd *hidpad_file_get_fd( struct object *obj )
{
    struct hidpad_file *file = (struct hidpad_file *)obj;
    return (struct fd *)grab_object( file->fd );
}

static void log_output( const char *why )
{
    const struct winios_hidpad_output *o = &hidpad.output;

    ws_log( "[hidpad] %s: rumble %u L%u R%u, lightbar %u #%02x%02x%02x, player LEDs %u %#x, mute LED %u %u, "
            "triggers L%u R%u, power %u %#x", why,
            o->rumble_valid, o->rumble_left, o->rumble_right,
            o->lightbar_valid, o->red, o->green, o->blue,
            o->player_leds_valid, o->player_leds, o->mute_led_valid, o->mute_led,
            o->trigger_valid[0], o->trigger_valid[1], o->power_valid, o->power_reduction );
}

/* B2: the last handle is gone (the game quit or crashed). A real pad keeps its motors and
 * trigger effects going; this one must not stay rumbling for the rest of the session. */
static void hidpad_file_destroy( struct object *obj )
{
    struct hidpad_file *file = (struct hidpad_file *)obj;

    list_remove( &file->entry );
    free_async_queue( &file->read_q );
    if (file->fd) release_object( file->fd );

    if (list_empty( &hidpad.files ) && ios_hidpad_output_release( &hidpad.output ))
    {
        winios_hidpad_set_output( &hidpad.output );
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

/* QUESTIONS 15-22: the pad was unpublished while `file` was open. Fail its waiting reads as
 * hidclass fails a removed device's (the first losses are logged). */
static void lose_handle( struct hidpad_file *file )
{
    struct async *async;

    if (!file->lost_logged)
    {
        file->lost_logged = 1;
        if (++hidpad.lost_handles <= IOS_HIDPAD_REFUSALS_LOGGED)
            ws_log( "[hid-pad] open handle lost: no PlayStation pad published any more" );
    }
    while ((async = find_pending_async( &file->read_q )))
    {
        async_request_complete( async, STATUS_DEVICE_NOT_CONNECTED, 0, 0, NULL );
        release_object( async );
    }
}

static void hidpad_tick( void *private );

static void start_tick(void)
{
    if (!hidpad.tick) hidpad.tick = add_timeout_user( -(timeout_t)IOS_HIDPAD_TICK_US * 10, hidpad_tick, NULL );
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
    uint8_t report[IOS_HIDPAD_DUALSENSE_INPUT_LEN];
    struct winios_hidpad pad;
    struct hidpad_file *file;
    struct async *async;
    data_size_t len;
    int waiting = 0, present;

    hidpad.tick = NULL;
    present = store_present( &pad );
    LIST_FOR_EACH_ENTRY( file, &hidpad.files, struct hidpad_file, entry )
    {
        if (!has_waiting_read( file )) continue;
        if (ios_hidpad_handle_lost( &file->requests, present ))
        {
            lose_handle( file );
            continue;
        }
        if (!(async = find_pending_async( &file->read_q ))) continue;
        len = ios_hidpad_read_report( &hidpad.requests, &file->requests, &pad, now_us(), report );
        async_request_complete_alloc( async, STATUS_SUCCESS, len, len, report );
        release_object( async );
        if (has_waiting_read( file )) waiting = 1;
    }
    if (waiting) start_tick();
}

/* ReadFile. A buffer shorter than one input report is refused as hidclass refuses it. The
 * read completes at once when this handle has had no report for 4 ms, or when the store
 * changed and 1 ms has passed; otherwise it waits for the device clock. Reports are built
 * when they are delivered, so a slow reader gets the current state, never a backlog. */
static void hidpad_file_read( struct fd *fd, struct async *async, file_pos_t pos )
{
    struct hidpad_file *file = get_fd_user( fd );
    uint8_t report[IOS_HIDPAD_DUALSENSE_INPUT_LEN];
    struct iosb *iosb = async_get_iosb( async );
    data_size_t size = iosb->out_size, len;
    struct winios_hidpad pad;

    release_object( iosb );
    if (size < hidpad.desc.input_len)
    {
        set_error( STATUS_INVALID_BUFFER_SIZE );
        return;
    }

    get_store( &pad );
    switch (ios_hidpad_read_decide( &hidpad.requests, &file->requests, &pad, has_waiting_read( file ), now_us() ))
    {
    case IOS_HIDPAD_READ_LOST:
        lose_handle( file );
        set_error( STATUS_DEVICE_NOT_CONNECTED );
        return;
    case IOS_HIDPAD_READ_NOW:
        len = ios_hidpad_read_report( &hidpad.requests, &file->requests, &pad, now_us(), report );
        complete( async, STATUS_SUCCESS, len, report, len );
        return;
    case IOS_HIDPAD_READ_WAIT:
        break;
    }

    queue_async( &file->read_q, async );
    start_tick();
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

/* Hand a request's answer back: logs and the store for output reports, then the reply. */
static void finish_request( struct async *async, const uint8_t *data, const struct ios_hidpad_reply *reply )
{
    if (reply->output_report)
    {
        log_output_report( data, reply );
        if (reply->output_changed) winios_hidpad_set_output( &hidpad.output );
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
    struct winios_hidpad pad;

    get_store( &pad );
    ios_hidpad_write( &hidpad.requests, &file->requests, &pad, now_us(), iosb->in_data, iosb->in_size, &reply );
    if (file->requests.lost) lose_handle( file );
    finish_request( async, iosb->in_data, &reply );
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
    struct winios_hidpad pad;

    get_store( &pad );
    ios_hidpad_ioctl( &hidpad.requests, &file->requests, code, iosb->in_data, iosb->in_size,
                      iosb->out_size, &pad, now_us(), &reply );
    if (file->requests.lost) lose_handle( file );
    finish_request( async, iosb->in_data, &reply );
    release_object( iosb );
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

/* The store poller: every IOS_PNP_POLL_US, sample the HID pad's presence and the XInput slots,
 * announce what changed to every listener, and lose the HID pad's open handles once the pad
 * is gone past the grace (clarification 5), even when no read waits on them. */
static void pnp_poll( void *private )
{
    struct ios_pnp_change changes[IOS_PNP_MAX_CHANGES];
    struct ios_pnp_sample sample;
    struct ios_pnp_record record;
    struct winios_hidpad pad;
    struct winios_gamepad gamepad;
    struct pnp_listener *listener;
    struct hidpad_file *file;
    unsigned int i, n;

    pnp.timer = NULL;
    memset( &sample, 0, sizeof(sample) );
    if (hidpad.enabled) sample.hid_present = store_present( &pad );
    for (i = 0; i < IOS_PNP_SLOTS; i++) sample.slot[i] = winios_gamepad_get_state( i, &gamepad ) != 0;

    n = ios_pnp_watch_update( &pnp.watch, &sample, changes );
    for (i = 0; i < n; i++)
    {
        if (!ios_pnp_record_build( &changes[i], hidpad.enabled ? &hidpad.id : NULL, &record )) continue;
        if (changes[i].which < 0)
            ws_log( "[pnp] %s: HID pad", changes[i].code == IOS_PNP_ARRIVAL ? "arrival" : "removal" );
        else
            ws_log( "[pnp] %s: XInput slot %d", changes[i].code == IOS_PNP_ARRIVAL ? "arrival" : "removal",
                    changes[i].which );
        LIST_FOR_EACH_ENTRY( listener, &pnp.listeners, struct pnp_listener, entry )
            pnp_deliver( listener, &record );
    }

    if (hidpad.enabled && !sample.hid_present)
    {
        LIST_FOR_EACH_ENTRY( file, &hidpad.files, struct hidpad_file, entry )
            if (!file->requests.lost && ios_hidpad_handle_lost( &file->requests, 0 )) lose_handle( file );
    }

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

/* Called once by the wineserver after its object namespace and the registry exist (spec 08
 * clarification 6). Without an identity it does nothing. */
void ios_hidpad_server_init(void)
{
    static int done;
    WCHAR device_buf[64], link_buf[256], target_buf[64];
    struct unicode_str device_name, link_name, target;
    char link[256];
    struct object *device, *symlink;

    if (done) return;
    done = 1;

    list_init( &hidpad.files );
    pnp_init();     /* clarification 6: also announces the XInput slots, with or without a HID pad */
    if (!ios_hidpad_identity_select( getenv( "MADEIRA_HIDPAD" ), getenv( "MADEIRA_HIDPAD_NAME" ), &hidpad.id ))
        return;

    /* B6: the preparsed data hid.dll reads, built once from A3's descriptor */
    if (!ios_hidparse_parse( hidpad.id.descriptor, hidpad.id.descriptor_len, &hidpad.desc ))
    {
        ws_log( "[hidpad] %s: the report descriptor didn't parse, no device", hidpad.id.name );
        return;
    }

    if (!ios_hidpad_symlink_path( &hidpad.id, link, sizeof(link) ) ||
        !make_unicode( link, link_buf, sizeof(link_buf) / sizeof(WCHAR), &link_name ) ||
        !make_unicode( IOS_HIDPAD_DEVICE_OBJECT, device_buf, sizeof(device_buf) / sizeof(WCHAR), &device_name ) ||
        !make_unicode( IOS_HIDPAD_DEVICE_OBJECT, target_buf, sizeof(target_buf) / sizeof(WCHAR), &target ))
    {
        ws_log( "[hidpad] %s: names too long, no device", hidpad.id.name );
        return;
    }

    if (!(device = create_named_object( NULL, &hidpad_device_ops, &device_name, OBJ_PERMANENT, NULL )))
    {
        ws_log( "[hidpad] %s: creating %s failed, status %#x", hidpad.id.name, IOS_HIDPAD_DEVICE_OBJECT, get_error() );
        return;
    }
    release_object( device );

    if (!(symlink = create_symlink( NULL, &link_name, OBJ_PERMANENT, &target, NULL )))
    {
        ws_log( "[hidpad] %s: creating %s failed, status %#x", hidpad.id.name, link, get_error() );
        return;
    }
    release_object( symlink );
    clear_error();

    hidpad.requests.id = &hidpad.id;
    hidpad.requests.desc = &hidpad.desc;
    hidpad.requests.report_state = &hidpad.report_state;
    hidpad.requests.output = &hidpad.output;
    hidpad.requests.poll_interval = 0;      /* not polled */
    hidpad.enabled = 1;
    ws_log( "[hidpad] %s %04X:%04X v%04x \"%s\" / \"%s\" / \"%s\", descriptor %u bytes, input report %u, "
            "output %u, feature %u, preparsed %u bytes; %s -> %s",
            hidpad.id.name, hidpad.id.vendor_id, hidpad.id.product_id, hidpad.id.version,
            hidpad.id.product, hidpad.id.manufacturer, hidpad.id.serial,
            (unsigned int)hidpad.id.descriptor_len, (unsigned int)hidpad.desc.input_len,
            (unsigned int)hidpad.desc.output_len, (unsigned int)hidpad.desc.feature_len,
            (unsigned int)hidpad.desc.preparsed_len, link, IOS_HIDPAD_DEVICE_OBJECT );
}
