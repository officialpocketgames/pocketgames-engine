// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 item 91: shared textures and fences inside the app's one process, the Windows half.
// DXMT's own texture code (dxmt_texture.cpp: allocate, import, the allocation's release) against
// fake Metal whose MTLDevice_newSharedTexture does what winemetal_unix.c does on iOS, over the real
// share table (winemetal/unix/dxmt_share_table.h):
// - create shared, then open by key: the same Metal object, its description given back;
// - open after the creator's release while the opener holds it; the last release frees the entry;
// - a missing key is refused (no assert) and a mismatched description is found (DescriptionDiff);
// - a full table still makes the texture, as not shareable (key 0, nothing to give back);
// - a shared fence opens as the same event and signals through it; a texture's key isn't a fence.
// Also the share name, the lines and their 10 s counts (d3d11_share.hpp, d3d11_share.cpp with the
// logger faked) and the keyed mutex with a fake texture (dxgi_keyed_mutex.hpp).

#include "test_common.hpp"

#include "d3d11_share.hpp"
#include "dxgi_keyed_mutex.hpp"
#include "dxmt_texture.hpp"
#include "log/log.hpp"

extern "C" {
#include "dxmt_share_table.h"
}

#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

/* ---- fake Metal, with the iOS unix side's share table --------------------------------------- */

namespace {

struct FakeObject {
  int refs = 0;
  WMTTextureInfo info{};  // a texture's description
  uint64_t value = 0;     // an event's signaled value
};

std::map<obj_handle_t, FakeObject> g_objects;
obj_handle_t g_next = 0x1000;
dxmt_share_table g_table;
unsigned g_share_releases = 0;

void
TableRetain(uint64_t object) {
  g_objects[object].refs++;
}

void
TableRelease(uint64_t object) {
  g_objects[object].refs--;
}

obj_handle_t
NewObject() {
  obj_handle_t h = g_next++;
  g_objects[h].refs = 1;
  return h;
}

int
Refs(obj_handle_t h) {
  auto it = g_objects.find(h);
  return it == g_objects.end() ? -1 : it->second.refs;
}

} // namespace

extern "C" {

obj_handle_t
MTLDevice_newBuffer(obj_handle_t, struct WMTBufferInfo *info) {
  obj_handle_t h = NewObject();
  if (!info->memory.get())
    info->memory.set(std::calloc(1, info->length)); // never freed: a few bytes in a test
  return h;
}

obj_handle_t
MTLDevice_newTexture(obj_handle_t, struct WMTTextureInfo *info) {
  info->gpu_resource_id = 0x77;
  info->mach_port = 0;
  info->allocated_kb = 64;
  obj_handle_t h = NewObject();
  g_objects[h].info = *info;
  return h;
}

/* winemetal_unix.c's iOS _MTLDevice_newSharedTexture: key 0 makes and registers a texture (the
   key comes back, 0 when the table is full); a key opens the registered one (+1, one more user,
   its description written back, the key kept); null for a key no texture has */
obj_handle_t
MTLDevice_newSharedTexture(obj_handle_t device, struct WMTTextureInfo *info) {
  if (info->mach_port) {
    uint64_t object = 0;
    if (dxmt_share_open(&g_table, DXMT_SHARE_TEXTURE, info->mach_port, &object) != DXMT_SHARE_FOUND)
      return 0;
    const uint32_t key = info->mach_port;
    *info = g_objects[object].info;
    info->mach_port = key;
    info->gpu_resource_id = 0x88;
    return object;
  }
  obj_handle_t h = MTLDevice_newTexture(device, info);
  info->mach_port = dxmt_share_add(&g_table, DXMT_SHARE_TEXTURE, h);
  return h;
}

obj_handle_t
MTLBuffer_newTexture(obj_handle_t, struct WMTTextureInfo *info, uint64_t, uint64_t) {
  info->gpu_resource_id = 0x78;
  info->allocated_kb = 0;
  return NewObject();
}

obj_handle_t
MTLTexture_newTextureView(
    obj_handle_t, enum WMTPixelFormat, enum WMTTextureType, uint16_t, uint16_t, uint16_t, uint16_t,
    struct WMTTextureSwizzleChannels, uint64_t *out_gpu_resource_id
) {
  *out_gpu_resource_id = 0x79;
  return NewObject();
}

bool
MTLDevice_supportsBCTextureCompression(obj_handle_t) {
  return true;
}

bool
MTLDevice_supportsFamily(obj_handle_t, enum WMTGPUFamily) {
  return true;
}

void
NSObject_retain(obj_handle_t obj) {
  g_objects[obj].refs++;
}

void
NSObject_release(obj_handle_t obj) {
  g_objects[obj].refs--;
}

void
MTLResidencySet_addAllocations(obj_handle_t, const obj_handle_t *, uint64_t) {}
void
MTLResidencySet_removeAllocations(obj_handle_t, const obj_handle_t *, uint64_t) {}
void
MTLResidencySet_commit(obj_handle_t) {}

/* the shared event and the control ops 11-13 (winemetal_unix.c's _DXMTIOS_control on iOS) */
obj_handle_t
MTLDevice_newSharedEvent(obj_handle_t) {
  return NewObject();
}

void
MTLSharedEvent_signalValue(obj_handle_t event, uint64_t value) {
  g_objects[event].value = value;
}

uint64_t
MTLSharedEvent_signaledValue(obj_handle_t event) {
  return g_objects[event].value;
}

uint32_t
WMTShareEvent(obj_handle_t event) {
  return event ? dxmt_share_add(&g_table, DXMT_SHARE_EVENT, event) : 0;
}

obj_handle_t
WMTOpenSharedEvent(uint32_t key, bool *wrong_kind) {
  uint64_t object = 0;
  dxmt_share_open_result result = dxmt_share_open(&g_table, DXMT_SHARE_EVENT, key, &object);
  if (wrong_kind)
    *wrong_kind = result == DXMT_SHARE_WRONG_KIND;
  return object;
}

bool
WMTShareRelease(uint32_t key) {
  g_share_releases++;
  return dxmt_share_release(&g_table, key);
}

} // extern "C"

