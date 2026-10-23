// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 Part D: the D3DKMT adapter entry points (w32ios_kmt.c), for files that have the D3DKMT
// types (d3dkmt.c, sysparams.c). Include after w32ios.h.

#ifndef W32IOS_KMT_H
#define W32IOS_KMT_H

BOOL w32ios_kmt_is_adapter_luid( const LUID *luid );
NTSTATUS w32ios_kmt_enum_adapters2( D3DKMT_ENUMADAPTERS2 *desc );
NTSTATUS w32ios_kmt_open_adapter_from_hdc( D3DKMT_OPENADAPTERFROMHDC *desc );
NTSTATUS w32ios_kmt_open_adapter_from_device_name( D3DKMT_OPENADAPTERFROMDEVICENAME *desc );
/* TRUE (with *status) when the type is answered for the adapter, FALSE for upstream's answer */
BOOL w32ios_kmt_query_adapter_info( D3DKMT_QUERYADAPTERINFO *desc, NTSTATUS *status );
BOOL w32ios_kmt_statistics( D3DKMT_QUERYSTATISTICS *stats, NTSTATUS *status );
NTSTATUS w32ios_kmt_video_memory( D3DKMT_QUERYVIDEOMEMORYINFO *desc );

#endif /* W32IOS_KMT_H */
