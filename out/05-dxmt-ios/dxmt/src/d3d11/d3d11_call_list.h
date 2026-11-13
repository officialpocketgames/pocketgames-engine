// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 63 (DXMT_D11_CALL_STATS): every method the Direct3D 11 front end
// counts, as X-macro lists usable from C and C++. Pure preprocessor text: no includes, no
// types.
//
// X(method): one entry per vtable slot, slot 0 first, all of them, in the order of the
// vtables in the mingw-w64 d3d11*.h headers in include/native/directx:
//   D11_CALLS_CONTEXT  ID3D11DeviceContext4 (IUnknown and ID3D11DeviceChild first); counted
//                      apart for the immediate context and for the deferred ones;
//   D11_CALLS_DEVICE   ID3D11Device5 (IUnknown first);
//   D11_CALLS_OTHER    the swap chain's two Presents, and the three IUnknown methods of every
//                      other object the device makes (resources, views, states, shaders,
//                      layouts, queries, command lists), shared.
// tests/test_d3d11_call_list.c checks the first two lists against the headers' vtables, slot
// by slot; tests/check-d3d11-call-sites.sh checks that each listed method is counted once,
// first thing in the method that implements it.

#pragma once

#define D11_CALLS_CONTEXT(X) \
  X(QueryInterface) \
  X(AddRef) \
  X(Release) \
  X(GetDevice) \
  X(GetPrivateData) \
  X(SetPrivateData) \
  X(SetPrivateDataInterface) \
  X(VSSetConstantBuffers) \
  X(PSSetShaderResources) \
  X(PSSetShader) \
  X(PSSetSamplers) \
  X(VSSetShader) \
  X(DrawIndexed) \
  X(Draw) \
  X(Map) \
  X(Unmap) \
  X(PSSetConstantBuffers) \
  X(IASetInputLayout) \
  X(IASetVertexBuffers) \
  X(IASetIndexBuffer) \
  X(DrawIndexedInstanced) \
  X(DrawInstanced) \
  X(GSSetConstantBuffers) \
  X(GSSetShader) \
  X(IASetPrimitiveTopology) \
  X(VSSetShaderResources) \
  X(VSSetSamplers) \
  X(Begin) \
  X(End) \
  X(GetData) \
  X(SetPredication) \
  X(GSSetShaderResources) \
  X(GSSetSamplers) \
  X(OMSetRenderTargets) \
  X(OMSetRenderTargetsAndUnorderedAccessViews) \
  X(OMSetBlendState) \
  X(OMSetDepthStencilState) \
  X(SOSetTargets) \
  X(DrawAuto) \
  X(DrawIndexedInstancedIndirect) \
  X(DrawInstancedIndirect) \
  X(Dispatch) \
  X(DispatchIndirect) \
  X(RSSetState) \
  X(RSSetViewports) \
  X(RSSetScissorRects) \
  X(CopySubresourceRegion) \
  X(CopyResource) \
  X(UpdateSubresource) \
  X(CopyStructureCount) \
  X(ClearRenderTargetView) \
  X(ClearUnorderedAccessViewUint) \
  X(ClearUnorderedAccessViewFloat) \
  X(ClearDepthStencilView) \
  X(GenerateMips) \
  X(SetResourceMinLOD) \
  X(GetResourceMinLOD) \
  X(ResolveSubresource) \
  X(ExecuteCommandList) \
  X(HSSetShaderResources) \
  X(HSSetShader) \
  X(HSSetSamplers) \
  X(HSSetConstantBuffers) \
  X(DSSetShaderResources) \
  X(DSSetShader) \
  X(DSSetSamplers) \
  X(DSSetConstantBuffers) \
  X(CSSetShaderResources) \
  X(CSSetUnorderedAccessViews) \
  X(CSSetShader) \
  X(CSSetSamplers) \
  X(CSSetConstantBuffers) \
  X(VSGetConstantBuffers) \
  X(PSGetShaderResources) \
  X(PSGetShader) \
  X(PSGetSamplers) \
  X(VSGetShader) \
  X(PSGetConstantBuffers) \
  X(IAGetInputLayout) \
  X(IAGetVertexBuffers) \
  X(IAGetIndexBuffer) \
  X(GSGetConstantBuffers) \
  X(GSGetShader) \
  X(IAGetPrimitiveTopology) \
  X(VSGetShaderResources) \
  X(VSGetSamplers) \
  X(GetPredication) \
  X(GSGetShaderResources) \
  X(GSGetSamplers) \
  X(OMGetRenderTargets) \
  X(OMGetRenderTargetsAndUnorderedAccessViews) \
  X(OMGetBlendState) \
  X(OMGetDepthStencilState) \
  X(SOGetTargets) \
  X(RSGetState) \
  X(RSGetViewports) \
  X(RSGetScissorRects) \
  X(HSGetShaderResources) \
  X(HSGetShader) \
  X(HSGetSamplers) \
  X(HSGetConstantBuffers) \
  X(DSGetShaderResources) \
  X(DSGetShader) \
  X(DSGetSamplers) \
  X(DSGetConstantBuffers) \
  X(CSGetShaderResources) \
  X(CSGetUnorderedAccessViews) \
  X(CSGetShader) \
  X(CSGetSamplers) \
  X(CSGetConstantBuffers) \
  X(ClearState) \
  X(Flush) \
  X(GetType) \
  X(GetContextFlags) \
  X(FinishCommandList) \
  X(CopySubresourceRegion1) \
  X(UpdateSubresource1) \
  X(DiscardResource) \
  X(DiscardView) \
  X(VSSetConstantBuffers1) \
  X(HSSetConstantBuffers1) \
  X(DSSetConstantBuffers1) \
  X(GSSetConstantBuffers1) \
  X(PSSetConstantBuffers1) \
  X(CSSetConstantBuffers1) \
  X(VSGetConstantBuffers1) \
  X(HSGetConstantBuffers1) \
  X(DSGetConstantBuffers1) \
  X(GSGetConstantBuffers1) \
  X(PSGetConstantBuffers1) \
  X(CSGetConstantBuffers1) \
  X(SwapDeviceContextState) \
  X(ClearView) \
  X(DiscardView1) \
  X(UpdateTileMappings) \
  X(CopyTileMappings) \
  X(CopyTiles) \
  X(UpdateTiles) \
  X(ResizeTilePool) \
  X(TiledResourceBarrier) \
  X(IsAnnotationEnabled) \
  X(SetMarkerInt) \
  X(BeginEventInt) \
  X(EndEvent) \
  X(Flush1) \
  X(SetHardwareProtectionState) \
  X(GetHardwareProtectionState) \
  X(Signal) \
  X(Wait)

