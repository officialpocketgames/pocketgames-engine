// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 item 90 (round 20): the Metal memory ledger and its lines (dxmt_mem_ledger.hpp).
#include "dxmt_mem_ledger.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>

namespace dxmt::memledger {

const char *
KindName(Kind kind) {
  switch (kind) {
  case Kind::RtDs:
    return "rt/ds";
  case Kind::Texture:
    return "textures";
  case Kind::Buffer:
    return "buffers";
  case Kind::Staging:
    return "staging";
  case Kind::Internal:
    return "internal";
  }
  return "?";
}

/* ---- Ledger ---- */

bool
Ledger::inWindow(const Second &s, uint64_t now_ms) {
  uint64_t now_s = now_ms / 1000;
  return s.second != ~0ull && s.second <= now_s && now_s - s.second < kSeconds;
}

Ledger::Second &
Ledger::secondFor(uint64_t now_ms) {
  uint64_t now_s = now_ms / 1000;
  Second &s = seconds_[now_s % kSeconds];
  if (s.second != now_s) {
    s = Second{};
    s.second = now_s;
  }
  return s;
}

uint64_t
Ledger::add(const Info &info, const void *owner, const void *group, uint64_t now_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  uint64_t id = next_id_++;
  records_.emplace(id, Record{info, owner, group, false});
  if (group)
    groups_[group].push_back(id);
  live_bytes_ += info.bytes;

  Second &s = secondFor(now_ms);
  s.created_count++;
  s.created_bytes += info.bytes;
  SiteBytes *slot = nullptr;
  for (unsigned i = 0; i < s.site_count; i++) {
    if (s.sites[i].site == info.site) {
      slot = &s.sites[i];
      break;
    }
  }
  if (!slot && s.site_count < kSitesPerSecond) {
    slot = &s.sites[s.site_count++];
    *slot = SiteBytes{info.site, 0, 0};
  }
  if (!slot)
    slot = &s.other;
  slot->bytes += info.bytes;
  slot->count++;
  return id;
}

void
Ledger::remove(uint64_t id, uint64_t now_ms) {
  if (!id)
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = records_.find(id);
  if (it == records_.end())
    return;
  uint64_t bytes = it->second.info.bytes;
  if (const void *group = it->second.group) {
    auto g = groups_.find(group);
    if (g != groups_.end()) {
      auto &ids = g->second;
      for (size_t i = 0; i < ids.size(); i++) {
        if (ids[i] == id) {
          ids[i] = ids.back();
          ids.pop_back();
          break;
        }
      }
      if (ids.empty())
        groups_.erase(g);
    }
  }
  records_.erase(it);
  live_bytes_ -= bytes;
  Second &s = secondFor(now_ms);
  s.released_count++;
  s.released_bytes += bytes;
}

void
Ledger::markGroupReleased(const void *group) {
  if (!group)
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  auto g = groups_.find(group);
  if (g == groups_.end())
    return;
  for (uint64_t id : g->second) {
    auto it = records_.find(id);
    if (it != records_.end())
      it->second.program_released = true;
  }
}

Totals
Ledger::totals(uint64_t now_ms, WaitingFn waiting) const {
  Totals t;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto &[id, record] : records_) {
    const Info &info = record.info;
    KindTotal &k = t.kind[unsigned(info.kind)];
    k.count++;
    k.bytes += info.bytes;
    if (info.storage == Storage::Shared)
      k.shared_bytes += info.bytes;
    t.count++;
    t.bytes += info.bytes;
    if (info.bc_uncompressed) {
      t.bc_count++;
      t.bc_bytes += info.bytes;
    }
    if (record.program_released || (record.owner && waiting && waiting(record.owner))) {
      t.waiting_count++;
      t.waiting_bytes += info.bytes;
    }
  }
  for (const Second &s : seconds_) {
    if (!inWindow(s, now_ms))
      continue;
    t.created_count += s.created_count;
    t.created_bytes += s.created_bytes;
    t.released_count += s.released_count;
    t.released_bytes += s.released_bytes;
  }
  return t;
}

