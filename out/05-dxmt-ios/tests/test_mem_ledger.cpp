// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 item 90 (round 20): the Metal memory ledger (dxmt_mem_ledger.cpp) on its own: records
// made and released, totals by kind with shared storage apart, the deferred release (waiting for
// the GPU, by the owner's test and by a released group), the last 10 s, the order of the 10
// largest, the creation sites, the lines' text, when the lines are due, and the creation scopes.
//   c++ -std=c++20 -I../dxmt/src/dxmt test_mem_ledger.cpp ../dxmt/src/dxmt/dxmt_mem_ledger.cpp

#include "test_common.hpp"

#include "dxmt_mem_ledger.hpp"

#include <cstring>
#include <string>
#include <vector>

using namespace dxmt::memledger;

/* the lines compared as text, both shown when they differ */
#define CHECK_STR(a, b)                                                                                                \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    std::string got_ = (a), want_ = (b);                                                                               \
    if (got_ != want_) {                                                                                               \
      g_failures++;                                                                                                    \
      std::printf("FAIL %s:%d:\n  got  %s\n  want %s\n", __FILE__, __LINE__, got_.c_str(), want_.c_str());           \
    }                                                                                                                  \
  } while (0)

namespace {

constexpr uint64_t MB = 1ull << 20;
constexpr uint64_t GB = 1ull << 30;

Info
TextureOf(Kind kind, uint64_t bytes, Storage storage = Storage::Private) {
  Info info;
  info.kind = kind;
  info.texture = true;
  info.storage = storage;
  info.bytes = bytes;
  info.width = 256;
  info.height = 256;
  return info;
}

Info
BufferOf(Kind kind, uint64_t bytes, Storage storage = Storage::Shared, uint64_t site = 0) {
  Info info;
  info.kind = kind;
  info.storage = storage;
  info.bytes = bytes;
  info.width = uint32_t(bytes);
  info.site = site;
  return info;
}

/* the owners' waiting test: an owner is a bool saying whether only the GPU holds it */
bool
FakeWaiting(const void *owner) {
  return *static_cast<const bool *>(owner);
}

bool
Contains(const std::string &text, const char *part) {
  return text.find(part) != std::string::npos;
}

std::string
FakeFormatName(uint32_t format) {
  return format == 70 ? "RGBA8Unorm" : "format" + std::to_string(format);
}

std::string
FakeSiteName(uint64_t site) {
  char text[64];
  snprintf(text, sizeof(text), "game.exe+0x%llx", (unsigned long long)(site - 0x400000));
  return text;
}

void
TestTotalsByKind() {
  Ledger ledger;
  uint64_t t = 1'000'000;
  uint64_t rt = ledger.add(TextureOf(Kind::RtDs, 64 * MB), nullptr, nullptr, t);
  ledger.add(TextureOf(Kind::RtDs, 16 * MB, Storage::Shared), nullptr, nullptr, t);
  ledger.add(TextureOf(Kind::Texture, 8 * MB), nullptr, nullptr, t);
  ledger.add(BufferOf(Kind::Buffer, 4 * MB), nullptr, nullptr, t);
  ledger.add(BufferOf(Kind::Staging, 32 * MB), nullptr, nullptr, t);
  ledger.add(BufferOf(Kind::Internal, MB, Storage::Private), nullptr, nullptr, t);

  Totals totals = ledger.totals(t, nullptr);
  CHECK_EQ(totals.count, 6u);
  CHECK_EQ(totals.bytes, 125 * MB);
  CHECK_EQ(totals.kind[unsigned(Kind::RtDs)].count, 2u);
  CHECK_EQ(totals.kind[unsigned(Kind::RtDs)].bytes, 80 * MB);
  CHECK_EQ(totals.kind[unsigned(Kind::RtDs)].shared_bytes, 16 * MB);
  CHECK_EQ(totals.kind[unsigned(Kind::Texture)].bytes, 8 * MB);
  CHECK_EQ(totals.kind[unsigned(Kind::Texture)].shared_bytes, 0u);
  CHECK_EQ(totals.kind[unsigned(Kind::Buffer)].shared_bytes, 4 * MB);
  CHECK_EQ(totals.kind[unsigned(Kind::Staging)].count, 1u);
  CHECK_EQ(totals.kind[unsigned(Kind::Internal)].bytes, MB);
  CHECK_EQ(ledger.liveBytes(), 125 * MB);
  CHECK_EQ(ledger.liveCount(), 6u);

  // released: gone from the live totals, counted as released
  ledger.remove(rt, t + 10);
  totals = ledger.totals(t + 10, nullptr);
  CHECK_EQ(totals.count, 5u);
  CHECK_EQ(totals.kind[unsigned(Kind::RtDs)].count, 1u);
  CHECK_EQ(totals.kind[unsigned(Kind::RtDs)].bytes, 16 * MB);
  CHECK_EQ(ledger.liveBytes(), 61 * MB);
  CHECK_EQ(totals.created_count, 6u);
  CHECK_EQ(totals.created_bytes, 125 * MB);
  CHECK_EQ(totals.released_count, 1u);
  CHECK_EQ(totals.released_bytes, 64 * MB);

  // an unknown id, 0 and a second remove change nothing
  ledger.remove(rt, t + 20);
  ledger.remove(0, t + 20);
  ledger.remove(999999, t + 20);
  CHECK_EQ(ledger.liveCount(), 5u);
  CHECK_EQ(ledger.totals(t + 20, nullptr).released_count, 1u);

  // BC textures stored uncompressed are counted apart, inside their kind
  Info bc = TextureOf(Kind::Texture, 2 * MB);
  bc.bc_uncompressed = true;
  ledger.add(bc, nullptr, nullptr, t + 30);
  totals = ledger.totals(t + 30, nullptr);
  CHECK_EQ(totals.bc_count, 1u);
  CHECK_EQ(totals.bc_bytes, 2 * MB);
  CHECK_EQ(totals.kind[unsigned(Kind::Texture)].bytes, 10 * MB);
}

void
TestDeferredRelease() {
  Ledger ledger;
  uint64_t t = 5'000'000;
  bool held_by_program = false, gpu_only = true;
  // the owner's test: only command chunks hold it
  ledger.add(BufferOf(Kind::Buffer, 3 * MB), &held_by_program, nullptr, t);
  uint64_t discarded = ledger.add(BufferOf(Kind::Buffer, 5 * MB), &gpu_only, nullptr, t);
  // a released group: the program let go of the resource, whatever of it lives waits
  int texture_a = 0, texture_b = 0;
  uint64_t a1 = ledger.add(TextureOf(Kind::RtDs, 10 * MB), &held_by_program, &texture_a, t);
  uint64_t a2 = ledger.add(TextureOf(Kind::RtDs, 10 * MB), &held_by_program, &texture_a, t);
  ledger.add(TextureOf(Kind::Texture, 7 * MB), &held_by_program, &texture_b, t);

  Totals totals = ledger.totals(t, FakeWaiting);
  CHECK_EQ(totals.waiting_count, 1u);
  CHECK_EQ(totals.waiting_bytes, 5 * MB);
  // without a test, an owner is never asked
  CHECK_EQ(ledger.totals(t, nullptr).waiting_count, 0u);

  ledger.markGroupReleased(&texture_a);
  totals = ledger.totals(t, FakeWaiting);
  CHECK_EQ(totals.waiting_count, 3u);
  CHECK_EQ(totals.waiting_bytes, 25 * MB);
  CHECK_EQ(totals.count, 5u); // still live: waiting is part of the live totals

  // the GPU finishes: the records go, and with them the waiting
  ledger.remove(a1, t + 1);
  ledger.remove(a2, t + 1);
  ledger.remove(discarded, t + 1);
  totals = ledger.totals(t + 1, FakeWaiting);
  CHECK_EQ(totals.waiting_count, 0u);
  CHECK_EQ(totals.count, 2u);

  // a group released twice, an unknown group and a null one change nothing; a new resource at
  // a group's old address starts unmarked
  ledger.markGroupReleased(&texture_a);
  ledger.markGroupReleased(nullptr);
  int never = 0;
  ledger.markGroupReleased(&never);
  uint64_t again = ledger.add(TextureOf(Kind::RtDs, MB), &held_by_program, &texture_a, t + 2);
  CHECK_EQ(ledger.totals(t + 2, FakeWaiting).waiting_count, 0u);
  ledger.markGroupReleased(&texture_a);
  CHECK_EQ(ledger.totals(t + 2, FakeWaiting).waiting_count, 1u);
  ledger.remove(again, t + 3);
}

void
TestLastTenSeconds() {
  Ledger ledger;
  uint64_t t0 = 100'000; // a whole second
  ledger.add(BufferOf(Kind::Buffer, MB), nullptr, nullptr, t0);
  uint64_t b = ledger.add(BufferOf(Kind::Buffer, 2 * MB), nullptr, nullptr, t0 + 4'500);
  ledger.remove(b, t0 + 9'000);

  Totals totals = ledger.totals(t0 + 9'999, nullptr);
  CHECK_EQ(totals.created_count, 2u);
  CHECK_EQ(totals.created_bytes, 3 * MB);
  CHECK_EQ(totals.released_count, 1u);
  // 10 s after the first second, it has left the window; the later ones haven't
  totals = ledger.totals(t0 + 10'000, nullptr);
  CHECK_EQ(totals.created_count, 1u);
  CHECK_EQ(totals.created_bytes, 2 * MB);
  CHECK_EQ(totals.released_count, 1u);
  totals = ledger.totals(t0 + 19'000, nullptr);
  CHECK_EQ(totals.created_count, 0u);
  CHECK_EQ(totals.released_count, 0u);
  // a slot reused for a new second starts empty
  ledger.add(BufferOf(Kind::Buffer, 4 * MB), nullptr, nullptr, t0 + 20'000);
  totals = ledger.totals(t0 + 20'000, nullptr);
  CHECK_EQ(totals.created_count, 1u);
  CHECK_EQ(totals.created_bytes, 4 * MB);
  // the live totals don't depend on the window
  CHECK_EQ(totals.count, 2u);
}

void
TestLargestOrder() {
  Ledger ledger;
  uint64_t t = 1'000;
  const uint64_t sizes[] = {5, 90, 12, 90, 1, 64, 33, 7, 64, 2, 48, 80};
  for (unsigned i = 0; i < 12; i++) {
    Info info = TextureOf(Kind::Texture, sizes[i] * MB);
    info.width = i; // which one it is
    ledger.add(info, nullptr, nullptr, t);
  }
  std::vector<Info> top = ledger.largest(10);
  CHECK_EQ(top.size(), 10u);
  const uint64_t expected_bytes[] = {90, 90, 80, 64, 64, 48, 33, 12, 7, 5};
  for (unsigned i = 0; i < 10; i++)
    CHECK_EQ(top[i].bytes, expected_bytes[i] * MB);
  // the same size: the one made first first
  CHECK_EQ(top[0].width, 1u);
  CHECK_EQ(top[1].width, 3u);
  CHECK_EQ(top[3].width, 5u);
  CHECK_EQ(top[4].width, 8u);
  // fewer records than asked for
  CHECK_EQ(ledger.largest(20).size(), 12u);
  Ledger empty;
  CHECK(empty.largest(10).empty());
}

void
TestSites() {
  Ledger ledger;
  uint64_t t0 = 50'000;
  ledger.add(BufferOf(Kind::Buffer, 10 * MB, Storage::Shared, 0x401000), nullptr, nullptr, t0);
  ledger.add(BufferOf(Kind::Buffer, 10 * MB, Storage::Shared, 0x401000), nullptr, nullptr, t0 + 1'000);
  ledger.add(TextureOf(Kind::RtDs, 64 * MB), nullptr, nullptr, t0 + 2'000); // no Direct3D call
  ledger.add(BufferOf(Kind::Buffer, 30 * MB, Storage::Shared, 0x402000), nullptr, nullptr, t0 + 3'000);
  ledger.add(BufferOf(Kind::Buffer, MB, Storage::Shared, 0x403000), nullptr, nullptr, t0 + 3'000);

  std::vector<SiteBytes> sites = ledger.topSites(5, t0 + 3'000);
  CHECK_EQ(sites.size(), 4u);
  CHECK_EQ(sites[0].site, 0u);
  CHECK_EQ(sites[0].bytes, 64 * MB);
  CHECK_EQ(sites[1].site, 0x402000u);
  CHECK_EQ(sites[2].site, 0x401000u);
  CHECK_EQ(sites[2].bytes, 20 * MB);
  CHECK_EQ(sites[2].count, 2u);
  CHECK_EQ(sites[3].site, 0x403000u);
  CHECK_EQ(ledger.topSites(2, t0 + 3'000).size(), 2u);
  // the first second has left the window
  sites = ledger.topSites(5, t0 + 10'000);
  CHECK_EQ(sites[2].site, 0x401000u);
  CHECK_EQ(sites[2].bytes, 10 * MB);

  // past the per-second table, the rest go to "other sites"
  Ledger crowded;
  uint64_t t1 = 90'000;
  for (unsigned i = 0; i < Ledger::kSitesPerSecond + 3; i++)
    crowded.add(BufferOf(Kind::Buffer, MB, Storage::Shared, 0x500000 + i), nullptr, nullptr, t1);
  sites = crowded.topSites(100, t1);
  CHECK_EQ(sites.size(), size_t(Ledger::kSitesPerSecond) + 1);
  bool other = false;
  for (auto &s : sites) {
    if (s.site == kOtherSites) {
      other = true;
      CHECK_EQ(s.count, 3u);
      CHECK_EQ(s.bytes, 3 * MB);
    }
  }
  CHECK(other);
}

void
TestLines() {
  Totals totals;
  totals.kind[unsigned(Kind::RtDs)] = {12, 1850 * MB, 0};
  totals.kind[unsigned(Kind::Texture)] = {340, 612 * MB, 0};
  totals.kind[unsigned(Kind::Buffer)] = {210, 96 * MB, 40 * MB};
  totals.kind[unsigned(Kind::Staging)] = {6, 256 * MB, 192 * MB};
  totals.kind[unsigned(Kind::Internal)] = {4, 3 * MB, MB};
  totals.count = 572;
  totals.bytes = 2817 * MB;
  totals.waiting_count = 31;
  totals.waiting_bytes = 480 * MB;
  totals.created_count = 44;
  totals.created_bytes = 900 * MB;
  totals.released_count = 9;
  totals.released_bytes = 120 * MB;
  totals.bc_count = 5;
  totals.bc_bytes = 160 * MB;

  std::string line = StatusLine(totals, nullptr);
  CHECK_STR(
      line, std::string("[dxmt-mem] 2817 MB live: rt/ds 12 (1850 MB), textures 340 (612 MB), buffers 210 (96 MB), "
                        "staging 6 blocks (256 MB), internal 3 MB; waiting release 31 (480 MB); last 10 s created "
                        "44 (900 MB) released 9 (120 MB)")
  );
  uint64_t graphics = 3900 * MB;
  CHECK_STR(StatusLine(totals, &graphics), line + "; graphics footprint 3900 MB");
  // to the nearest MB
  Totals small;
  small.bytes = MB / 2;
  CHECK(Contains(StatusLine(small, nullptr), "[dxmt-mem] 1 MB live"));
  small.bytes = MB / 2 - 1;
  CHECK(Contains(StatusLine(small, nullptr), "[dxmt-mem] 0 MB live"));

  CHECK_STR(
      StorageLine(totals), std::string("[dxmt-mem] shared storage: rt/ds 0 MB, textures 0 MB, buffers 40 MB, staging "
                                       "192 MB, internal 1 MB; BC stored uncompressed 5 (160 MB)")
  );

  Info rt = TextureOf(Kind::RtDs, uint64_t(63.3 * MB));
  rt.texture_type = 2;
  rt.width = 3840;
  rt.height = 2160;
  rt.format = 70;
  rt.api = 11;
  rt.d3d_usage = 0;
  rt.d3d_bind = 0x20 | 0x8;
  CHECK_STR(
      LargestLine(1, rt, FakeFormatName),
      std::string("[dxmt-mem]   1. rt/ds 2D 3840x2160 array 1 mips 1 RGBA8Unorm samples 1, D3D11 DEFAULT bind "
                  "SRV|RT, private 63.3 MB")
  );
  Info volume = TextureOf(Kind::Texture, 16 * MB);
  volume.texture_type = 7;
  volume.width = 128;
  volume.height = 128;
  volume.depth = 64;
  volume.mips = 8;
  volume.format = 3;
  volume.bc_uncompressed = true;
  volume.api = 9;
  volume.d3d_usage = 0;
  volume.d3d_bind = 1;
  CHECK_STR(
      LargestLine(2, volume, FakeFormatName),
      std::string("[dxmt-mem]   2. textures 3D 128x128x64 array 1 mips 8 format3 (stored uncompressed) samples 1, D3D9 "
                  "usage 0x0 pool MANAGED, private 16.0 MB")
  );
  Info block = BufferOf(Kind::Staging, 64 * MB, Storage::Private);
  block.what = "copy temporaries";
  CHECK_STR(
      LargestLine(3, block, FakeFormatName),
      std::string("[dxmt-mem]   3. staging buffer 67108864 bytes (copy temporaries), no Direct3D call, private 64.0 MB")
  );
  Info mapped = BufferOf(Kind::Buffer, 4 * MB);
  mapped.api = 11;
  mapped.call = "Map";
  CHECK(Contains(LargestLine(4, mapped, nullptr), "buffer 4194304 bytes, D3D11 Map, shared 4.0 MB"));

  // the flags' words
  Info flags;
  flags.api = 11;
  flags.d3d_usage = 2;
  flags.d3d_bind = 0x1 | 0x800;
  flags.d3d_cpu = 0x10000;
  flags.d3d_misc = 0x40;
  CHECK_STR(D3DFlagsText(flags), std::string("D3D11 DYNAMIC bind VB|0x800 cpu 0x10000 misc 0x40"));
  flags.d3d_usage = 7;
  flags.d3d_bind = 0;
  flags.d3d_cpu = flags.d3d_misc = 0;
  CHECK_STR(D3DFlagsText(flags), std::string("D3D11 usage 7 bind none"));
  flags.api = 9;
  flags.d3d_usage = 0x201;
  flags.d3d_bind = 9;
  CHECK_STR(D3DFlagsText(flags), std::string("D3D9 usage 0x201 pool 9"));

  std::vector<SiteBytes> sites = {{0x5a1b2c, 812 * MB, 12}, {0, 64 * MB, 1}, {kOtherSites, 3 * MB, 3}};
  CHECK_STR(
      SitesLine(sites, FakeSiteName),
      std::string("[dxmt-mem] top creation sites, last 10 s: game.exe+0x1a1b2c 812.0 MB (12), (no Direct3D call) "
                  "64.0 MB (1), (other sites) 3.0 MB (3)")
  );
  CHECK(Contains(SitesLine({{0x1234, MB, 1}}, nullptr), " 0x1234 1.0 MB (1)"));
  CHECK_STR(SitesLine({}, FakeSiteName), std::string("[dxmt-mem] top creation sites, last 10 s: none"));
  CHECK_STR(std::string(KindName(Kind::Staging)), std::string("staging"));
}

void
TestSchedule() {
  ReportSchedule schedule;
  uint64_t t = 7'000'000;
  // the first tick takes the starting point only (2.5 GB footprint, no warnings)
  ReportDue due = schedule.tick(t, 0, 2560 * MB, 0);
  CHECK(!due.status && !due.warning && !due.footprint_gb && !due.total_gb);
  due = schedule.tick(t + 9'750, 0, 2600 * MB, 0);
  CHECK(!due.status);
  due = schedule.tick(t + 10'000, 0, 2600 * MB, 0);
  CHECK(due.status && !due.warning && !due.footprint_gb);
  due = schedule.tick(t + 10'250, 0, 2600 * MB, 0);
  CHECK(!due.status);
  // the footprint passes 3 GB: at once
  due = schedule.tick(t + 11'000, 0, 3300 * MB, 0);
  CHECK(due.status);
  CHECK_EQ(due.footprint_gb, 3u);
  // and not again for 3 GB after a dip
  schedule.tick(t + 11'250, 0, 2900 * MB, 0);
  due = schedule.tick(t + 11'500, 0, 3100 * MB, 0);
  CHECK_EQ(due.footprint_gb, 0u);
  CHECK(!due.status);
  // two GB passed in one tick: the highest
  due = schedule.tick(t + 11'750, 0, 5400 * MB, 0);
  CHECK_EQ(due.footprint_gb, 5u);
  // a memory warning: at once
  due = schedule.tick(t + 12'000, 1, 5400 * MB, 0);
  CHECK(due.status && due.warning);
  due = schedule.tick(t + 12'250, 1, 5400 * MB, 0);
  CHECK(!due.warning && !due.status);
  // the 10 s go on from the last periodic line
  due = schedule.tick(t + 20'000, 1, 5400 * MB, 0);
  CHECK(due.status && !due.warning);
  // DXMT's total: 1, 2 and 3 GB once each; a jump past two of them gives the higher one
  CHECK_EQ(schedule.tick(t + 20'250, 1, 0, GB - 1).total_gb, 0u);
  CHECK_EQ(schedule.tick(t + 20'500, 1, 0, GB).total_gb, 1u);
  CHECK_EQ(schedule.tick(t + 20'750, 1, 0, GB + MB).total_gb, 0u);
  CHECK_EQ(schedule.tick(t + 21'000, 1, 0, 3 * GB + MB).total_gb, 3u);
  CHECK_EQ(schedule.tick(t + 21'250, 1, 0, 2 * GB).total_gb, 0u);
  CHECK_EQ(schedule.tick(t + 21'500, 1, 0, 9 * GB).total_gb, 0u);
  // the total is looked at on the first tick too
  ReportSchedule early;
  CHECK_EQ(early.tick(t, 0, 0, 2 * GB).total_gb, 2u);
}

void
TestScopes() {
  Ledger &ledger = Global();
  uint64_t before = ledger.liveCount();
  int site_a = 0, site_b = 0;
  uint64_t outside = Add(BufferOf(Kind::Internal, MB));
  uint64_t inside, nested, call;
  {
    Scope scope(11, 0, 0x8 | 0x20, 0, 0, &site_a);
    inside = Add(TextureOf(Kind::RtDs, 2 * MB));
    {
      Scope inner(11, 3, 0, 0x10000, 0, &site_b); // a call made inside another: the outer one wins
      nested = Add(BufferOf(Kind::Buffer, MB));
    }
  }
  {
    Scope scope(11, "Map", &site_b);
    call = Add(BufferOf(Kind::Buffer, MB));
  }
  // what the records say
  std::vector<Info> all = ledger.largest(1000);
  auto find_bytes_site = [&](uint64_t bytes, uint64_t site) -> const Info * {
    for (auto &info : all)
      if (info.bytes == bytes && info.site == site)
        return &info;
    return nullptr;
  };
  const Info *in = find_bytes_site(2 * MB, uint64_t(uintptr_t(&site_a)));
  CHECK(in != nullptr);
  if (in) {
    CHECK_EQ(unsigned(in->api), 11u);
    CHECK_EQ(in->d3d_bind, 0x28u);
  }
  const Info *nest = find_bytes_site(MB, uint64_t(uintptr_t(&site_a)));
  CHECK(nest != nullptr);
  if (nest)
    CHECK_EQ(nest->d3d_usage, 0u);
  const Info *mapped = find_bytes_site(MB, uint64_t(uintptr_t(&site_b)));
  CHECK(mapped != nullptr);
  if (mapped) {
    CHECK(mapped->call && std::strcmp(mapped->call, "Map") == 0);
    CHECK_EQ(mapped->d3d_bind, 0u);
  }
  const Info *out = find_bytes_site(MB, 0);
  CHECK(out != nullptr);
  if (out)
    CHECK_EQ(unsigned(out->api), 0u);
  // a record that already says its call keeps it
  {
    Scope scope(11, 0, 0x40, 0, 0, &site_a);
    Info own = BufferOf(Kind::Internal, 3 * MB);
    own.api = 9;
    own.site = 0x1234;
    uint64_t id = Add(own);
    std::vector<Info> now = ledger.largest(1000);
    bool kept = false;
    for (auto &info : now)
      if (info.bytes == 3 * MB && info.site == 0x1234 && info.api == 9 && info.d3d_bind == 0)
        kept = true;
    CHECK(kept);
    Remove(id);
  }
  CHECK_EQ(ledger.liveCount(), before + 4);

  // an Entry removes its record when it goes, once, and moves without double counting
  {
    Entry entry(Add(BufferOf(Kind::Internal, MB)));
    CHECK_EQ(ledger.liveCount(), before + 5);
    Entry moved(std::move(entry));
    CHECK_EQ(entry.id(), 0u);
    CHECK_EQ(ledger.liveCount(), before + 5);
    Entry assigned;
    assigned = std::move(moved);
    CHECK_EQ(ledger.liveCount(), before + 5);
    assigned = Entry(Add(BufferOf(Kind::Internal, MB))); // the old record goes
    CHECK_EQ(ledger.liveCount(), before + 5);
  }
  CHECK_EQ(ledger.liveCount(), before + 4);
  for (uint64_t id : {outside, inside, nested, call})
    Remove(id);
  CHECK_EQ(ledger.liveCount(), before);
  // MarkReleased on the process's ledger
  int group = 0;
  bool held = false;
  uint64_t id = Global().add(BufferOf(Kind::Buffer, MB), &held, &group, NowMs());
  MarkReleased(&group);
  CHECK_EQ(Global().totals(NowMs(), FakeWaiting).waiting_count, 1u);
  Remove(id);
  CHECK_EQ(Global().totals(NowMs(), FakeWaiting).waiting_count, 0u);
}

/* Spec 05 item 94: the texture total without the lock, the clamps in the status line, the
   groups by format and size, and the site walk past DXMT's own module. */
uint64_t g_walk_seen = 0;
uint64_t
FakeWalk(uint64_t return_address) {
  g_walk_seen = return_address;
  return 0x7000;
}

void
TestItem94() {
  Ledger ledger;
  // only textures (rt/ds and sampled) count toward the texture total
  Info rt = TextureOf(Kind::RtDs, 100 * MB);
  rt.format = 70;
  Info small = TextureOf(Kind::Texture, 2 * MB);
  small.format = 70;
  Info big = TextureOf(Kind::Texture, 300 * MB);
  big.format = 130;
  big.width = 4096;
  big.height = 2048;
  Info mid = TextureOf(Kind::Texture, 40 * MB);
  mid.format = 130;
  mid.width = 2048;
  mid.height = 1024;
  uint64_t id_rt = ledger.add(rt, nullptr, nullptr, 1000);
  uint64_t id_small = ledger.add(small, nullptr, nullptr, 1000);
  uint64_t id_big = ledger.add(big, nullptr, nullptr, 1000);
  uint64_t id_mid = ledger.add(mid, nullptr, nullptr, 1000);
  uint64_t id_buf = ledger.add(BufferOf(Kind::Buffer, 500 * MB), nullptr, nullptr, 1000);
  uint64_t id_stage = ledger.add(BufferOf(Kind::Staging, 64 * MB), nullptr, nullptr, 1000);
  CHECK_EQ(ledger.textureLiveBytes(), 442 * MB);
  ledger.remove(id_rt, 1100);
  ledger.remove(id_buf, 1100);
  CHECK_EQ(ledger.textureLiveBytes(), 342 * MB);

  // the groups: by format, the most bytes first; by size, the larger side
  TextureGroups groups = ledger.textureGroups(8);
  CHECK_EQ(groups.formats.size(), 2u);
  if (groups.formats.size() == 2) {
    CHECK_EQ(groups.formats[0].key, 130u);
    CHECK_EQ(groups.formats[0].count, 2u);
    CHECK_EQ(groups.formats[0].bytes, 340 * MB);
    CHECK_EQ(groups.formats[1].key, 70u);
  }
  CHECK_EQ(groups.sizes[0].count, 1u); // 256x256
  CHECK_EQ(groups.sizes[1].count, 0u);
  CHECK_EQ(groups.sizes[2].count, 1u); // 2048x1024
  CHECK_EQ(groups.sizes[3].count, 1u); // 4096x2048
  CHECK(GroupsLine(groups, FakeFormatName) ==
        "[dxmt-mem] textures by format: format130 2 (340.0 MB), RGBA8Unorm 1 (2.0 MB); by size (larger side): <=256 1 "
        "(2.0 MB), <=1024 0 (0.0 MB), <=2048 1 (40.0 MB), >2048 1 (300.0 MB)");
  CHECK_EQ(ledger.textureGroups(1).formats.size(), 1u);
  CHECK_EQ(SizeClassOf(256, 256), 0u);
  CHECK_EQ(SizeClassOf(257, 16), 1u);
  CHECK_EQ(SizeClassOf(16, 1024), 1u);
  CHECK_EQ(SizeClassOf(2049, 1), 3u);

  // the clamps: counted with what they saved, and in the status line once there is one
  Totals before = ledger.totals(1200, nullptr);
  CHECK_EQ(before.clamp_count, 0u);
  CHECK(!Contains(StatusLine(before, nullptr), "mip clamp"));
  ledger.noteMipClamp(12 * MB);
  ledger.noteMipClamp(30 * MB);
  Totals after = ledger.totals(1200, nullptr);
  CHECK_EQ(after.clamp_count, 2u);
  CHECK_EQ(after.clamp_saved_bytes, 42 * MB);
  CHECK(Contains(StatusLine(after, nullptr), "; mip clamp 2 textures (42 MB saved)"));
  for (uint64_t id : {id_small, id_big, id_mid, id_stage})
    ledger.remove(id, 1300);
  CHECK_EQ(ledger.textureLiveBytes(), 0u);

  // the site walk: the first return address outside the module
  const uint64_t frames[] = {0x10001000, 0x10002000, 0x10003000, 0x00401234, 0x00402000};
  CHECK_EQ(FirstFrameOutside(frames, 5, 0x10000000, 0x10100000, 0x10002000), 0x00401234u);
  CHECK_EQ(FirstFrameOutside(frames, 3, 0x10000000, 0x10100000, 0x10002000), 0x10002000u); // none: as given
  const uint64_t with_null[] = {0, 0x10001000, 0x20000000};
  CHECK_EQ(FirstFrameOutside(with_null, 3, 0x10000000, 0x10100000, 0x1), 0x20000000u); // 0 skipped
  CHECK_EQ(FirstFrameOutside(frames, 0, 0x10000000, 0x10100000, 0x99), 0x99u);
  // an outermost scope takes its site through the walk; without one, as given
  SetSiteWalk(FakeWalk);
  {
    Scope scope(11, 0, 0x8, 0, 0, reinterpret_cast<const void *>(uintptr_t(0x10002000)));
    Info info = TextureOf(Kind::Texture, MB);
    ApplyScope(info);
    CHECK_EQ(info.site, 0x7000u);
    CHECK_EQ(g_walk_seen, 0x10002000u);
  }
  SetSiteWalk(nullptr);
  {
    Scope scope(11, 0, 0x8, 0, 0, reinterpret_cast<const void *>(uintptr_t(0x10002000)));
    Info info = TextureOf(Kind::Texture, MB);
    ApplyScope(info);
    CHECK_EQ(info.site, 0x10002000u);
  }
}

} // namespace

int
main() {
  TestTotalsByKind();
  TestDeferredRelease();
  TestLastTenSeconds();
  TestLargestOrder();
  TestSites();
  TestLines();
  TestSchedule();
  TestScopes();
  TestItem94();
  return TestSummary("test_mem_ledger");
}
