// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 clarification 6: arrivals and removals from samples of the app's stores. The event
// shape (a device interface of GUID_DEVINTERFACE_HID, DBT_DEVICEARRIVAL /
// DBT_DEVICEREMOVECOMPLETE) is Microsoft's documented WM_DEVICECHANGE contract and what Wine's
// ntoskrnl sends through plugplay (refs/wine-11.4/dlls/ntoskrnl.exe/pnp.c); the code is our own.

#include <string.h>

#include "ios_hidpad_pnp.h"

/* {4d1e55b2-f16f-11cf-88cb-001111000030} in GUID memory layout */
static const uint8_t guid_devinterface_hid[16] =
{
    0xb2, 0x55, 0x1e, 0x4d, 0x6f, 0xf1, 0xcf, 0x11, 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30,
};

unsigned int ios_pnp_watch_update(struct ios_pnp_watch *watch, const struct ios_pnp_sample *now,
                                  struct ios_pnp_change out[IOS_PNP_MAX_CHANGES])
{
    unsigned int n = 0;
    int i;

    if (!watch->started)
    {
        watch->started = 1;
        watch->last = *now;
        return 0;
    }

    if (!!now->hid_present != !!watch->last.hid_present)
    {
        out[n].code = now->hid_present ? IOS_PNP_ARRIVAL : IOS_PNP_REMOVECOMPLETE;
        out[n].which = -1;
        n++;
    }
    for (i = 0; i < IOS_PNP_SLOTS; i++)
    {
        if (!!now->slot[i] == !!watch->last.slot[i]) continue;
        out[n].code = now->slot[i] ? IOS_PNP_ARRIVAL : IOS_PNP_REMOVECOMPLETE;
        out[n].which = i;
        n++;
    }
    watch->last = *now;
    return n;
}

int ios_pnp_record_build(const struct ios_pnp_change *change, const struct ios_hidpad_identity *id,
                         struct ios_pnp_record *record)
{
    char name[IOS_PNP_NAME_MAX];
    size_t i, len;

    memset(record, 0, sizeof(*record));
    record->code = change->code;
    memcpy(record->class_guid, guid_devinterface_hid, sizeof(guid_devinterface_hid));

    if (change->which < 0)
    {
        if (!id || !(len = ios_hidpad_user_path(id, name, sizeof(name)))) return 0;
    }
    else
    {
        /* clarification 8: the slot's own device (ios_hidpad_identity_slot) */
        struct ios_hidpad_identity slot;
        if (!ios_hidpad_identity_slot((unsigned int)change->which, &slot)) return 0;
        if (!(len = ios_hidpad_user_path(&slot, name, sizeof(name)))) return 0;
    }

    for (i = 0; i < len; i++) record->name[i] = (uint8_t)name[i];
    record->name[len] = 0;
    return 1;
}

void ios_pnp_queue_push(struct ios_pnp_queue *queue, const struct ios_pnp_record *record)
{
    if (queue->count == IOS_PNP_QUEUE_MAX)
    {
        queue->head = (queue->head + 1) % IOS_PNP_QUEUE_MAX;
        queue->count--;
    }
    queue->records[(queue->head + queue->count) % IOS_PNP_QUEUE_MAX] = *record;
    queue->count++;
}

int ios_pnp_queue_pop(struct ios_pnp_queue *queue, struct ios_pnp_record *record)
{
    if (!queue->count) return 0;
    *record = queue->records[queue->head];
    queue->head = (queue->head + 1) % IOS_PNP_QUEUE_MAX;
    queue->count--;
    return 1;
}