/* what the DLL's other files give DXMT's texture code, and the logger d3d11_share.cpp writes to */
namespace {
std::vector<std::string> g_warn_lines, g_info_lines;
}

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
void
Logger::warn(const std::string &line) {
  g_warn_lines.push_back(line);
}
void
Logger::info(const std::string &line) {
  g_info_lines.push_back(line);
}
namespace env {
std::string
getEnvVar(const char *) {
  return "";
}
} // namespace env
} // namespace dxmt

using namespace dxmt;

namespace {

WMT::Device
FakeDevice() {
  WMT::Device device;
  device.handle = 0x42;
  return device;
}

WMTTextureInfo
VideoInfo(uint32_t width = 1280, uint32_t height = 720) {
  WMTTextureInfo info{};
  info.type = WMTTextureType2D;
  info.pixel_format = WMTPixelFormatBGRA8Unorm;
  info.width = width;
  info.height = height;
  info.depth = 1;
  info.array_length = 1;
  info.mipmap_level_count = 1;
  info.sample_count = 1;
  info.usage = WMTTextureUsageShaderRead;
  return info;
}

Flags<TextureAllocationFlag>
SharedFlags() {
  Flags<TextureAllocationFlag> flags;
  flags.set(TextureAllocationFlag::GpuPrivate);
  flags.set(TextureAllocationFlag::Shared);
  return flags;
}

void
ResetTable(uint32_t capacity) {
  dxmt_share_init(&g_table, capacity, TableRetain, TableRelease);
  g_share_releases = 0;
}

/* Create shared, open by key, release in either order (the creator's first here). */
void
TestCreateOpenRelease() {
  ResetTable(DXMT_SHARE_TABLE_CAPACITY);
  Rc<Texture> creator_texture = new Texture(VideoInfo(), FakeDevice());
  Rc<TextureAllocation> created = creator_texture->allocate(SharedFlags());
  const uint32_t key = created->machPort;
  const obj_handle_t object = created->texture().handle;
  CHECK(key != 0);
  CHECK(object != 0);
  CHECK_EQ(dxmt_share_users(&g_table, key), 1u);
  CHECK_EQ(Refs(object), 2); // the creator's and the table's

  // the opener: the same Metal object, the shared texture's own description
  Rc<Texture> opener_texture = new Texture(VideoInfo(), FakeDevice());
  WMTTextureInfo found{};
  Rc<TextureAllocation> opened = opener_texture->import(key, &found);
  CHECK(opened.ptr() != nullptr);
  if (!opened.ptr())
    return;
  CHECK_EQ(opened->texture().handle, object);
  CHECK_EQ(opened->machPort, key);
  CHECK_EQ(opened->gpuResourceID, 0x88u);
  CHECK_EQ(found.width, 1280u);
  CHECK_EQ(found.height, 720u);
  CHECK(share::DescriptionDiff(share::DescOf(VideoInfo()), share::DescOf(found)).empty());
  CHECK_EQ(dxmt_share_users(&g_table, key), 2u);
  CHECK_EQ(Refs(object), 3);

  // the creator goes; the opener still has it, and another open still finds it
  created = Rc<TextureAllocation>();
  CHECK_EQ(g_share_releases, 1u);
  CHECK_EQ(dxmt_share_users(&g_table, key), 1u);
  CHECK_EQ(Refs(object), 2);
  Rc<Texture> late_texture = new Texture(VideoInfo(), FakeDevice());
  Rc<TextureAllocation> late = late_texture->import(key);
  CHECK(late.ptr() && late->texture().handle == object);
  CHECK_EQ(dxmt_share_users(&g_table, key), 2u);

  // the last two go: the entry is freed and nothing holds the texture
  late = Rc<TextureAllocation>();
  opened = Rc<TextureAllocation>();
  CHECK(dxmt_share_find(&g_table, key) == nullptr);
  CHECK_EQ(g_table.used, 0u);
  CHECK_EQ(Refs(object), 0);

  // now the key is missing
  Rc<TextureAllocation> gone = late_texture->import(key);
  CHECK(gone.ptr() == nullptr);
}

/* The opener goes first, and a missing key and a mismatched description are refused. */
void
TestRefusals() {
  ResetTable(DXMT_SHARE_TABLE_CAPACITY);
  Rc<Texture> creator_texture = new Texture(VideoInfo(), FakeDevice());
  Rc<TextureAllocation> created = creator_texture->allocate(SharedFlags());
  const uint32_t key = created->machPort;
  const obj_handle_t object = created->texture().handle;

  // a key no texture has: null, no assert, nothing counted
  Rc<Texture> other = new Texture(VideoInfo(), FakeDevice());
  WMTTextureInfo found{};
  CHECK(other->import(key + 100, &found).ptr() == nullptr);
  CHECK_EQ(dxmt_share_users(&g_table, key), 1u);

  // an opener asking for another size: ImportSharedTextureInternal refuses with E_INVALIDARG and
  // drops the allocation, which gives its use of the entry back
  {
    Rc<Texture> wrong_texture = new Texture(VideoInfo(640, 360), FakeDevice());
    Rc<TextureAllocation> wrong = wrong_texture->import(key, &found);
    CHECK(wrong.ptr() != nullptr);
    CHECK_EQ(dxmt_share_users(&g_table, key), 2u);
    CHECK(
        share::DescriptionDiff(share::DescOf(VideoInfo(640, 360)), share::DescOf(found)) ==
        "width 640 vs 1280, height 360 vs 720"
    );
  }
  CHECK_EQ(dxmt_share_users(&g_table, key), 1u);

  // an opener released before the creator
  {
    Rc<Texture> opener = new Texture(VideoInfo(), FakeDevice());
    Rc<TextureAllocation> opened = opener->import(key);
    CHECK(opened.ptr() && opened->texture().handle == object);
  }
  CHECK_EQ(dxmt_share_users(&g_table, key), 1u);
  created = Rc<TextureAllocation>();
  CHECK_EQ(g_table.used, 0u);
  CHECK_EQ(Refs(object), 0);
}

/* A full table: the texture is still made, as not shareable (key 0), and nothing is given back. */
void
TestFullTable() {
  ResetTable(2);
  Rc<Texture> a = new Texture(VideoInfo(), FakeDevice());
  Rc<Texture> b = new Texture(VideoInfo(), FakeDevice());
  Rc<Texture> c = new Texture(VideoInfo(), FakeDevice());
  Rc<TextureAllocation> first = a->allocate(SharedFlags());
  Rc<TextureAllocation> second = b->allocate(SharedFlags());
  Rc<TextureAllocation> third = c->allocate(SharedFlags());
  CHECK(first->machPort != 0 && second->machPort != 0);
  CHECK_EQ(third->machPort, 0u); // CreateSharedDeviceTextureIOS: "not shareable (the share table is full)"
  CHECK(third->texture().handle != 0);
  const obj_handle_t object = third->texture().handle;
  CHECK_EQ(Refs(object), 1); // the creator's only: the table took none
  third = Rc<TextureAllocation>();
  CHECK_EQ(g_share_releases, 0u);
  CHECK_EQ(Refs(object), 0);
  first = Rc<TextureAllocation>();
  second = Rc<TextureAllocation>();
  CHECK_EQ(g_share_releases, 2u);
  CHECK_EQ(g_table.used, 0u);
}

/* A shared fence: the opener gets the same event, a signal through either is seen by the other;
   a texture's key isn't a fence. */
void
TestFence() {
  ResetTable(DXMT_SHARE_TABLE_CAPACITY);
  auto event = FakeDevice().newSharedEvent();
  const uint32_t key = WMTShareEvent(event.handle);
  CHECK(key != 0);
  CHECK_EQ(Refs(event.handle), 2);

  bool wrong_kind = true;
  obj_handle_t opened_handle = WMTOpenSharedEvent(key, &wrong_kind);
  CHECK_EQ(opened_handle, event.handle);
  CHECK(!wrong_kind);
  {
    WMT::Reference<WMT::SharedEvent> opened(opened_handle); // adopts the opener's +1
    CHECK_EQ(Refs(event.handle), 3);
    event.signalValue(7);
    CHECK_EQ(opened.signaledValue(), 7u);
    opened.signalValue(9); // the opener's Signal
    CHECK_EQ(event.signaledValue(), 9u);
    WMTShareRelease(key); // the opener's fence goes
  }
  CHECK_EQ(dxmt_share_users(&g_table, key), 1u);

  // a texture's key opened as a fence
  Rc<Texture> texture = new Texture(VideoInfo(), FakeDevice());
  Rc<TextureAllocation> shared = texture->allocate(SharedFlags());
  CHECK(WMTOpenSharedEvent(shared->machPort, &wrong_kind) == 0);
  CHECK(wrong_kind);
  CHECK(WMTOpenSharedEvent(0xabcdef, &wrong_kind) == 0);
  CHECK(!wrong_kind);

  WMTShareRelease(key); // the creator's fence goes
  CHECK(dxmt_share_find(&g_table, key) == nullptr);
  const obj_handle_t handle = event.handle;
  event = WMT::Reference<WMT::SharedEvent>();
  CHECK_EQ(Refs(handle), 0);
}

void
TestNames() {
  char name[share::kNameSize];
  share::ShareName(0x2a, name);
  CHECK(std::string(name) == "DXMT_ios_share_0000002a");
  uint32_t key = 0;
  CHECK(share::ParseShareName(name, sizeof(name), &key));
  CHECK_EQ(key, 0x2au);
  share::ShareName(0xffffffffu, name);
  CHECK(share::ParseShareName(name, sizeof(name), &key) && key == 0xffffffffu);

  // a macOS name, key 0, upper case, a short buffer, no terminator: not ours
  char mac[share::kNameSize] = "DXMT_shared_resource_0123456789abcdef0123456789abcdef";
  CHECK(!share::ParseShareName(mac, sizeof(mac), &key));
  CHECK_EQ(key, 0u);
  char zero[share::kNameSize] = "DXMT_ios_share_00000000";
  CHECK(!share::ParseShareName(zero, sizeof(zero), &key));
  char upper[share::kNameSize] = "DXMT_ios_share_0000002A";
  CHECK(!share::ParseShareName(upper, sizeof(upper), &key));
  char longer[share::kNameSize] = "DXMT_ios_share_0000002a0";
  CHECK(!share::ParseShareName(longer, sizeof(longer), &key));
  share::ShareName(5, name);
  CHECK(!share::ParseShareName(name, 10, &key));
  char unterminated[24];
  std::memcpy(unterminated, "DXMT_ios_share_0000002a!", 24);
  CHECK(!share::ParseShareName(unterminated, sizeof(unterminated), &key));
  CHECK(!share::ParseShareName(nullptr, 54, &key));
}

void
TestLines() {
  CHECK(
      share::TextureLine(1280, 720, "B8G8R8A8_UNORM", 0x2, 0x28, "handle 0x40000042, share key 3") ==
      "[d3d11] shared texture 1280x720 B8G8R8A8_UNORM misc 0x2 bind 0x28 -> handle 0x40000042, share key 3"
  );
  CHECK(
      share::OpenLine("texture", "handle 0x40000042", "found", "share key 3") ==
      "[d3d11] open shared handle 0x40000042: found (share key 3)"
  );
  CHECK(
      share::OpenLine("texture", "handle 0x40000042", "mismatch", "width 640 vs 1280") ==
      "[d3d11] open shared handle 0x40000042: mismatch (width 640 vs 1280)"
  );
  CHECK(
      share::OpenLine("fence", "NT handle 0x1c", "missing", "") == "[d3d11] open shared fence NT handle 0x1c: missing"
  );
  CHECK(
      share::KmtLine("D3DKMTCreateAllocation2", 0, "share key 3, handle 0x40000042") ==
      "[d3d11] D3DKMTCreateAllocation2 -> 0x00000000; share key 3, handle 0x40000042"
  );
  CHECK(share::KmtLine("D3DKMTEscape", 0xC000000Du, "") == "[d3d11] D3DKMTEscape -> 0xc000000d");

  share::Desc a, b;
  a.type = b.type = 2;
  CHECK(share::DescriptionDiff(a, b).empty());
  b.format = 80;
  b.samples = 4;
  b.mip_levels = 3;
  b.array_length = 6;
  b.depth = 2;
  CHECK(
      share::DescriptionDiff(a, b) == "format 0 vs 80, depth 0 vs 2, array 0 vs 6, levels 0 vs 3, samples 0 vs 4"
  );
}

/* Each kind's first line at once, the rest counted; every 10 s one line per kind that came again. */
void
TestShareLog() {
  share::ShareLog log;
  auto out = log.note("shared texture", false, "[d3d11] shared texture 1x1 X -> a", 1000);
  CHECK_EQ(out.size(), 1u);
  if (out.size() == 1)
    CHECK(!out[0].warn && out[0].line == "[d3d11] shared texture 1x1 X -> a");
  CHECK(!log.pending());
  // repeats from 2 s: counted, the last kept, nothing until 12 s
  CHECK(log.note("shared texture", false, "[d3d11] shared texture 1x1 X -> b", 2000).empty());
  CHECK(log.note("shared texture", false, "[d3d11] shared texture 1x1 X -> c", 3000).empty());
  CHECK(log.pending());
  // another kind's first line still comes at once, and is a warning
  out = log.note("open shared missing", true, "[d3d11] open shared handle 0x4: missing", 4000);
  CHECK_EQ(out.size(), 1u);
  if (out.size() == 1)
    CHECK(out[0].warn);
  CHECK(log.tick(11999).empty());
  out = log.tick(12000);
  CHECK_EQ(out.size(), 1u);
  if (out.size() == 1)
    CHECK(out[0].line == "[d3d11] shared texture: 2 more (last: shared texture 1x1 X -> c)");
  CHECK(!log.pending());
  CHECK(log.tick(30000).empty());
  // a repeat that comes with a due count gets both at once (the note's own line is counted)
  CHECK(log.note("open shared missing", true, "[d3d11] open shared handle 0x5: missing", 40000).empty());
  out = log.note("open shared missing", true, "[d3d11] open shared handle 0x6: missing", 50000);
  CHECK_EQ(out.size(), 1u);
  if (out.size() == 1)
    CHECK(out[0].warn && out[0].line == "[d3d11] open shared missing: 2 more (last: open shared handle 0x6: missing)");
  // at most 64 kinds; one more is dropped
  share::ShareLog many;
  unsigned first_lines = 0;
  for (unsigned i = 0; i < share::kMaxKinds + 5; i++)
    first_lines += unsigned(many.note("kind " + std::to_string(i), false, "line", 1).size());
  CHECK_EQ(first_lines, share::kMaxKinds);
}

/* The process's log through the logger (d3d11_share.cpp). */
void
TestNoteThroughLogger() {
  g_warn_lines.clear();
  g_info_lines.clear();
  share::Note("test found", false, "[d3d11] open shared handle 0x8: found");
  share::Note("test missing", true, "[d3d11] open shared handle 0x9: missing");
  share::Note("test missing", true, "[d3d11] open shared handle 0xa: missing"); // counted
  share::Tick();                                                              // not due yet
  CHECK_EQ(g_info_lines.size(), 1u);
  CHECK_EQ(g_warn_lines.size(), 1u);
  if (g_warn_lines.size() == 1)
    CHECK(g_warn_lines[0] == "[d3d11] open shared handle 0x9: missing");
}

/* ---- the keyed mutex with a fake texture ---------------------------------------------------- */

struct FakeKeyedTexture {
  ULONG refs = 1;
  unsigned flushes = 0;
  unsigned queries = 0;
  unsigned device_queries = 0;
  unsigned private_data = 0;

