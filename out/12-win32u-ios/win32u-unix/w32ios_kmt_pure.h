// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 Part D: D3DKMTQueryAdapterInfo answers for the D3DKMT adapter, as a WDDM 3.1 desktop
// driver would give them. Pure (no Wine headers): w32ios_kmt.c maps Wine's KMTQAITYPE_* values to
// the W32IOS_QAI_* below and the host tests call the answers directly.
//
// The structures are the Windows SDK's (d3dkmthk.h, documented with D3DKMTQueryAdapterInfo and
// the KMTQAITYPE_* types); Wine's headers lack them. Bit fields of the SDK are written here as
// one 32-bit value with the bits named below (first field = bit 0).

#ifndef W32IOS_KMT_PURE_H
#define W32IOS_KMT_PURE_H

#include <stdint.h>

typedef uint16_t w32ios_wchar;

/* KMTQAITYPE_ADAPTERTYPE: D3DKMT_ADAPTERTYPE */
struct w32ios_kmt_adapter_type { uint32_t Value; };
#define W32IOS_ADAPTERTYPE_RENDER_SUPPORTED   0x00000001u
#define W32IOS_ADAPTERTYPE_DISPLAY_SUPPORTED  0x00000002u
#define W32IOS_ADAPTERTYPE_SOFTWARE_DEVICE    0x00000004u

/* KMTQAITYPE_WDDM_1_2_CAPS: D3DKMT_WDDM_1_2_CAPS (D3DKMT_PREEMPTION_CAPS, then the flags) */
struct w32ios_kmt_wddm_1_2_caps
{
    uint32_t GraphicsPreemptionGranularity;   /* D3DKMDT_GRAPHICS_PREEMPTION_GRANULARITY */
    uint32_t ComputePreemptionGranularity;    /* D3DKMDT_COMPUTE_PREEMPTION_GRANULARITY */
    uint32_t Value;   /* SupportNonVGA, SupportSmoothRotation, SupportPerEngineTDR,
                         SupportKernelModeCommandBuffer, SupportCCD, SupportSoftwareDeviceBitmaps,
                         SupportGammaRamp, SupportHWCursor, SupportHWVSync,
                         SupportSurpriseRemovalInHibernation */
};
#define W32IOS_GRAPHICS_PREEMPTION_PRIMITIVE_BOUNDARY 200
#define W32IOS_COMPUTE_PREEMPTION_DISPATCH_BOUNDARY   200

/* KMTQAITYPE_WDDM_1_3_CAPS: SupportMiracast, IsHybridIntegratedGPU, IsHybridDiscreteGPU,
 * SupportPowerManagementPStates, SupportVirtualModes, SupportCrossAdapterResource */
struct w32ios_kmt_wddm_1_3_caps { uint32_t Value; };
/* KMTQAITYPE_WDDM_2_0_CAPS: Support64BitAtomics, GpuMmuSupported, IoMmuSupported,
 * FlipOverwriteSupported, SupportContextlessPresent, SupportSurpriseRemoval */
struct w32ios_kmt_wddm_2_0_caps { uint32_t Value; };
#define W32IOS_WDDM20_64BIT_ATOMICS   0x1u
#define W32IOS_WDDM20_GPU_MMU         0x2u
/* KMTQAITYPE_WDDM_2_7_CAPS: HwSchSupported, HwSchEnabled, HwSchEnabledByDefault,
 * IndependentVidPnVSyncControl */
struct w32ios_kmt_wddm_2_7_caps { uint32_t Value; };
#define W32IOS_WDDM27_HWSCH_SUPPORTED 0x1u
#define W32IOS_WDDM27_HWSCH_ENABLED   0x2u
/* KMTQAITYPE_WDDM_2_9_CAPS: HwSchSupportState:2 (DXGK_FEATURE_SUPPORT_*), HwSchEnabled,
 * SelfRefreshMemorySupported */
struct w32ios_kmt_wddm_2_9_caps { uint32_t Value; };
#define W32IOS_FEATURE_SUPPORT_STABLE 2u
#define W32IOS_WDDM29_HWSCH_ENABLED   0x4u
/* KMTQAITYPE_WDDM_3_0_CAPS: HwFlipQueueSupportState:2, HwFlipQueueEnabled, DisplayableSupported */
struct w32ios_kmt_wddm_3_0_caps { uint32_t Value; };
/* KMTQAITYPE_WDDM_3_1_CAPS: NativeGpuFenceSupported */
struct w32ios_kmt_wddm_3_1_caps { uint32_t Value; };

/* KMTQAITYPE_UMD_DRIVER_VERSION / KMTQAITYPE_KMD_DRIVER_VERSION */
struct w32ios_kmt_driver_version { int64_t DriverVersion; };   /* LARGE_INTEGER */

