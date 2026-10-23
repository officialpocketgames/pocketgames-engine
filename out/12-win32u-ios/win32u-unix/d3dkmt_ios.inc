// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Included at the end of d3dkmt.c (patches/D-01-d3dkmt.patch), under WINE_IOS. Spec 12 Part D:
// an adapter handle is the D3DKMT adapter when it was opened for its LUID.

static BOOL w32ios_is_kmt_adapter( D3DKMT_HANDLE handle )
{
    struct d3dkmt_adapter *adapter;

    if (!w32ios_kmt_enabled()) return FALSE;
    if (!(adapter = get_d3dkmt_object( handle, D3DKMT_ADAPTER ))) return FALSE;
    return w32ios_kmt_is_adapter_luid( &adapter->luid );
}
