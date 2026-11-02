// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarification 3 (call statistics): every (interface, method) pair the
// Direct3D 9 frontend counts, as X-macro lists usable from C and C++. Pure
// preprocessor text: no includes, no types.
//
// X(label, vtable, method):
//   label  the interface named in the report ("<label>::<method>"). It is the
//          interface the method belongs to as an app sees it: the base name for
//          the IUnknown and pre-Ex methods of the three Ex objects
//          (IDirect3D9, IDirect3DDevice9, IDirect3DSwapChain9), the Ex name for
//          the methods only the Ex interface declares, and the object's own
//          interface for every other object (IDirect3DTexture9::AddRef,
//          IDirect3DTexture9::GetDevice, ...), so each object type has its own
//          counters.
//   vtable the interface whose vtable the frontend hands out for that object
//          (d3d9.h's <vtable>Vtbl in C), or none for the three lists after the
//          d3d9.h interfaces.
//   method the vtable slot's name.
// Each list holds its vtable's slots in order, slot 0 first, all of them.
// tests/test_d3d9_call_list.c checks the d3d9.h lists against d3d9.h's
// vtables, slot by slot; tests/check-d3d9-call-sites.sh checks the three other
// lists against d3d9.cpp, d3d9_diag.hpp and d3d9.def, and that every pair is
// counted exactly once, first thing in the method that implements it.

#pragma once

#define D9_CALLS_IDirect3D9Ex(X) \
  X(IDirect3D9, IDirect3D9Ex, QueryInterface) \
  X(IDirect3D9, IDirect3D9Ex, AddRef) \
  X(IDirect3D9, IDirect3D9Ex, Release) \
  X(IDirect3D9, IDirect3D9Ex, RegisterSoftwareDevice) \
  X(IDirect3D9, IDirect3D9Ex, GetAdapterCount) \
  X(IDirect3D9, IDirect3D9Ex, GetAdapterIdentifier) \
  X(IDirect3D9, IDirect3D9Ex, GetAdapterModeCount) \
  X(IDirect3D9, IDirect3D9Ex, EnumAdapterModes) \
  X(IDirect3D9, IDirect3D9Ex, GetAdapterDisplayMode) \
  X(IDirect3D9, IDirect3D9Ex, CheckDeviceType) \
  X(IDirect3D9, IDirect3D9Ex, CheckDeviceFormat) \
  X(IDirect3D9, IDirect3D9Ex, CheckDeviceMultiSampleType) \
  X(IDirect3D9, IDirect3D9Ex, CheckDepthStencilMatch) \
  X(IDirect3D9, IDirect3D9Ex, CheckDeviceFormatConversion) \
  X(IDirect3D9, IDirect3D9Ex, GetDeviceCaps) \
  X(IDirect3D9, IDirect3D9Ex, GetAdapterMonitor) \
  X(IDirect3D9, IDirect3D9Ex, CreateDevice) \
  X(IDirect3D9Ex, IDirect3D9Ex, GetAdapterModeCountEx) \
  X(IDirect3D9Ex, IDirect3D9Ex, EnumAdapterModesEx) \
  X(IDirect3D9Ex, IDirect3D9Ex, GetAdapterDisplayModeEx) \
  X(IDirect3D9Ex, IDirect3D9Ex, CreateDeviceEx) \
  X(IDirect3D9Ex, IDirect3D9Ex, GetAdapterLUID)

