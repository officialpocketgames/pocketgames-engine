/* Changed for MYIOSDECK (spec 33 clarification 7: object lifetime hooks of the ios_wow_native build). Upstream: DXMT a3a6e8f, MIT (see LICENSE). */
/*
 * This file is part of DXMT, Copyright (c) 2023 Feifan He
 *
 * Derived from a part of DXVK (originally under zlib License),
 * Copyright (c) 2017 Philip Rebohle
 * Copyright (c) 2019 Joshua Ashton
 *
 * See <https://github.com/doitsujin/dxvk/blob/master/LICENSE>
 */
#pragma once

#include <atomic>
#include "unknwn.h"
#include "../util_likely.hpp"
#ifdef DXMT_IOS_WOW_NATIVE
#include <tuple> /* MYIOSDECK spec 33 */
#endif

namespace dxmt {

template <typename T> class NoWrapper : public T {

public:
  virtual ~NoWrapper() {}
};

/**
 * \brief Reference-counted COM object
 *
 * This can serve as a templated base class for most
 * COM objects. It implements AddRef and Release from
 * \c IUnknown, and provides methods to increment and
 * decrement private references which are not visible
 * to the application.
 *
 * Having two reference counters is sadly necessary
 * in order to not break games which steal internal
 * references if the refefence count of an object is
 + greater than they expect. DXVK sometimes requires
 * holding on to objects which the application wants
 * to delete.
 */
template <typename... Base> class ComObject : public Base... {

public:
#ifdef DXMT_IOS_WOW_NATIVE
  /* MYIOSDECK spec 33 clarifications 5 and 7: the seam is told once for every object
     destroyed, with the interface address handed out for it (the first base), on whatever
     thread runs the destructor, before the memory is freed. Clarification 13: an object
     remembers the generation it was made in, so one orphaned by its process's end no longer
     counts when it is destroyed later. */
  ComObject() : m_nativeGeneration(::dxmt::ios_wow_native::objectBorn()) {}
  virtual ~ComObject() {
    using First = std::tuple_element_t<0, std::tuple<Base...>>;
    ::dxmt::ios_wow_native::objectGone(static_cast<First *>(this), m_nativeGeneration);
  }
#else
  virtual ~ComObject() {}
#endif

  ULONG STDMETHODCALLTYPE AddRef() {
    uint32_t refCount = m_refCount++;
    if (unlikely(!refCount))
      AddRefPrivate();
    return refCount + 1;
  }

  ULONG STDMETHODCALLTYPE Release() {
    uint32_t refCount = --m_refCount;
    if (unlikely(!refCount))
      ReleasePrivate();
    return refCount;
  }

  void AddRefPrivate() { ++m_refPrivate; }

  void ReleasePrivate() {
    uint32_t refPrivate = --m_refPrivate;
    if (unlikely(!refPrivate)) {
      m_refPrivate += 0x80000000;
      delete this;
    }
  }

  ULONG GetPrivateRefCount() { return m_refPrivate.load(); }

protected:
  std::atomic<uint32_t> m_refCount = {0ul};
  std::atomic<uint32_t> m_refPrivate = {0ul};
#ifdef DXMT_IOS_WOW_NATIVE
  const uint32_t m_nativeGeneration; /* MYIOSDECK spec 33 clarification 13 */
#endif
};

template <typename... Base> class ComObjectWithInitialRef : public Base... {

public:
  virtual ~ComObjectWithInitialRef() {}

  ULONG STDMETHODCALLTYPE AddRef() {
    uint32_t refCount = m_refCount++;
    if (unlikely(!refCount))
      AddRefPrivate();
    return refCount + 1;
  }

  ULONG STDMETHODCALLTYPE Release() {
    uint32_t refCount = --m_refCount;
    if (unlikely(!refCount))
      ReleasePrivate();
    return refCount;
  }

  void AddRefPrivate() { ++m_refPrivate; }

  void ReleasePrivate() {
    uint32_t refPrivate = --m_refPrivate;
    if (unlikely(!refPrivate)) {
      m_refPrivate += 0x80000000;
      delete this;
    }
  }

protected:
  std::atomic<uint32_t> m_refCount = {1ul};
  std::atomic<uint32_t> m_refPrivate = {1ul};
};

/**
 * \brief Clamped, reference-counted COM object
 *
 * This version of ComObject ensures that the reference
 * count does not wrap around if a release happens at zero.
 * eg. [m_refCount = 0]
 *     Release()
 *     [m_refCount = 0]
 * This is a notable quirk of D3D9's COM implementation
 * and is relied upon by some games.
 */
template <typename Base> class ComObjectClamp : public ComObject<Base> {

public:
  ULONG STDMETHODCALLTYPE Release() {
    ULONG refCount = this->m_refCount;
    if (likely(refCount != 0ul)) {
      this->m_refCount--;
      refCount--;

      if (refCount == 0ul)
        this->ReleasePrivate();
    }

    return refCount;
  }
};

template <typename T> inline void InitReturnPtr(T **ptr) {
  if (ptr != nullptr)
    *ptr = nullptr;
}

} // namespace dxmt
