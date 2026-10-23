// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 clarification 6: device arrival and removal without the plugplay service.
//
// On iOS no plugplay service runs, so nothing announces a device interface coming or going.
// The wineserver serves a small channel instead: every handle opened on \??\MyiosdeckPnp
// receives, one record per ReadFile, the arrivals and removals that happen after it was
// opened (the HID pad as the app publishes and unpublishes it, the XInput slots as pads come
// and go). sechost's device-notification thread reads it and hands each record to the
// registered notifications exactly as a plugplay event: a DEV_BROADCAST_DEVICEINTERFACE_W with
// the record's class and name, the code DBT_DEVICEARRIVAL or DBT_DEVICEREMOVECOMPLETE.
//
// Plain C with fixed-width types: the server (unix side) and sechost (Windows side) both
// include it.

#ifndef MYIOSDECK_IOS_PNP_H
#define MYIOSDECK_IOS_PNP_H

#include <stdint.h>

/* The channel's names: the device object, the symlink, the path a program opens. */
#define IOS_PNP_DEVICE_OBJECT   "\\Device\\MyiosdeckPnp"
#define IOS_PNP_SYMLINK         "\\??\\MyiosdeckPnp"
#define IOS_PNP_USER_PATH_W     L"\\\\?\\MyiosdeckPnp"

#define IOS_PNP_ARRIVAL         0x8000u   /* DBT_DEVICEARRIVAL */
#define IOS_PNP_REMOVECOMPLETE  0x8004u   /* DBT_DEVICEREMOVECOMPLETE */

#define IOS_PNP_NAME_MAX        200       /* UTF-16 units, with the terminator */

/* One record; a read returns exactly one (the buffer must hold sizeof(struct ios_pnp_record)). */
struct ios_pnp_record
{
    uint32_t code;                        /* IOS_PNP_ARRIVAL or IOS_PNP_REMOVECOMPLETE */
    uint8_t  class_guid[16];              /* the interface class, GUID memory layout */
    uint16_t name[IOS_PNP_NAME_MAX];      /* \\?\ path of the interface, NUL-terminated */
};

#ifdef __WINE_DBT_H   /* include dbt.h first */
#include <stddef.h>
#include <string.h>

/* The size of a DEV_BROADCAST_DEVICEINTERFACE_W that holds any record's name. */
#define IOS_PNP_BROADCAST_MAX offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name[IOS_PNP_NAME_MAX])

/* sechost's side: `record` as the broadcast a notification receives (`iface` holds
 * IOS_PNP_BROADCAST_MAX bytes). dbcc_size counts the name with its terminator, as plugplay's. */
static inline void ios_pnp_record_to_broadcast(struct ios_pnp_record *record,
                                               DEV_BROADCAST_DEVICEINTERFACE_W *iface)
{
    size_t len;

    record->name[IOS_PNP_NAME_MAX - 1] = 0;
    for (len = 0; record->name[len]; len++) ;
    iface->dbcc_size = (DWORD)offsetof(DEV_BROADCAST_DEVICEINTERFACE_W, dbcc_name[len + 1]);
    iface->dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    iface->dbcc_reserved = 0;
    memcpy(&iface->dbcc_classguid, record->class_guid, sizeof(iface->dbcc_classguid));
    memcpy(iface->dbcc_name, record->name, (len + 1) * sizeof(record->name[0]));
}
#endif

#endif