#define D9_CALLS_IDirect3DDevice9Ex(X) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, QueryInterface) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, AddRef) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, Release) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, TestCooperativeLevel) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetAvailableTextureMem) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, EvictManagedResources) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetDirect3D) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetDeviceCaps) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetDisplayMode) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetCreationParameters) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetCursorProperties) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetCursorPosition) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, ShowCursor) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, CreateAdditionalSwapChain) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetSwapChain) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetNumberOfSwapChains) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, Reset) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, Present) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetBackBuffer) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetRasterStatus) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetDialogBoxMode) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetGammaRamp) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetGammaRamp) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, CreateTexture) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, CreateVolumeTexture) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, CreateCubeTexture) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, CreateVertexBuffer) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, CreateIndexBuffer) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, CreateRenderTarget) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, CreateDepthStencilSurface) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, UpdateSurface) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, UpdateTexture) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetRenderTargetData) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetFrontBufferData) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, StretchRect) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, ColorFill) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, CreateOffscreenPlainSurface) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetRenderTarget) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetRenderTarget) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetDepthStencilSurface) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetDepthStencilSurface) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, BeginScene) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, EndScene) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, Clear) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetTransform) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetTransform) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, MultiplyTransform) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetViewport) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetViewport) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetMaterial) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetMaterial) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetLight) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetLight) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, LightEnable) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetLightEnable) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetClipPlane) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetClipPlane) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetRenderState) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetRenderState) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, CreateStateBlock) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, BeginStateBlock) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, EndStateBlock) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetClipStatus) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetClipStatus) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetTexture) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetTexture) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetTextureStageState) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetTextureStageState) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetSamplerState) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetSamplerState) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, ValidateDevice) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetPaletteEntries) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetPaletteEntries) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetCurrentTexturePalette) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetCurrentTexturePalette) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetScissorRect) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetScissorRect) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetSoftwareVertexProcessing) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetSoftwareVertexProcessing) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetNPatchMode) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetNPatchMode) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, DrawPrimitive) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, DrawIndexedPrimitive) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, DrawPrimitiveUP) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, DrawIndexedPrimitiveUP) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, ProcessVertices) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, CreateVertexDeclaration) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetVertexDeclaration) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetVertexDeclaration) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetFVF) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetFVF) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, CreateVertexShader) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetVertexShader) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetVertexShader) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetVertexShaderConstantF) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetVertexShaderConstantF) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetVertexShaderConstantI) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetVertexShaderConstantI) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetVertexShaderConstantB) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetVertexShaderConstantB) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetStreamSource) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetStreamSource) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetStreamSourceFreq) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetStreamSourceFreq) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetIndices) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetIndices) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, CreatePixelShader) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetPixelShader) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetPixelShader) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetPixelShaderConstantF) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetPixelShaderConstantF) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetPixelShaderConstantI) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetPixelShaderConstantI) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, SetPixelShaderConstantB) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, GetPixelShaderConstantB) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, DrawRectPatch) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, DrawTriPatch) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, DeletePatch) \
  X(IDirect3DDevice9, IDirect3DDevice9Ex, CreateQuery) \
  X(IDirect3DDevice9Ex, IDirect3DDevice9Ex, SetConvolutionMonoKernel) \
  X(IDirect3DDevice9Ex, IDirect3DDevice9Ex, ComposeRects) \
  X(IDirect3DDevice9Ex, IDirect3DDevice9Ex, PresentEx) \
  X(IDirect3DDevice9Ex, IDirect3DDevice9Ex, GetGPUThreadPriority) \
  X(IDirect3DDevice9Ex, IDirect3DDevice9Ex, SetGPUThreadPriority) \
  X(IDirect3DDevice9Ex, IDirect3DDevice9Ex, WaitForVBlank) \
  X(IDirect3DDevice9Ex, IDirect3DDevice9Ex, CheckResourceResidency) \
  X(IDirect3DDevice9Ex, IDirect3DDevice9Ex, SetMaximumFrameLatency) \
  X(IDirect3DDevice9Ex, IDirect3DDevice9Ex, GetMaximumFrameLatency) \
  X(IDirect3DDevice9Ex, IDirect3DDevice9Ex, CheckDeviceState) \
  X(IDirect3DDevice9Ex, IDirect3DDevice9Ex, CreateRenderTargetEx) \
  X(IDirect3DDevice9Ex, IDirect3DDevice9Ex, CreateOffscreenPlainSurfaceEx) \
  X(IDirect3DDevice9Ex, IDirect3DDevice9Ex, CreateDepthStencilSurfaceEx) \
  X(IDirect3DDevice9Ex, IDirect3DDevice9Ex, ResetEx) \
  X(IDirect3DDevice9Ex, IDirect3DDevice9Ex, GetDisplayModeEx)

