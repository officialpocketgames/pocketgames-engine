// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 15 Part B6: compiles upstream Wine 11.4's dlls/hidparse.sys/main.c UNCHANGED into the
// wineserver library and wraps it in plain C types (ios_hidparse.h).
//
// Licensing: this wrapper is MIT, but the object file it produces also contains Wine's
// hidparse code (LGPL-2.1-or-later, "Copyright 2021 Rémi Bernon for CodeWeavers"), which
// keeps its licence. See PROVENANCE.md.
//
// main.c is included as it is. Around it:
//   - its exported names (and its one other non-static function) are prefixed by macros,
//     so they can't clash with anything else linked into the app;
//   - its pool allocations (ExAllocatePool / ExFreePool) become malloc / free;
//   - its debug macros compile to nothing: the server thread has no Windows thread context,
//     so Wine's debug output functions must not be reached.
// The build passes the path of main.c as IOS_HIDPARSE_MAIN_C (a quoted string); the default
// is this repository's refs/wine-11.4 copy.

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define HidP_GetCollectionDescription  ios_hidparse_GetCollectionDescription
#define HidP_FreeCollectionDescription ios_hidparse_FreeCollectionDescription
#define DriverEntry                    ios_hidparse_DriverEntry
#define parse_descriptor               ios_hidparse_parse_descriptor

/* The same headers, in the same order, as main.c: its own includes then add nothing. */
#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"

#include <ddk/wdm.h>
#include <ddk/hidpddi.h>

#include "wine/hid.h"
#include "wine/list.h"
#include "wine/debug.h"

/* Pool allocations: plain heap memory (the declarations above are already parsed). */
#define ExAllocatePool(pool_type, size) malloc(size)
#define ExFreePool(ptr) free(ptr)

/* Debug output compiled out. */
#undef WINE_DEFAULT_DEBUG_CHANNEL
#define WINE_DEFAULT_DEBUG_CHANNEL(ch) \
    static const int ios_hidparse_channel_##ch __attribute__((unused)) = 0
#undef TRACE
#undef WARN
#undef ERR
#undef FIXME
#undef TRACE_ON
#define TRACE(...) do { } while (0)
#define WARN(...) do { } while (0)
#define ERR(...) do { } while (0)
#define FIXME(...) do { } while (0)
#define TRACE_ON(ch) 0
#define wine_dbg_sprintf(...) ""

#ifndef IOS_HIDPARSE_MAIN_C
#define IOS_HIDPARSE_MAIN_C "../../../refs/wine-11.4/dlls/hidparse.sys/main.c"
#endif
#include IOS_HIDPARSE_MAIN_C

#undef ExAllocatePool
#undef ExFreePool

#include "ios_hidparse.h"

int ios_hidparse_parse(const uint8_t *desc, size_t len, struct ios_hidparse_desc *out)
{
    HIDP_DEVICE_DESC device_desc;
    HIDP_COLLECTION_DESC *collection;
    ULONG i;

    memset(out, 0, sizeof(*out));
    if (!desc || !len) return 0;

    memset(&device_desc, 0, sizeof(device_desc));
    if (ios_hidparse_GetCollectionDescription((PHIDP_REPORT_DESCRIPTOR)desc, (ULONG)len, NonPagedPool,
                                              &device_desc) != HIDP_STATUS_SUCCESS)
        return 0;

    if (!device_desc.CollectionDescLength) goto failed;
    collection = &device_desc.CollectionDesc[0];
    if (!(out->preparsed = malloc(collection->PreparsedDataLength))) goto failed;
    memcpy(out->preparsed, collection->PreparsedData, collection->PreparsedDataLength);
    out->preparsed_len = collection->PreparsedDataLength;
    out->usage_page = collection->UsagePage;
    out->usage = collection->Usage;
    out->input_len = collection->InputLength;
    out->output_len = collection->OutputLength;
    out->feature_len = collection->FeatureLength;

    for (i = 0; i < device_desc.ReportIDsLength && i < 256; i++)
    {
        struct ios_hidparse_report *report = &out->reports[out->report_count++];
        report->id = device_desc.ReportIDs[i].ReportID;
        report->collection = (uint8_t)device_desc.ReportIDs[i].CollectionNumber;
        report->input_len = (uint16_t)device_desc.ReportIDs[i].InputLength;
        report->output_len = (uint16_t)device_desc.ReportIDs[i].OutputLength;
        report->feature_len = (uint16_t)device_desc.ReportIDs[i].FeatureLength;
    }

    ios_hidparse_FreeCollectionDescription(&device_desc);
    return 1;

failed:
    ios_hidparse_FreeCollectionDescription(&device_desc);
    free(out->preparsed);
    memset(out, 0, sizeof(*out));
    return 0;
}

void ios_hidparse_release(struct ios_hidparse_desc *desc)
{
    free(desc->preparsed);
    memset(desc, 0, sizeof(*desc));
}

/* ios_hidparse_find_report is pure and lives with the request rules in
 * hidpad/ios_hidpad_request.c. */
