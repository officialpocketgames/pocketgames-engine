// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 item 90 (round 20): every Metal texture and buffer DXMT makes, counted by kind while
// it lives (DXMT makes no Metal heaps), and the [dxmt-mem] lines that say what holds the memory.
// Plain C++ with no Metal or Win32 in it, so the host tests run it as it is.
// dxmt_mem_track.hpp fills records from DXMT's types; dxmt_mem_report.cpp writes the lines.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace dxmt::memledger {

enum class Kind : uint8_t {
  RtDs,     // a program's texture Metal may render to: render target or depth-stencil
  Texture,  // a program's texture that is only sampled (or read and written as a UAV)
  Buffer,   // a program's buffer
  Staging,  // a block of a command queue's rings: staging and upload, copy temporaries,
            // argument buffers (the allocator behind "forced to allocate new block")
  Internal, // DXMT's own: emulation, clears and blits, caches, the frame dump
};
constexpr unsigned kKindCount = 5;

/* "rt/ds", "textures", "buffers", "staging", "internal": the names the lines use */
const char *KindName(Kind kind);

enum class Storage : uint8_t {
  Private, // only the GPU sees it: memory no mapped region of the process shows
  Shared,  // the CPU sees it too (Metal's shared and managed storage)
};

/* What a record says about its Metal object. The Direct3D fields are those of the call the
   object was made in (Scope below), 0 when it was made outside one. */
struct Info {
  Kind kind = Kind::Internal;
  Storage storage = Storage::Private;
  bool texture = false;
  bool bc_uncompressed = false; // a BC format stored uncompressed (spec 05 Part C)
  uint8_t api = 0;              // 11 or 9 inside a Direct3D call, 0 outside one
  uint8_t mips = 1;
  uint8_t samples = 1;
  uint8_t texture_type = 0;     // WMTTextureType
  uint32_t format = 0;          // WMTPixelFormat as the program sees it
  uint32_t width = 0;           // a buffer's length in bytes
  uint32_t height = 1;
  uint32_t depth = 1;
  uint32_t array_length = 1;
  /* Direct3D 11: the description's Usage, BindFlags, CPUAccessFlags and MiscFlags.
     Direct3D 9: Usage and Pool (the last two 0). */
  uint32_t d3d_usage = 0;
  uint32_t d3d_bind = 0;
  uint32_t d3d_cpu = 0;
  uint32_t d3d_misc = 0;
  uint64_t bytes = 0;           // Metal's allocatedSize (buffers: the length in whole 16 KB pages)
  uint64_t site = 0;            // the program's return address from the creating call; 0 unknown
  const char *what = nullptr;   // a staging block's ring or an internal object's name (static)
  const char *call = nullptr;   // a Direct3D call without a description: "Map", "UpdateSubresource"
};

struct KindTotal {
  uint64_t count = 0;
  uint64_t bytes = 0;
  uint64_t shared_bytes = 0; // of `bytes`, those in shared storage
};

struct Totals {
  KindTotal kind[kKindCount];
  uint64_t count = 0;
  uint64_t bytes = 0;
  /* live, but held only until the GPU finishes command buffers that use them: the program (or
     DXMT) has let go of them (the deferred release queue: the command chunks' references) */
  uint64_t waiting_count = 0;
  uint64_t waiting_bytes = 0;
  /* made and released in the last 10 s */
  uint64_t created_count = 0;
  uint64_t created_bytes = 0;
  uint64_t released_count = 0;
  uint64_t released_bytes = 0;
  /* BC textures stored uncompressed, live */
  uint64_t bc_count = 0;
  uint64_t bc_bytes = 0;
};

struct SiteBytes {
  uint64_t site = 0; // 0: made outside a Direct3D call; kOtherSites: past the per-second table
  uint64_t bytes = 0;
  uint64_t count = 0;
};
constexpr uint64_t kOtherSites = ~0ull;