std::vector<Info>
Ledger::largest(unsigned n) const {
  std::vector<std::pair<uint64_t, const Info *>> all;
  std::vector<Info> out;
  std::lock_guard<std::mutex> lock(mutex_);
  all.reserve(records_.size());
  for (const auto &[id, record] : records_)
    all.emplace_back(id, &record.info);
  auto bigger = [](const std::pair<uint64_t, const Info *> &a, const std::pair<uint64_t, const Info *> &b) {
    if (a.second->bytes != b.second->bytes)
      return a.second->bytes > b.second->bytes;
    return a.first < b.first; // the older first
  };
  size_t keep = std::min<size_t>(n, all.size());
  std::partial_sort(all.begin(), all.begin() + keep, all.end(), bigger);
  out.reserve(keep);
  for (size_t i = 0; i < keep; i++)
    out.push_back(*all[i].second);
  return out;
}

std::vector<SiteBytes>
Ledger::topSites(unsigned n, uint64_t now_ms) const {
  std::vector<SiteBytes> merged;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto merge = [&](const SiteBytes &from) {
      if (!from.count)
        return;
      for (auto &m : merged) {
        if (m.site == from.site) {
          m.bytes += from.bytes;
          m.count += from.count;
          return;
        }
      }
      merged.push_back(from);
    };
    for (const Second &s : seconds_) {
      if (!inWindow(s, now_ms))
        continue;
      for (unsigned i = 0; i < s.site_count; i++)
        merge(s.sites[i]);
      merge(s.other);
    }
  }
  std::stable_sort(merged.begin(), merged.end(), [](const SiteBytes &a, const SiteBytes &b) {
    return a.bytes > b.bytes;
  });
  if (merged.size() > n)
    merged.resize(n);
  return merged;
}

uint64_t
Ledger::liveBytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return live_bytes_;
}

uint64_t
Ledger::liveCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return records_.size();
}

/* ---- The process's ledger ---- */

Ledger &
Global() {
  static Ledger *ledger = new Ledger(); // never destroyed: objects may go after static destructors
  return *ledger;
}

uint64_t
NowMs() {
  return uint64_t(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()
  );
}

namespace {
struct ScopeState {
  uint8_t api;
  uint32_t usage, bind, cpu, misc;
  uint64_t site;
  const char *call;
};
thread_local ScopeState t_scope;
thread_local bool t_in_scope = false;
} // namespace

Scope::Scope(uint8_t api, uint32_t usage, uint32_t bind, uint32_t cpu, uint32_t misc, const void *site) :
    outermost_(!t_in_scope) {
  if (!outermost_)
    return;
  t_scope = ScopeState{api, usage, bind, cpu, misc, uint64_t(uintptr_t(site)), nullptr};
  t_in_scope = true;
}

Scope::Scope(uint8_t api, const char *call, const void *site) : outermost_(!t_in_scope) {
  if (!outermost_)
    return;
  t_scope = ScopeState{api, 0, 0, 0, 0, uint64_t(uintptr_t(site)), call};
  t_in_scope = true;
}

Scope::~Scope() {
  if (outermost_)
    t_in_scope = false;
}

void
ApplyScope(Info &info) {
  if (!t_in_scope)
    return;
  if (!info.api) {
    info.api = t_scope.api;
    info.d3d_usage = t_scope.usage;
    info.d3d_bind = t_scope.bind;
    info.d3d_cpu = t_scope.cpu;
    info.d3d_misc = t_scope.misc;
    info.call = t_scope.call;
  }
  if (!info.site)
    info.site = t_scope.site;
}

uint64_t
Add(Info info, const void *owner, const void *group) {
  ApplyScope(info);
  return Global().add(info, owner, group, NowMs());
}

void
Remove(uint64_t id) {
  if (id)
    Global().remove(id, NowMs());
}

void
MarkReleased(const void *group) {
  Global().markGroupReleased(group);
}

/* ---- The lines ---- */

