// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 Part D4: see w32ios_kmt_pure.h.

#include <string.h>
#include "w32ios_kmt_pure.h"

static void ascii_to_wide( w32ios_wchar *dst, size_t count, const char *src )
{
    size_t i;

    if (!count) return;
    for (i = 0; i + 1 < count && src && src[i]; i++) dst[i] = (unsigned char)src[i];
    dst[i] = 0;
}

#define NEED(type) do { if (size < sizeof(type)) { *status = W32IOS_STATUS_INVALID_PARAMETER; return 1; } } while (0)

int w32ios_kmt_answer( enum w32ios_qai type, void *data, uint32_t size,
                       const struct w32ios_kmt_adapter_desc *desc, uint32_t *status )
{
    *status = W32IOS_STATUS_SUCCESS;

    switch (type)
    {
    case W32IOS_QAI_OTHER:
        return 0;

    case W32IOS_QAI_DRIVERVERSION:
    {
        uint32_t version = W32IOS_KMT_DRIVERVERSION_WDDM_3_1;   /* D3DKMT_DRIVERVERSION */
        NEED(uint32_t);
        memcpy( data, &version, sizeof(version) );
        return 1;
    }
    case W32IOS_QAI_UMD_DRIVER_VERSION:
    case W32IOS_QAI_KMD_DRIVER_VERSION:
    {
        struct w32ios_kmt_driver_version *v = data;
        NEED(*v);
        v->DriverVersion = (int64_t)desc->driver_version;
        return 1;
    }
    case W32IOS_QAI_ADAPTERTYPE:
    {
        struct w32ios_kmt_adapter_type *t = data;
        NEED(*t);
        t->Value = W32IOS_ADAPTERTYPE_RENDER_SUPPORTED | W32IOS_ADAPTERTYPE_DISPLAY_SUPPORTED;
        return 1;
    }
    case W32IOS_QAI_PHYSICALADAPTERCOUNT:
    {
        struct w32ios_kmt_physical_adapter_count *c = data;
        NEED(*c);
        c->Count = 1;
        return 1;
    }
    case W32IOS_QAI_PHYSICALADAPTERDEVICEIDS:
    {
        struct w32ios_kmt_query_device_ids *ids = data;
        NEED(*ids);
        if (ids->PhysicalAdapterIndex) { *status = W32IOS_STATUS_INVALID_PARAMETER; return 1; }
        memset( &ids->DeviceIds, 0, sizeof(ids->DeviceIds) );
        ids->DeviceIds.VendorID = desc->vendor;
        ids->DeviceIds.DeviceID = desc->device;
        return 1;
    }
    case W32IOS_QAI_ADAPTERADDRESS:
    {
        struct w32ios_kmt_adapter_address *a = data;
        NEED(*a);
        a->BusNumber = a->DeviceNumber = a->FunctionNumber = 0;
        return 1;
    }
    case W32IOS_QAI_WDDM_1_2_CAPS:
    {
        struct w32ios_kmt_wddm_1_2_caps *c = data;
        NEED(*c);
        c->GraphicsPreemptionGranularity = W32IOS_GRAPHICS_PREEMPTION_PRIMITIVE_BOUNDARY;
        c->ComputePreemptionGranularity = W32IOS_COMPUTE_PREEMPTION_DISPATCH_BOUNDARY;
        /* SupportNonVGA, SupportSmoothRotation, SupportPerEngineTDR, SupportCCD,
         * SupportGammaRamp, SupportHWCursor, SupportHWVSync */
        c->Value = 0x1 | 0x2 | 0x4 | 0x10 | 0x40 | 0x80 | 0x100;
        return 1;
    }
    case W32IOS_QAI_WDDM_1_3_CAPS:
    {
        struct w32ios_kmt_wddm_1_3_caps *c = data;
        NEED(*c);
        c->Value = 0x8 | 0x20;   /* SupportPowerManagementPStates, SupportCrossAdapterResource */
        return 1;
    }
    case W32IOS_QAI_WDDM_2_0_CAPS:
    {
        struct w32ios_kmt_wddm_2_0_caps *c = data;
        NEED(*c);
        c->Value = W32IOS_WDDM20_64BIT_ATOMICS | W32IOS_WDDM20_GPU_MMU;
        return 1;
    }
    case W32IOS_QAI_WDDM_2_7_CAPS:
    {
        struct w32ios_kmt_wddm_2_7_caps *c = data;
        NEED(*c);
        c->Value = W32IOS_WDDM27_HWSCH_SUPPORTED | W32IOS_WDDM27_HWSCH_ENABLED;
        return 1;
    }
    case W32IOS_QAI_WDDM_2_9_CAPS:
    {
        struct w32ios_kmt_wddm_2_9_caps *c = data;
        NEED(*c);
        c->Value = W32IOS_FEATURE_SUPPORT_STABLE | W32IOS_WDDM29_HWSCH_ENABLED;
        return 1;
    }
    case W32IOS_QAI_WDDM_3_0_CAPS:
    {
        struct w32ios_kmt_wddm_3_0_caps *c = data;
        NEED(*c);
        c->Value = 0;   /* no hardware flip queue */
        return 1;
    }
    case W32IOS_QAI_WDDM_3_1_CAPS:
    {
        struct w32ios_kmt_wddm_3_1_caps *c = data;
        NEED(*c);
        c->Value = 0;   /* no native GPU fence */
        return 1;
    }
    case W32IOS_QAI_DRIVER_DESCRIPTION:
    {
        struct w32ios_kmt_driver_description *d = data;
        NEED(*d);
        ascii_to_wide( d->DriverDescription, 4096, desc->name );
        return 1;
    }
    case W32IOS_QAI_ADAPTERREGISTRYINFO:
    {
        struct w32ios_kmt_adapter_registry_info *r = data;
        NEED(*r);
        /* the strings the registry GPU's Class key holds (C6) */
        ascii_to_wide( r->AdapterString, 260, desc->name );
        ascii_to_wide( r->BiosString, 260, desc->name );
        ascii_to_wide( r->DacType, 260, "Integrated RAMDAC" );
        ascii_to_wide( r->ChipType, 260, desc->name );
        return 1;
    }
    case W32IOS_QAI_NODEMETADATA:
    {
        struct w32ios_kmt_node_metadata *n = data;
        NEED(*n);
        if (n->NodeOrdinalAndAdapterIndex & 0xffff) { *status = W32IOS_STATUS_INVALID_PARAMETER; return 1; }
        n->EngineType = W32IOS_ENGINE_TYPE_3D;
        ascii_to_wide( n->FriendlyName, 32, "3D" );
        n->Flags = 0;
        n->GpuMmuSupported = 1;
        n->IoMmuSupported = 0;
        return 1;
    }
    case W32IOS_QAI_GETSEGMENTSIZE:
    {
        struct w32ios_kmt_segment_size *s = data;
        NEED(*s);
        s->DedicatedVideoMemorySize = desc->dedicated;
        s->DedicatedSystemMemorySize = 0;
        s->SharedSystemMemorySize = 0;
        return 1;
    }
    case W32IOS_QAI_GETSEGMENTGROUPSIZE:
    {
        struct w32ios_kmt_segment_group_size *g = data;
        NEED(*g);
        if (g->PhysicalAdapterIndex) { *status = W32IOS_STATUS_INVALID_PARAMETER; return 1; }
        g->LegacyInfo.DedicatedVideoMemorySize = desc->dedicated;
        g->LegacyInfo.DedicatedSystemMemorySize = 0;
        g->LegacyInfo.SharedSystemMemorySize = 0;
        g->LocalMemory = desc->dedicated;
        g->NonLocalMemory = 0;
        g->NonBudgetMemory = 0;
        return 1;
    }
    case W32IOS_QAI_ADAPTERPERFDATA:
    {
        struct w32ios_kmt_adapter_perfdata *p = data;
        NEED(*p);
        if (p->PhysicalAdapterIndex) { *status = W32IOS_STATUS_INVALID_PARAMETER; return 1; }
        /* an idle desktop */
        p->MemoryFrequency = 810000000ull;
        p->MaxMemoryFrequency = 7501000000ull;
        p->MaxMemoryFrequencyOC = 7501000000ull;
        p->MemoryBandwidth = 1000000000ull;
        p->PCIEBandwidth = 250000000ull;
        p->FanRPM = 0;
        p->Power = 50;          /* 5.0 % */
        p->Temperature = 380;   /* 38.0 degrees Celsius */
        p->PowerStateOverride = 0;
        return 1;
    }
    case W32IOS_QAI_ADAPTERPERFDATA_CAPS:
    {
        struct w32ios_kmt_adapter_perfdata_caps *c = data;
        NEED(*c);
        if (c->PhysicalAdapterIndex) { *status = W32IOS_STATUS_INVALID_PARAMETER; return 1; }
        c->MaxMemoryBandwidth = 360000000000ull;
        c->MaxPCIEBandwidth = 31500000000ull;
        c->MaxFanRPM = 3000;
        c->TemperatureMax = 930;
        c->TemperatureWarning = 880;
        return 1;
    }
    }
    return 0;
}