/* The records. Every member takes the ledger's lock; none calls out while holding it except
   the waiting test given to totals(), which must not take a lock of its own. */
class Ledger {
public:
  using WaitingFn = bool (*)(const void *owner);
  static constexpr uint64_t kWindowMs = 10000;   // "the last 10 s"
  static constexpr unsigned kSeconds = 10;       // one slot per second of the window
  static constexpr unsigned kSitesPerSecond = 32;

  /* A new record; returns its id (never 0). `owner` is what the waiting test is asked about
     (DXMT's Allocation), or null. `group` is the resource it belongs to (DXMT's Buffer or
     Texture), or null; it is a key only, never read through. */
  uint64_t add(const Info &info, const void *owner, const void *group, uint64_t now_ms);

  /* The record goes (its object is released). An unknown id or 0 is ignored. */
  void remove(uint64_t id, uint64_t now_ms);

  /* The program has released the resource `group`: whatever of it still lives waits for the
     GPU (command buffers not yet finished hold it). */
  void markGroupReleased(const void *group);

  /* The live totals and the last 10 s. `waiting` is asked about each record with an owner. */
  Totals totals(uint64_t now_ms, WaitingFn waiting) const;

  /* The `n` largest live records, largest first (the older of two the same size first). */
  std::vector<Info> largest(unsigned n) const;

  /* The `n` creation sites that made the most bytes in the last 10 s, most first. */
  std::vector<SiteBytes> topSites(unsigned n, uint64_t now_ms) const;

  uint64_t liveBytes() const;
  uint64_t liveCount() const;

private:
  struct Record {
    Info info;
    const void *owner;
    const void *group;
    bool program_released;
  };
  struct Second {
    uint64_t second = ~0ull;
    uint64_t created_count = 0, created_bytes = 0;
    uint64_t released_count = 0, released_bytes = 0;
    unsigned site_count = 0;
    SiteBytes sites[kSitesPerSecond];
    SiteBytes other{kOtherSites, 0, 0};
  };

  Second &secondFor(uint64_t now_ms);
  /* whether slot `s` holds a second of the window that ends at now_ms */
  static bool inWindow(const Second &s, uint64_t now_ms);

  mutable std::mutex mutex_;
  std::unordered_map<uint64_t, Record> records_;
  std::unordered_map<const void *, std::vector<uint64_t>> groups_;
  uint64_t next_id_ = 1;
  uint64_t live_bytes_ = 0;
  Second seconds_[kSeconds];
};

/* ---- The process's ledger, and what DXMT's creation paths call ---- */

Ledger &Global();
uint64_t NowMs();

/* A record in the process's ledger, the Direct3D fields and the site taken from this thread's
   Scope when the caller left them 0. Returns the id. */
uint64_t Add(Info info, const void *owner = nullptr, const void *group = nullptr);
void Remove(uint64_t id);
void MarkReleased(const void *group);

/* The Direct3D call this thread is in: what Add() takes the Direct3D fields and the site from.
   Scopes nest; the outermost wins, so a call that forwards to another keeps its own caller. */
class Scope {
public:
  Scope(uint8_t api, uint32_t usage, uint32_t bind, uint32_t cpu, uint32_t misc, const void *site);
  /* a call that makes memory but has no description (Map, UpdateSubresource); `call` static */
  Scope(uint8_t api, const char *call, const void *site);
  ~Scope();
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;

private:
  bool outermost_;
};

/* Fills info's Direct3D fields and site from this thread's Scope, where info has none. */
void ApplyScope(Info &info);

/* The scope of a Direct3D 11 resource creation with description `desc` (may be null), and of a
   call without one; the site is the caller's return address. */
#define DXMT_MEM_SCOPE_D3D11(desc)                                                                                     \
  ::dxmt::memledger::Scope dxmt_mem_scope_(                                                                            \
      11, (desc) ? uint32_t((desc)->Usage) : 0u, (desc) ? uint32_t((desc)->BindFlags) : 0u,                            \
      (desc) ? uint32_t((desc)->CPUAccessFlags) : 0u, (desc) ? uint32_t((desc)->MiscFlags) : 0u,                       \
      __builtin_return_address(0)                                                                                      \
  )