#define D9_CALLS_IDirect3DSwapChain9Ex(X) \
  X(IDirect3DSwapChain9, IDirect3DSwapChain9Ex, QueryInterface) \
  X(IDirect3DSwapChain9, IDirect3DSwapChain9Ex, AddRef) \
  X(IDirect3DSwapChain9, IDirect3DSwapChain9Ex, Release) \
  X(IDirect3DSwapChain9, IDirect3DSwapChain9Ex, Present) \
  X(IDirect3DSwapChain9, IDirect3DSwapChain9Ex, GetFrontBufferData) \
  X(IDirect3DSwapChain9, IDirect3DSwapChain9Ex, GetBackBuffer) \
  X(IDirect3DSwapChain9, IDirect3DSwapChain9Ex, GetRasterStatus) \
  X(IDirect3DSwapChain9, IDirect3DSwapChain9Ex, GetDisplayMode) \
  X(IDirect3DSwapChain9, IDirect3DSwapChain9Ex, GetDevice) \
  X(IDirect3DSwapChain9, IDirect3DSwapChain9Ex, GetPresentParameters) \
  X(IDirect3DSwapChain9Ex, IDirect3DSwapChain9Ex, GetLastPresentCount) \
  X(IDirect3DSwapChain9Ex, IDirect3DSwapChain9Ex, GetPresentStats) \
  X(IDirect3DSwapChain9Ex, IDirect3DSwapChain9Ex, GetDisplayModeEx)

#define D9_CALLS_IDirect3DSurface9(X) \
  X(IDirect3DSurface9, IDirect3DSurface9, QueryInterface) \
  X(IDirect3DSurface9, IDirect3DSurface9, AddRef) \
  X(IDirect3DSurface9, IDirect3DSurface9, Release) \
  X(IDirect3DSurface9, IDirect3DSurface9, GetDevice) \
  X(IDirect3DSurface9, IDirect3DSurface9, SetPrivateData) \
  X(IDirect3DSurface9, IDirect3DSurface9, GetPrivateData) \
  X(IDirect3DSurface9, IDirect3DSurface9, FreePrivateData) \
  X(IDirect3DSurface9, IDirect3DSurface9, SetPriority) \
  X(IDirect3DSurface9, IDirect3DSurface9, GetPriority) \
  X(IDirect3DSurface9, IDirect3DSurface9, PreLoad) \
  X(IDirect3DSurface9, IDirect3DSurface9, GetType) \
  X(IDirect3DSurface9, IDirect3DSurface9, GetContainer) \
  X(IDirect3DSurface9, IDirect3DSurface9, GetDesc) \
  X(IDirect3DSurface9, IDirect3DSurface9, LockRect) \
  X(IDirect3DSurface9, IDirect3DSurface9, UnlockRect) \
  X(IDirect3DSurface9, IDirect3DSurface9, GetDC) \
  X(IDirect3DSurface9, IDirect3DSurface9, ReleaseDC)

#define D9_CALLS_IDirect3DTexture9(X) \
  X(IDirect3DTexture9, IDirect3DTexture9, QueryInterface) \
  X(IDirect3DTexture9, IDirect3DTexture9, AddRef) \
  X(IDirect3DTexture9, IDirect3DTexture9, Release) \
  X(IDirect3DTexture9, IDirect3DTexture9, GetDevice) \
  X(IDirect3DTexture9, IDirect3DTexture9, SetPrivateData) \
  X(IDirect3DTexture9, IDirect3DTexture9, GetPrivateData) \
  X(IDirect3DTexture9, IDirect3DTexture9, FreePrivateData) \
  X(IDirect3DTexture9, IDirect3DTexture9, SetPriority) \
  X(IDirect3DTexture9, IDirect3DTexture9, GetPriority) \
  X(IDirect3DTexture9, IDirect3DTexture9, PreLoad) \
  X(IDirect3DTexture9, IDirect3DTexture9, GetType) \
  X(IDirect3DTexture9, IDirect3DTexture9, SetLOD) \
  X(IDirect3DTexture9, IDirect3DTexture9, GetLOD) \
  X(IDirect3DTexture9, IDirect3DTexture9, GetLevelCount) \
  X(IDirect3DTexture9, IDirect3DTexture9, SetAutoGenFilterType) \
  X(IDirect3DTexture9, IDirect3DTexture9, GetAutoGenFilterType) \
  X(IDirect3DTexture9, IDirect3DTexture9, GenerateMipSubLevels) \
  X(IDirect3DTexture9, IDirect3DTexture9, GetLevelDesc) \
  X(IDirect3DTexture9, IDirect3DTexture9, GetSurfaceLevel) \
  X(IDirect3DTexture9, IDirect3DTexture9, LockRect) \
  X(IDirect3DTexture9, IDirect3DTexture9, UnlockRect) \
  X(IDirect3DTexture9, IDirect3DTexture9, AddDirtyRect)

