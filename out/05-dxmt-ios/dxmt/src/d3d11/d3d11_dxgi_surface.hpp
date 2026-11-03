// SPDX-License-Identifier: LGPL-2.1-or-later
// From steamac's DXMT patch 0013 (hanzpo, "feat(d3d11): expose IDXGISurface on single-subresource
// 2D textures", hanzpo/dxmt 1b1aa5b5), added for MYIOSDECK spec 05 clarification 10 (see
// LICENSE-NOTES.md).
#pragma once

/* header only: use it as an aggregated object, like MTLDXGIResource */

#include "d3d11_1.h"
#include "dxgi1_2.h"
#include "com/com_pointer.hpp"
#include "log/log.hpp"

namespace dxmt {

/**
IDXGISurface view of a single-subresource 2D texture.

Direct2D interop (ID2D1Factory::CreateDxgiSurfaceRenderTarget, ID2D1DeviceContext::CreateBitmapFromDxgiSurface)
obtains one of these by querying a texture (or a swapchain back buffer) for IDXGISurface.
*/
template <typename IResource> class MTLDXGISurface : public IDXGISurface2 {
public:
  MTLDXGISurface(IResource *pResource) : resource_(pResource) {}
  ~MTLDXGISurface() {}

  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) final {
    return resource_->QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE
  AddRef() final {
    return resource_->AddRef();
  }

  ULONG STDMETHODCALLTYPE
  Release() final {
    return resource_->Release();
  }

  HRESULT
  STDMETHODCALLTYPE
  SetPrivateData(REFGUID guid, UINT data_size, const void *data) final {
    return resource_->SetPrivateData(guid, data_size, data);
  }

  HRESULT
  STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID guid, const IUnknown *object) final {
    return resource_->SetPrivateDataInterface(guid, object);
  }

  HRESULT
  STDMETHODCALLTYPE
  GetPrivateData(REFGUID guid, UINT *data_size, void *data) final {
    return resource_->GetPrivateData(guid, data_size, data);
  }

  HRESULT
  STDMETHODCALLTYPE
  GetParent(REFIID riid, void **parent) final {
    return GetDevice(riid, parent);
  }

  HRESULT
  STDMETHODCALLTYPE
  GetDevice(REFIID riid, void **ppDevice) final {
    return resource_->GetDeviceInterface(riid, ppDevice);
  }

  HRESULT
  STDMETHODCALLTYPE
  GetDesc(DXGI_SURFACE_DESC *pDesc) final {
    if (!pDesc)
      return E_INVALIDARG;

    Com<ID3D11Texture2D> texture;
    if (FAILED(resource_->QueryInterface(IID_PPV_ARGS(&texture))))
      return E_FAIL;

    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    pDesc->Width = desc.Width;
    pDesc->Height = desc.Height;
    pDesc->Format = desc.Format;
    pDesc->SampleDesc = desc.SampleDesc;
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  Map(DXGI_MAPPED_RECT *pLockedRect, UINT MapFlags) final {
    if (!pLockedRect)
      return E_INVALIDARG;

    D3D11_MAP map_type;
    if ((MapFlags & DXGI_MAP_READ) && (MapFlags & DXGI_MAP_WRITE))
      map_type = D3D11_MAP_READ_WRITE;
    else if (MapFlags & DXGI_MAP_DISCARD)
      map_type = D3D11_MAP_WRITE_DISCARD;
    else if (MapFlags & DXGI_MAP_WRITE)
      map_type = D3D11_MAP_WRITE;
    else if (MapFlags & DXGI_MAP_READ)
      map_type = D3D11_MAP_READ;
    else
      return E_INVALIDARG;

    Com<ID3D11DeviceContext> context;
    if (FAILED(GetImmediateContext(&context)))
      return E_FAIL;

    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context->Map(resource_, 0, map_type, 0, &mapped);
    if (FAILED(hr))
      return hr;
    pLockedRect->Pitch = mapped.RowPitch;
    pLockedRect->pBits = reinterpret_cast<BYTE *>(mapped.pData);
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  Unmap() final {
    Com<ID3D11DeviceContext> context;
    if (FAILED(GetImmediateContext(&context)))
      return E_FAIL;
    context->Unmap(resource_, 0);
    return S_OK;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetDC(BOOL Discard, HDC *phdc) final {
    ERR_ONCE("DXGISurface::GetDC: GDI interop not supported");
    if (phdc)
      *phdc = nullptr;
    return DXGI_ERROR_INVALID_CALL;
  }

  HRESULT
  STDMETHODCALLTYPE
  ReleaseDC(RECT *pDirtyRect) final {
    return DXGI_ERROR_INVALID_CALL;
  }

  HRESULT
  STDMETHODCALLTYPE
  GetResource(REFIID riid, void **ppParentResource, UINT *pSubresourceIndex) final {
    if (pSubresourceIndex)
      *pSubresourceIndex = 0;
    return resource_->QueryInterface(riid, ppParentResource);
  }

private:
  HRESULT
  GetImmediateContext(ID3D11DeviceContext **ppContext) {
    Com<ID3D11Device> device;
    HRESULT hr = resource_->GetDeviceInterface(IID_PPV_ARGS(&device));
    if (FAILED(hr))
      return hr;
    device->GetImmediateContext(ppContext);
    return S_OK;
  }

  IResource *resource_; // since it's aggregated, no extra reference is needed
};

} // namespace dxmt
