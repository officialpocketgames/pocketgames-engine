// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 Part D: a D3DKMT adapter (opt-in). With MADEIRA_KMT_ADAPTER=1 (or on/true/yes) the GPU
// that DXGI reports (DXMT: Metal's default device) becomes a D3DKMT adapter: its LUID is
// bswap64(MTLDevice.registryID), its dedicated memory spec 05's vram-mb (256 MB or more) or
// 4096 MB; the registry GPU of Part C6 gets both. EnumAdapters2 lists it; OpenAdapterFromHdc,
// OpenAdapterFromDeviceName and OpenAdapterFromGdiDisplayName (C7) open it; QueryAdapterInfo,
// QueryStatistics and QueryVideoMemoryInfo answer for it. Off (the default), nothing here
// changes an answer: the registry GPU keeps its own LUID and 4096 MB.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ntstatus.h"
#include "ntgdi_private.h"
#include "win32u_private.h"
#include "ntuser_private.h"
#include "wine/debug.h"
#include "w32ios.h"
#include "w32ios_kmt.h"
#include "w32ios_kmt_pure.h"
#include "w32ios_metal.h"

#define DEFAULT_MEMORY (4096ull << 20)

static pthread_once_t luid_once = PTHREAD_ONCE_INIT;
static BOOL have_luid;
static LUID metal_luid;

BOOL w32ios_kmt_enabled( void )
{
    static int enabled = -1;
    if (enabled < 0) enabled = w32ios_env_bool( "MADEIRA_KMT_ADAPTER", FALSE );
    return enabled;
}

static void init_luid( void )
{
    uint64_t id, value;

    if (!w32ios_metal_registry_id( &id ))
    {
        w32ios_log( "MADEIRA_KMT_ADAPTER: no Metal device, so no D3DKMT adapter" );
        return;
    }
    value = w32ios_luid_from_registry_id( id );
    metal_luid.LowPart = (DWORD)value;
    metal_luid.HighPart = (LONG)(value >> 32);
    have_luid = TRUE;
    w32ios_log( "D3DKMT adapter: Metal registryID %016llx, LUID %08x:%08x, %u MB", (unsigned long long)id,
                (unsigned int)metal_luid.HighPart, (unsigned int)metal_luid.LowPart,
                (unsigned int)(w32ios_kmt_memory() >> 20) );
}

BOOL w32ios_kmt_luid( LUID *luid )
{
    if (!w32ios_kmt_enabled()) return FALSE;
    pthread_once( &luid_once, init_luid );
    if (!have_luid) return FALSE;
    *luid = metal_luid;
    return TRUE;
}

ULONGLONG w32ios_kmt_memory( void )
{
    static ULONGLONG memory;
    char value[32];

    if (!w32ios_kmt_enabled()) return DEFAULT_MEMORY;
    if (!memory)
        memory = w32ios_dedicated_memory( w32ios_settings_get( "vram-mb", value, sizeof(value) ) ? value : NULL,
                                          DEFAULT_MEMORY );
    return memory;
}

BOOL w32ios_kmt_is_adapter_luid( const LUID *luid )
{
    LUID ours;
    return w32ios_kmt_luid( &ours ) && luid->LowPart == ours.LowPart && luid->HighPart == ours.HighPart;
}

static NTSTATUS open_adapter( D3DKMT_HANDLE *handle, LUID *luid )
{
    D3DKMT_OPENADAPTERFROMLUID desc = {0};
    NTSTATUS status;

    if (!w32ios_kmt_luid( &desc.AdapterLuid )) return STATUS_UNSUCCESSFUL;
    if ((status = NtGdiDdDDIOpenAdapterFromLuid( &desc ))) return status;
    *handle = desc.hAdapter;
    *luid = desc.AdapterLuid;
    return STATUS_SUCCESS;
}

NTSTATUS w32ios_kmt_enum_adapters2( D3DKMT_ENUMADAPTERS2 *desc )
{
    D3DKMT_HANDLE handle;
    NTSTATUS status;
    ULONG count;
    LUID luid;

    if (!desc) return STATUS_INVALID_PARAMETER;
    count = w32ios_kmt_luid( &luid ) ? 1 : 0;
    if (!desc->pAdapters)
    {
        desc->NumAdapters = count;
        return STATUS_SUCCESS;
    }
    if (count > desc->NumAdapters) return STATUS_BUFFER_TOO_SMALL;
    if (count)
    {
        if ((status = open_adapter( &handle, &luid ))) return status;
        desc->pAdapters[0].hAdapter = handle;
        desc->pAdapters[0].AdapterLuid = luid;
        desc->pAdapters[0].NumOfSources = 1;
        desc->pAdapters[0].bPrecisePresentRegionsPreferred = FALSE;
    }
    desc->NumAdapters = count;
    return STATUS_SUCCESS;
}