#define D9_CALLS_IDirect3DCubeTexture9(X) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, QueryInterface) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, AddRef) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, Release) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, GetDevice) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, SetPrivateData) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, GetPrivateData) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, FreePrivateData) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, SetPriority) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, GetPriority) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, PreLoad) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, GetType) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, SetLOD) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, GetLOD) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, GetLevelCount) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, SetAutoGenFilterType) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, GetAutoGenFilterType) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, GenerateMipSubLevels) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, GetLevelDesc) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, GetCubeMapSurface) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, LockRect) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, UnlockRect) \
  X(IDirect3DCubeTexture9, IDirect3DCubeTexture9, AddDirtyRect)

#define D9_CALLS_IDirect3DVolumeTexture9(X) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, QueryInterface) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, AddRef) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, Release) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, GetDevice) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, SetPrivateData) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, GetPrivateData) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, FreePrivateData) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, SetPriority) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, GetPriority) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, PreLoad) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, GetType) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, SetLOD) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, GetLOD) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, GetLevelCount) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, SetAutoGenFilterType) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, GetAutoGenFilterType) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, GenerateMipSubLevels) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, GetLevelDesc) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, GetVolumeLevel) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, LockBox) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, UnlockBox) \
  X(IDirect3DVolumeTexture9, IDirect3DVolumeTexture9, AddDirtyBox)

#define D9_CALLS_IDirect3DVolume9(X) \
  X(IDirect3DVolume9, IDirect3DVolume9, QueryInterface) \
  X(IDirect3DVolume9, IDirect3DVolume9, AddRef) \
  X(IDirect3DVolume9, IDirect3DVolume9, Release) \
  X(IDirect3DVolume9, IDirect3DVolume9, GetDevice) \
  X(IDirect3DVolume9, IDirect3DVolume9, SetPrivateData) \
  X(IDirect3DVolume9, IDirect3DVolume9, GetPrivateData) \
  X(IDirect3DVolume9, IDirect3DVolume9, FreePrivateData) \
  X(IDirect3DVolume9, IDirect3DVolume9, GetContainer) \
  X(IDirect3DVolume9, IDirect3DVolume9, GetDesc) \
  X(IDirect3DVolume9, IDirect3DVolume9, LockBox) \
  X(IDirect3DVolume9, IDirect3DVolume9, UnlockBox)

#define D9_CALLS_IDirect3DVertexBuffer9(X) \
  X(IDirect3DVertexBuffer9, IDirect3DVertexBuffer9, QueryInterface) \
  X(IDirect3DVertexBuffer9, IDirect3DVertexBuffer9, AddRef) \
  X(IDirect3DVertexBuffer9, IDirect3DVertexBuffer9, Release) \
  X(IDirect3DVertexBuffer9, IDirect3DVertexBuffer9, GetDevice) \
  X(IDirect3DVertexBuffer9, IDirect3DVertexBuffer9, SetPrivateData) \
  X(IDirect3DVertexBuffer9, IDirect3DVertexBuffer9, GetPrivateData) \
  X(IDirect3DVertexBuffer9, IDirect3DVertexBuffer9, FreePrivateData) \
  X(IDirect3DVertexBuffer9, IDirect3DVertexBuffer9, SetPriority) \
  X(IDirect3DVertexBuffer9, IDirect3DVertexBuffer9, GetPriority) \
  X(IDirect3DVertexBuffer9, IDirect3DVertexBuffer9, PreLoad) \
  X(IDirect3DVertexBuffer9, IDirect3DVertexBuffer9, GetType) \
  X(IDirect3DVertexBuffer9, IDirect3DVertexBuffer9, Lock) \
  X(IDirect3DVertexBuffer9, IDirect3DVertexBuffer9, Unlock) \
  X(IDirect3DVertexBuffer9, IDirect3DVertexBuffer9, GetDesc)