#define DXMT_MEM_SCOPE_CALL(api, name) ::dxmt::memledger::Scope dxmt_mem_scope_(api, name, __builtin_return_address(0))

/* Removes its record when it goes; for Metal objects held outside an Allocation. */
class Entry {
public:
  Entry() = default;
  explicit Entry(uint64_t id) : id_(id) {}
  Entry(Entry &&other) noexcept : id_(other.id_) { other.id_ = 0; }
  Entry &
  operator=(Entry &&other) noexcept {
    if (this != &other) {
      reset();
      id_ = other.id_;
      other.id_ = 0;
    }
    return *this;
  }
  Entry(const Entry &) = delete;
  Entry &operator=(const Entry &) = delete;
  ~Entry() { reset(); }

  void
  reset() {
    if (id_)
      Remove(id_);
    id_ = 0;
  }

  uint64_t
  id() const {
    return id_;
  }

private:
  uint64_t id_ = 0;
};

/* ---- The lines (dxmt_mem_report.cpp writes them; the host tests check them) ---- */

/* `[dxmt-mem] <total MB> live: rt/ds <n> (<MB>), textures <n> (<MB>), buffers <n> (<MB>),
   staging <n> blocks (<MB>), internal <MB>; waiting release <n> (<MB>); last 10 s created <n>
   (<MB>) released <n> (<MB>)[; graphics footprint <MB>]`, without the graphics footprint when
   `graphics_footprint` is null. */
std::string StatusLine(const Totals &totals, const uint64_t *graphics_footprint);

/* `[dxmt-mem] shared storage: rt/ds <MB>, textures <MB>, buffers <MB>, staging <MB>, internal
   <MB>; BC stored uncompressed <n> (<MB>)` */
std::string StorageLine(const Totals &totals);

using FormatNameFn = std::string (*)(uint32_t format);

/* One of the largest: `[dxmt-mem]   <rank>. <kind> ...`, the dimensions, array size, mips, format,
   sample count, Direct3D usage and bind flags, storage and size. */
std::string LargestLine(unsigned rank, const Info &info, FormatNameFn format_name);

/* What a site is called in the sites line: a module and offset where the caller can say. */
using SiteNameFn = std::string (*)(uint64_t site);

/* `[dxmt-mem] top creation sites, last 10 s: <site> <MB> (<n>), ...` */
std::string SitesLine(const std::vector<SiteBytes> &sites, SiteNameFn site_name);

/* Direct3D 11's usage and bind flags as words ("DEFAULT", "SRV|RT"); Direct3D 9's usage and
   pool. Part of LargestLine; exposed for the tests. */
std::string D3DFlagsText(const Info &info);

/* ---- When the lines are written (dxmt_mem_report.cpp asks every 250 ms) ---- */

struct ReportDue {
  bool status = false;       // the status line
  bool warning = false;      // a new memory warning (also the storage line)
  unsigned footprint_gb = 0; // the footprint passed this whole GB for the first time (0: no)
  unsigned total_gb = 0;     // DXMT's total first passed this GB, 1-3: the 10 largest and the sites
};

/* The status line every 10 s, and at once when the memory warnings rise or the footprint first
   passes a whole GB; the largest the first time DXMT's live total passes 1, 2 and 3 GB (once
   for the highest of them passed at one tick). The first tick only takes the starting point. */
class ReportSchedule {
public:
  ReportDue tick(uint64_t now_ms, uint64_t warnings, uint64_t footprint, uint64_t live_bytes);

private:
  bool started_ = false;
  uint64_t next_status_ms_ = 0;
  uint64_t warnings_ = 0;
  unsigned footprint_gb_ = 0;
  unsigned total_gb_ = 0;
};

} // namespace dxmt::memledger
