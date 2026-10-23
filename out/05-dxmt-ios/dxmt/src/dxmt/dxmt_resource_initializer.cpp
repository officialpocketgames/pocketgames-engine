// Changed for MYIOSDECK (spec 05 C, D3, D8; spec 06 A). Upstream: DXMT a3a6e8f, MIT (see LICENSE).
#include "Metal.hpp"
#include "dxmt_resource_initializer.hpp"
#include "dxmt_format.hpp"
#include "util_math.hpp"
#include <cstdint>
#include <cstdlib>
#include <mutex>
#ifdef DXMT_IOS
#include "dxmt_bc_emulation.hpp"
#include "util_bitness.hpp"
#include "util_env.hpp"
#include "util_ios_switch.hpp"
#include "util_log_rare.hpp"
#endif

namespace dxmt {

#define ALLOC_BLIT(type, cmd)                                                                                          \
  type *cmd = nullptr;                                                                                                 \
  if (!allocateBlit(&cmd)) {                                                                                           \
    flushInternal();                                                                                                   \
    continue;                                                                                                          \
  }

#define ALLOC_CLEAR(info)                                                                                              \
  WMTRenderPassInfo *info;                                                                                             \
  if (!allocateClear(&info)) {                                                                                         \
    flushInternal();                                                                                                   \
    continue;                                                                                                          \
  }

#define ALLOC_GPU(buffer, size)                                                                                        \
  WMT::Buffer buffer;                                                                                                  \
  size_t buffer##_offset;                                                                                              \
  if (!(buffer = allocateGpuHeap(size, buffer##_offset))) {                                                            \
    flushInternal();                                                                                                   \
    continue;                                                                                                          \
  }

#define ALLOC_ZERO(buffer, size)                                                                                       \
  WMT::Buffer buffer;                                                                                                  \
  if (!(buffer = allocateZeroBuffer(size))) {                                                                          \
    flushInternal();                                                                                                   \
    continue;                                                                                                          \
  }

#define RETAIN(allocation)                                                                                             \
  if (!retainAllocation(allocation)) {                                                                                 \
    flushInternal();                                                                                                   \
    continue;                                                                                                          \
  }

ResourceInitializer::ResourceInitializer(WMT::Device device) :
    device_(device),
    gpu_command_heap_allocator(StagingBufferBlockAllocator(
        device, WMTResourceStorageModeManaged | WMTResourceHazardTrackingModeUntracked, false
    )) {
  upload_queue_ = device.newCommandQueue(kResourceInitializerChunks);
  upload_queue_event_ = device.newSharedEvent();

  cpu_command_heap_size = kResourceInitializerCpuCommandHeapSize;
  cpu_command_heap = malloc(cpu_command_heap_size);
  reset();
}

ResourceInitializer::~ResourceInitializer() {
  free(cpu_command_heap);
}

uint64_t
ResourceInitializer::initWithZero(BufferAllocation *buffer, uint64_t offset, uint64_t length) {
  std::lock_guard<dxmt::mutex> lock(mutex_);

  do {
    RETAIN(buffer);
    ALLOC_BLIT(wmtcmd_blit_fillbuffer, fill);
    fill->type = WMTBlitCommandFillBuffer;
    fill->buffer = buffer->buffer();
    fill->offset = offset;
    fill->length = length;
    fill->value = 0;

  } while (0);

  return current_seq_id_;
}

uint64_t
ResourceInitializer::initDepthStencilWithZero(
    const Texture *texture, TextureAllocation *allocation, uint32_t slice, uint32_t level, uint32_t dsv_planar
) {
  auto width_sub = std::max(1u, texture->width() >> level);
  auto height_sub = std::max(1u, texture->height() >> level);

  std::lock_guard<dxmt::mutex> lock(mutex_);
  do {
    RETAIN(allocation);
    ALLOC_CLEAR(info);

    info->render_target_array_length = 0;
    info->render_target_width = width_sub;
    info->render_target_height = height_sub;
    if (dsv_planar & 1) {
      info->depth.texture = allocation->texture();
      info->depth.clear_depth = 0;
      info->depth.load_action = WMTLoadActionClear;
      info->depth.store_action = WMTStoreActionStore;
      info->depth.slice = slice;
      info->depth.level = level;
    }
    if (dsv_planar & 2) {
      info->stencil.texture = allocation->texture();
      info->stencil.clear_stencil = 0;
      info->stencil.load_action = WMTLoadActionClear;
      info->stencil.store_action = WMTStoreActionStore;
      info->stencil.slice = slice;
      info->stencil.level = level;
    }

  } while (0);

  return current_seq_id_;
}

uint64_t
ResourceInitializer::initRenderTargetWithZero(
    const Texture *texture, TextureAllocation *allocation, uint32_t slice, uint32_t level
) {
  auto width_sub = std::max(1u, texture->width() >> level);
  auto height_sub = std::max(1u, texture->height() >> level);

  std::lock_guard<dxmt::mutex> lock(mutex_);
  do {
    RETAIN(allocation);
    ALLOC_CLEAR(info);

    info->render_target_array_length = texture->textureType() == WMTTextureType3D ? texture->depth() : 0;
    info->render_target_width = width_sub;
    info->render_target_height = height_sub;
    info->colors[0].texture = allocation->texture();
    info->colors[0].load_action = WMTLoadActionClear;
    info->colors[0].clear_color = {0, 0, 0, 0};
    info->colors[0].store_action = WMTStoreActionStore;
    info->colors[0].slice = slice;
    info->colors[0].level = level;

  } while (0);

  return current_seq_id_;
}

uint64_t
ResourceInitializer::initWithZero(
    const Texture *texture, TextureAllocation *allocation, uint32_t slice, uint32_t level
) {
  if (auto dsv_planar = DepthStencilPlanarFlags(texture->pixelFormat())) {
    return initDepthStencilWithZero(texture, allocation, slice, level, dsv_planar);
  }
  if (texture->usage() & WMTTextureUsageRenderTarget) {
    return initRenderTargetWithZero(texture, allocation, slice, level);
  }
#ifdef DXMT_IOS
  if (IsEmulatedBCFormat(device_, texture->pixelFormat()))
    return initEmulatedBCWithZero(texture, allocation, slice, level);
#endif

  auto width_sub = std::max(1u, texture->width() >> level);
  auto height_sub = std::max(1u, texture->height() >> level);
  auto depth_sub = std::max(1u, texture->depth() >> level);

  auto block_size = 1u;

  switch (texture->pixelFormat()) {
  case WMTPixelFormatBC1_RGBA:
  case WMTPixelFormatBC1_RGBA_sRGB:
  case WMTPixelFormatBC2_RGBA:
  case WMTPixelFormatBC2_RGBA_sRGB:
  case WMTPixelFormatBC3_RGBA:
  case WMTPixelFormatBC3_RGBA_sRGB:
  case WMTPixelFormatBC4_RSnorm:
  case WMTPixelFormatBC4_RUnorm:
  case WMTPixelFormatBC5_RGUnorm:
  case WMTPixelFormatBC5_RGSnorm:
  case WMTPixelFormatBC6H_RGBUfloat:
  case WMTPixelFormatBC6H_RGBFloat:
  case WMTPixelFormatBC7_RGBAUnorm:
  case WMTPixelFormatBC7_RGBAUnorm_sRGB:
    block_size = 4u;
    break;
  default:
    break;
  }

  bool is_3d_tex = texture->textureType() == WMTTextureType3D;
  size_t texel_size = MTLGetTexelSize(texture->pixelFormat());
  size_t bytes_per_row_needed = texel_size * align(width_sub, block_size) / block_size;
  size_t bytes_per_image_needed = bytes_per_row_needed * align(height_sub, block_size) / block_size;
  size_t total_bytes_needed = bytes_per_image_needed * depth_sub;

  std::lock_guard<dxmt::mutex> lock(mutex_);

  do {
    ALLOC_ZERO(zero, total_bytes_needed);
    RETAIN(allocation);
    ALLOC_BLIT(wmtcmd_blit_copy_from_buffer_to_texture, copy);

    copy->type = WMTBlitCommandCopyFromBufferToTexture;
    copy->src = zero;
    copy->src_offset = 0;
    copy->bytes_per_row = bytes_per_row_needed;
    copy->bytes_per_image = is_3d_tex ? bytes_per_image_needed : 0;
    copy->size = {width_sub, height_sub, depth_sub};
    copy->dst = allocation->texture();
    copy->slice = slice;
    copy->level = level;
    copy->origin = {0, 0, 0};

  } while (0);

  return current_seq_id_;
}

uint64_t
ResourceInitializer::initWithData(
    const Texture *texture, TextureAllocation *allocation, uint32_t slice, uint32_t level, const void *data,
    size_t row_pitch, size_t depth_pitch
) {
#ifdef DXMT_IOS
  if (IsEmulatedBCFormat(device_, texture->pixelFormat()))
    return initEmulatedBCWithData(texture, allocation, slice, level, data, row_pitch, depth_pitch);
#endif
  auto width_sub = std::max(1u, texture->width() >> level);
  auto height_sub = std::max(1u, texture->height() >> level);
  auto depth_sub = std::max(1u, texture->depth() >> level);

  auto block_size = 1u;

  switch (texture->pixelFormat()) {
  case WMTPixelFormatBC1_RGBA:
  case WMTPixelFormatBC1_RGBA_sRGB:
  case WMTPixelFormatBC2_RGBA:
  case WMTPixelFormatBC2_RGBA_sRGB:
  case WMTPixelFormatBC3_RGBA:
  case WMTPixelFormatBC3_RGBA_sRGB:
  case WMTPixelFormatBC4_RSnorm:
  case WMTPixelFormatBC4_RUnorm:
  case WMTPixelFormatBC5_RGUnorm:
  case WMTPixelFormatBC5_RGSnorm:
  case WMTPixelFormatBC6H_RGBUfloat:
  case WMTPixelFormatBC6H_RGBFloat:
  case WMTPixelFormatBC7_RGBAUnorm:
  case WMTPixelFormatBC7_RGBAUnorm_sRGB:
    block_size = 4u;
    break;
  default:
    break;
  }

  bool is_1d_tex = (texture->textureType() == WMTTextureType1D) || (texture->textureType() == WMTTextureType1DArray);
  bool is_3d_tex = texture->textureType() == WMTTextureType3D;
  size_t texel_size = MTLGetTexelSize(texture->pixelFormat());
  size_t bytes_per_row_needed = texel_size * align(width_sub, block_size) / block_size;
  size_t bytes_per_row_increment = is_1d_tex ? bytes_per_row_needed : row_pitch;
  size_t bytes_per_row_valid = is_1d_tex ? bytes_per_row_needed : std::min(row_pitch, bytes_per_row_needed);
  size_t bytes_per_image_needed = bytes_per_row_needed * align(height_sub, block_size) / block_size;
  size_t bytes_per_image_increment = is_3d_tex ? depth_pitch : bytes_per_image_needed;
  size_t bytes_per_image_valid = is_3d_tex ? std::min(depth_pitch, bytes_per_image_needed) : bytes_per_image_needed;
  size_t total_bytes_needed = bytes_per_image_needed * depth_sub;

  std::lock_guard<dxmt::mutex> lock(mutex_);
#ifdef DXMT_IOS
  flushIfUploadBudgetReached(total_bytes_needed);
#endif
  do {
    RETAIN(allocation);
    ALLOC_BLIT(wmtcmd_blit_copy_from_buffer_to_texture, copy);
    ALLOC_GPU(temp, total_bytes_needed);
#ifdef DXMT_IOS
    upload_bytes_since_flush_ += total_bytes_needed;
#endif

    for (auto depth = 0u; depth < depth_sub; depth++) {
      if (bytes_per_row_increment != bytes_per_row_needed) {
        for (auto row = 0u; row < (align(height_sub, block_size) / block_size); row++) {
          auto offset = temp_offset + depth * bytes_per_image_needed + row * bytes_per_row_needed;
          auto length = bytes_per_row_valid;
          auto src_data = ptr_add(data, depth * bytes_per_image_increment + row * bytes_per_row_increment);
          temp.updateContents(offset, src_data, length);
        }
      } else {
        auto offset = temp_offset + depth * bytes_per_image_needed;
        auto length = bytes_per_image_valid;
        auto src_data = ptr_add(data, depth * bytes_per_image_increment);
        temp.updateContents(offset, src_data, length);
      }
    }

    copy->type = WMTBlitCommandCopyFromBufferToTexture;
    copy->src = temp;
    copy->src_offset = temp_offset;
    copy->bytes_per_row = bytes_per_row_needed;
    copy->bytes_per_image = is_3d_tex ? bytes_per_image_needed : 0;
    copy->size = {width_sub, height_sub, depth_sub};
    copy->dst = allocation->texture();
    copy->slice = slice;
    copy->level = level;
    copy->origin = {0, 0, 0};

  } while (0);

  return current_seq_id_;
}

#ifdef DXMT_IOS

void
ResourceInitializer::flushIfUploadBudgetReached(size_t next_upload_bytes) {
  constexpr size_t kUploadFlushThreshold = 64ull << 20;
  if (upload_bytes_since_flush_ && upload_bytes_since_flush_ + next_upload_bytes > kUploadFlushThreshold)
    flushInternal(); // resets upload_bytes_since_flush_
}

uint64_t
ResourceInitializer::uploadPhysical(
    const Texture *texture, TextureAllocation *allocation, uint32_t slice, uint32_t level, const void *texels,
    size_t row_bytes, uint32_t width, uint32_t height, uint32_t depth
) {
  bool is_3d_tex = texture->textureType() == WMTTextureType3D;
  size_t bytes_per_image = row_bytes * height;
  size_t total_bytes = bytes_per_image * depth;

  std::lock_guard<dxmt::mutex> lock(mutex_);
  flushIfUploadBudgetReached(total_bytes);
  /* Unlike the upstream paths (do { } while (0)), a failed allocation flushes
     and retries once, so the decoded upload isn't lost. */
  for (unsigned attempt = 0; attempt < 2; attempt++) {
    RETAIN(allocation);
    ALLOC_BLIT(wmtcmd_blit_copy_from_buffer_to_texture, copy);
    ALLOC_GPU(temp, total_bytes);
    upload_bytes_since_flush_ += total_bytes;

    temp.updateContents(temp_offset, texels, total_bytes);

    copy->type = WMTBlitCommandCopyFromBufferToTexture;
    copy->src = temp;
    copy->src_offset = temp_offset;
    copy->bytes_per_row = row_bytes;
    copy->bytes_per_image = is_3d_tex ? bytes_per_image : 0;
    copy->size = {width, height, depth};
    copy->dst = allocation->texture();
    copy->slice = slice;
    copy->level = level;
    copy->origin = {0, 0, 0};
    break;
  }
  return current_seq_id_;
}

uint64_t
ResourceInitializer::initEmulatedBCWithData(
    const Texture *texture, TextureAllocation *allocation, uint32_t slice, uint32_t level, const void *data,
    size_t row_pitch, size_t depth_pitch
) {
  WMTPixelFormat format = texture->pixelFormat();
  /* Spec 05 clarification 3: a 3D texture is decoded slice by slice (each
     depth slice an independent 2D BC image, depth_pitch apart). */
  bool is_3d_tex = texture->textureType() == WMTTextureType3D;
  uint32_t width = std::max(1u, texture->width() >> level);
  uint32_t height = std::max(1u, texture->height() >> level);
  uint32_t depth = is_3d_tex ? std::max(1u, texture->depth() >> level) : 1u;
  size_t row_bytes = size_t(width) * BCPhysicalBytesPerTexel(format);
  void *decoded = malloc(row_bytes * height * depth);
  if (!decoded) {
    WARN_RARE("BC emulation: no memory to decode ", width, "x", height, "x", depth, " initial data, upload skipped");
    std::lock_guard<dxmt::mutex> lock(mutex_);
    return current_seq_id_;
  }
  if (!BCDecodeVolumeToPhysical(format, data, row_pitch, depth_pitch, decoded, width, height, depth)) {
    WARN_RARE(
        "BC emulation: initial data with row pitch ", row_pitch, " / depth pitch ", depth_pitch,
        " can't be decoded, upload skipped"
    );
    free(decoded);
    std::lock_guard<dxmt::mutex> lock(mutex_);
    return current_seq_id_;
  }
  auto seq = uploadPhysical(texture, allocation, slice, level, decoded, row_bytes, width, height, depth);
  free(decoded);
  return seq;
}

uint64_t
ResourceInitializer::initEmulatedBCWithZero(
    const Texture *texture, TextureAllocation *allocation, uint32_t slice, uint32_t level
) {
  WMTPixelFormat format = texture->pixelFormat();
  bool is_3d_tex = texture->textureType() == WMTTextureType3D;
  uint32_t width = std::max(1u, texture->width() >> level);
  uint32_t height = std::max(1u, texture->height() >> level);
  uint32_t depth = is_3d_tex ? std::max(1u, texture->depth() >> level) : 1u;
  size_t row_bytes = size_t(width) * BCPhysicalBytesPerTexel(format);
  size_t bytes_per_image = row_bytes * height;
  size_t total_bytes = bytes_per_image * depth;

  if (format == WMTPixelFormatBC6H_RGBFloat || format == WMTPixelFormatBC6H_RGBUfloat) {
    /* BC6H isn't decoded; its texture starts opaque grey, not black. */
    void *grey = malloc(total_bytes);
    if (!grey) {
      WARN_RARE("BC emulation: no memory for the BC6H fill, texture left uninitialised");
      std::lock_guard<dxmt::mutex> lock(mutex_);
      return current_seq_id_;
    }
    BCDecodeToPhysical(format, nullptr, 0, grey, row_bytes, width, height * depth);
    auto seq = uploadPhysical(texture, allocation, slice, level, grey, row_bytes, width, height, depth);
    free(grey);
    return seq;
  }

  /* The zero buffer and pitches are sized for the physical format, or Metal
     would read past the zero buffer. */
  std::lock_guard<dxmt::mutex> lock(mutex_);
  do {
    ALLOC_ZERO(zero, total_bytes);
    RETAIN(allocation);
    ALLOC_BLIT(wmtcmd_blit_copy_from_buffer_to_texture, copy);

    copy->type = WMTBlitCommandCopyFromBufferToTexture;
    copy->src = zero;
    copy->src_offset = 0;
    copy->bytes_per_row = row_bytes;
    copy->bytes_per_image = is_3d_tex ? bytes_per_image : 0;
    copy->size = {width, height, depth};
    copy->dst = allocation->texture();
    copy->slice = slice;
    copy->level = level;
    copy->origin = {0, 0, 0};
  } while (0);
  return current_seq_id_;
}

#endif

std::uint64_t
ResourceInitializer::flushInternal() {
  auto pool = WMT::MakeAutoreleasePool();

  auto seq_id = current_seq_id_++;
  auto cmdbuf = upload_queue_.commandBuffer();
  encode(cmdbuf);
  cmdbuf.encodeSignalEvent(upload_queue_event_, seq_id);
  cmdbuf.commit();
  reset();
  cached_coherent_seq_id = upload_queue_event_.signaledValue();
  gpu_command_heap_allocator.free_blocks(cached_coherent_seq_id);
  return seq_id;
}

uint64_t
ResourceInitializer::flushToWait() {
  std::lock_guard<dxmt::mutex> lock(mutex_);

  if (idle()) {
    gpu_command_heap_allocator.free_blocks(cached_coherent_seq_id);
    if (cached_coherent_seq_id == current_seq_id_ - 1)
      return 0;
    cached_coherent_seq_id = upload_queue_event_.signaledValue();
    if (cached_coherent_seq_id == current_seq_id_ - 1)
      return 0;
    return current_seq_id_ - 1;
  }

  return flushInternal();
}

void
ResourceInitializer::reset() {
  cpu_command_heap_offset = 0;
#ifdef DXMT_IOS
  upload_bytes_since_flush_ = 0;
#endif

  clear_render_pass_head.next = nullptr;
  clear_render_pass_tail = &clear_render_pass_head;

  blit_cmd_head.type = WMTBlitCommandNop;
  blit_cmd_head.next.set(nullptr);
  blit_cmd_tail = (wmtcmd_base *)&blit_cmd_head;

  ref_tracker.clear();
}

void
ResourceInitializer::encode(WMT::CommandBuffer cmdbuf) {

  auto clear_pass = clear_render_pass_head.next;
  while (clear_pass) {
    auto r = cmdbuf.renderCommandEncoder(clear_pass->info);
    r.endEncoding();
    clear_pass = clear_pass->next;
  }

  if (blit_cmd_head.next.ptr) {
    auto b = cmdbuf.blitCommandEncoder();
    b.encodeCommands(&blit_cmd_head);
    b.endEncoding();
  }
}

WMT::Buffer
ResourceInitializer::allocateGpuHeap(size_t size, size_t &offset) {
  auto [block, offset_] = gpu_command_heap_allocator.allocate(
      current_seq_id_, cached_coherent_seq_id, size, kResourceInitializerGpuUploadHeapAlignment
  );
  offset = offset_;
  return block.buffer;
}

bool
ResourceInitializer::retainAllocation(Allocation *allocation) {
  constexpr size_t block_size = 0x20;
  while (unlikely(!ref_tracker.track(allocation))) {
    auto temp = allocateCpuHeap<intptr_t[block_size]>();
    if (!temp)
      return false;
    ref_tracker.addStorage(temp, block_size * sizeof(intptr_t));
  }
  return true;
}

WMT::Buffer
ResourceInitializer::allocateZeroBuffer(size_t size) {
  if (zero_buffer_size_ < size) {
    if (zero_buffer_size_) {
      flushInternal(); // keep a reference of old zero buffer
      zero_buffer_size_ = 0;
      return {};
    }

    wmtcmd_blit_fillbuffer *fill = nullptr;
    if (!allocateBlit(&fill)) {
      return {};
    }

#ifdef DXMT_IOS
    // Spec 05 D8: switch DXMT_ZERO_BUFFER_POW2 (default: on in the i386 build
    // only, spec 06 A) grows the zero buffer to the next power of two, at
    // least 1 MB, so it is replaced less often; the whole new buffer is filled
    // with zeros below.
    static const bool pow2_growth = env::readSwitch("DXMT_ZERO_BUFFER_POW2", kI386Build);
    if (pow2_growth) {
      size_t grown = 1ull << 20;
      while (grown < size)
        grown <<= 1;
      size = grown;
    }
#endif

    WMTBufferInfo buffer_info;
    buffer_info.gpu_address = 0;
    buffer_info.length = size;
    buffer_info.memory.set(nullptr);
    buffer_info.options = WMTResourceStorageModePrivate | WMTResourceHazardTrackingModeUntracked;
    zero_buffer_ = device_.newBuffer(buffer_info);
    zero_buffer_size_ = size;

    fill->type = WMTBlitCommandFillBuffer;
    fill->buffer = zero_buffer_;
    fill->length = size;
    fill->offset = 0;
    fill->value = 0;
  }
  return zero_buffer_;
}

} // namespace dxmt