// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 item 91: a minimal IDXGIKeyedMutex for a texture made with
// D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX (what Media Foundation's MF_SA_D3D11_SHARED asks for).
// Inside the app's one process the creator and every opener use the same Metal texture, so the
// mutex has nothing to hand over:
// - AcquireSync keeps the key and succeeds at once;
// - ReleaseSync keeps the key, flushes the device's immediate context, so the GPU work written
//   before it is committed before what the other side submits after, and succeeds.
// Header only, aggregated in the texture like MTLDXGIResource: its references are the texture's.

#pragma once

#include "dxgi.h"

#include <atomic>
#include <cstdint>

namespace dxmt {

/* The keyed mutex's own state (tests/test_share.cpp checks it with a fake texture). */
class KeyedMutexState {
public:
  void
  acquire(uint64_t key) {
    key_.store(key, std::memory_order_relaxed);
    acquires_.fetch_add(1, std::memory_order_relaxed);
  }

  void
  release(uint64_t key) {
    key_.store(key, std::memory_order_relaxed);
    releases_.fetch_add(1, std::memory_order_relaxed);
  }

  uint64_t
  key() const {
    return key_.load(std::memory_order_relaxed);
  }

  uint64_t
  acquires() const {
    return acquires_.load(std::memory_order_relaxed);
  }

  uint64_t
  releases() const {
    return releases_.load(std::memory_order_relaxed);
  }

private:
  std::atomic<uint64_t> key_{0};
  std::atomic<uint64_t> acquires_{0};
  std::atomic<uint64_t> releases_{0};
};

/* IResource: the texture, with IUnknown's methods, the private data, GetDeviceInterface and
   FlushForKeyedMutex (the flush ReleaseSync orders the GPU work with). */
template <typename IResource> class MTLDXGIKeyedMutex : public IDXGIKeyedMutex {
public:
  explicit MTLDXGIKeyedMutex(IResource *resource) : resource_(resource) {}
  virtual ~MTLDXGIKeyedMutex() {}

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

  HRESULT STDMETHODCALLTYPE
  SetPrivateData(REFGUID guid, UINT data_size, const void *data) final {
    return resource_->SetPrivateData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID guid, const IUnknown *object) final {
    return resource_->SetPrivateDataInterface(guid, object);
  }

  HRESULT STDMETHODCALLTYPE
  GetPrivateData(REFGUID guid, UINT *data_size, void *data) final {
    return resource_->GetPrivateData(guid, data_size, data);
  }

  HRESULT STDMETHODCALLTYPE
  GetParent(REFIID riid, void **parent) final {
    return GetDevice(riid, parent);
  }

  HRESULT STDMETHODCALLTYPE
  GetDevice(REFIID riid, void **ppDevice) final {
    return resource_->GetDeviceInterface(riid, ppDevice);
  }

  HRESULT STDMETHODCALLTYPE
  AcquireSync(UINT64 Key, DWORD dwMilliseconds) final {
    (void)dwMilliseconds; // nothing to wait for
    state_.acquire(Key);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE
  ReleaseSync(UINT64 Key) final {
    state_.release(Key);
    resource_->FlushForKeyedMutex();
    return S_OK;
  }

  const KeyedMutexState &
  state() const {
    return state_;
  }

private:
  IResource *resource_; // aggregated: no reference of its own
  KeyedMutexState state_;
};

} // namespace dxmt
