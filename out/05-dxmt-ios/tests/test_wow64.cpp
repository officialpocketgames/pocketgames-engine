// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 Part F: host tests for the 32-bit helpers of the unix half
// (dxmt/src/winemetal/unix/ios_wow64.h, ios_wow64_objects.h): pointer
// conversion (C1, C2), the command-chain walker (C4) and the record table of
// Part E. Plain C++20 with a fake window base; no Metal, no Wine.
//
//   c++ -std=c++20 -I../dxmt/src/winemetal -I../dxmt/src/winemetal/unix test_wow64.cpp -o test_wow64

#include "test_common.hpp"

#define WINEMETAL_API extern "C"
#include "winemetal.h"
#include "ios_wow64.h"
#include "ios_wow64_objects.h"

#include <cstdlib>
#include <map>
#include <vector>

namespace {

constexpr uint64_t kBase = 0x0000000700000000ull; // a fake window base above 4 GB

uint64_t
Field(uint64_t value) {
  return value;
}

/* ---- C1 / C2 -------------------------------------------------------------- */

void
TestPointerToHost() {
  uint64_t f = Field(0);
  CHECK(dxmt_wow_ptr_to_host(&f, kBase));
  CHECK_EQ(f, 0); // NULL stays NULL

  f = Field(0x00401000);
  CHECK(dxmt_wow_ptr_to_host(&f, kBase));
  CHECK_EQ(f, kBase + 0x00401000); // a normal value

  f = Field(0xFFFFFFFFull);
  CHECK(dxmt_wow_ptr_to_host(&f, kBase));
  CHECK_EQ(f, kBase + 0xFFFFFFFFull); // the window's last byte

  f = Field(0x100000000ull); // high half set: not a guest address
  CHECK(!dxmt_wow_ptr_to_host(&f, kBase));
  CHECK_EQ(f, 0x100000000ull); // left alone

  // WMTMemoryPointer fields convert the same way.
  WMTMemoryPointer p;
  p.ptr = (void *)(uintptr_t)0x1234;
  CHECK(dxmt_wow_ptr_to_host(&p, kBase));
  CHECK_EQ((uint64_t)(uintptr_t)p.ptr, kBase + 0x1234);
}

void
TestPointerToGuest() {
  bool inside = false;
  CHECK_EQ(dxmt_wow_ptr_to_guest(0, kBase, &inside), 0); // NULL stays 0
  CHECK(inside);

  CHECK_EQ(dxmt_wow_ptr_to_guest(kBase + 0x00401000, kBase, &inside), 0x00401000);
  CHECK(inside);

  CHECK_EQ(dxmt_wow_ptr_to_guest(kBase + 0xFFFFFFFFull, kBase, &inside), 0xFFFFFFFFull); // last byte
  CHECK(inside);

  // Host pointers outside the window become NULL and are reported.
  CHECK_EQ(dxmt_wow_ptr_to_guest(kBase + 0x100000000ull, kBase, &inside), 0); // just past the end
  CHECK(!inside);
  CHECK_EQ(dxmt_wow_ptr_to_guest(kBase - 1, kBase, &inside), 0); // just below
  CHECK(!inside);
  CHECK_EQ(dxmt_wow_ptr_to_guest(0x0000000280000000ull, kBase, &inside), 0); // Metal-allocated memory elsewhere
  CHECK(!inside);
}

/* ---- C4: command chains ------------------------------------------------------ */

/* Records live in a host arena; their guest addresses are (address - base),
   with base chosen so the arena starts at guest 0x10000. */
struct Arena {
  std::vector<uint8_t> storage;
  uint64_t base = 0;

  explicit Arena(size_t records) : storage(records * 128 + 64) {
    base = (uint64_t)(uintptr_t)storage.data() - 0x10000;
  }

  template <typename T>
  T *
  at(size_t index) {
    return reinterpret_cast<T *>(storage.data() + 64 + index * 128);
  }