namespace {

uint64_t
WholeMb(uint64_t bytes) {
  return (bytes + (512u << 10)) >> 20; // to the nearest MB
}

std::string
Mb1(uint64_t bytes) {
  char text[32];
  snprintf(text, sizeof(text), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
  return text;
}

void
Append(std::string &out, const char *format, ...) __attribute__((format(printf, 2, 3)));

void
Append(std::string &out, const char *format, ...) {
  char text[512];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  out += text;
}

const char *
D3D11UsageName(uint32_t usage) {
  switch (usage) {
  case 0:
    return "DEFAULT";
  case 1:
    return "IMMUTABLE";
  case 2:
    return "DYNAMIC";
  case 3:
    return "STAGING";
  }
  return nullptr;
}

const char *
D3D9PoolName(uint32_t pool) {
  switch (pool) {
  case 0:
    return "DEFAULT";
  case 1:
    return "MANAGED";
  case 2:
    return "SYSTEMMEM";
  case 3:
    return "SCRATCH";
  }
  return nullptr;
}

const char *
TextureTypeName(uint8_t type) {
  /* WMTTextureType, which follows MTLTextureType */
  switch (type) {
  case 0:
    return "1D";
  case 1:
    return "1D array";
  case 2:
    return "2D";
  case 3:
    return "2D array";
  case 4:
    return "2D MS";
  case 5:
    return "cube";
  case 6:
    return "cube array";
  case 7:
    return "3D";
  case 8:
    return "2D MS array";
  case 9:
    return "texture buffer";
  }
  return "texture";
}

} // namespace

std::string
D3DFlagsText(const Info &info) {
  std::string out;
  if (info.api && info.call) {
    Append(out, "D3D%u %s", unsigned(info.api), info.call);
  } else if (info.api == 11) {
    const char *usage = D3D11UsageName(info.d3d_usage);
    if (usage)
      Append(out, "D3D11 %s", usage);
    else
      Append(out, "D3D11 usage %u", info.d3d_usage);
    static const struct {
      uint32_t bit;
      const char *name;
    } binds[] = {
        {0x1, "VB"},  {0x2, "IB"},  {0x4, "CB"},   {0x8, "SRV"},     {0x10, "SO"},
        {0x20, "RT"}, {0x40, "DS"}, {0x80, "UAV"}, {0x200, "DECODER"}, {0x400, "ENCODER"},
    };
    out += " bind ";
    if (!info.d3d_bind) {
      out += "none";
    } else {
      uint32_t rest = info.d3d_bind;
      bool first = true;
      for (const auto &b : binds) {
        if (!(rest & b.bit))
          continue;
        if (!first)
          out += "|";
        out += b.name;
        first = false;
        rest &= ~b.bit;
      }
      if (rest)
        Append(out, "%s0x%x", first ? "" : "|", rest);
    }
    if (info.d3d_cpu)
      Append(out, " cpu 0x%x", info.d3d_cpu);
    if (info.d3d_misc)
      Append(out, " misc 0x%x", info.d3d_misc);
  } else if (info.api == 9) {
    const char *pool = D3D9PoolName(info.d3d_bind);
    Append(out, "D3D9 usage 0x%x pool ", info.d3d_usage);
    if (pool)
      out += pool;
    else
      Append(out, "%u", info.d3d_bind);
  } else {
    out += "no Direct3D call";
  }
  return out;
}

std::string
StatusLine(const Totals &t, const uint64_t *graphics_footprint) {
  std::string out;
  const KindTotal &rt = t.kind[unsigned(Kind::RtDs)];
  const KindTotal &tex = t.kind[unsigned(Kind::Texture)];
  const KindTotal &buf = t.kind[unsigned(Kind::Buffer)];
  const KindTotal &stg = t.kind[unsigned(Kind::Staging)];
  const KindTotal &internal = t.kind[unsigned(Kind::Internal)];
  Append(
      out,
      "[dxmt-mem] %" PRIu64 " MB live: rt/ds %" PRIu64 " (%" PRIu64 " MB), textures %" PRIu64 " (%" PRIu64
      " MB), buffers %" PRIu64 " (%" PRIu64 " MB), staging %" PRIu64 " blocks (%" PRIu64 " MB), internal %" PRIu64
      " MB; ",
      WholeMb(t.bytes), rt.count, WholeMb(rt.bytes), tex.count, WholeMb(tex.bytes), buf.count, WholeMb(buf.bytes),
      stg.count, WholeMb(stg.bytes), WholeMb(internal.bytes)
  );
  Append(
      out,
      "waiting release %" PRIu64 " (%" PRIu64 " MB); last 10 s created %" PRIu64 " (%" PRIu64 " MB) released %" PRIu64
      " (%" PRIu64 " MB)",
      t.waiting_count, WholeMb(t.waiting_bytes), t.created_count, WholeMb(t.created_bytes), t.released_count,
      WholeMb(t.released_bytes)
  );
  if (graphics_footprint)
    Append(out, "; graphics footprint %" PRIu64 " MB", WholeMb(*graphics_footprint));
  return out;
}

std::string
StorageLine(const Totals &t) {
  std::string out = "[dxmt-mem] shared storage:";
  for (unsigned k = 0; k < kKindCount; k++)
    Append(out, "%s %s %" PRIu64 " MB", k ? "," : "", KindName(Kind(k)), WholeMb(t.kind[k].shared_bytes));
  Append(out, "; BC stored uncompressed %" PRIu64 " (%" PRIu64 " MB)", t.bc_count, WholeMb(t.bc_bytes));
  return out;
}

std::string
LargestLine(unsigned rank, const Info &info, FormatNameFn format_name) {
  std::string out;
  Append(out, "[dxmt-mem]   %u. %s ", rank, KindName(info.kind));
  if (info.texture) {
    Append(out, "%s %ux%u", TextureTypeName(info.texture_type), info.width, info.height);
    if (info.depth > 1)
      Append(out, "x%u", info.depth);
    Append(out, " array %u mips %u ", info.array_length, unsigned(info.mips));
    out += format_name ? format_name(info.format) : std::to_string(info.format);
    if (info.bc_uncompressed)
      out += " (stored uncompressed)";
    Append(out, " samples %u", unsigned(info.samples));
  } else {
    Append(out, "buffer %u bytes", info.width);
  }
  if (info.what)
    Append(out, " (%s)", info.what);
  out += ", ";
  out += D3DFlagsText(info);
  out += info.storage == Storage::Shared ? ", shared " : ", private ";
  out += Mb1(info.bytes);
  return out;
}

ReportDue
ReportSchedule::tick(uint64_t now_ms, uint64_t warnings, uint64_t footprint, uint64_t live_bytes) {
  ReportDue due;
  unsigned footprint_gb = unsigned(footprint >> 30);
  if (!started_) {
    started_ = true;
    next_status_ms_ = now_ms + Ledger::kWindowMs;
    warnings_ = warnings;
    footprint_gb_ = footprint_gb;
  } else {
    if (now_ms >= next_status_ms_) {
      due.status = true;
      next_status_ms_ = now_ms + Ledger::kWindowMs;
    }
    if (warnings > warnings_) {
      due.warning = true;
      due.status = true;
      warnings_ = warnings;
    }
    if (footprint_gb > footprint_gb_) {
      due.footprint_gb = footprint_gb;
      due.status = true;
      footprint_gb_ = footprint_gb;
    }
  }
  unsigned total_gb = unsigned(std::min<uint64_t>(live_bytes >> 30, 3));
  if (total_gb > total_gb_) {
    due.total_gb = total_gb;
    total_gb_ = total_gb;
  }
  return due;
}

std::string
SitesLine(const std::vector<SiteBytes> &sites, SiteNameFn site_name) {
  std::string out = "[dxmt-mem] top creation sites, last 10 s:";
  if (sites.empty()) {
    out += " none";
    return out;
  }
  bool first = true;
  for (const auto &s : sites) {
    out += first ? " " : ", ";
    first = false;
    if (s.site == kOtherSites)
      out += "(other sites)";
    else if (!s.site)
      out += "(no Direct3D call)";
    else if (site_name)
      out += site_name(s.site);
    else
      Append(out, "0x%" PRIx64, s.site);
    Append(out, " %s (%" PRIu64 ")", Mb1(s.bytes).c_str(), s.count);
  }
  return out;
}

} // namespace dxmt::memledger
