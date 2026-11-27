// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 item 90 (round 20): the memory ledger fed by DXMT's own code against fake Metal: a
// buffer's and a texture's allocations recorded with their kind, size and storage, the record
// gone when the allocation goes; the deferred release seen through the command chunks' references
// (AllocationRefTracking) and through a released resource; ring blocks by ring; the record
// helpers' rules (16 KB pages, storage, render target or sampled, the size the unix side wrote).

#include "test_common.hpp"

#include "dxmt_allocation.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_mem_ledger.hpp"
#include "dxmt_mem_track.hpp"
#include "dxmt_ring_bump_allocator.hpp"
#include "dxmt_texture.hpp"

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

/* ---- fake Metal ------------------------------------------------------------ */

namespace {

struct FakeMetal {
  obj_handle_t next_handle = 0x1000;
  std::map<obj_handle_t, void *> owned_contents;
  std::map<obj_handle_t, unsigned> released;
  uint32_t texture_kb = 0; // what newTexture reports as allocatedSize
  bool bc_supported = true;
};

FakeMetal g_metal;

} // namespace

extern "C" {

obj_handle_t
MTLDevice_newBuffer(obj_handle_t, struct WMTBufferInfo *info) {
  obj_handle_t h = g_metal.next_handle++;
  if (!info->memory.get() && (info->options & WMTResourceStorageModePrivate) == 0) {
    void *mem = std::calloc(1, info->length);
    g_metal.owned_contents[h] = mem;
    info->memory.set(mem);
  }
  info->gpu_address = 0x100000000ull + uint64_t(h) * 0x10000ull;
  return h;
}

obj_handle_t
MTLDevice_newTexture(obj_handle_t, struct WMTTextureInfo *info) {
  info->gpu_resource_id = 0x77;
  info->mach_port = 0;
  info->allocated_kb = g_metal.texture_kb;
  return g_metal.next_handle++;
}

obj_handle_t
MTLDevice_newSharedTexture(obj_handle_t device, struct WMTTextureInfo *info) {
  return MTLDevice_newTexture(device, info);
}

obj_handle_t
MTLBuffer_newTexture(obj_handle_t, struct WMTTextureInfo *info, uint64_t, uint64_t) {
  info->gpu_resource_id = 0x78;
  info->allocated_kb = 0;
  return g_metal.next_handle++;
}

obj_handle_t
MTLTexture_newTextureView(
    obj_handle_t, enum WMTPixelFormat, enum WMTTextureType, uint16_t, uint16_t, uint16_t, uint16_t,
    struct WMTTextureSwizzleChannels, uint64_t *out_gpu_resource_id
) {
  *out_gpu_resource_id = 0x79;
  return g_metal.next_handle++;
}

bool
MTLDevice_supportsBCTextureCompression(obj_handle_t) {
  return g_metal.bc_supported;
}

bool
MTLDevice_supportsFamily(obj_handle_t, enum WMTGPUFamily) {
  return true;
}

void
NSObject_retain(obj_handle_t) {}

void
NSObject_release(obj_handle_t obj) {
  g_metal.released[obj]++;
  auto it = g_metal.owned_contents.find(obj);
  if (it != g_metal.owned_contents.end()) {
    std::free(it->second);
    g_metal.owned_contents.erase(it);
  }
}

/* spec 05 item 91: a shared allocation gives its share key back (none are made here) */
bool
WMTShareRelease(uint32_t) {
  return false;
}

void
MTLResidencySet_addAllocations(obj_handle_t, const obj_handle_t *, uint64_t) {}
void
MTLResidencySet_removeAllocations(obj_handle_t, const obj_handle_t *, uint64_t) {}
void
MTLResidencySet_commit(obj_handle_t) {}

} // extern "C"