  HRESULT
  QueryInterface(REFIID, void **object) {
    queries++;
    *object = this;
    return S_OK;
  }
  ULONG
  AddRef() {
    return ++refs;
  }
  ULONG
  Release() {
    return --refs;
  }
  HRESULT
  SetPrivateData(REFGUID, UINT, const void *) {
    private_data++;
    return S_OK;
  }
  HRESULT
  SetPrivateDataInterface(REFGUID, const IUnknown *) {
    private_data++;
    return S_OK;
  }
  HRESULT
  GetPrivateData(REFGUID, UINT *, void *) {
    private_data++;
    return S_OK;
  }
  HRESULT
  GetDeviceInterface(REFIID, void **device) {
    device_queries++;
    *device = nullptr;
    return S_OK;
  }
  void
  FlushForKeyedMutex() {
    flushes++;
  }
};

void
TestKeyedMutex() {
  FakeKeyedTexture texture;
  MTLDXGIKeyedMutex<FakeKeyedTexture> mutex(&texture);
  IDXGIKeyedMutex *com = &mutex;

  // the texture's references and interfaces
  CHECK_EQ(com->AddRef(), 2u);
  CHECK_EQ(com->Release(), 1u);
  GUID any{};
  void *object = nullptr;
  CHECK(com->QueryInterface(any, &object) == S_OK);
  CHECK(object == &texture);
  CHECK(com->GetDevice(any, &object) == S_OK);
  CHECK(com->GetParent(any, &object) == S_OK);
  CHECK_EQ(texture.device_queries, 2u);
  UINT size = 0;
  CHECK(com->SetPrivateData(any, 0, nullptr) == S_OK);
  CHECK(com->GetPrivateData(any, &size, nullptr) == S_OK);
  CHECK_EQ(texture.private_data, 2u);

  // AcquireSync keeps the key and succeeds at once, whatever the wait; ReleaseSync keeps the key,
  // flushes once and succeeds
  CHECK(com->AcquireSync(0, 0xFFFFFFFFu) == S_OK);
  CHECK_EQ(mutex.state().key(), 0u);
  CHECK_EQ(texture.flushes, 0u);
  CHECK(com->ReleaseSync(1) == S_OK);
  CHECK_EQ(mutex.state().key(), 1u);
  CHECK_EQ(texture.flushes, 1u);
  CHECK(com->AcquireSync(1, 0) == S_OK);
  CHECK(com->ReleaseSync(0) == S_OK);
  CHECK_EQ(mutex.state().key(), 0u);
  CHECK_EQ(mutex.state().acquires(), 2u);
  CHECK_EQ(mutex.state().releases(), 2u);
  CHECK_EQ(texture.flushes, 2u);
  CHECK_EQ(texture.refs, 1u);
}

} // namespace

int
main() {
  TestCreateOpenRelease();
  TestRefusals();
  TestFullTable();
  TestFence();
  TestNames();
  TestLines();
  TestShareLog();
  TestNoteThroughLogger();
  TestKeyedMutex();
  return TestSummary("test_share");
}