  uint64_t
  guest(const void *p) const {
    return (uint64_t)(uintptr_t)p - base;
  }
};

void
Link(Arena &a, wmtcmd_base *from, const void *to) {
  dxmt_wow_store(&from->next, to ? a.guest(to) : 0);
}

wmtcmd_base *
Rec(Arena &a, size_t i, uint16_t type = WMTRenderCommandNop) {
  auto *r = a.at<wmtcmd_base>(i);
  std::memset(r, 0, 128);
  r->type = type;
  return r;
}

void
TestChainEmpty() {
  Arena a(1);
  uint64_t head = 0;
  CHECK_EQ(dxmt_wow_chain_to_host(&head, a.base, dxmt_wow_render_payload, 16), 0);
  CHECK_EQ(head, 0);
  dxmt_wow_chain_to_guest(&head, a.base, dxmt_wow_render_payload, 0);
  CHECK_EQ(head, 0);
}

void
TestChainOne() {
  Arena a(1);
  auto *r = reinterpret_cast<wmtcmd_render_setbytes *>(Rec(a, 0, WMTRenderCommandSetFragmentBytes));
  uint64_t bytes_guest = 0x00500000;
  dxmt_wow_store(&r->bytes, bytes_guest);
  uint64_t head = a.guest(r);

  CHECK_EQ(dxmt_wow_chain_to_host(&head, a.base, dxmt_wow_render_payload, 16), 1);
  CHECK_EQ(head, (uint64_t)(uintptr_t)r);
  CHECK_EQ(dxmt_wow_load(&r->bytes), a.base + bytes_guest); // payload converted
  CHECK_EQ(dxmt_wow_load(&r->next), 0);

  dxmt_wow_chain_to_guest(&head, a.base, dxmt_wow_render_payload, 1);
  CHECK_EQ(head, a.guest(r));
  CHECK_EQ(dxmt_wow_load(&r->bytes), bytes_guest); // converted back
  CHECK_EQ(dxmt_wow_load(&r->next), 0);
}

void
TestChainMany() {
  const size_t n = 50;
  Arena a(n);
  std::vector<wmtcmd_base *> recs;
  for (size_t i = 0; i < n; i++) {
    // Every third record carries a viewport array, the others are draws.
    recs.push_back(Rec(a, i, i % 3 == 0 ? WMTRenderCommandSetViewports : WMTRenderCommandDraw));
    if (i % 3 == 0)
      dxmt_wow_store(&reinterpret_cast<wmtcmd_render_setviewports *>(recs[i])->viewports, 0x00600000 + i * 0x100);
  }
  for (size_t i = 0; i + 1 < n; i++)
    Link(a, recs[i], recs[i + 1]);
  uint64_t head = a.guest(recs[0]);

  CHECK_EQ(dxmt_wow_chain_to_host(&head, a.base, dxmt_wow_render_payload, 1000), (long)n);
  // Walk the converted chain as the 64-bit handler does.
  size_t walked = 0;
  for (auto *c = (wmtcmd_base *)(uintptr_t)head; c; c = (wmtcmd_base *)(uintptr_t)dxmt_wow_load(&c->next)) {
    CHECK(c == recs[walked]);
    if (walked % 3 == 0)
      CHECK_EQ(
          dxmt_wow_load(&reinterpret_cast<wmtcmd_render_setviewports *>(c)->viewports),
          a.base + 0x00600000 + walked * 0x100
      );
    walked++;
  }
  CHECK_EQ(walked, n);

  dxmt_wow_chain_to_guest(&head, a.base, dxmt_wow_render_payload, (long)n);
  CHECK_EQ(head, a.guest(recs[0]));
  for (size_t i = 0; i < n; i++) {
    CHECK_EQ(dxmt_wow_load(&recs[i]->next), i + 1 < n ? a.guest(recs[i + 1]) : 0);
    if (i % 3 == 0)
      CHECK_EQ(dxmt_wow_load(&reinterpret_cast<wmtcmd_render_setviewports *>(recs[i])->viewports), 0x00600000 + i * 0x100);
  }

  // Compute chains convert SetBytes; blit chains have no payload.
  Arena b(2);
  auto *bytes = reinterpret_cast<wmtcmd_compute_setbytes *>(Rec(b, 0, WMTComputeCommandSetBytes));
  dxmt_wow_store(&bytes->bytes, 0x700);
  auto *dispatch = Rec(b, 1, WMTComputeCommandDispatch);
  Link(b, (wmtcmd_base *)bytes, dispatch);
  uint64_t chead = b.guest(bytes);
  CHECK_EQ(dxmt_wow_chain_to_host(&chead, b.base, dxmt_wow_compute_payload, 16), 2);
  CHECK_EQ(dxmt_wow_load(&bytes->bytes), b.base + 0x700);
  dxmt_wow_chain_to_guest(&chead, b.base, dxmt_wow_compute_payload, 2);
  CHECK_EQ(dxmt_wow_load(&bytes->bytes), 0x700);
}

/* Snapshot of every record's bytes, to check that a refused chain is left
   exactly as it was. */
std::vector<uint8_t>
Snapshot(const Arena &a) {
  return a.storage;
}

void
TestChainCycle() {
  Arena a(4);
  if (a.base <= DXMT_WOW_GUEST_MAX) {
    // The walker relies on windows above 4 GB (always true on iOS).
    std::printf("note: arena below 4 GB on this host; cycle test skipped\n");
    return;
  }
  auto *r0 = Rec(a, 0, WMTRenderCommandDraw);
  auto *r1 = reinterpret_cast<wmtcmd_render_setbytes *>(Rec(a, 1, WMTRenderCommandSetFragmentBytes));
  auto *r2 = Rec(a, 2, WMTRenderCommandDraw);
  dxmt_wow_store(&r1->bytes, 0x900);
  Link(a, r0, r1);
  Link(a, (wmtcmd_base *)r1, r2);
  Link(a, r2, r0); // cycle back to the head
  uint64_t head = a.guest(r0);
  auto before = Snapshot(a);
  uint64_t head_before = head;

  CHECK_EQ(dxmt_wow_chain_to_host(&head, a.base, dxmt_wow_render_payload, 1000), -1);
  CHECK_EQ(head, head_before);
  CHECK(Snapshot(a) == before); // everything converted back

  // A record pointing at itself.
  Arena s(1);
  if (s.base > DXMT_WOW_GUEST_MAX) {
    auto *self = Rec(s, 0, WMTRenderCommandDraw);
    Link(s, self, self);
    uint64_t shead = s.guest(self);
    auto sbefore = Snapshot(s);
    CHECK_EQ(dxmt_wow_chain_to_host(&shead, s.base, dxmt_wow_render_payload, 1000), -1);
    CHECK_EQ(shead, s.guest(self));
    CHECK(Snapshot(s) == sbefore);
  }
}

void
TestChainTooLong() {
  const size_t n = 10;
  Arena a(n);
  std::vector<wmtcmd_base *> recs;
  for (size_t i = 0; i < n; i++)
    recs.push_back(Rec(a, i, WMTRenderCommandDraw));
  for (size_t i = 0; i + 1 < n; i++)
    Link(a, recs[i], recs[i + 1]);
  uint64_t head = a.guest(recs[0]);
  auto before = Snapshot(a);

  CHECK_EQ(dxmt_wow_chain_to_host(&head, a.base, dxmt_wow_render_payload, 9), -1); // one too many
  CHECK_EQ(head, a.guest(recs[0]));
  CHECK(Snapshot(a) == before);

  CHECK_EQ(dxmt_wow_chain_to_host(&head, a.base, dxmt_wow_render_payload, 10), 10); // exactly the maximum
  dxmt_wow_chain_to_guest(&head, a.base, dxmt_wow_render_payload, 10);
  CHECK(Snapshot(a) == before);
}

void
TestChainBadLinks() {
  Arena a(3);
  auto *r0 = Rec(a, 0, WMTRenderCommandDraw);
  auto *r1 = reinterpret_cast<wmtcmd_render_setscissorrects *>(Rec(a, 1, WMTRenderCommandSetScissorRects));
  Link(a, r0, r1);
  dxmt_wow_store(&r1->scissor_rects, 0x1FFFFFFFFull); // high half set
  uint64_t head = a.guest(r0);
  auto before = Snapshot(a);
  CHECK_EQ(dxmt_wow_chain_to_host(&head, a.base, dxmt_wow_render_payload, 16), -1);
  CHECK_EQ(head, a.guest(r0));
  CHECK(Snapshot(a) == before);

  uint64_t bad_head = 0x100000010ull;
  CHECK_EQ(dxmt_wow_chain_to_host(&bad_head, a.base, nullptr, 16), -1);
  CHECK_EQ(bad_head, 0x100000010ull);
}

/* ---- Part E: record table ------------------------------------------------------- */

/* Fake objects: a reference count per address; 0 means freed. */
std::map<uint64_t, uint64_t> g_refs;

void
FakeRetain(uint64_t o) {
  g_refs[o]++;
}

void
FakeRelease(uint64_t o) {
  CHECK(g_refs[o] > 0); // never released twice
  if (g_refs[o] > 0)
    g_refs[o]--;
}

uint64_t
FakeRetainCount(uint64_t o) {
  return g_refs[o];
}

/* What the unix half does around a program's calls. */
uint64_t
Create(dxmt_wow_objects &t, uint64_t object, uint64_t base) {
  g_refs[object] = 1; // the program's reference from creation
  dxmt_wow_objects_created(&t, object, base);
  return object;
}

void
ProgramRetain(dxmt_wow_objects &t, uint64_t o) {
  dxmt_wow_objects_retained(&t, o);
  FakeRetain(o);
}

void
ProgramRelease(dxmt_wow_objects &t, uint64_t o) {
  bool last = dxmt_wow_objects_released(&t, o);
  FakeRelease(o);
  if (last)
    FakeRelease(o);
}

uint32_t
Teardown(dxmt_wow_objects &t, uint64_t base) {
  dxmt_wow_record *records = nullptr;
  uint32_t n = dxmt_wow_objects_take_window(&t, base, &records);
  dxmt_wow_objects_give_back(&t, records, n);
  std::free(records);
  return n;
}

struct Table {
  std::vector<dxmt_wow_record> slots;
  dxmt_wow_objects t;
  explicit Table(uint32_t capacity) : slots(capacity) {
    dxmt_wow_objects_init(&t, slots.data(), capacity, FakeRetain, FakeRelease, FakeRetainCount);
  }
};

void
TestRecordCreateRetainRelease() {
  g_refs.clear();
  Table tb(64);
  const uint64_t B = 0x700000000ull;
  uint64_t tex = Create(tb.t, 0x10000, B);
  CHECK_EQ(g_refs[tex], 2); // program + table
  CHECK(dxmt_wow_objects_find(&tb.t, tex) >= 0);

  ProgramRetain(tb.t, tex);
  CHECK_EQ(g_refs[tex], 3);
  ProgramRelease(tb.t, tex);
  CHECK_EQ(g_refs[tex], 2);
  ProgramRelease(tb.t, tex); // the program's last reference: the table lets go too
  CHECK_EQ(g_refs[tex], 0);
  CHECK(dxmt_wow_objects_find(&tb.t, tex) < 0);
  CHECK_EQ(tb.t.used, 0);
}

void
TestRecordTeardownWithReferencesLeft() {
  g_refs.clear();
  Table tb(64);
  const uint64_t B = 0x700000000ull, Other = 0x900000000ull;
  uint64_t a = Create(tb.t, 0x10000, B);
  uint64_t b = Create(tb.t, 0x20000, B);
  uint64_t c = Create(tb.t, 0x30000, Other); // another program's
  ProgramRetain(tb.t, a);
  ProgramRetain(tb.t, a); // a: program holds 3

  CHECK_EQ(Teardown(tb.t, B), 2);
  CHECK_EQ(g_refs[a], 0);
  CHECK_EQ(g_refs[b], 0);
  CHECK_EQ(g_refs[c], 2); // untouched
  CHECK(dxmt_wow_objects_find(&tb.t, c) >= 0);
  CHECK_EQ(Teardown(tb.t, B), 0); // nothing left for that window
}

void
TestRecordTeardownAfterProgramReleasedEverything() {
  g_refs.clear();
  Table tb(64);
  const uint64_t B = 0x700000000ull;
  uint64_t a = Create(tb.t, 0x10000, B);
  ProgramRelease(tb.t, a);
  CHECK_EQ(g_refs[a], 0);
  CHECK_EQ(Teardown(tb.t, B), 0);
  CHECK_EQ(g_refs[a], 0); // not released twice (FakeRelease checks)
}

void
TestRecordUncountedRelease() {
  g_refs.clear();
  Table tb(64);
  const uint64_t B = 0x700000000ull;
  uint64_t a = Create(tb.t, 0x10000, B);
  ProgramRetain(tb.t, a); // count 2, refs 3
  FakeRelease(a);         // released through a path the count didn't see: refs 2
  // Teardown gives back the program's references only while more than the
  // table's own is left, then the table's own: exactly 2 releases.
  CHECK_EQ(Teardown(tb.t, B), 1);
  CHECK_EQ(g_refs[a], 0);
}

void
TestRecordAddressReused() {
  g_refs.clear();
  Table tb(64);
  const uint64_t B = 0x700000000ull, B2 = 0x800000000ull;
  uint64_t a = Create(tb.t, 0x10000, B);
  // The object was freed through uncounted paths (both references), and the
  // address comes back from a new creation by another program.
  g_refs[a] = 0;
  uint64_t again = Create(tb.t, 0x10000, B2);
  CHECK_EQ(again, a);
  CHECK_EQ(g_refs[again], 2); // fresh record: program + table
  CHECK_EQ(tb.t.used, 1);
  CHECK_EQ(Teardown(tb.t, B), 0); // the stale record is gone
  CHECK_EQ(Teardown(tb.t, B2), 1);
  CHECK_EQ(g_refs[again], 0);
}

void
TestRecordTableFullAndCollisions() {
  g_refs.clear();
  Table tb(8); // keeps one slot free: 7 records at most
  const uint64_t B = 0x700000000ull;
  for (uint64_t i = 0; i < 7; i++)
    CHECK_EQ(dxmt_wow_objects_created(&tb.t, 0x1000 * (i + 1), B), DXMT_WOW_RECORDED);
  CHECK_EQ(dxmt_wow_objects_created(&tb.t, 0x9000, B), DXMT_WOW_TABLE_FULL);
  CHECK_EQ(g_refs[0x9000], 0); // not recorded, no reference taken
  // Every recorded object is still found after removals in the middle
  // (backward-shift deletion keeps probe chains intact).
  CHECK(dxmt_wow_objects_released(&tb.t, 0x3000));
  CHECK(dxmt_wow_objects_released(&tb.t, 0x5000));
  for (uint64_t i : {1, 2, 4, 6, 7})
    CHECK(dxmt_wow_objects_find(&tb.t, 0x1000 * i) >= 0);
  CHECK(dxmt_wow_objects_find(&tb.t, 0x3000) < 0);
  CHECK_EQ(tb.t.used, 5);
  dxmt_wow_record *records = nullptr;
  CHECK_EQ(dxmt_wow_objects_take_window(&tb.t, B, &records), 5);
  std::free(records);
  CHECK_EQ(tb.t.used, 0);
}

} // namespace

int
main() {
  TestPointerToHost();
  TestPointerToGuest();
  TestChainEmpty();
  TestChainOne();
  TestChainMany();
  TestChainCycle();
  TestChainTooLong();
  TestChainBadLinks();
  TestRecordCreateRetainRelease();
  TestRecordTeardownWithReferencesLeft();
  TestRecordTeardownAfterProgramReleasedEverything();
  TestRecordUncountedRelease();
  TestRecordAddressReused();
  TestRecordTableFullAndCollisions();
  return TestSummary("test_wow64");
}