namespace dxmt {
uint32_t
MTLGetTexelSize(WMTPixelFormat) {
  return 4;
}
namespace wsi {
void *
aligned_malloc(size_t size, size_t alignment) {
  return std::aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
}
void
aligned_free(void *ptr) {
  std::free(ptr);
}
} // namespace wsi
namespace guest_memory {
void
Write(const std::string &, bool) {}
} // namespace guest_memory
/* the ring's "forced to allocate" warning and its switch (log.cpp, util_env.cpp in the DLL) */
void
Logger::warn(const std::string &) {}
void
Logger::info(const std::string &) {}
namespace env {
std::string
getEnvVar(const char *) {
  return "";
}
} // namespace env
} // namespace dxmt

using namespace dxmt;
namespace ml = dxmt::memledger;

namespace {

constexpr uint64_t MB = 1ull << 20;

bool
AllocationWaiting(const void *owner) {
  return static_cast<const Allocation *>(owner)->waitingForGpu();
}

ml::Totals
Now() {
  return ml::Global().totals(ml::NowMs(), AllocationWaiting);
}

const ml::KindTotal &
KindOf(const ml::Totals &totals, ml::Kind kind) {
  return totals.kind[unsigned(kind)];
}

WMT::Device
FakeDevice() {
  WMT::Device device;
  device.handle = 0x42;
  return device;
}

void
TestRecordHelpers() {
  CHECK_EQ(ml::BufferBytes(1), 16384u);
  CHECK_EQ(ml::BufferBytes(16384), 16384u);
  CHECK_EQ(ml::BufferBytes(16385), 32768u);
  CHECK_EQ(ml::BufferBytes(0), 0u);
  CHECK(ml::StorageOf(WMTResourceStorageModePrivate) == ml::Storage::Private);
  CHECK(ml::StorageOf(WMTResourceStorageModeMemoryless) == ml::Storage::Private);
  CHECK(ml::StorageOf(WMTResourceStorageModeShared) == ml::Storage::Shared);
  CHECK(ml::StorageOf(WMTResourceStorageModeManaged | WMTResourceHazardTrackingModeUntracked) == ml::Storage::Shared);
  CHECK(ml::StorageOf(WMTResourceStorageModePrivate | WMTResourceCPUCacheModeWriteCombined) == ml::Storage::Private);

  ml::Info buffer = ml::BufferRecord(ml::Kind::Staging, 100000, WMTResourceStorageModeShared, "staging");
  CHECK(buffer.kind == ml::Kind::Staging);
  CHECK(!buffer.texture);
  CHECK_EQ(buffer.width, 100000u);
  CHECK_EQ(buffer.bytes, 7u * 16384u);
  CHECK(std::strcmp(buffer.what, "staging") == 0);

  WMTTextureInfo info{};
  info.type = WMTTextureType2DArray;
  info.pixel_format = WMTPixelFormatDepth32Float;
  info.width = 2048;
  info.height = 1024;
  info.depth = 1;
  info.array_length = 4;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = WMTTextureUsage(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
  info.options = WMTResourceStorageModePrivate;
  info.allocated_kb = 32768;
  CHECK_EQ(ml::TextureBytes(info), 32 * MB);
  ml::Info rt = ml::TextureRecord(info, ml::TextureBytes(info), false);
  CHECK(rt.kind == ml::Kind::RtDs);
  CHECK(rt.texture);
  CHECK(rt.storage == ml::Storage::Private);
  CHECK_EQ(rt.array_length, 4u);
  CHECK_EQ(rt.format, uint32_t(WMTPixelFormatDepth32Float));
  CHECK_EQ(unsigned(rt.texture_type), unsigned(WMTTextureType2DArray));
  info.usage = WMTTextureUsageShaderRead;
  info.options = WMTResourceStorageModeShared;
  ml::Info sampled = ml::TextureRecord(info, MB, true);
  CHECK(sampled.kind == ml::Kind::Texture);
  CHECK(sampled.storage == ml::Storage::Shared);
  CHECK(sampled.bc_uncompressed);
}

void
TestBufferAllocations() {
  uint64_t before = ml::Global().liveCount();
  Rc<Buffer> buffer = new Buffer(100000, FakeDevice());
  {
    Rc<BufferAllocation> a = buffer->allocate(BufferAllocationFlag::GpuPrivate);
    CHECK(a->mem_record != 0);
    ml::Totals totals = Now();
    CHECK_EQ(ml::Global().liveCount(), before + 1);
    CHECK_EQ(KindOf(totals, ml::Kind::Buffer).count, 1u);
    CHECK_EQ(KindOf(totals, ml::Kind::Buffer).bytes, 7u * 16384u);
    CHECK_EQ(KindOf(totals, ml::Kind::Buffer).shared_bytes, 0u);
    CHECK_EQ(totals.waiting_count, 0u);

    // a command chunk takes a reference: still held by the buffer, so not waiting
    AllocationRefTracking chunk;
    CHECK(chunk.track(a.ptr()));
    CHECK(!a->waitingForGpu());
    CHECK_EQ(Now().waiting_count, 0u);

    // the program's handle goes (a rename, a discard): only the chunk holds it now
    Allocation *raw = a.ptr();
    a = Rc<BufferAllocation>();
    CHECK(raw->waitingForGpu());
    CHECK_EQ(Now().waiting_count, 1u);
    CHECK_EQ(Now().waiting_bytes, 7u * 16384u);
    CHECK_EQ(ml::Global().liveCount(), before + 1);

    // the GPU finishes the chunk: the allocation and its record go
    chunk.clear();
    CHECK_EQ(ml::Global().liveCount(), before);
    CHECK_EQ(Now().waiting_count, 0u);
  }

  // a shared allocation of several slices counts its whole length
  {
    Rc<BufferAllocation> a = buffer->allocate(BufferAllocationFlag::GpuManaged, 3);
    ml::Totals totals = Now();
    CHECK_EQ(KindOf(totals, ml::Kind::Buffer).shared_bytes, ml::BufferBytes(3 * 100096));
  }
  CHECK_EQ(ml::Global().liveCount(), before);

  // the program releases the resource while a chunk still holds its allocation through the
  // resource (a command holds the Buffer): waiting until the chunk lets go
  {
    Rc<BufferAllocation> a = buffer->allocate(BufferAllocationFlag::GpuPrivate);
    auto old = buffer->rename(std::move(a));
    CHECK(!old.ptr());
    CHECK_EQ(Now().waiting_count, 0u);
    buffer->releasedByProgram();
    CHECK_EQ(Now().waiting_count, 1u);
    old = buffer->rename(Rc<BufferAllocation>()); // the resource itself goes
  }
  CHECK_EQ(ml::Global().liveCount(), before);
  CHECK_EQ(Now().waiting_count, 0u);
}

void
TestTextureAllocations() {
  uint64_t before = ml::Global().liveCount();
  WMTTextureInfo info{};
  info.type = WMTTextureType2D;
  info.pixel_format = WMTPixelFormatDepth32Float_Stencil8;
  info.width = 4096;
  info.height = 4096;
  info.depth = 1;
  info.array_length = 1;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = WMTTextureUsage(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
  Rc<Texture> texture = new Texture(info, FakeDevice());
  g_metal.texture_kb = 81920; // 80 MB, as the unix side would write it back
  {
    Rc<TextureAllocation> a = texture->allocate(TextureAllocationFlag::GpuPrivate);
    CHECK(a->mem_record != 0);
    ml::Totals totals = Now();
    CHECK_EQ(KindOf(totals, ml::Kind::RtDs).count, 1u);
    CHECK_EQ(KindOf(totals, ml::Kind::RtDs).bytes, 80 * MB);
    CHECK_EQ(KindOf(totals, ml::Kind::RtDs).shared_bytes, 0u);
    std::vector<ml::Info> largest = ml::Global().largest(1);
    CHECK(largest.size() == 1 && largest[0].width == 4096 && largest[0].texture);
  }
  CHECK_EQ(ml::Global().liveCount(), before);

  // a sampled BC texture on a device without BC: stored uncompressed
  WMTTextureInfo bc = info;
  bc.pixel_format = WMTPixelFormatBC1_RGBA;
  bc.width = bc.height = 1024;
  bc.usage = WMTTextureUsageShaderRead;
  Rc<Texture> bc_texture = new Texture(bc, FakeDevice());
  g_metal.bc_supported = false; // read once per process, here first
  g_metal.texture_kb = 4096;
  {
    Rc<TextureAllocation> a = bc_texture->allocate(TextureAllocationFlag::GpuPrivate);
    ml::Totals totals = Now();
    CHECK_EQ(KindOf(totals, ml::Kind::Texture).count, 1u);
#ifdef DXMT_IOS
    CHECK_EQ(totals.bc_count, 1u);
    CHECK_EQ(totals.bc_bytes, 4 * MB);
#endif
  }
  CHECK_EQ(ml::Global().liveCount(), before);

  // a linear texture on its own buffer counts the buffer's pages
  WMTTextureInfo linear = info;
  linear.pixel_format = WMTPixelFormatRGBA8Unorm;
  linear.width = 300;
  linear.height = 200;
  linear.usage = WMTTextureUsageShaderRead;
  Rc<Texture> linear_texture = new Texture(300 * 200 * 4, 300 * 4, linear, FakeDevice());
  {
    Rc<TextureAllocation> a = linear_texture->allocate(TextureAllocationFlag::GpuManaged);
    ml::Totals totals = Now();
    CHECK_EQ(KindOf(totals, ml::Kind::Texture).count, 1u);
    CHECK_EQ(KindOf(totals, ml::Kind::Texture).bytes, ml::BufferBytes(300 * 200 * 4));
    CHECK_EQ(KindOf(totals, ml::Kind::Texture).shared_bytes, ml::BufferBytes(300 * 200 * 4));
  }
  CHECK_EQ(ml::Global().liveCount(), before);

  // a released texture: what still lives waits
  {
    texture->rename(texture->allocate(TextureAllocationFlag::GpuPrivate));
    texture->releasedByProgram();
    CHECK_EQ(Now().waiting_count, 1u);
    texture->rename(Rc<TextureAllocation>());
  }
  CHECK_EQ(Now().waiting_count, 0u);
  CHECK_EQ(ml::Global().liveCount(), before);
}

void
TestRingBlocks() {
  uint64_t before = ml::Global().liveCount();
  {
    RingBumpState<StagingBufferBlockAllocator, 0x100000> staging(
        StagingBufferBlockAllocator(FakeDevice(), WMTResourceStorageModeShared, false, "test staging")
    );
    RingBumpState<GpuPrivateBufferBlockAllocator, 0x100000> temp(
        GpuPrivateBufferBlockAllocator(FakeDevice(), WMTResourceStorageModePrivate)
    );
    staging.allocate(1, 0, 64, 16);
    staging.allocate(1, 0, 3 * 0x100000, 16); // larger than a block: a block of its own
    temp.allocate(1, 0, 64, 16);
    ml::Totals totals = Now();
    CHECK_EQ(KindOf(totals, ml::Kind::Staging).count, 3u);
    CHECK_EQ(KindOf(totals, ml::Kind::Staging).bytes, 0x100000u + 3 * 0x100000u + 0x100000u);
    CHECK_EQ(KindOf(totals, ml::Kind::Staging).shared_bytes, 4 * 0x100000u);
    bool named = false, temp_named = false;
    for (auto &info : ml::Global().largest(100)) {
      if (info.kind == ml::Kind::Staging && info.what && std::strcmp(info.what, "test staging") == 0)
        named = true;
      if (info.kind == ml::Kind::Staging && info.what && std::strcmp(info.what, "copy temporaries") == 0)
        temp_named = true;
    }
    CHECK(named);
    CHECK(temp_named);
    // a finished block reused (moved to the back of the ring) keeps one record
    staging.allocate(5, 2, 0x100000 - 64, 16);
    CHECK_EQ(KindOf(Now(), ml::Kind::Staging).count, 3u);
    // the chunks finish long ago: the blocks go, and their records
    staging.free_blocks(~0ull);
    temp.free_blocks(~0ull);
    CHECK_EQ(KindOf(Now(), ml::Kind::Staging).count, 0u);
  }
  CHECK_EQ(ml::Global().liveCount(), before);
}

WMTTextureInfo
DepthInfo(uint32_t size) {
  WMTTextureInfo info{};
  info.type = WMTTextureType2D;
  info.pixel_format = WMTPixelFormatDepth32Float;
  info.width = size;
  info.height = size;
  info.depth = 1;
  info.array_length = 1;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = WMTTextureUsage(WMTTextureUsageRenderTarget | WMTTextureUsageShaderRead);
  return info;
}

/* Round 20's first fix: a depth texture's rename copies are bounded (dxmt_texture.hpp) */
void
TestDepthRenamePool() {
  // the rule alone
  CHECK(RenamePoolMayGrow(0, 1ull << 40, ~0ull)); // the first copy is always made
  CHECK(RenamePoolMayGrow(3, 16 * MB, 0));        // 4 x 16 = 64 MB
  CHECK(!RenamePoolMayGrow(4, 16 * MB, 0));       // 5 x 16 > 64 MB
  CHECK(!RenamePoolMayGrow(1, 33 * MB, 0));       // 2 x 33 > 64 MB: no second copy
  CHECK(RenamePoolMayGrow(1, MB, kRenameExtraBytes - MB));
  CHECK(!RenamePoolMayGrow(1, MB, kRenameExtraBytes - MB + 1));

  uint64_t before = ml::Global().liveCount();
  CHECK_EQ(RenamableTexturePool::extraBytesInUse(), 0u);
  Flags<TextureAllocationFlag> flags;
  flags.set(TextureAllocationFlag::GpuPrivate);

  // an 80 MB shadow-map-sized depth texture: no second copy; every clear takes the one it has
  {
    Rc<Texture> texture = new Texture(DepthInfo(4096), FakeDevice());
    g_metal.texture_kb = 80 * 1024;
    Rc<RenamableTexturePool> pool = new RenamableTexturePool(texture.ptr(), 32, flags);
    Rc<TextureAllocation> first = pool->getNext(0);
    for (unsigned i = 0; i < 20; i++)
      CHECK(pool->getNext(7).ptr() == first.ptr());
    CHECK_EQ(pool->copiesMade(), 1u);
    CHECK_EQ(RenamableTexturePool::extraBytesInUse(), 0u);
    CHECK_EQ(KindOf(Now(), ml::Kind::RtDs).count, 1u);
  }
  CHECK_EQ(ml::Global().liveCount(), before);

  // a 15 MB screen-sized one: 4 copies (60 MB), then the clears take them in turn; a new chunk
  // starts again from the first
  Rc<Texture> screen = new Texture(DepthInfo(1984), FakeDevice());
  g_metal.texture_kb = 15 * 1024;
  Rc<RenamableTexturePool> screen_pool = new RenamableTexturePool(screen.ptr(), 32, flags);
  std::vector<TextureAllocation *> seen;
  seen.push_back(screen_pool->getNext(0).ptr());
  for (unsigned i = 1; i < 10; i++)
    seen.push_back(screen_pool->getNext(0).ptr());
  CHECK_EQ(screen_pool->copiesMade(), 4u);
  CHECK(seen[4] == seen[0] && seen[5] == seen[1] && seen[8] == seen[0]);
  CHECK(seen[0] != seen[1] && seen[1] != seen[2] && seen[2] != seen[3]);
  CHECK(screen_pool->getNext(9).ptr() == seen[0]);
  CHECK_EQ(RenamableTexturePool::extraBytesInUse(), 3 * 15 * MB);
  CHECK_EQ(KindOf(Now(), ml::Kind::RtDs).count, 4u);
  CHECK_EQ(KindOf(Now(), ml::Kind::RtDs).bytes, 60 * MB);

  // 8 MB ones: 8 copies each (64 MB) while the 256 MB of extra copies last
  g_metal.texture_kb = 8 * 1024;
  std::vector<Rc<Texture>> textures;
  std::vector<Rc<RenamableTexturePool>> pools;
  for (unsigned p = 0; p < 5; p++) {
    textures.push_back(new Texture(DepthInfo(1024), FakeDevice()));
    pools.push_back(new RenamableTexturePool(textures.back().ptr(), 32, flags));
    for (unsigned i = 0; i < 32; i++)
      pools.back()->getNext(100 + p);
  }
  // 45 MB in use by the screen's: 3 x 56, then 5 of 8 MB fit the 43 left, then none
  CHECK_EQ(pools[0]->copiesMade(), 8u);
  CHECK_EQ(pools[1]->copiesMade(), 8u);
  CHECK_EQ(pools[2]->copiesMade(), 8u);
  CHECK_EQ(pools[3]->copiesMade(), 6u);
  CHECK_EQ(pools[4]->copiesMade(), 1u);
  CHECK(RenamableTexturePool::extraBytesInUse() <= kRenameExtraBytes);
  CHECK_EQ(RenamableTexturePool::extraBytesInUse(), 45 * MB + 3 * 56 * MB + 40 * MB);

  // a pool that goes gives its share back, and another may grow into it
  screen_pool = Rc<RenamableTexturePool>();
  screen = Rc<Texture>();
  CHECK_EQ(RenamableTexturePool::extraBytesInUse(), 3 * 56 * MB + 40 * MB);
  for (unsigned i = 0; i < 32; i++)
    pools[4]->getNext(200);
  CHECK_EQ(pools[4]->copiesMade(), 7u); // 48 MB left: 6 more copies of 8 MB
  pools.clear();
  textures.clear();
  CHECK_EQ(RenamableTexturePool::extraBytesInUse(), 0u);
  CHECK_EQ(ml::Global().liveCount(), before);
}

/* Round 20's second fix: a ring block made for one large request goes once the GPU has finished
   with it, even behind a block kept for reuse (dxmt_ring_bump_allocator.hpp) */
void
TestRingOneOffBlocks() {
  uint64_t before = ml::Global().liveCount();
  auto staging_blocks = [] { return KindOf(Now(), ml::Kind::Staging).count; };
  {
    RingBumpState<StagingBufferBlockAllocator, 0x100000> ring(
        StagingBufferBlockAllocator(FakeDevice(), WMTResourceStorageModeShared, false, "test staging")
    );
    ring.allocate(1, 0, 64, 16);           // the ring's own 1 MB block
    ring.allocate(1, 0, 3 * 0x100000, 16); // one-off blocks of 3 MB
    ring.allocate(2, 0, 3 * 0x100000, 16);
    ring.allocate(3, 0, 3 * 0x100000, 16);
    CHECK_EQ(staging_blocks(), 4u);
    // chunks 1 and 2 finished: their one-off blocks go, though the 1 MB block (finished, kept
    // for reuse) is in front of them; chunk 3's stays
    ring.free_blocks(2);
    CHECK_EQ(staging_blocks(), 2u);
    ring.free_blocks(3);
    CHECK_EQ(staging_blocks(), 1u);
    // the kept block is still the ring's: a small request reuses it, no new block
    ring.allocate(4, 3, 64, 16);
    CHECK_EQ(staging_blocks(), 1u);
    // a one-off block the GPU still uses stays, also when it is the latest
    ring.allocate(5, 3, 2 * 0x100000, 16);
    ring.free_blocks(4);
    CHECK_EQ(staging_blocks(), 2u);
    ring.free_blocks(5);
    CHECK_EQ(staging_blocks(), 1u);
    // and the ring goes on: a request after it suballocates as before
    ring.allocate(6, 5, 64, 16);
    CHECK_EQ(staging_blocks(), 1u);
  }
  CHECK_EQ(ml::Global().liveCount(), before);
}

} // namespace

int
main() {
  TestRecordHelpers();
  TestBufferAllocations();
  TestTextureAllocations();
  TestRingBlocks();
  TestDepthRenamePool();
  TestRingOneOffBlocks();
  return TestSummary("test_mem_track");
}