NTSTATUS w32ios_kmt_open_adapter_from_hdc( D3DKMT_OPENADAPTERFROMHDC *desc )
{
    NTSTATUS status;

    if (!desc) return STATUS_INVALID_PARAMETER;
    if ((status = open_adapter( &desc->hAdapter, &desc->AdapterLuid ))) return status;
    desc->VidPnSourceId = 1;   /* \\.\DISPLAY1, as upstream numbers sources for GDI names */
    return STATUS_SUCCESS;
}

static WCHAR upper( WCHAR c )
{
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    if (c == '#') return '\\';
    return c;
}

NTSTATUS w32ios_kmt_open_adapter_from_device_name( D3DKMT_OPENADAPTERFROMDEVICENAME *desc )
{
    char expected[256];
    const char *name;
    UINT16 vendor, device;
    size_t i;

    if (!desc || !desc->pDeviceName) return STATUS_INVALID_PARAMETER;

    /* the adapter's interface: \\?\<its Enum path>\{display device arrival} with '#' for '\' */
    w32ios_gpu_identity( &vendor, &device, &name );
    snprintf( expected, sizeof(expected), "\\\\?\\PCI\\VEN_%04X&DEV_%04X&SUBSYS_00000000&REV_00\\00000000\\"
              "{1CA05180-A699-450A-9A0C-DE4FBE3DDD89}", vendor, device );
    for (i = 0; expected[i]; i++)
        if (i < 4 ? desc->pDeviceName[i] != (WCHAR)expected[i]
                  : upper( desc->pDeviceName[i] ) != (WCHAR)expected[i])
            return STATUS_INVALID_PARAMETER;
    if (desc->pDeviceName[i]) return STATUS_INVALID_PARAMETER;

    return open_adapter( &desc->hAdapter, &desc->AdapterLuid );
}

static enum w32ios_qai map_type( KMTQUERYADAPTERINFOTYPE type )
{
    switch (type)
    {
    case KMTQAITYPE_DRIVERVERSION:            return W32IOS_QAI_DRIVERVERSION;
    case KMTQAITYPE_UMD_DRIVER_VERSION:       return W32IOS_QAI_UMD_DRIVER_VERSION;
    case KMTQAITYPE_KMD_DRIVER_VERSION:       return W32IOS_QAI_KMD_DRIVER_VERSION;
    case KMTQAITYPE_ADAPTERTYPE:              return W32IOS_QAI_ADAPTERTYPE;
    case KMTQAITYPE_PHYSICALADAPTERCOUNT:     return W32IOS_QAI_PHYSICALADAPTERCOUNT;
    case KMTQAITYPE_PHYSICALADAPTERDEVICEIDS: return W32IOS_QAI_PHYSICALADAPTERDEVICEIDS;
    case KMTQAITYPE_ADAPTERADDRESS:           return W32IOS_QAI_ADAPTERADDRESS;
    case KMTQAITYPE_WDDM_1_2_CAPS:            return W32IOS_QAI_WDDM_1_2_CAPS;
    case KMTQAITYPE_WDDM_1_3_CAPS:            return W32IOS_QAI_WDDM_1_3_CAPS;
    case KMTQAITYPE_WDDM_2_0_CAPS:            return W32IOS_QAI_WDDM_2_0_CAPS;
    case KMTQAITYPE_WDDM_2_7_CAPS:            return W32IOS_QAI_WDDM_2_7_CAPS;
    case KMTQAITYPE_WDDM_2_9_CAPS:            return W32IOS_QAI_WDDM_2_9_CAPS;
    case KMTQAITYPE_WDDM_3_0_CAPS:            return W32IOS_QAI_WDDM_3_0_CAPS;
    case KMTQAITYPE_WDDM_3_1_CAPS:            return W32IOS_QAI_WDDM_3_1_CAPS;
    case KMTQAITYPE_DRIVER_DESCRIPTION:       return W32IOS_QAI_DRIVER_DESCRIPTION;
    case KMTQAITYPE_ADAPTERREGISTRYINFO:      return W32IOS_QAI_ADAPTERREGISTRYINFO;
    case KMTQAITYPE_NODEMETADATA:             return W32IOS_QAI_NODEMETADATA;
    case KMTQAITYPE_GETSEGMENTSIZE:           return W32IOS_QAI_GETSEGMENTSIZE;
    case KMTQAITYPE_GETSEGMENTGROUPSIZE:      return W32IOS_QAI_GETSEGMENTGROUPSIZE;
    case KMTQAITYPE_ADAPTERPERFDATA:          return W32IOS_QAI_ADAPTERPERFDATA;
    case KMTQAITYPE_ADAPTERPERFDATA_CAPS:     return W32IOS_QAI_ADAPTERPERFDATA_CAPS;
    default:                                  return W32IOS_QAI_OTHER;
    }
}