#define D9_CALLS_IDirect3DIndexBuffer9(X) \
  X(IDirect3DIndexBuffer9, IDirect3DIndexBuffer9, QueryInterface) \
  X(IDirect3DIndexBuffer9, IDirect3DIndexBuffer9, AddRef) \
  X(IDirect3DIndexBuffer9, IDirect3DIndexBuffer9, Release) \
  X(IDirect3DIndexBuffer9, IDirect3DIndexBuffer9, GetDevice) \
  X(IDirect3DIndexBuffer9, IDirect3DIndexBuffer9, SetPrivateData) \
  X(IDirect3DIndexBuffer9, IDirect3DIndexBuffer9, GetPrivateData) \
  X(IDirect3DIndexBuffer9, IDirect3DIndexBuffer9, FreePrivateData) \
  X(IDirect3DIndexBuffer9, IDirect3DIndexBuffer9, SetPriority) \
  X(IDirect3DIndexBuffer9, IDirect3DIndexBuffer9, GetPriority) \
  X(IDirect3DIndexBuffer9, IDirect3DIndexBuffer9, PreLoad) \
  X(IDirect3DIndexBuffer9, IDirect3DIndexBuffer9, GetType) \
  X(IDirect3DIndexBuffer9, IDirect3DIndexBuffer9, Lock) \
  X(IDirect3DIndexBuffer9, IDirect3DIndexBuffer9, Unlock) \
  X(IDirect3DIndexBuffer9, IDirect3DIndexBuffer9, GetDesc)

#define D9_CALLS_IDirect3DVertexDeclaration9(X) \
  X(IDirect3DVertexDeclaration9, IDirect3DVertexDeclaration9, QueryInterface) \
  X(IDirect3DVertexDeclaration9, IDirect3DVertexDeclaration9, AddRef) \
  X(IDirect3DVertexDeclaration9, IDirect3DVertexDeclaration9, Release) \
  X(IDirect3DVertexDeclaration9, IDirect3DVertexDeclaration9, GetDevice) \
  X(IDirect3DVertexDeclaration9, IDirect3DVertexDeclaration9, GetDeclaration)

#define D9_CALLS_IDirect3DVertexShader9(X) \
  X(IDirect3DVertexShader9, IDirect3DVertexShader9, QueryInterface) \
  X(IDirect3DVertexShader9, IDirect3DVertexShader9, AddRef) \
  X(IDirect3DVertexShader9, IDirect3DVertexShader9, Release) \
  X(IDirect3DVertexShader9, IDirect3DVertexShader9, GetDevice) \
  X(IDirect3DVertexShader9, IDirect3DVertexShader9, GetFunction)

#define D9_CALLS_IDirect3DPixelShader9(X) \
  X(IDirect3DPixelShader9, IDirect3DPixelShader9, QueryInterface) \
  X(IDirect3DPixelShader9, IDirect3DPixelShader9, AddRef) \
  X(IDirect3DPixelShader9, IDirect3DPixelShader9, Release) \
  X(IDirect3DPixelShader9, IDirect3DPixelShader9, GetDevice) \
  X(IDirect3DPixelShader9, IDirect3DPixelShader9, GetFunction)

#define D9_CALLS_IDirect3DStateBlock9(X) \
  X(IDirect3DStateBlock9, IDirect3DStateBlock9, QueryInterface) \
  X(IDirect3DStateBlock9, IDirect3DStateBlock9, AddRef) \
  X(IDirect3DStateBlock9, IDirect3DStateBlock9, Release) \
  X(IDirect3DStateBlock9, IDirect3DStateBlock9, GetDevice) \
  X(IDirect3DStateBlock9, IDirect3DStateBlock9, Capture) \
  X(IDirect3DStateBlock9, IDirect3DStateBlock9, Apply)

#define D9_CALLS_IDirect3DQuery9(X) \
  X(IDirect3DQuery9, IDirect3DQuery9, QueryInterface) \
  X(IDirect3DQuery9, IDirect3DQuery9, AddRef) \
  X(IDirect3DQuery9, IDirect3DQuery9, Release) \
  X(IDirect3DQuery9, IDirect3DQuery9, GetDevice) \
  X(IDirect3DQuery9, IDirect3DQuery9, GetType) \
  X(IDirect3DQuery9, IDirect3DQuery9, GetDataSize) \
  X(IDirect3DQuery9, IDirect3DQuery9, Issue) \
  X(IDirect3DQuery9, IDirect3DQuery9, GetData)

/* The private diagnostic interface the device also hands out (d3d9_diag.hpp:
   QueryInterface with IID_IDxmtDiag9, used by tests only). */
#define D9_CALLS_IDxmtDiag9(X) \
  X(IDxmtDiag9, none, GetLosableResourceCount)

