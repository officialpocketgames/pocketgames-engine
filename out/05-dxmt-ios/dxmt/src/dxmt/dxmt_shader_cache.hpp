#pragma once
// Changed for MYIOSDECK (spec 05 F2, H3, H4). Upstream: DXMT a3a6e8f, MIT (see LICENSE).
#include "Metal.hpp"
#include "thread.hpp"

namespace dxmt {

// MYIOSDECK spec 05 F2-F4 changed the generated shaders: 15 -> 16, so shaders
// cached before the change aren't used.
constexpr int kDXMTShaderCacheVersion = 16;

/* Spec 05 H3: DXMT_SHADER_CACHE_PATH (absolute only, with a trailing '/') or
   "dxmt/<exe name>/". A relative path is resolved by the unix half (on iOS,
   under the app's caches directory). */
std::string ShaderCacheDirectory();

class ShaderCache {
public:
  template <typename T> class LockProtected {
  public:
    [[nodiscard]] LockProtected(dxmt::mutex &mutex, T object) : object(object), mutex_(&mutex) {
      mutex_->lock();
    }
    [[nodiscard]] LockProtected() : object({}), mutex_(nullptr) {}
    ~LockProtected() {
      if (mutex_) {
        mutex_->unlock();
        mutex_ = nullptr;
      }
      object = {};
    }
    T object;
    LockProtected(const LockProtected &copy) = delete;
    LockProtected(LockProtected &&move) = default;

    operator bool() const {
      return mutex_ != nullptr;
    }

    T *
    operator->() {
      return &object;
    }

  private:
    dxmt::mutex *mutex_;
  };

  static ShaderCache &getInstance(WMTMetalVersion version);

  LockProtected<WMT::CacheWriter>
  getWriter() {
    if (!scache_writer_)
      return {};
    return {scache_writer_mutex_, scache_writer_};
  }

  LockProtected<WMT::CacheReader>
  getReader() {
    if (!scache_reader_)
      return {};
    return {scache_reader_mutex_, scache_reader_};
  }

  ShaderCache(WMTMetalVersion metal_version);
  ShaderCache(const ShaderCache &copy) = delete;

private:
  WMT::Reference<WMT::CacheWriter> scache_writer_;
  dxmt::mutex scache_writer_mutex_;

  WMT::Reference<WMT::CacheReader> scache_reader_;
  dxmt::mutex scache_reader_mutex_;
};

} // namespace dxmt