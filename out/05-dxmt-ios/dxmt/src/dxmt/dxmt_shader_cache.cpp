// Changed for MYIOSDECK (spec 05 H3, H4, clarification 12). Upstream: DXMT a3a6e8f, MIT (see LICENSE).
#include "dxmt_shader_cache.hpp"
#include "dxmt_shader_cache_log.hpp"
#include "log/log.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <atomic>
#include <chrono>
#include <mutex>

namespace dxmt {

/* ---- spec 05 clarification 12: the cache's lookups in the device log ---- */

namespace {

std::atomic<uint64_t> g_cache_hits{0};
std::atomic<uint64_t> g_cache_converted{0};
std::atomic<uint64_t> g_cache_stored{0};
dxmt::mutex g_cache_log_mutex; // guards g_cache_log_rule
ShaderCacheLogRule g_cache_log_rule;

ShaderCacheCounts
CacheCountsNow() {
  return {
      g_cache_hits.load(std::memory_order_relaxed),
      g_cache_converted.load(std::memory_order_relaxed),
      g_cache_stored.load(std::memory_order_relaxed),
  };
}

uint64_t
CacheLogNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

void
ShaderCacheNoteLookup(bool hit) {
  (hit ? g_cache_hits : g_cache_converted).fetch_add(1, std::memory_order_relaxed);
  ShaderCacheLine line;
  ShaderCacheCounts counts;
  {
    std::lock_guard<dxmt::mutex> lock(g_cache_log_mutex);
    counts = CacheCountsNow();
    line = g_cache_log_rule.onLookup(hit, counts, CacheLogNowMs());
  }
  if (line != ShaderCacheLine::None)
    Logger::info(FormatShaderCacheLine(line, counts));
}

void
ShaderCacheNoteStore() {
  g_cache_stored.fetch_add(1, std::memory_order_relaxed); // logged with the next summary
}

void
ShaderCacheLogAtDetach() {
  // try_lock: at process exit a thread that held the lock may have been stopped
  std::unique_lock<dxmt::mutex> lock(g_cache_log_mutex, std::try_to_lock);
  if (!lock)
    return;
  auto counts = CacheCountsNow();
  bool due = g_cache_log_rule.onDetach(counts);
  lock.unlock();
  if (due)
    Logger::info(FormatShaderCacheLine(ShaderCacheLine::Summary, counts));
}

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
  if (env::getEnvVar("DXMT_SHADER_CACHE") == "0") {
    // spec 05 clarification 12: the unix half (which logs an opened cache) is never asked
    static std::atomic<bool> off_logged{false};
    if (!off_logged.exchange(true))
      Logger::info("dxmt: shader cache off (DXMT_SHADER_CACHE=0)");
    return;
  }
  std::string path = ShaderCacheDirectory();
  path += str::format("shaders_", (unsigned int)metal_version, ".db");
  scache_writer_ = WMT::CacheWriter::alloc_init(path.c_str(), kDXMTShaderCacheVersion);
  scache_reader_ = WMT::CacheReader::alloc_init(path.c_str(), kDXMTShaderCacheVersion);
}

} // namespace dxmt