/* KMTQAITYPE_PHYSICALADAPTERCOUNT */
struct w32ios_kmt_physical_adapter_count { uint32_t Count; };

/* KMTQAITYPE_PHYSICALADAPTERDEVICEIDS: D3DKMT_QUERY_DEVICE_IDS */
struct w32ios_kmt_device_ids
{
    uint32_t VendorID, DeviceID, SubVendorID, SubSystemID, RevisionID, BusType;
};
struct w32ios_kmt_query_device_ids
{
    uint32_t PhysicalAdapterIndex;   /* in */
    struct w32ios_kmt_device_ids DeviceIds;
};

/* KMTQAITYPE_ADAPTERADDRESS */
struct w32ios_kmt_adapter_address { uint32_t BusNumber, DeviceNumber, FunctionNumber; };

/* KMTQAITYPE_DRIVER_DESCRIPTION */
struct w32ios_kmt_driver_description { w32ios_wchar DriverDescription[4096]; };

/* KMTQAITYPE_ADAPTERREGISTRYINFO */
struct w32ios_kmt_adapter_registry_info
{
    w32ios_wchar AdapterString[260];
    w32ios_wchar BiosString[260];
    w32ios_wchar DacType[260];
    w32ios_wchar ChipType[260];
};

/* KMTQAITYPE_NODEMETADATA: D3DKMT_NODEMETADATA with DXGK_NODEMETADATA */
struct w32ios_kmt_node_metadata
{
    uint32_t     NodeOrdinalAndAdapterIndex;   /* in: node ordinal in the low 16 bits */
    uint32_t     EngineType;                   /* DXGK_ENGINE_TYPE */
    w32ios_wchar FriendlyName[32];             /* DXGK_MAX_METADATA_NAME_LENGTH */
    uint32_t     Flags;                        /* DXGK_NODEMETADATA_FLAGS */
    uint8_t      GpuMmuSupported;              /* BOOLEAN */
    uint8_t      IoMmuSupported;               /* BOOLEAN */
};
#define W32IOS_ENGINE_TYPE_3D 1

/* KMTQAITYPE_GETSEGMENTSIZE: D3DKMT_SEGMENTSIZEINFO */
struct w32ios_kmt_segment_size
{
    uint64_t DedicatedVideoMemorySize;
    uint64_t DedicatedSystemMemorySize;
    uint64_t SharedSystemMemorySize;
};
/* KMTQAITYPE_GETSEGMENTGROUPSIZE: D3DKMT_SEGMENTGROUPSIZEINFO */
struct w32ios_kmt_segment_group_size
{
    uint32_t PhysicalAdapterIndex;   /* in */
    struct w32ios_kmt_segment_size LegacyInfo;
    uint64_t LocalMemory;
    uint64_t NonLocalMemory;
    uint64_t NonBudgetMemory;
};

/* KMTQAITYPE_ADAPTERPERFDATA: D3DKMT_ADAPTER_PERFDATA */
struct w32ios_kmt_adapter_perfdata
{
    uint32_t PhysicalAdapterIndex;   /* in */
    uint64_t MemoryFrequency;        /* Hz */
    uint64_t MaxMemoryFrequency;
    uint64_t MaxMemoryFrequencyOC;
    uint64_t MemoryBandwidth;        /* bytes per second */
    uint64_t PCIEBandwidth;
    uint32_t FanRPM;
    uint32_t Power;                  /* tenths of a percent */
    uint32_t Temperature;            /* tenths of a degree Celsius */
    uint8_t  PowerStateOverride;
};
/* KMTQAITYPE_ADAPTERPERFDATA_CAPS: D3DKMT_ADAPTER_PERFDATACAPS */
struct w32ios_kmt_adapter_perfdata_caps
{
    uint32_t PhysicalAdapterIndex;   /* in */
    uint64_t MaxMemoryBandwidth;
    uint64_t MaxPCIEBandwidth;
    uint32_t MaxFanRPM;
    uint32_t TemperatureMax;
    uint32_t TemperatureWarning;
};

