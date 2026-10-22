// Changed for MYIOSDECK (spec 05 H3, H4). Upstream: DXMT a3a6e8f, MIT (see LICENSE).
#include "dxmt_shader_cache.hpp"
#include "util_env.hpp"
#include "util_string.hpp"

namespace dxmt {

std::string
ShaderCacheDirectory() {
  std::string path;
  if (path = env::getEnvVar("DXMT_SHADER_CACHE_PATH"); !path.empty() && path.starts_with("/")) {
    if (!path.ends_with('/'))
      path += "/";
  } else {
    path = str::format("dxmt/", env::getExeName(), "/");
  }
  return path;
}

#ifdef DXMT_IOS

// Spec 05 H4: one fixed slot per Metal version (there are two) in plain static
// storage, instead of a hash map in a function-local static, so unloading and
// reloading d3d11.dll can't corrupt the lookup.
static dxmt::mutex g_shader_cache_mutex;
static ShaderCache *g_shader_cache_slots[2] = {nullptr, nullptr};

ShaderCache &
ShaderCache::getInstance(WMTMetalVersion version) {
  unsigned slot = version == WMTMetal320 ? 1 : 0;
  std::lock_guard<dxmt::mutex> lock(g_shader_cache_mutex);
  if (!g_shader_cache_slots[slot])
    g_shader_cache_slots[slot] = new ShaderCache(version);
  return *g_shader_cache_slots[slot];
}

#else

ShaderCache &
ShaderCache::getInstance(WMTMetalVersion version) {
  static dxmt::mutex mutex;
  static std::unordered_map<WMTMetalVersion, std::unique_ptr<ShaderCache>> caches;

  std::lock_guard<dxmt::mutex> lock(mutex);
  auto iter = caches.find(version);
  if (iter == caches.end()) {
    auto inserted = caches.insert({version, std::make_unique<ShaderCache>(version)});
    return *inserted.first->second;
  }
  return *iter->second;
}

#endif

ShaderCache::ShaderCache(WMTMetalVersion metal_version) {
  if (env::getEnvVar("DXMT_SHADER_CACHE") == "0")
    return;
  std::string path = ShaderCacheDirectory();
  path += str::format("shaders_", (unsigned int)metal_version, ".db");
  scache_writer_ = WMT::CacheWriter::alloc_init(path.c_str(), kDXMTShaderCacheVersion);
  scache_reader_ = WMT::CacheReader::alloc_init(path.c_str(), kDXMTShaderCacheVersion);
}

} // namespace dxmt