#define D11_CALLS_DEVICE(X) \
  X(QueryInterface) \
  X(AddRef) \
  X(Release) \
  X(CreateBuffer) \
  X(CreateTexture1D) \
  X(CreateTexture2D) \
  X(CreateTexture3D) \
  X(CreateShaderResourceView) \
  X(CreateUnorderedAccessView) \
  X(CreateRenderTargetView) \
  X(CreateDepthStencilView) \
  X(CreateInputLayout) \
  X(CreateVertexShader) \
  X(CreateGeometryShader) \
  X(CreateGeometryShaderWithStreamOutput) \
  X(CreatePixelShader) \
  X(CreateHullShader) \
  X(CreateDomainShader) \
  X(CreateComputeShader) \
  X(CreateClassLinkage) \
  X(CreateBlendState) \
  X(CreateDepthStencilState) \
  X(CreateRasterizerState) \
  X(CreateSamplerState) \
  X(CreateQuery) \
  X(CreatePredicate) \
  X(CreateCounter) \
  X(CreateDeferredContext) \
  X(OpenSharedResource) \
  X(CheckFormatSupport) \
  X(CheckMultisampleQualityLevels) \
  X(CheckCounterInfo) \
  X(CheckCounter) \
  X(CheckFeatureSupport) \
  X(GetPrivateData) \
  X(SetPrivateData) \
  X(SetPrivateDataInterface) \
  X(GetFeatureLevel) \
  X(GetCreationFlags) \
  X(GetDeviceRemovedReason) \
  X(GetImmediateContext) \
  X(SetExceptionMode) \
  X(GetExceptionMode) \
  X(GetImmediateContext1) \
  X(CreateDeferredContext1) \
  X(CreateBlendState1) \
  X(CreateRasterizerState1) \
  X(CreateDeviceContextState) \
  X(OpenSharedResource1) \
  X(OpenSharedResourceByName) \
  X(GetImmediateContext2) \
  X(CreateDeferredContext2) \
  X(GetResourceTiling) \
  X(CheckMultisampleQualityLevels1) \
  X(CreateTexture2D1) \
  X(CreateTexture3D1) \
  X(CreateRasterizerState2) \
  X(CreateShaderResourceView1) \
  X(CreateUnorderedAccessView1) \
  X(CreateRenderTargetView1) \
  X(CreateQuery1) \
  X(GetImmediateContext3) \
  X(CreateDeferredContext3) \
  X(WriteToSubresource) \
  X(ReadFromSubresource) \
  X(RegisterDeviceRemovedEvent) \
  X(UnregisterDeviceRemoved) \
  X(OpenSharedFence) \
  X(CreateFence)

#define D11_CALLS_OTHER(X) \
  X(SwapChain_Present) \
  X(SwapChain_Present1) \
  X(Child_QueryInterface) \
  X(Child_AddRef) \
  X(Child_Release)