_Static_assert( sizeof(struct w32ios_kmt_adapter_type) == 4, "D3DKMT_ADAPTERTYPE" );
_Static_assert( sizeof(struct w32ios_kmt_wddm_1_2_caps) == 12, "D3DKMT_WDDM_1_2_CAPS" );
_Static_assert( sizeof(struct w32ios_kmt_wddm_1_3_caps) == 4, "D3DKMT_WDDM_1_3_CAPS" );
_Static_assert( sizeof(struct w32ios_kmt_wddm_2_0_caps) == 4, "D3DKMT_WDDM_2_0_CAPS" );
_Static_assert( sizeof(struct w32ios_kmt_wddm_2_7_caps) == 4, "D3DKMT_WDDM_2_7_CAPS" );
_Static_assert( sizeof(struct w32ios_kmt_wddm_2_9_caps) == 4, "D3DKMT_WDDM_2_9_CAPS" );
_Static_assert( sizeof(struct w32ios_kmt_wddm_3_0_caps) == 4, "D3DKMT_WDDM_3_0_CAPS" );
_Static_assert( sizeof(struct w32ios_kmt_wddm_3_1_caps) == 4, "D3DKMT_WDDM_3_1_CAPS" );
_Static_assert( sizeof(struct w32ios_kmt_driver_version) == 8, "D3DKMT_UMD_DRIVER_VERSION" );
_Static_assert( sizeof(struct w32ios_kmt_physical_adapter_count) == 4, "D3DKMT_PHYSICAL_ADAPTER_COUNT" );
_Static_assert( sizeof(struct w32ios_kmt_query_device_ids) == 28, "D3DKMT_QUERY_DEVICE_IDS" );
_Static_assert( sizeof(struct w32ios_kmt_adapter_address) == 12, "D3DKMT_ADAPTERADDRESS" );
_Static_assert( sizeof(struct w32ios_kmt_driver_description) == 8192, "D3DKMT_DRIVER_DESCRIPTION" );
_Static_assert( sizeof(struct w32ios_kmt_adapter_registry_info) == 2080, "D3DKMT_ADAPTERREGISTRYINFO" );
_Static_assert( sizeof(struct w32ios_kmt_node_metadata) == 80, "D3DKMT_NODEMETADATA" );
_Static_assert( sizeof(struct w32ios_kmt_segment_size) == 24, "D3DKMT_SEGMENTSIZEINFO" );
_Static_assert( sizeof(struct w32ios_kmt_segment_group_size) == 56, "D3DKMT_SEGMENTGROUPSIZEINFO" );
_Static_assert( sizeof(struct w32ios_kmt_adapter_perfdata) == 64, "D3DKMT_ADAPTER_PERFDATA" );
_Static_assert( sizeof(struct w32ios_kmt_adapter_perfdata_caps) == 40, "D3DKMT_ADAPTER_PERFDATACAPS" );

/* the query types answered (w32ios_kmt.c maps Wine's KMTQAITYPE_* to these) */
enum w32ios_qai
{
    W32IOS_QAI_OTHER,                 /* keeps upstream's answer */
    W32IOS_QAI_DRIVERVERSION,
    W32IOS_QAI_UMD_DRIVER_VERSION,
    W32IOS_QAI_KMD_DRIVER_VERSION,
    W32IOS_QAI_ADAPTERTYPE,
    W32IOS_QAI_PHYSICALADAPTERCOUNT,
    W32IOS_QAI_PHYSICALADAPTERDEVICEIDS,
    W32IOS_QAI_ADAPTERADDRESS,
    W32IOS_QAI_WDDM_1_2_CAPS,
    W32IOS_QAI_WDDM_1_3_CAPS,
    W32IOS_QAI_WDDM_2_0_CAPS,
    W32IOS_QAI_WDDM_2_7_CAPS,
    W32IOS_QAI_WDDM_2_9_CAPS,
    W32IOS_QAI_WDDM_3_0_CAPS,
    W32IOS_QAI_WDDM_3_1_CAPS,
    W32IOS_QAI_DRIVER_DESCRIPTION,
    W32IOS_QAI_ADAPTERREGISTRYINFO,
    W32IOS_QAI_NODEMETADATA,
    W32IOS_QAI_GETSEGMENTSIZE,
    W32IOS_QAI_GETSEGMENTGROUPSIZE,
    W32IOS_QAI_ADAPTERPERFDATA,
    W32IOS_QAI_ADAPTERPERFDATA_CAPS,
};

#define W32IOS_KMT_DRIVERVERSION_WDDM_3_1 3100   /* KMT_DRIVERVERSION_WDDM_3_1 */
#define W32IOS_STATUS_SUCCESS           0x00000000u
#define W32IOS_STATUS_INVALID_PARAMETER 0xc000000du

/* what the answers are made of */
struct w32ios_kmt_adapter_desc
{
    uint16_t vendor, device;
    uint64_t dedicated;          /* bytes */
    uint64_t driver_version;     /* C6's QWORD */
    const char *name;            /* the registry GPU's name (ASCII) */
};

/* Fills data for type and returns 1 with *status set (SUCCESS, or INVALID_PARAMETER for a short
 * buffer or a physical adapter / node index other than 0); returns 0 for W32IOS_QAI_OTHER. */
int w32ios_kmt_answer( enum w32ios_qai type, void *data, uint32_t size,
                       const struct w32ios_kmt_adapter_desc *desc, uint32_t *status );

#endif /* W32IOS_KMT_PURE_H */