/* The shader validator Direct3DShaderValidatorCreate9 hands out (d3d9.cpp's
   IDirect3DShaderValidator9Vtbl, implemented by shader_validator_<method>). */
#define D9_CALLS_IDirect3DShaderValidator9(X) \
  X(IDirect3DShaderValidator9, none, QueryInterface) \
  X(IDirect3DShaderValidator9, none, AddRef) \
  X(IDirect3DShaderValidator9, none, Release) \
  X(IDirect3DShaderValidator9, none, Begin) \
  X(IDirect3DShaderValidator9, none, Instruction) \
  X(IDirect3DShaderValidator9, none, End)

/* The DLL's exports, in d3d9.def's order. */
#define D9_CALLS_EXPORTS(X) \
  X(d3d9, none, Direct3DCreate9) \
  X(d3d9, none, Direct3DCreate9Ex) \
  X(d3d9, none, D3DPERF_BeginEvent) \
  X(d3d9, none, D3DPERF_EndEvent) \
  X(d3d9, none, D3DPERF_GetStatus) \
  X(d3d9, none, D3DPERF_QueryRepeatFrame) \
  X(d3d9, none, D3DPERF_SetMarker) \
  X(d3d9, none, D3DPERF_SetOptions) \
  X(d3d9, none, D3DPERF_SetRegion) \
  X(d3d9, none, DebugSetLevel) \
  X(d3d9, none, DebugSetMute) \
  X(d3d9, none, Direct3DShaderValidatorCreate9)

/* The interfaces d3d9.h declares, one list per object type. */
#define D9_CALLS_COM(X) \
  D9_CALLS_IDirect3D9Ex(X) \
  D9_CALLS_IDirect3DDevice9Ex(X) \
  D9_CALLS_IDirect3DSwapChain9Ex(X) \
  D9_CALLS_IDirect3DSurface9(X) \
  D9_CALLS_IDirect3DTexture9(X) \
  D9_CALLS_IDirect3DCubeTexture9(X) \
  D9_CALLS_IDirect3DVolumeTexture9(X) \
  D9_CALLS_IDirect3DVolume9(X) \
  D9_CALLS_IDirect3DVertexBuffer9(X) \
  D9_CALLS_IDirect3DIndexBuffer9(X) \
  D9_CALLS_IDirect3DVertexDeclaration9(X) \
  D9_CALLS_IDirect3DVertexShader9(X) \
  D9_CALLS_IDirect3DPixelShader9(X) \
  D9_CALLS_IDirect3DStateBlock9(X) \
  D9_CALLS_IDirect3DQuery9(X)

/* Every counted pair: the counter table's order. */
#define D9_CALLS(X) \
  D9_CALLS_COM(X) \
  D9_CALLS_IDxmtDiag9(X) \
  D9_CALLS_IDirect3DShaderValidator9(X) \
  D9_CALLS_EXPORTS(X)

/* C(list, class): the frontend class whose member functions implement a list
   (it derives from ComObject<list>, or from the list for IDxmtDiag9). Read by
   tests/check-d3d9-call-sites.sh; the shader validator and the exports are free
   functions in d3d9.cpp. */
#define D9_CALL_IMPLEMENTERS(C) \
  C(IDirect3D9Ex, MTLD3D9Interface) \
  C(IDirect3DDevice9Ex, MTLD3D9Device) \
  C(IDirect3DSwapChain9Ex, MTLD3D9SwapChain) \
  C(IDirect3DSurface9, MTLD3D9Surface) \
  C(IDirect3DTexture9, MTLD3D9Texture) \
  C(IDirect3DCubeTexture9, MTLD3D9CubeTexture) \
  C(IDirect3DVolumeTexture9, MTLD3D9VolumeTexture) \
  C(IDirect3DVolume9, MTLD3D9Volume) \
  C(IDirect3DVertexBuffer9, MTLD3D9VertexBuffer) \
  C(IDirect3DIndexBuffer9, MTLD3D9IndexBuffer) \
  C(IDirect3DVertexDeclaration9, MTLD3D9VertexDeclaration) \
  C(IDirect3DVertexShader9, MTLD3D9VertexShader) \
  C(IDirect3DPixelShader9, MTLD3D9PixelShader) \
  C(IDirect3DStateBlock9, MTLD3D9StateBlock) \
  C(IDirect3DQuery9, MTLD3D9Query) \
  C(IDxmtDiag9, MTLD3D9Device)
