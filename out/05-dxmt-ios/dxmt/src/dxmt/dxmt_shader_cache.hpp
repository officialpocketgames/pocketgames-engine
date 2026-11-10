#pragma once
// Changed for MYIOSDECK (spec 05 F2, H3, H4, clarifications 10, 12 and 13). Upstream: DXMT a3a6e8f, MIT (see LICENSE).
#include "Metal.hpp"
#include "thread.hpp"

namespace dxmt {

// MYIOSDECK spec 05 F2-F4 changed the generated shaders: 15 -> 16, so shaders
// cached before the change aren't used. Spec 05 clarification 10: 17 for
// steamac's DXMT patch 0018 (buffer atomics bound-checked), 18 for its patch
// 0022 (small dynamically indexed temp arrays as select chains); the patches
// bump AIRCONV_VERSION, which this tree's cache doesn't key on. Spec 05
// clarification 13: 19 for the six upstream DXMT fixes (typed-buffer bounds,
// NaN-to-0 sample coordinates, udiv by 0, sample_l bias, interpolate offset,
// mul_hi, geometry-shader pass-through).
constexpr int kDXMTShaderCacheVersion = 19;

/* Spec 05 H3: DXMT_SHADER_CACHE_PATH (absolute only, with a trailing '/') or
   "dxmt/<exe name>/". A relative path is resolved by the unix half (on iOS,
   under the app's caches directory). */
std::string ShaderCacheDirectory();

/* Spec 05 clarification 12: per-process counters of the converted-shader cache, for the
   device log (rule: dxmt_shader_cache_log.hpp). The Direct3D 11 path (find_cached_variant /
   update_cached_variant) and the Direct3D 9 path (d9_cache_lookup / d9_cache_store) report
   each lookup (hit, or a miss that is then converted) and each entry stored. */
void ShaderCacheNoteLookup(bool hit);
void ShaderCacheNoteStore();
/* DLL_PROCESS_DETACH: the last summary, when it differs from the last one logged. */
void ShaderCacheLogAtDetach();

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