/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 the MYIOSDECK author */

/*
 * Spec 19 Part A9: the decisions of nsi.dll's fallback to its unix-call table when \\.\Nsi
 * doesn't exist (the nsi.dll side of spec 13 Part I1). Pure code shared by nsi.c and the host
 * tests.
 *
 * The pending-request functions work on the includer's OVERLAPPED: define NSI_OVL as the type
 * (it needs the fields Internal, InternalHigh and hEvent), NSI_HANDLE as the handle type, and
 * NSI_RESET_EVENT(h) / NSI_SET_EVENT(h) before including this file. nsi.c maps them to
 * OVERLAPPED, HANDLE, ResetEvent and SetEvent; the tests to a fake.
 */

#ifndef MYIOSDECK_NSI_FALLBACK_H
#define MYIOSDECK_NSI_FALLBACK_H

#include <stddef.h>

/* the unix-call table's entries (spec 13 Part I1) */
enum nsi_unix_entry
{
    NSI_UNIX_ENUMERATE_ALL = 0,       /* struct nsi_enumerate_all_ex *, count in/out */
    NSI_UNIX_GET_ALL_PARAMETERS = 1,  /* struct nsi_get_all_parameters_ex * */
    NSI_UNIX_GET_PARAMETER = 2,       /* struct nsi_get_parameter_ex * */
};

/* the status and error values involved, by number (ntstatus.h, winerror.h) */
#define NSI_FB_STATUS_SUCCESS          0x00000000u
#define NSI_FB_STATUS_PENDING          0x00000103u
#define NSI_FB_STATUS_BUFFER_OVERFLOW  0x80000005u
#define NSI_FB_STATUS_NOT_SUPPORTED    0xc00000bbu
#define NSI_FB_STATUS_CANCELLED        0xc0000120u
#define NSI_FB_ERROR_SUCCESS           0u
#define NSI_FB_ERROR_FILE_NOT_FOUND    2u
#define NSI_FB_ERROR_MORE_DATA         234u
#define NSI_FB_ERROR_IO_PENDING        997u
#define NSI_FB_ERROR_NOT_FOUND         1168u

/* Item 7: a switch is off when its variable is set and starts with '0'. value is the variable's
 * value, NULL when unset. */
static inline int nsi_switch_on( const char *value )
{
    return !(value && value[0] == '0');
}

/* Items 4 and 5: what a status from the unix-call table becomes */
enum nsi_status_action
{
    NSI_MAP_DEVICE_ERROR,   /* return the error opening \\.\Nsi gave */
    NSI_MAP_MORE_DATA,      /* return ERROR_MORE_DATA */
    NSI_MAP_CONVERT,        /* return RtlNtStatusToDosError( status ) */
};

/* enumerate: entry 0 */
static inline enum nsi_status_action nsi_map_enumerate_status( unsigned int status )
{
    if (status == NSI_FB_STATUS_BUFFER_OVERFLOW) return NSI_MAP_MORE_DATA;
    if (status == NSI_FB_STATUS_NOT_SUPPORTED) return NSI_MAP_DEVICE_ERROR;
    return NSI_MAP_CONVERT;
}

/* one row or one field: entries 1 and 2 */
static inline enum nsi_status_action nsi_map_parameter_status( unsigned int status )
{
    if (status == NSI_FB_STATUS_NOT_SUPPORTED) return NSI_MAP_DEVICE_ERROR;
    return NSI_MAP_CONVERT;
}

/* Item 2: entries 1 and 2 are used when entry 0 answered the count-only probe with this status */
static inline int nsi_probe_allows_parameters( unsigned int status )
{
    return status != NSI_FB_STATUS_NOT_SUPPORTED;
}

/* Item 3: is a failed open of \\.\Nsi remembered? */
static inline int nsi_remember_missing_device( unsigned int open_error, int fallback, int cache_switch_on )
{
    return open_error == NSI_FB_ERROR_FILE_NOT_FOUND && fallback && cache_switch_on;
}

/* Item 6: do change requests stay pending? */
static inline int nsi_notify_stays_pending( unsigned int device_error, int fallback, int pending_switch_on )
{
    return device_error == NSI_FB_ERROR_FILE_NOT_FOUND && fallback && pending_switch_on;
}

#ifdef NSI_OVL

/* the event of an OVERLAPPED without Windows' "don't post to a completion port" flag (bit 0) */
static inline NSI_HANDLE nsi_ovl_event( const NSI_OVL *ovr )
{
    return (NSI_HANDLE)((size_t)ovr->hEvent & ~(size_t)1);
}

/* Item 6: an overlapped change request that never completes */
static inline unsigned int nsi_pending_request( NSI_OVL *ovr )
{
    NSI_HANDLE event = nsi_ovl_event( ovr );

    ovr->Internal = NSI_FB_STATUS_PENDING;
    ovr->InternalHigh = 0;
    if (event) NSI_RESET_EVENT( event );
    return NSI_FB_ERROR_IO_PENDING;
}

/* Item 6: cancelling such a request */
static inline unsigned int nsi_pending_cancel( NSI_OVL *ovr )
{
    NSI_HANDLE event;

    if (!ovr || ovr->Internal != NSI_FB_STATUS_PENDING) return NSI_FB_ERROR_NOT_FOUND;
    ovr->Internal = NSI_FB_STATUS_CANCELLED;
    if ((event = nsi_ovl_event( ovr ))) NSI_SET_EVENT( event );
    return NSI_FB_ERROR_SUCCESS;
}

#endif /* NSI_OVL */

#endif /* MYIOSDECK_NSI_FALLBACK_H */