BOOL w32ios_kmt_query_adapter_info( D3DKMT_QUERYADAPTERINFO *desc, NTSTATUS *status )
{
    struct w32ios_kmt_adapter_desc adapter;
    enum w32ios_qai type = map_type( desc->Type );
    uint32_t result;

    if (type == W32IOS_QAI_OTHER) return FALSE;
    w32ios_gpu_identity( &adapter.vendor, &adapter.device, &adapter.name );
    adapter.dedicated = w32ios_kmt_memory();
    adapter.driver_version = w32ios_pack_version( w32ios_driver_version( adapter.vendor ) );
    if (!w32ios_kmt_answer( type, desc->pPrivateDriverData, desc->PrivateDriverDataSize, &adapter, &result ))
        return FALSE;
    *status = result;
    return TRUE;
}

BOOL w32ios_kmt_statistics( D3DKMT_QUERYSTATISTICS *stats, NTSTATUS *status )
{
    if (!stats || !w32ios_kmt_is_adapter_luid( &stats->AdapterLuid )) return FALSE;

    *status = STATUS_SUCCESS;
    memset( &stats->QueryResult, 0, sizeof(stats->QueryResult) );
    switch (stats->Type)
    {
    case D3DKMT_QUERYSTATISTICS_ADAPTER:
        stats->QueryResult.AdapterInformation.NbSegments = 1;
        stats->QueryResult.AdapterInformation.NodeCount = 1;
        stats->QueryResult.AdapterInformation.VidPnSourceCount = 1;
        break;
    case D3DKMT_QUERYSTATISTICS_PROCESS_ADAPTER:
        stats->QueryResult.ProcessAdapterInformation.NbSegments = 1;
        stats->QueryResult.ProcessAdapterInformation.NodeCount = 1;
        stats->QueryResult.ProcessAdapterInformation.VidPnSourceCount = 1;
        break;
    case D3DKMT_QUERYSTATISTICS_PROCESS:
        stats->QueryResult.ProcessInformation.NodeCount = 1;
        stats->QueryResult.ProcessInformation.VidPnSourceCount = 1;
        break;
    case D3DKMT_QUERYSTATISTICS_SEGMENT:
        if (stats->QuerySegment.SegmentId) *status = STATUS_INVALID_PARAMETER;
        else
        {
            stats->QueryResult.SegmentInformation.CommitLimit = w32ios_kmt_memory();
            stats->QueryResult.SegmentInformation.Aperture = 0;   /* local video memory */
        }
        break;
    case D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT:
        if (stats->QueryProcessSegment.SegmentId) *status = STATUS_INVALID_PARAMETER;
        break;
    case D3DKMT_QUERYSTATISTICS_NODE:
        if (stats->QueryNode.NodeId) *status = STATUS_INVALID_PARAMETER;
        break;
    case D3DKMT_QUERYSTATISTICS_PROCESS_NODE:
        if (stats->QueryProcessNode.NodeId) *status = STATUS_INVALID_PARAMETER;
        break;
    case D3DKMT_QUERYSTATISTICS_VIDPNSOURCE:
    case D3DKMT_QUERYSTATISTICS_PROCESS_VIDPNSOURCE:
        break;
    default:
        return FALSE;
    }
    return TRUE;
}

NTSTATUS w32ios_kmt_video_memory( D3DKMT_QUERYVIDEOMEMORYINFO *desc )
{
    desc->Budget = desc->MemorySegmentGroup == D3DKMT_MEMORY_SEGMENT_GROUP_LOCAL ? w32ios_kmt_memory() : 0;
    desc->CurrentUsage = 0;
    desc->CurrentReservation = 0;
    desc->AvailableForReservation = desc->Budget / 2;
    return STATUS_SUCCESS;
}
