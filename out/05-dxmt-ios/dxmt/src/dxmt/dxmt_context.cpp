// Changed for MYIOSDECK (spec 05 B8, D5, E2, H1; spec 06 A, B; spec 07 A2: LGPL-2.1-or-later
// blocks from dxmt-d3d9 v0.4-d3d9 (e8dd4c6), marked inline, see LICENSE-NOTES.md; spec 05
// clarification 17: the read-write UAV format line; 21, 24, 27, 28: the frame trace and dump).
// Upstream: DXMT a3a6e8f, MIT (see LICENSE).
#include "dxmt_context.hpp"
#include "Metal.hpp"
#include "dxmt_command_queue.hpp"
#include "util_placed_memory.hpp"
#ifdef DXMT_IOS
#include "util_bitness.hpp"
#include "util_ios_switch.hpp"
#endif
#include "dxmt_format.hpp"
#include "dxmt_uav_format_log.hpp"
#include "log/log.hpp"
#include "dxmt_occlusion_query.hpp"
#include "dxmt_presenter.hpp"
#include "wsi_platform.hpp"
#include <cstdint>
#include <cfloat>
#include <cstdio>
#include <filesystem>

namespace dxmt {

void
ArgumentEncodingContext::noteReadWriteUAV(WMTPixelFormat format) {
  // MYIOSDECK spec 05 clarification 17
  if (!uav_format_caps_ready_) {
    uav_format_caps_.Inspect(device_);
    uav_format_caps_ready_ = true;
  }
  auto base = UAVBaseFormat(format);
  auto found = uav_format_caps_.textureCapabilities.find(base);
  auto capability = found == uav_format_caps_.textureCapabilities.end() ? FormatCapability::None : found->second;
  if (ReadWriteUAVFormatLog::global().firstUnsupported(base, capability))
    Logger::warn(ReadWriteUAVLogLine(base));
}

ArgumentEncodingContext::ArgumentEncodingContext(CommandQueue &queue, WMT::Device device, InternalCommandLibrary &lib) :
    emulated_cmd(device, lib, *this),
    clear_rt_cmd(device, lib, *this),
    resolve_texture_cmd(device, lib, *this), /* spec 07 */
    stretch_blit_cmd(device, lib, *this),    /* spec 07 */
    blit_depth_stencil_cmd(device, lib, *this),
    clear_res_cmd(device, lib, *this),
    mv_scale_cmd(device, lib, *this),
    device_(device),
    queue_(queue) {
  dummy_sampler_info_.support_argument_buffers = true;
  dummy_sampler_info_.border_color = WMTSamplerBorderColorTransparentBlack;
  dummy_sampler_info_.compare_function = WMTCompareFunctionNever;
  dummy_sampler_info_.normalized_coords = true;
  dummy_sampler_info_.r_address_mode = WMTSamplerAddressModeClampToEdge;
  dummy_sampler_info_.s_address_mode = WMTSamplerAddressModeClampToEdge;
  dummy_sampler_info_.t_address_mode = WMTSamplerAddressModeClampToEdge;
  dummy_sampler_info_.min_filter = WMTSamplerMinMagFilterNearest;
  dummy_sampler_info_.mag_filter = WMTSamplerMinMagFilterNearest;
  dummy_sampler_info_.mip_filter = WMTSamplerMipFilterNotMipmapped;
  dummy_sampler_info_.lod_min_clamp = 0.0f;
  dummy_sampler_info_.lod_max_clamp = FLT_MAX;
  dummy_sampler_info_.max_anisotroy = 1;
  dummy_sampler_info_.lod_average = false;
  dummy_sampler_ = device.newSamplerState(dummy_sampler_info_);
  dummy_cbuffer_host_ = wsi::aligned_malloc(65536, kPlacedMemoryAlignment); // 16 KB pages on iOS
  dummy_cbuffer_info_.length = 65536;
  dummy_cbuffer_info_.memory.set(dummy_cbuffer_host_);
  dummy_cbuffer_info_.options = WMTResourceOptionCPUCacheModeWriteCombined | WMTResourceStorageModeShared |
                                WMTResourceHazardTrackingModeUntracked;
  dummy_cbuffer_ = device.newBuffer(dummy_cbuffer_info_);
  std::memset(dummy_cbuffer_info_.memory.get(), 0, 65536);
  cpu_buffer_chunks_.emplace_back();
};

ArgumentEncodingContext::~ArgumentEncodingContext() {
  wsi::aligned_free(dummy_cbuffer_host_);
};

template void ArgumentEncodingContext::encodeVertexBuffers<PipelineKind::Ordinary>(uint32_t slot_mask, uint64_t argument_buffer_offset);
template void ArgumentEncodingContext::encodeVertexBuffers<PipelineKind::Tessellation>(uint32_t slot_mask, uint64_t argument_buffer_offset);
template void ArgumentEncodingContext::encodeVertexBuffers<PipelineKind::Geometry>(uint32_t slot_mask, uint64_t argument_buffer_offset);

template <PipelineKind kind>
void
ArgumentEncodingContext::encodeVertexBuffers(uint32_t slot_mask, uint64_t offset) {
  struct VERTEX_BUFFER_ENTRY {
    uint64_t buffer_handle;
    uint32_t stride;
    uint32_t length;
  };
  uint32_t max_slot = 32 - __builtin_clz(slot_mask);

  VERTEX_BUFFER_ENTRY *entries = getMappedArgumentBuffer<VERTEX_BUFFER_ENTRY>(offset);

  for (unsigned slot = 0, index = 0; slot < max_slot; slot++) {
    if (!(slot_mask & (1 << slot)))
      continue;
    auto &state = vbuf_[slot];
    auto &buffer = state.buffer;
    if (!buffer.ptr()) {
      entries[index].buffer_handle = 0;
      entries[index].stride = 0;
      entries[index++].length = 0;
      continue;
    }
    auto valid_length = buffer->length() > state.offset ? buffer->length() - state.offset : 0;
    auto [buffer_alloc, buffer_offset] = access<true>(buffer, state.offset, valid_length, DXMT_ENCODER_RESOURCE_ACESS_READ);
    entries[index].buffer_handle = buffer_alloc->gpuAddress() + buffer_offset + state.offset;
    entries[index].stride = state.stride;
    entries[index++].length = valid_length;
    // FIXME: did we intended to use the whole buffer?
    makeResident<PipelineStage::Vertex, kind>(buffer.ptr());
  };
  {
    auto &cmd = encodeRenderCommand<wmtcmd_render_setbufferoffset>();
    cmd.offset = getFinalArgumentBufferOffset(offset);
    cmd.index = 16;
    if constexpr (kind == PipelineKind::Geometry || kind == PipelineKind::Tessellation)
      cmd.type = WMTRenderCommandSetObjectBufferOffset;
    else
      cmd.type = WMTRenderCommandSetVertexBufferOffset;
  }
}

template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Vertex, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Pixel, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Vertex, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Pixel, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Hull, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Domain, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Compute, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Vertex, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Geometry, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeConstantBuffers<PipelineStage::Pixel, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *constant_buffers,
    uint64_t argument_buffer_offset
);

template <PipelineStage stage, PipelineKind kind>
void
ArgumentEncodingContext::encodeConstantBuffers(const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT * constant_buffers, uint64_t offset) {
  uint64_t *encoded_buffer = getMappedArgumentBuffer<uint64_t, stage == PipelineStage::Compute>(offset);

  constexpr bool PreRasterStage = stage == PipelineStage::Vertex || stage == PipelineStage::Domain ||
                                 stage == PipelineStage::Hull || stage == PipelineStage::Geometry;

  for (unsigned i = 0; i < reflection->NumConstantBuffers; i++) {
    auto &arg = constant_buffers[i];
    auto slot = 14 * unsigned(stage) + arg.SM50BindingSlot;
    switch (arg.Type) {
    case SM50BindingType::ConstantBuffer: {
      auto &cbuf = cbuf_[slot];
      if (!cbuf.buffer.ptr()) {
        encoded_buffer[arg.StructurePtrOffset] = dummy_cbuffer_info_.gpu_address;
        makeResident<stage, kind>(dummy_cbuffer_, GetResidencyMask<kind>(stage, true, false));
        continue;
      }
      auto argbuf = cbuf.buffer;
      auto valid_length = argbuf->length() > cbuf.offset ? argbuf->length() - cbuf.offset : 0;
      auto [argbuf_alloc, argbuf_offset] = access<PreRasterStage>(argbuf, cbuf.offset, valid_length, DXMT_ENCODER_RESOURCE_ACESS_READ);
      encoded_buffer[arg.StructurePtrOffset] = argbuf_alloc->gpuAddress() + argbuf_offset + cbuf.offset;
      makeResident<stage, kind>(argbuf.ptr());
      break;
    }
    default:
      DXMT_UNREACHABLE
    }
  }

  /* kConstantBufferTableBinding = 29 */
  if constexpr (stage == PipelineStage::Compute) {
    auto &cmd = encodeComputeCommand<wmtcmd_compute_setbufferoffset>();
    cmd.type = WMTComputeCommandSetBufferOffset;
    cmd.offset = getFinalArgumentBufferOffset<true>(offset);
    cmd.index = 29;
  } else {
    auto &cmd = encodeRenderCommand<wmtcmd_render_setbufferoffset>();
    cmd.offset = getFinalArgumentBufferOffset(offset);
    cmd.index = 29;
    if constexpr (stage == PipelineStage::Vertex) {
      if constexpr (kind == PipelineKind::Geometry)
        cmd.type = WMTRenderCommandSetObjectBufferOffset;
      else if constexpr (kind == PipelineKind::Tessellation) {
        cmd.type = WMTRenderCommandSetObjectBufferOffset;
        cmd.index = 27;
      } else
        cmd.type = WMTRenderCommandSetVertexBufferOffset;
    } else if constexpr (stage == PipelineStage::Pixel) {
      cmd.type = WMTRenderCommandSetFragmentBufferOffset;
    } else if constexpr (stage == PipelineStage::Hull) {
      cmd.type = WMTRenderCommandSetObjectBufferOffset;
    } else if constexpr (stage == PipelineStage::Domain) {
      cmd.type = WMTRenderCommandSetMeshBufferOffset;
    } else if constexpr (stage == PipelineStage::Geometry) {
      cmd.type = WMTRenderCommandSetMeshBufferOffset;
    } else {
      assert(0 && "Not implemented or unreachable");
    }
  }
};

template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Vertex, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Pixel, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Vertex, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Pixel, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Hull, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Domain, PipelineKind::Tessellation>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Compute, PipelineKind::Ordinary>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Vertex, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Geometry, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset
);
template void ArgumentEncodingContext::encodeShaderResources<PipelineStage::Pixel, PipelineKind::Geometry>(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t argument_buffer_offset
);

inline uint64_t
TextureMetadata(uint32_t array_length, float min_lod) {
  return ((uint64_t)array_length << 32) | (uint64_t)std::bit_cast<uint32_t>(min_lod);
}

template <PipelineStage stage, PipelineKind kind>
void
ArgumentEncodingContext::encodeShaderResources(
    const MTL_SHADER_REFLECTION *reflection, const MTL_SM50_SHADER_ARGUMENT *arguments, uint64_t offset
) {
  auto BindingCount = reflection->NumArguments;
  uint64_t *encoded_buffer = getMappedArgumentBuffer<uint64_t, stage == PipelineStage::Compute>(offset);

  auto &UAVBindingSet = stage == PipelineStage::Compute ? cs_uav_ : om_uav_;

  constexpr bool PreRasterStage = stage == PipelineStage::Vertex || stage == PipelineStage::Domain ||
                                 stage == PipelineStage::Hull || stage == PipelineStage::Geometry;

  // spec 05 clarification 21: the compute shader's UAVs, for the traced frame's dispatches
  const bool trace_uavs = stage == PipelineStage::Compute && trace_active_;
  if (trace_uavs) {
    trace_cs_uavs_.clear();
    trace_cs_uav_bindings_.clear(); // spec 05 clarifications 27 and 28
  }

  for (unsigned i = 0; i < BindingCount; i++) {
    auto &arg = arguments[i];
    switch (arg.Type) {
    case SM50BindingType::ConstantBuffer: {
      DXMT_UNREACHABLE
    }
    case SM50BindingType::Sampler: {
      auto slot = 16 * unsigned(stage) + arg.SM50BindingSlot;
      auto &sampler = sampler_[slot].sampler;
      if (!sampler) {
        encoded_buffer[arg.StructurePtrOffset] = dummy_sampler_info_.gpu_resource_id;
        encoded_buffer[arg.StructurePtrOffset + 1] = dummy_sampler_info_.gpu_resource_id;
        encoded_buffer[arg.StructurePtrOffset + 2] = (uint64_t)std::bit_cast<uint32_t>(0.0f);
        break;
      }
      encoded_buffer[arg.StructurePtrOffset] = sampler->sampler_state_handle;
      encoded_buffer[arg.StructurePtrOffset + 1] = sampler->sampler_state_cube_handle;
      encoded_buffer[arg.StructurePtrOffset + 2] = (uint64_t)std::bit_cast<uint32_t>(sampler->lod_bias);
      break;
    }
    case SM50BindingType::SRV: {
      auto slot = 128 * unsigned(stage) + arg.SM50BindingSlot;
      auto &srv = resview_[slot];

      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER) {
        if (srv.buffer.ptr()) {
          auto [srv_alloc, offset] = access<PreRasterStage>(srv.buffer, srv.slice.byteOffset, srv.slice.byteLength, DXMT_ENCODER_RESOURCE_ACESS_READ);
          encoded_buffer[arg.StructurePtrOffset] = srv_alloc->gpuAddress() + offset + srv.slice.byteOffset;
          encoded_buffer[arg.StructurePtrOffset + 1] = srv.slice.byteLength;
          makeResident<stage, kind>(srv.buffer.ptr());
        } else {
          encoded_buffer[arg.StructurePtrOffset] = 0;
          encoded_buffer[arg.StructurePtrOffset + 1] = 0;
        }
      } else if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE) {
        if (srv.buffer.ptr()) {
          assert(arg.Flags & MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET);
          auto [view, offset] = access<PreRasterStage>(srv.buffer, srv.viewId, DXMT_ENCODER_RESOURCE_ACESS_READ);
          encoded_buffer[arg.StructurePtrOffset] = view.gpu_resource_id;
          encoded_buffer[arg.StructurePtrOffset + 1] =
              ((uint64_t)srv.slice.elementCount << 32) | (uint64_t)(srv.slice.firstElement + offset);
          makeResident<stage, kind>(srv.buffer.ptr(), srv.viewId);
        } else if (srv.texture.ptr()) {
          assert(arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP);
          auto viewIdChecked = srv.texture->checkViewUseArray(srv.viewId, arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY);
          encoded_buffer[arg.StructurePtrOffset] =
              access<PreRasterStage>(srv.texture, viewIdChecked, DXMT_ENCODER_RESOURCE_ACESS_READ).gpuResourceID;
#ifdef DXMT_IOS
          // Spec 05 D5: never start sampling at a lost (unfilled) mip level.
          encoded_buffer[arg.StructurePtrOffset + 1] = TextureMetadata(
              srv.texture->arrayLength(viewIdChecked), srv.texture->lostLevelMinLod(viewIdChecked)
          );
#else
          encoded_buffer[arg.StructurePtrOffset + 1] = TextureMetadata(srv.texture->arrayLength(viewIdChecked), 0);
#endif
          makeResident<stage, kind>(srv.texture.ptr(), viewIdChecked);
        } else {
          encoded_buffer[arg.StructurePtrOffset] = 0;
          encoded_buffer[arg.StructurePtrOffset + 1] = 0;
        }
      }
      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER) {
        assert(0 && "srv can not have counter associated");
      }
      break;
    }
    case SM50BindingType::UAV: {
      auto &uav = UAVBindingSet[arg.SM50BindingSlot];
      bool read = (arg.Flags >> 10) & 1, write = (arg.Flags >> 10) & 2;
      auto access_flags =  (DXMT_ENCODER_RESOURCE_ACESS)((arg.Flags >> 10) & 3);

      if (trace_uavs) { // spec 05 clarification 21
        std::string desc = "u" + std::to_string(arg.SM50BindingSlot) + " ";
        if (uav.texture.ptr())
          desc += TraceTexture(uav.texture->pixelFormat(uav.viewId), uav.texture->width(uav.viewId),
                               uav.texture->height(uav.viewId));
        else if (uav.buffer.ptr() && (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE))
          desc += TracePixelFormat(uav.buffer->pixelFormat(uav.viewId)) + " " +
                  std::to_string(uav.slice.elementCount) + " elements";
        else if (uav.buffer.ptr())
          desc += "buffer " + std::to_string(uav.slice.byteLength) + " bytes";
        else
          desc += "null";
        trace_cs_uavs_ += (trace_cs_uavs_.empty() ? "" : ", ") + desc;
        trace_cs_uav_bindings_.push_back(
            {arg.SM50BindingSlot, (uint32_t)arg.Flags, uav.buffer, uav.texture, uav.viewId, uav.slice}
        );
      }

      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_BUFFER) {
        if (uav.buffer.ptr()) {
          auto [uav_alloc, offset] = access<PreRasterStage>(uav.buffer, uav.slice.byteOffset, uav.slice.byteLength, access_flags);
          encoded_buffer[arg.StructurePtrOffset] = uav_alloc->gpuAddress() + offset + uav.slice.byteOffset;
          encoded_buffer[arg.StructurePtrOffset + 1] = uav.slice.byteLength;
          makeResident<stage, kind>(uav.buffer.ptr(), read, write);
        } else {
          encoded_buffer[arg.StructurePtrOffset] = 0;
          encoded_buffer[arg.StructurePtrOffset + 1] = 0;
        }
      } else if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE) {
        if (uav.buffer.ptr()) {
          assert(arg.Flags & MTL_SM50_SHADER_ARGUMENT_TBUFFER_OFFSET);
          if (read && write) // spec 05 clarification 17
            noteReadWriteUAV(uav.buffer->pixelFormat(uav.viewId));
          auto [view, offset] = access<PreRasterStage>(uav.buffer, uav.viewId, access_flags);
          encoded_buffer[arg.StructurePtrOffset] = view.gpu_resource_id;
          encoded_buffer[arg.StructurePtrOffset + 1] =
              ((uint64_t)uav.slice.elementCount << 32) | (uint64_t)(uav.slice.firstElement + offset);
          makeResident<stage, kind>(uav.buffer.ptr(), uav.viewId, read, write);
        } else if (uav.texture.ptr()) {
          assert(arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_MINLOD_CLAMP);
          auto viewIdChecked = uav.texture->checkViewUseArray(uav.viewId, arg.Flags & MTL_SM50_SHADER_ARGUMENT_TEXTURE_ARRAY);
          if (read && write) // spec 05 clarification 17
            noteReadWriteUAV(uav.texture->pixelFormat(viewIdChecked));
          encoded_buffer[arg.StructurePtrOffset] = access<PreRasterStage>(uav.texture, viewIdChecked, access_flags).gpuResourceID;
          encoded_buffer[arg.StructurePtrOffset + 1] = TextureMetadata(uav.texture->arrayLength(viewIdChecked), 0);
          makeResident<stage, kind>(uav.texture.ptr(), viewIdChecked, read, write);
        } else {
          encoded_buffer[arg.StructurePtrOffset] = 0;
          encoded_buffer[arg.StructurePtrOffset + 1] = 0;
        }
      }
      if (arg.Flags & MTL_SM50_SHADER_ARGUMENT_UAV_COUNTER) {
        if (uav.counter) {
          auto [counter_alloc, offset] = access<PreRasterStage>(uav.counter, 0, 4, DXMT_ENCODER_RESOURCE_ACESS_READ | DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          encoded_buffer[arg.StructurePtrOffset + 2] = counter_alloc->gpuAddress() + offset;
          makeResident<stage, kind>(uav.counter.ptr(), true, true);
        } else {
          /*
           * potentially cause gpu pagefault, even providing a dummy buffer doesn't improve since the returned
           * counter value is likely to be used as an index to another read/write operation.
           */
          encoded_buffer[arg.StructurePtrOffset + 2] = 0;
        }
      }
      break;
    }
    }
  }

  if constexpr (stage == PipelineStage::Compute) {
    auto &cmd = encodeComputeCommand<wmtcmd_compute_setbufferoffset>();
    cmd.type = WMTComputeCommandSetBufferOffset;
    cmd.offset = getFinalArgumentBufferOffset<true>(offset);
    cmd.index = 30;
  } else {
    auto &cmd = encodeRenderCommand<wmtcmd_render_setbufferoffset>();
    cmd.offset = getFinalArgumentBufferOffset(offset);
    cmd.index = 30;
    if constexpr (stage == PipelineStage::Vertex) {
      if constexpr (kind == PipelineKind::Geometry)
        cmd.type = WMTRenderCommandSetObjectBufferOffset;
      else if constexpr (kind == PipelineKind::Tessellation) {
        cmd.type = WMTRenderCommandSetObjectBufferOffset;
        cmd.index = 28;
      } else
        cmd.type = WMTRenderCommandSetVertexBufferOffset;
    } else if constexpr (stage == PipelineStage::Pixel) {
      cmd.type = WMTRenderCommandSetFragmentBufferOffset;
    } else if constexpr (stage == PipelineStage::Hull) {
      cmd.type = WMTRenderCommandSetObjectBufferOffset;
    } else if constexpr (stage == PipelineStage::Domain) {
      cmd.type = WMTRenderCommandSetMeshBufferOffset;
    } else if constexpr (stage == PipelineStage::Geometry) {
      cmd.type = WMTRenderCommandSetMeshBufferOffset;
    } else {
      assert(0 && "Not implemented or unreachable");
    }
  }
}

void
ArgumentEncodingContext::retainAllocation(Allocation* allocation) {
  if (allocation->checkRetained(seq_id_))
    return;
  queue_.Retain(seq_id_, allocation);
}

void
ArgumentEncodingContext::clearColor(Rc<Texture> &&texture, unsigned viewId, unsigned arrayLength, WMTClearColor color) {
  assert(!encoder_current);
  auto encoder_info = allocate<ClearEncoderData>();
  encoder_info->type = EncoderType::Clear;
  encoder_info->id = nextEncoderId();
  encoder_info->clear_dsv = 0;
  encoder_info->color = color;
  encoder_info->array_length = arrayLength;
  encoder_info->width = texture->width();
  encoder_info->height = texture->height();
  encoder_current = encoder_info;

  encoder_info->attachment = access(texture, viewId, DXMT_ENCODER_RESOURCE_ACESS_WRITE);

  currentFrameStatistics().clear_pass_count++;

  endPass();
}

void
ArgumentEncodingContext::clearDepthStencil(
    Rc<Texture> &&texture, unsigned viewId, unsigned arrayLength, unsigned flag, float depth, uint8_t stencil
) {
  assert(!encoder_current);
  auto encoder_info = allocate<ClearEncoderData>();
  encoder_info->type = EncoderType::Clear;
  encoder_info->id = nextEncoderId();
  encoder_info->clear_dsv = flag & DepthStencilPlanarFlags(texture->pixelFormat());
  encoder_info->depth_stencil = {depth, stencil};
  encoder_info->array_length = arrayLength;
  encoder_info->width = texture->width();
  encoder_info->height = texture->height();
  encoder_current = encoder_info;

  encoder_info->attachment = access(texture, viewId, DXMT_ENCODER_RESOURCE_ACESS_WRITE);

  currentFrameStatistics().clear_pass_count++;
  
  endPass();
}

void
ArgumentEncodingContext::resolveTexture(
    Rc<Texture> &&src, TextureViewKey src_view, Rc<Texture> &&dst, TextureViewKey dst_view,
    WMT::RenderPipelineState pso, std::optional<WMTScissorRect> src_rect, WMTOrigin dst_origin,
    WMTSize resolve_size
) {
  assert(!encoder_current);
  auto encoder_info = allocate<ResolveEncoderData>();
  encoder_info->type = EncoderType::Resolve;
  encoder_info->id = nextEncoderId(); /* spec 07: as the other passes */
  encoder_current = encoder_info;

  encoder_info->src = access(src, src_view, DXMT_ENCODER_RESOURCE_ACESS_READ);
  encoder_info->dst = access(dst, dst_view, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
  /* spec 07 (LGPL, from v0.4-d3d9) */
  encoder_info->pso = pso;
  encoder_info->src_rect = src_rect;
  encoder_info->dst_origin = dst_origin;
  encoder_info->resolve_size = resolve_size;

  endPass();
};

/* ---- spec 07: LGPL-2.1-or-later block, from dxmt-d3d9 v0.4-d3d9 (e8dd4c6)
   dxmt_context.cpp, adapted to this tree's dependency tracking (read/write sets
   instead of the tag's fences) ---- */

void
ArgumentEncodingContext::resolveDepthTexture(
    Rc<Texture> &&src, TextureViewKey src_view, Rc<Texture> &&dst, TextureViewKey dst_view,
    WMT::RenderPipelineState pso, std::optional<WMTScissorRect> src_rect, WMTOrigin dst_origin, WMTSize resolve_size
) {
  assert(!encoder_current);
  auto encoder_info = allocate<ResolveEncoderData>();
  encoder_info->type = EncoderType::Resolve;
  encoder_info->id = nextEncoderId();
  encoder_current = encoder_info;

  encoder_info->src = access(src, src_view, DXMT_ENCODER_RESOURCE_ACESS_READ);
  encoder_info->dst = access(dst, dst_view, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
  encoder_info->pso = pso;
  encoder_info->src_rect = src_rect;
  encoder_info->dst_origin = dst_origin;
  encoder_info->resolve_size = resolve_size;
  encoder_info->is_depth = true;

  endPass();
};

void
ArgumentEncodingContext::stretchBlit(
    Rc<Texture> &&src, TextureViewKey src_view, Rc<Texture> &&dst, TextureViewKey dst_view,
    WMT::RenderPipelineState pso, WMT::SamplerState sampler,
    WMTOrigin src_origin, WMTSize src_size,
    WMTOrigin dst_origin, WMTSize dst_size
) {
  assert(!encoder_current);
  // The src texture-view's level dimensions normalize the sample sub-rect into
  // uv-space; width/height(view) are at least 1.
  uint32_t src_view_w = src ? src->width(src_view) : 1;
  uint32_t src_view_h = src ? src->height(src_view) : 1;

  auto encoder_info = allocate<StretchBlitEncoderData>();
  encoder_info->type = EncoderType::StretchBlit;
  encoder_info->id = nextEncoderId();
  encoder_current = encoder_info;

  encoder_info->src = access(src, src_view, DXMT_ENCODER_RESOURCE_ACESS_READ);
  encoder_info->dst = access(dst, dst_view, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
  encoder_info->pso = pso;
  encoder_info->sampler = sampler;
  encoder_info->src_uv_origin[0] = float(src_origin.x) / float(src_view_w);
  encoder_info->src_uv_origin[1] = float(src_origin.y) / float(src_view_h);
  encoder_info->src_uv_size[0] = float(src_size.width) / float(src_view_w);
  encoder_info->src_uv_size[1] = float(src_size.height) / float(src_view_h);
  encoder_info->dst_origin = dst_origin;
  encoder_info->dst_size = dst_size;

  endPass();
}

void
ArgumentEncodingContext::copyTexture(
    const Rc<Texture> &src, unsigned src_level, unsigned src_slice, WMTOrigin src_origin,
    const Rc<Texture> &dst, unsigned dst_level, unsigned dst_slice, WMTOrigin dst_origin,
    WMTSize size
) {
  assert(!encoder_current);
  startBlitPass();
  auto src_tex = access(src, src_level, src_slice, DXMT_ENCODER_RESOURCE_ACESS_READ);
  auto dst_tex = access(dst, dst_level, dst_slice, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
  auto &cmd = encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
  cmd.type = WMTBlitCommandCopyFromTextureToTexture;
  cmd.src = src_tex.handle;
  cmd.src_slice = src_slice;
  cmd.src_level = src_level;
  cmd.src_origin = src_origin;
  cmd.src_size = size;
  cmd.dst = dst_tex.handle;
  cmd.dst_slice = dst_slice;
  cmd.dst_level = dst_level;
  cmd.dst_origin = dst_origin;
  endPass();
}

void
ArgumentEncodingContext::optimizeTextureForGPUAccess(const Rc<Texture> &texture, unsigned level, unsigned slice) {
  assert(!encoder_current);
  // Write access: the re-tile mutates the texture's layout, so it must register
  // as the subresource's last writer for the following sample to wait on it.
  startBlitPass();
  auto tex = access(texture, level, slice, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
  auto &cmd = encodeBlitCommand<wmtcmd_blit_optimize_contents>();
  cmd.type = WMTBlitCommandOptimizeContentsForGPUAccess;
  cmd.texture = tex.handle;
  cmd.slice = slice;
  cmd.level = level;
  endPass();
}

void
ArgumentEncodingContext::signalEventByHandle(obj_handle_t event_handle, uint64_t value) {
  assert(!encoder_current);
  auto encoder_info = allocate<SignalEventData>();
  encoder_info->type = EncoderType::SignalEvent;
  encoder_info->id = nextEncoderId();
  // Reference operator=(non-retained object) retains; the SignalEventData
  // destructor releases. The retain runs on the encode thread.
  encoder_info->event = WMT::Event{event_handle};
  encoder_info->value = value;

  encoder_current = encoder_info;
  endPass();
}

/* ---- end of LGPL block ---- */

void
ArgumentEncodingContext::present(Rc<Texture> &texture, Rc<Presenter> &presenter, double after, DXMTPresentMetadata metadata) {
  assert(!encoder_current);
  if (dump_active_) // spec 05 clarification 35
    dumpPresent(texture);
  auto encoder_info = allocate<PresentData>();
  encoder_info->type = EncoderType::Present;
  encoder_info->id = nextEncoderId();
  encoder_info->backbuffer = texture->current()->texture();
  encoder_info->presenter = presenter;
  encoder_info->after = after;
  encoder_info->metadata = metadata;

  encoder_info->tex_read.add(texture->current()->depkey);

  encoder_current = encoder_info;
  endPass();
}

void
ArgumentEncodingContext::upscale(Rc<Texture> &texture, Rc<Texture> &upscaled, WMT::Reference<WMT::FXSpatialScaler> &scaler) {
  assert(!encoder_current);
  auto encoder_info = allocate<SpatialUpscaleData>();
  encoder_info->type = EncoderType::SpatialUpscale;
  encoder_info->id = nextEncoderId();
  encoder_info->backbuffer = texture->current()->texture();
  encoder_info->upscaled = upscaled->current()->texture();
  encoder_info->scaler = scaler;

  encoder_info->tex_read.add(texture->current()->depkey);
  encoder_info->tex_write.add(upscaled->current()->depkey);

  encoder_current = encoder_info;
  endPass();
}

void
ArgumentEncodingContext::upscaleTemporal(
    Rc<Texture> &input, Rc<Texture> &output, Rc<Texture> &depth, Rc<Texture> &motion_vector, TextureViewKey mvViewId,
    Rc<Texture> &exposure, WMT::Reference<WMT::FXTemporalScaler> &scaler, const WMTFXTemporalScalerProps &props
) {
  assert(!encoder_current);
  auto encoder_info = allocate<TemporalUpscaleData>();
  encoder_info->type = EncoderType::TemporalUpscale;
  encoder_info->id = nextEncoderId();
  encoder_info->input = input->current()->texture();
  encoder_info->output = output->current()->texture();
  encoder_info->depth = depth->current()->texture();
  encoder_info->motion_vector = motion_vector->view(mvViewId).texture;
  encoder_info->scaler = scaler;
  encoder_info->props = props;

  encoder_info->tex_read.add(input->current()->depkey);
  encoder_info->tex_read.add(depth->current()->depkey);
  encoder_info->tex_read.add(motion_vector->current()->depkey);
  encoder_info->tex_write.add(output->current()->depkey);
  if(exposure) {
    encoder_info->exposure = exposure->current()->texture();
    encoder_info->tex_read.add(exposure->current()->depkey);
  } else {
    encoder_info->exposure = nullptr;
  }

  encoder_current = encoder_info;
  endPass();
}

void
ArgumentEncodingContext::signalEvent(uint64_t value) {
  assert(!encoder_current);
  auto encoder_info = allocate<SignalEventData>();
  encoder_info->type = EncoderType::SignalEvent;
  encoder_info->id = nextEncoderId();
  encoder_info->event = queue_.event;
  encoder_info->value = value;

  encoder_current = encoder_info;
  endPass();
}

void
ArgumentEncodingContext::signalEvent(WMT::Reference<WMT::Event> &&event, uint64_t value) {
  assert(!encoder_current);
  auto encoder_info = allocate<SignalEventData>();
  encoder_info->type = EncoderType::SignalEvent;
  encoder_info->id = nextEncoderId();
  encoder_info->event = std::move(event);
  encoder_info->value = value;

  encoder_current = encoder_info;
  endPass();
}

void
ArgumentEncodingContext::waitEvent(WMT::Reference<WMT::Event> &&event, uint64_t value) {
  assert(!encoder_current);
  auto encoder_info = allocate<WaitForEventData>();
  encoder_info->type = EncoderType::WaitForEvent;
  encoder_info->id = nextEncoderId();
  encoder_info->event = std::move(event);
  encoder_info->value = value;

  encoder_current = encoder_info;
  endPass();
}

RenderEncoderData *
ArgumentEncodingContext::startRenderPass(
    uint8_t dsv_planar_flags, uint8_t dsv_readonly_flags, uint8_t render_target_count, uint64_t encoder_argbuf_size
) {
  assert(!encoder_current);
  auto encoder_info = allocate<RenderEncoderData>();
  encoder_info->type = EncoderType::Render;
  encoder_info->id = nextEncoderId();
  encoder_info->cmd_head.type = WMTRenderCommandNop;
  encoder_info->cmd_head.next.set(0);
  encoder_info->cmd_tail = (wmtcmd_base *)&encoder_info->cmd_head;
  encoder_info->dsv_planar_flags = dsv_planar_flags;
  encoder_info->dsv_readonly_flags = dsv_readonly_flags;
  encoder_info->render_target_count = render_target_count;
  auto [gpu_buffer_contents, gpu_buffer_, offset] = queue_.AllocateArgumentBuffer(seq_id_, encoder_argbuf_size);
  encoder_info->allocated_argbuf = gpu_buffer_;
  encoder_info->allocated_argbuf_offset = offset;
  encoder_info->allocated_argbuf_mapping = gpu_buffer_contents;
  encoder_current = encoder_info;

  currentFrameStatistics().render_pass_count++;

  vro_state_.beginEncoder();

  return encoder_info;
}

EncoderData *
ArgumentEncodingContext::startComputePass(uint64_t encoder_argbuf_size) {
  assert(!encoder_current);
  auto encoder_info = allocate<ComputeEncoderData>();
  encoder_info->type = EncoderType::Compute;
  encoder_info->id = nextEncoderId();
  encoder_info->cmd_head.type = WMTComputeCommandNop;
  encoder_info->cmd_head.next.set(0);
  encoder_info->cmd_tail = (wmtcmd_base *)&encoder_info->cmd_head;
  auto [gpu_buffer_contents, gpu_buffer_, offset] = queue_.AllocateArgumentBuffer(seq_id_, encoder_argbuf_size);
  encoder_info->allocated_argbuf = gpu_buffer_;
  encoder_info->allocated_argbuf_offset = offset;
  encoder_info->allocated_argbuf_mapping = gpu_buffer_contents;
  encoder_current = encoder_info;

  currentFrameStatistics().compute_pass_count++;

  return encoder_info;
}

EncoderData *
ArgumentEncodingContext::startBlitPass() {
  assert(!encoder_current);
  auto encoder_info = allocate<BlitEncoderData>();
  encoder_info->type = EncoderType::Blit;
  encoder_info->id = nextEncoderId();
  encoder_info->cmd_head.type = WMTBlitCommandNop;
  encoder_info->cmd_head.next.set(0);
  encoder_info->cmd_tail = (wmtcmd_base *)&encoder_info->cmd_head;
  encoder_current = encoder_info;

  currentFrameStatistics().blit_pass_count++;

  return encoder_info;
}

void
ArgumentEncodingContext::endPass() {
  assert(encoder_current);
  encoder_last->next = encoder_current;
  encoder_last = encoder_current;

  if (encoder_current->type == EncoderType::Render)
    vro_state_.endEncoder();

  auto ended = encoder_current;
  encoder_current = nullptr;
  encoder_count_++;

  // spec 05 clarification 28: passes are numbered as the trace numbers them (render and compute
  // passes of the traced frame, in order); in the dumped frame, a render pass's colour
  // attachments are copied in a blit pass right after it
  if (trace_active_ && (ended->type == EncoderType::Render || ended->type == EncoderType::Compute))
    trace_pass_count_++;
  // spec 05 clarification 33: in a dumped frame, every pass that writes textures (render,
  // clear, resolve, copy; dispatches through traceAfterDispatch) is followed by their copies
  if (dump_active_ && !dump_encoding_)
    dumpAfterPass(ended);
}

std::pair<WMT::Buffer, size_t>
ArgumentEncodingContext::allocateTempBuffer(size_t size, size_t alignment) {
  return queue_.AllocateTempBuffer(seq_id_, size, alignment);
};

AllocatedTempBufferSlice
ArgumentEncodingContext::allocateTempBuffer1(size_t size, size_t alignment) {
  return queue_.AllocateTempBuffer1(seq_id_, size, alignment);
};

void
ArgumentEncodingContext::beginVisibilityResultQuery(Rc<VisibilityResultQuery> &&query) {
  query->begin(seq_id_, vro_state_.getNextReadOffset());
  active_visibility_query_count_++;
  pending_queries_.push_back(std::move(query));
}
void
ArgumentEncodingContext::endVisibilityResultQuery(Rc<VisibilityResultQuery> &&query) {
  query->end(seq_id_, vro_state_.getNextReadOffset());
  assert(active_visibility_query_count_);
  active_visibility_query_count_--;
}
void
ArgumentEncodingContext::bumpVisibilityResultOffset() {
  auto render_encoder = currentRenderEncoder();
  render_encoder->use_visibility_result = render_encoder->use_visibility_result || bool(active_visibility_query_count_);

  uint64_t offset;
  if (vro_state_.tryGetNextWriteOffset(active_visibility_query_count_, offset)) {
    auto &cmd = encodeRenderCommand<wmtcmd_render_setvisibilitymode>();
    cmd.type = WMTRenderCommandSetVisibilityMode;
    if (~offset == 0) {
      cmd.mode = WMTVisibilityResultModeDisabled;
      cmd.offset = 0;
    } else {
      cmd.mode = WMTVisibilityResultModeCounting;
      cmd.offset = offset << 3;
    }
  }
}

FrameStatistics&
ArgumentEncodingContext::currentFrameStatistics() {
  return queue_.statistics.at(frame_id_);
}

void
ArgumentEncodingContext::$$setEncodingContext(uint64_t seq_id, uint64_t frame_id) {
  current_buffer_chunk_ = 0;
  cpu_buffer_ = cpu_buffer_chunks_[current_buffer_chunk_].ptr;
  cpu_buffer_offset_ = 0;
  seq_id_ = seq_id;
  frame_id_ = frame_id;
  // MYIOSDECK spec 05 clarification 21: frame_id counts the presents before this submission, so
  // its frame ends with present frame_id + 1. Leaving a traced frame logs its closing lines.
  if (FrameTraceSwitch() || frame_trace_.active()) {
    std::vector<std::string> log_lines;
    uint64_t present = frame_id + 1;
    // clarification 40: a present a dump was requested for is traced too
    bool traced = FrameTraceSwitch() && FrameTraced(present);
    if (frame_trace_.present() != (traced ? present : 0)) { // a new frame: spec 05 clarification 24
      trace_readback_count_ = 0;
      trace_pending_indirect_ = 0;
      trace_last_dispatch_k_ = 0; // clarification 27
      trace_pass_count_ = 0;      // clarification 28
      // clarifications 28, 33 and 36: leaving a dumped frame, say what it wrote and left out,
      // and hand its buffers and index to this submission, which completes after all of that
      // frame's (written by the finish thread then)
      if (dump_active_) {
        log_lines.push_back("dxmt-frame " + std::to_string(dump_present_) + ": " + dump_ledger_.summary());
        trace_srv_skipped_ = dump_ledger_.srvSkipped(); // clarification 50, for the trace's end
        trace_cb_skipped_ = dump_ledger_.cbSkipped();   // clarification 58
        dump_ready_.push_back({FrameDumpDirectory(dump_present_), std::move(dump_index_), std::move(dump_files_)});
        dump_ledger_ = {};
        dump_buffers_.clear();
        dump_texture_ids_.clear();
        dump_texture_types_.clear();
        dump_files_.clear();
        dump_index_.clear();
      }
      // clarification 39: leaving a traced frame, its closing line and counts go into its
      // record, which is handed to this submission like the dump
      if (frame_trace_.active() && trace_record_) {
        std::vector<std::string> closing;
        trace_record_->render_passes = frame_trace_.renderPasses();
        trace_record_->dispatches = frame_trace_.dispatchCount();
        trace_record_->draws = frame_trace_.draws();
        frame_trace_.finish(closing);
        if (trace_srv_skipped_) // clarification 50: over the 300 or the share
          closing.push_back(TraceSrvLeftOut(trace_srv_skipped_));
        if (trace_cb_skipped_) // clarification 58
          closing.push_back(TraceCbLeftOut(trace_cb_skipped_));
        for (auto &line : closing)
          trace_record_->lines.push_back(std::move(line));
        trace_ready_.push_back(std::move(trace_record_));
      }
      trace_record_ = nullptr;
      trace_draw_serial_ = 0; // clarifications 48-50
      trace_draw_indirect_k_ = 0;
      trace_srv_skipped_ = 0;
      trace_cb_skipped_ = 0;
      if (traced) {
        trace_record_ = std::make_shared<FrameTraceRecord>();
        trace_record_->present = present;
        trace_record_->directory = FrameDumpDirectory(present);
      }
      bool requested = FrameDumpRequested(present);
      bool fixed = FrameDumpActive(FrameDumpSwitch(), true, present);
      dump_active_ = traced && (fixed || FrameDumpActive(FrameDumpSwitch(), true, present, requested));
      if (dump_active_) {
        dump_present_ = present;
        if (!requested) // a requested dump counts from its request (FrameDumpCheckRequest)
          FrameDumpStarted();
      }
    }
    std::vector<std::string> unused; // the closing line went into the record above
    frame_trace_.setPresent(traced ? present : 0, unused);
    trace_active_ = traced;
    for (auto &line : log_lines)
      Logger::info(line);
  }
}

/* ---- spec 05 clarification 40: dumps on request ---- */

namespace {

/* the presents requests asked for, read without a lock by both threads */
std::atomic<uint64_t> g_requested_presents[kFrameDumpMaxRequests] = {};
/* dump frames requested or started and not yet written */
std::atomic<int> g_dumps_in_progress{0};
dxmt::mutex g_request_mutex;
FrameDumpRequestPolicy g_request_policy;

class RealFrameDumpRequestFiles : public FrameDumpRequestFiles {
public:
  bool
  read(std::string &line) override {
    FILE *file = std::fopen(kFrameDumpRequestFile, "r");
    if (!file)
      return false;
    char text[256] = {};
    bool ok = std::fgets(text, sizeof(text), file) != nullptr || std::feof(file);
    std::fclose(file);
    if (!ok)
      return false;
    line = text;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
      line.pop_back();
    return true;
  }
  bool
  remove() override {
    return std::remove(kFrameDumpRequestFile) == 0;
  }
};

} // namespace

bool
FrameDumpRequested(uint64_t present) {
  if (!present)
    return false;
  for (auto &requested : g_requested_presents)
    if (requested.load(std::memory_order_acquire) == present)
      return true;
  return false;
}

void
FrameDumpCheckRequest(uint64_t presented) {
  // only in test builds: App Store builds never set either switch, so never look for the file
  if (!FrameDumpSwitch() || !FrameTraceSwitch())
    return;
  if (presented % kFrameDumpRequestInterval != 0)
    return;
  std::lock_guard<dxmt::mutex> lock(g_request_mutex);
  RealFrameDumpRequestFiles files;
  std::string log;
  uint64_t present = g_request_policy.check(
      presented, true, g_dumps_in_progress.load(std::memory_order_acquire) > 0, files, log
  );
  if (!log.empty())
    Logger::info(log);
  if (present) {
    g_dumps_in_progress.fetch_add(1, std::memory_order_acq_rel);
    g_requested_presents[g_request_policy.granted() - 1].store(present, std::memory_order_release);
  }
}

void
FrameDumpStarted() {
  g_dumps_in_progress.fetch_add(1, std::memory_order_acq_rel);
}

void
FrameDumpFinished() {
  g_dumps_in_progress.fetch_sub(1, std::memory_order_acq_rel);
}

/* spec 05 clarification 39: on the finish thread, once the frame's command buffers completed */
void
WriteFrameTrace(FrameTraceRecord &record) {
  std::vector<std::string> lines;
  {
    std::lock_guard<std::mutex> lock(record.mutex);
    lines = FrameTraceFileLines(record.lines, record.readbacks, record.draw_anchors);
  }
  std::error_code error;
  std::filesystem::create_directories(record.directory, error);
  std::string path = record.directory + "\\trace.txt";
  if (FILE *out = std::fopen(path.c_str(), "w")) {
    for (auto &line : lines) {
      std::fwrite(line.data(), 1, line.size(), out);
      std::fputc('\n', out);
    }
    std::fclose(out);
  } else {
    WARN("dxmt: frame trace: can't write ", path);
  }
  Logger::info(FrameTraceSummary(
      record.present, lines.size(), record.render_passes, record.dispatches, record.draws, record.directory
  ));
}

/* spec 05 clarification 21: the one place the switch is read, on the Windows side, from the
   game's environment (GetEnvironmentVariableW through env::getEnvVar). The app sets it with
   setenv before Wine starts, in test builds only, and Wine copies it into the Windows
   environment. First called when the Direct3D 11 device is created; the result is kept for
   the process, and one line says it is on. */
bool
FrameTraceSwitch() {
  static const bool on = [] {
    bool value = FrameTraceSwitchValue(env::getEnvVar("PG_DXMT_FRAME_TRACE"));
    if (value) // spec 05 clarification 24: the schedule
      Logger::info("dxmt: frame trace on (PG_DXMT_FRAME_TRACE=1): present 600, then every 1200th, at most 8");
    return value;
  }();
  return on;
}

/* spec 05 clarification 28: the dump switch, read once per process like the trace's */
bool
FrameDumpSwitch() {
  static const bool on = [] {
    bool value = FrameTraceSwitchValue(env::getEnvVar("PG_DXMT_FRAME_DUMP"));
    if (value)
      Logger::info("dxmt: frame dump on (PG_DXMT_FRAME_DUMP=1): the traced frame at present 1800, to "
                   "C:\\users\\Public\\pg-dumps");
    return value;
  }();
  return on;
}

/* spec 05 clarifications 28 and 33: on the finish thread, once the dumped frame's command
   buffers have completed */
void
WriteFrameDumpFile(FrameDumpFile &file) {
  std::error_code error;
  std::filesystem::create_directories(file.directory, error);
  std::string path = file.directory + "\\" + file.name;
  if (FILE *out = std::fopen(path.c_str(), "wb")) {
    std::fwrite(file.host, 1, file.bytes, out);
    std::fclose(out);
  } else {
    WARN("dxmt: frame dump: can't write ", path);
  }
  file.buffer = {}; // the Metal buffer goes before the memory it was placed on
  wsi::aligned_free(file.host);
  file.host = nullptr;
}

void
WriteFrameDumpIndex(const std::string &directory, const std::string &text) {
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (FILE *index = std::fopen((directory + "\\index.txt").c_str(), "w")) {
    std::fwrite(text.data(), 1, text.size(), index);
    std::fclose(index);
  }
}

void
FreeFrameDump(FrameDumpReady &ready) {
  for (auto &file : ready.files)
    if (file.host) {
      file.buffer = {};
      wsi::aligned_free(file.host);
      file.host = nullptr;
    }
  ready.files.clear();
}

/* spec 05 clarification 33: a read-back buffer for the dump, placed on memory DXMT allocates.
   Hazard-tracked (unlike DXMT's own), so the copy of a last-state buffer into a first-write
   buffer and the next copy into the last-state one stay in order inside one blit pass. Returns
   the index in dump_files_, or -1. */
int
ArgumentEncodingContext::dumpNewBuffer(const std::string &name, uint64_t bytes) {
  FrameDumpFile file;
  file.directory = FrameDumpDirectory(dump_present_);
  file.name = name;
  file.bytes = bytes;
  file.host = wsi::aligned_malloc(PlacedMemoryLength(bytes), kPlacedMemoryAlignment);
  if (!file.host)
    return -1;
  WMTBufferInfo info;
  info.length = PlacedMemoryLength(bytes);
  info.memory.set(file.host);
  info.options = (WMTResourceOptions)WMTResourceStorageModeShared;
  info.gpu_address = 0;
  file.buffer = device_.newBuffer(info);
  if (!file.buffer) {
    wsi::aligned_free(file.host);
    return -1;
  }
  dump_files_.push_back(std::move(file));
  return int(dump_files_.size() - 1);
}

void
ArgumentEncodingContext::dumpCopyTexture(
    obj_handle_t texture, unsigned mip, unsigned slice, uint64_t width, uint64_t height, unsigned texel,
    WMTBlitOption option, obj_handle_t buffer
) {
  dumpCopyTexture3D(texture, mip, slice, 0, width, height, 1, texel, option, buffer);
}

/* spec 05 clarification 42: `depth` depth slices from `z` of a 3D mip (1 and 0 otherwise),
   slices one after the other in the buffer */
void
ArgumentEncodingContext::dumpCopyTexture3D(
    obj_handle_t texture, unsigned mip, unsigned slice, unsigned z, uint64_t width, uint64_t height, uint64_t depth,
    unsigned texel, WMTBlitOption option, obj_handle_t buffer
) {
  auto &cmd = encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
  cmd.type = WMTBlitCommandCopyFromTextureToBuffer;
  cmd.src = texture;
  cmd.slice = slice;
  cmd.level = mip;
  cmd.origin = {0, 0, z};
  cmd.size = {width, height, depth};
  cmd.dst = buffer;
  cmd.offset = 0;
  cmd.bytes_per_row = width * texel;
  cmd.bytes_per_image = width * height * texel;
  cmd.options = option; // a plane of a depth-stencil texture (clarification 31)
}

unsigned
ArgumentEncodingContext::dumpTextureId(obj_handle_t texture) {
  auto found = dump_texture_ids_.find(texture);
  if (found != dump_texture_ids_.end())
    return found->second;
  unsigned id = unsigned(dump_texture_ids_.size());
  dump_texture_ids_[texture] = id;
  return id;
}

/* spec 05 clarifications 33 and 34: describes the writes of one writer (a render pass, a
   dispatch, a copy, a clear, a resolve) in the index, and copies each dumped subresource into
   its buffer. In the open blit pass, or in one of its own right after the writer. */
void
ArgumentEncodingContext::dumpWrites(const std::string &writer, std::vector<DumpWrite> &writes, EncoderData *source) {
  struct Copy {
    obj_handle_t texture;
    unsigned mip, slice, z;
    uint64_t width, height, depth;
    unsigned texel;
    WMTBlitOption option;
    int last;
    int first; // -1 unless this write takes the first-write copy
    TextureAllocation *allocation;
  };
  /* one file's worth of a write: a layer, a whole 3D mip, or one depth slice of a large one */
  struct Part {
    unsigned key_slice; // the ledger key's slice: the layer, kFrameDumpWholeVolume, or the z
    unsigned slice, z;
    uint64_t depth;
    std::string layer; // for the index
    bool volume;       // named as a whole 3D mip
  };
  std::vector<Copy> copies;
  for (auto &write : writes) {
    auto type = write.type;
    auto layers = FrameDumpLayersOf(type);
    unsigned id = dumpTextureId(write.texture);
    FrameDumpPlane planes[2];
    unsigned plane_count = FrameDumpDepthPlanes(write.format, planes);
    if (!plane_count) {
      planes[0] = {"", nullptr, FrameDumpBytesPerTexel(write.format), WMTBlitOptionNone};
      plane_count = 1;
    }
    for (unsigned p = 0; p < plane_count; p++) {
      auto &plane = planes[p];
      std::string label = plane.format_label ? plane.format_label : TracePixelFormat(write.format);
      std::string slot = write.slot + (plane.slot[0] ? std::string(write.slot.empty() ? "" : " ") + plane.slot : "");
      FrameDumpReason write_reason = write.reason;
      if (write_reason == FrameDumpReason::Dumped && layers == FrameDumpLayers::NotDumped)
        write_reason = FrameDumpReason::Type;
      if (write_reason == FrameDumpReason::Dumped && !plane.bytes_per_texel)
        write_reason = FrameDumpReason::Format;
      // clarification 42: a 3D mip whole when it fits 16 MB, else depth slices 0, d/2, d-1
      std::vector<Part> parts;
      std::string size = FrameDumpSizeText(write.width, write.height);
      if (layers == FrameDumpLayers::Z0) {
        uint64_t depth = std::max<uint64_t>(write.depth, 1);
        size = FrameDumpSizeText(write.width, write.height, depth);
        auto plan = FrameDumpPlan3D(write.width, write.height, depth, std::max(plane.bytes_per_texel, 1u));
        if (plan.whole) {
          parts.push_back({kFrameDumpWholeVolume, 0, 0, depth, "mip" + std::to_string(write.mip) + "-3d", true});
        } else {
          std::string note = " (slices";
          for (unsigned i = 0; i < plan.count; i++)
            note += (i ? ", " : " ") + std::to_string(plan.slices[i]);
          note += " of " + std::to_string(depth) + ")";
          for (unsigned i = 0; i < plan.count; i++)
            parts.push_back(
                {plan.slices[i], 0, plan.slices[i], 1, FrameDumpDepthSliceName(write.mip, plan.slices[i]) + note, false}
            );
        }
      } else {
        parts.push_back(
            {write.slice, write.slice, 0, 1,
             FrameDumpLayerName(layers, type == WMTTextureTypeCubeArray, write.mip, write.slice), false}
        );
      }
      for (auto &part : parts) {
        FrameDumpReason reason = write_reason;
        auto file_name = [&](bool first) {
          if (part.volume)
            return FrameDumpVolumeName(id, label, write.width, write.height, part.depth, write.mip, first);
          std::string layer = part.layer.substr(0, part.layer.find(' ')); // without the note
          return FrameDumpSubresourceName(id, label, write.width, write.height, layer, first);
        };
        std::string name = file_name(false);
        if (reason == FrameDumpReason::Dumped) {
          uint64_t bytes = write.width * write.height * part.depth * plane.bytes_per_texel;
          FrameDumpKey key{write.texture, write.mip, part.key_slice, p};
          auto action = dump_ledger_.write(key, bytes);
          switch (action) {
          case FrameDumpAction::Skip:
            reason = FrameDumpReason::Budget;
            break;
          case FrameDumpAction::CopyNew: {
            int last = dumpNewBuffer(name, bytes);
            if (last < 0) {
              reason = FrameDumpReason::Budget;
              break;
            }
            dump_buffers_[key] = last;
            copies.push_back(
                {write.texture, write.mip, part.slice, part.z, write.width, write.height, part.depth,
                 plane.bytes_per_texel, plane.option, last, -1, write.allocation}
            );
            break;
          }
          case FrameDumpAction::Copy:
          case FrameDumpAction::CopyWithFirst: {
            auto found = dump_buffers_.find(key);
            if (found == dump_buffers_.end()) {
              reason = FrameDumpReason::Budget;
              break;
            }
            int first = -1;
            if (action == FrameDumpAction::CopyWithFirst)
              first = dumpNewBuffer(file_name(true), bytes);
            copies.push_back(
                {write.texture, write.mip, part.slice, part.z, write.width, write.height, part.depth,
                 plane.bytes_per_texel, plane.option, found->second, first, write.allocation}
            );
            name = dump_files_[found->second].name;
            break;
          }
          }
        }
        dump_index_ += FrameDumpIndexLine(writer, slot, id, label, type, size, part.layer, reason, name);
        dump_index_ += "\n";
      }
    }
  }
  if (copies.empty())
    return;
  bool own_pass = !encoder_current;
  if (own_pass) {
    dump_encoding_ = true;
    startBlitPass();
  }
  if (source) // the pass we copy from wrote these textures: our pass reads them
    encoder_current->tex_read.merge(source->tex_write);
  for (auto &copy : copies) {
    if (copy.allocation)
      trackTexture<false>(copy.allocation, DXMT_ENCODER_RESOURCE_ACESS_READ);
    auto &last = dump_files_[copy.last];
    if (copy.first >= 0) { // keep the state after the first write before it is overwritten
      auto &cmd = encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
      cmd.src = last.buffer;
      cmd.src_offset = 0;
      cmd.dst = dump_files_[copy.first].buffer;
      cmd.dst_offset = 0;
      cmd.copy_length = last.bytes;
    }
    dumpCopyTexture3D(
        copy.texture, copy.mip, copy.slice, copy.z, copy.width, copy.height, copy.depth, copy.texel, copy.option,
        last.buffer
    );
  }
  if (own_pass) {
    endPass();
    dump_encoding_ = false;
  }
}

/* the writes through a texture view: `slices` layers from `slice` at `level` of the view */
void
ArgumentEncodingContext::dumpViewWrites(
    std::vector<DumpWrite> &writes, TextureView *view, unsigned level, unsigned slice, unsigned slices,
    const std::string &slot, FrameDumpReason reason
) {
  auto *texture = view->allocation->descriptor;
  unsigned mip = texture->firstMiplevel(view->key) + level;
  unsigned first = texture->firstArraySlice(view->key) + slice;
  obj_handle_t handle = view->allocation->texture().handle;
  dump_texture_types_[handle] = texture->textureType();
  uint64_t width = std::max(texture->width() >> mip, 1u);
  uint64_t height = std::max(texture->height() >> mip, 1u);
  uint64_t depth = std::max(texture->depth() >> mip, 1u);
  // clarification 42: a 3D texture is one write of its whole mip, whatever W slices the view had
  if (texture->textureType() == WMTTextureType3D) {
    writes.push_back(
        {handle, view->allocation, texture->pixelFormat(), texture->textureType(), width, height, depth, mip, 0, slot,
         reason}
    );
    return;
  }
  for (unsigned s = 0; s < std::max(slices, 1u); s++)
    writes.push_back(
        {handle, view->allocation, texture->pixelFormat(), texture->textureType(), width, height, 1, mip, first + s,
         slot, reason}
    );
}

/* the write of a blit command into `texture` (no view: what Metal says about the texture) */
void
ArgumentEncodingContext::dumpHandleWrite(
    std::vector<DumpWrite> &writes, obj_handle_t texture, unsigned mip, unsigned slice
) {
  WMTTextureType type = WMTTextureType2D;
  if (auto known = dump_texture_types_.find(texture); known != dump_texture_types_.end())
    type = known->second;
  else if (MTLTexture_depth(texture) > 1)
    type = WMTTextureType3D;
  else if (MTLTexture_arrayLength(texture) > 1)
    type = WMTTextureType2DArray;
  uint64_t width = std::max<uint64_t>(MTLTexture_width(texture) >> mip, 1);
  uint64_t height = std::max<uint64_t>(MTLTexture_height(texture) >> mip, 1);
  uint64_t depth = type == WMTTextureType3D ? std::max<uint64_t>(MTLTexture_depth(texture) >> mip, 1) : 1;
  writes.push_back(
      {texture, nullptr, MTLTexture_pixelFormat(texture), type, width, height, depth, mip,
       type == WMTTextureType3D ? 0u : slice, "", FrameDumpReason::Dumped}
  );
}

/* spec 05 clarifications 33 and 34: called by endPass for each pass of a dumped frame (not our
   own copy passes); the textures it wrote, copied in a blit pass right after it */
void
ArgumentEncodingContext::dumpAfterPass(EncoderData *ended) {
  std::vector<DumpWrite> writes;
  std::string writer;
  switch (ended->type) {
  case EncoderType::Render: {
    auto data = static_cast<RenderEncoderData *>(ended);
    writer = "pass " + std::to_string(trace_pass_count_ - 1);
    unsigned layers = std::max<unsigned>(data->render_target_array_length, 1);
    for (unsigned i = 0; i < data->colors.size(); i++) {
      auto &color = data->colors[i];
      if (!color.attachment)
        continue;
      auto reason = color.store_action == WMTStoreActionStore ? FrameDumpReason::Dumped : FrameDumpReason::NotStored;
      dumpViewWrites(writes, color.attachment.ptr(), color.level, color.slice, layers, "c" + std::to_string(i), reason);
      if (color.resolve_attachment) // an MSAA target's resolve
        dumpViewWrites(
            writes, color.resolve_attachment.ptr(), color.resolve_level, color.resolve_slice, 1,
            "c" + std::to_string(i) + " resolve", FrameDumpReason::Dumped
        );
    }
    // depth (and, through its planes, stencil): written unless read-only, dumped when stored
    if (data->depth.attachment && !(data->dsv_readonly_flags & 1)) {
      auto &depth = data->depth;
      auto reason = depth.store_action == WMTStoreActionStore ? FrameDumpReason::Dumped : FrameDumpReason::NotStored;
      dumpViewWrites(writes, depth.attachment.ptr(), depth.level, depth.slice, layers, "", reason);
    } else if (!data->depth.attachment && data->stencil.attachment && !(data->dsv_readonly_flags & 2)) {
      auto &stencil = data->stencil; // a stencil-only target
      auto reason = stencil.store_action == WMTStoreActionStore ? FrameDumpReason::Dumped : FrameDumpReason::NotStored;
      dumpViewWrites(writes, stencil.attachment.ptr(), stencil.level, stencil.slice, layers, "", reason);
    }
    break;
  }
  case EncoderType::Clear: {
    auto data = static_cast<ClearEncoderData *>(ended);
    writer = "clear";
    if (data->attachment)
      dumpViewWrites(writes, data->attachment.ptr(), 0, 0, data->array_length, "", FrameDumpReason::Dumped);
    break;
  }
  case EncoderType::Resolve: {
    auto data = static_cast<ResolveEncoderData *>(ended);
    writer = "resolve";
    if (data->dst)
      dumpViewWrites(writes, data->dst.ptr(), 0, 0, 1, "", FrameDumpReason::Dumped);
    break;
  }
  case EncoderType::Blit: {
    auto data = static_cast<BlitEncoderData *>(ended);
    writer = "copy";
    for (auto cmd = (wmtcmd_base *)&data->cmd_head; cmd; cmd = (wmtcmd_base *)cmd->next.get()) {
      if (cmd->type == WMTBlitCommandCopyFromBufferToTexture) {
        auto body = (wmtcmd_blit_copy_from_buffer_to_texture *)cmd;
        dumpHandleWrite(writes, body->dst, body->level, body->slice);
      } else if (cmd->type == WMTBlitCommandCopyFromTextureToTexture) {
        auto body = (wmtcmd_blit_copy_from_texture_to_texture *)cmd;
        dumpHandleWrite(writes, body->dst, body->dst_level, body->dst_slice);
      }
    }
    break;
  }
  default:
    return;
  }
  if (!writes.empty())
    dumpWrites(writer, writes, ended);
}

/* spec 05 clarification 35: the image about to be presented, outside the budget */
void
ArgumentEncodingContext::dumpPresent(Rc<Texture> &texture) {
  auto allocation = texture->current();
  obj_handle_t handle = allocation->texture().handle;
  auto format = texture->pixelFormat();
  uint64_t width = texture->width(), height = texture->height();
  unsigned texel = FrameDumpBytesPerTexel(format);
  std::string name = FrameDumpPresentName(format, width, height);
  if (!texel || texture->textureType() != WMTTextureType2D) {
    dump_index_ += "present: " + TraceTexture(format, width, height) + " not dumped (format)\n";
    return;
  }
  int file = dumpNewBuffer(name, width * height * texel);
  if (file < 0)
    return;
  dump_index_ += "present: " + TraceTexture(format, width, height) + " -> " + name + "\n";
  dump_encoding_ = true;
  startBlitPass();
  trackTexture<false>(allocation, DXMT_ENCODER_RESOURCE_ACESS_READ);
  dumpCopyTexture(handle, 0, 0, width, height, texel, WMTBlitOptionNone, dump_files_[file].buffer);
  endPass();
  dump_encoding_ = false;
}

void
ArgumentEncodingContext::traceAfterDispatch() {
  // spec 05 clarifications 27 and 28; runs in the blit pass the context opened after the dispatch
  if (!trace_active_ || !trace_last_dispatch_k_)
    return;
  unsigned k = trace_last_dispatch_k_;
  std::vector<DumpWrite> dispatch_writes; // spec 05 clarification 33
  for (auto &uav : trace_cs_uav_bindings_) {
    if (uav.buffer.ptr()) {
      // raw and structured buffers, and typed R32Uint / R32Sint ones
      uint32_t byte_offset = 0, byte_length = 0;
      if (uav.flags & MTL_SM50_SHADER_ARGUMENT_BUFFER) {
        byte_offset = uav.slice.byteOffset;
        byte_length = uav.slice.byteLength;
      } else {
        auto format = uav.buffer->pixelFormat(uav.viewId) & 0xFFFF;
        if (format != WMTPixelFormatR32Uint && format != WMTPixelFormatR32Sint)
          continue;
        byte_offset = uav.slice.firstElement * 4;
        byte_length = uav.slice.elementCount * 4;
      }
      unsigned length = std::min<uint32_t>(byte_length, kFrameTraceUAVReadbackValues * 4) & ~3u;
      if (!length || trace_readback_count_ >= kFrameTraceMaxReadbacks)
        continue;
      trace_readback_count_++;
      auto [src, src_offset] = access(uav.buffer, byte_offset, length, DXMT_ENCODER_RESOURCE_ACESS_READ);
      auto [mapped, readback, readback_offset] = queue_.AllocateArgumentBuffer(seq_id_, kFrameTraceUAVReadbackValues * 4);
      auto &cmd = encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
      cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
      cmd.src = src->buffer();
      cmd.src_offset = src_offset + byte_offset;
      cmd.dst = readback;
      cmd.dst_offset = readback_offset;
      cmd.copy_length = length;
      FrameTraceReadback entry{frame_trace_.present(), k, length / 4, false, mapped};
      entry.uav = true;
      entry.slot = uav.slot;
      trace_readbacks_.push_back(entry);
    } else if (uav.texture.ptr() && dump_active_) {
      // spec 05 clarifications 33 and 34: every layer of the view, into its subresource's buffer
      auto &view = access(uav.texture, uav.viewId, DXMT_ENCODER_RESOURCE_ACESS_READ);
      dumpViewWrites(
          dispatch_writes, &view, 0, 0, uav.texture->arrayLength(uav.viewId), "u" + std::to_string(uav.slot),
          FrameDumpReason::Dumped
      );
    }
  }
  if (!dispatch_writes.empty()) // copied in this blit pass
    dumpWrites("dispatch #" + std::to_string(k) + " " + FrameDumpDispatchTag(trace_cs_hash_), dispatch_writes, nullptr);
}

void
ArgumentEncodingContext::traceSrvReadback(std::vector<TraceSrvBuffer> &&buffers) {
  // spec 05 clarification 50; runs in the blit pass the context opened before the draw, whose
  // serial in the frame is the next one
  if (!trace_active_ || !dump_active_)
    return;
  unsigned serial = trace_draw_serial_ + 1;
  constexpr unsigned bytes = kFrameTraceSrvReadbackValues * 4;
  for (auto &srv : buffers) {
    if (!srv.buffer.ptr())
      continue;
    unsigned length = std::min<uint32_t>(srv.byte_length, bytes) & ~3u;
    if (!length)
      continue;
    // at most 300 a frame, out of the dump's 64 MB share (clarification 41)
    if (!dump_ledger_.takeSrvReadback(bytes))
      continue;
    auto [src, src_offset] = access(srv.buffer, srv.byte_offset, length, DXMT_ENCODER_RESOURCE_ACESS_READ);
    auto [mapped, readback, readback_offset] = queue_.AllocateArgumentBuffer(seq_id_, bytes);
    auto &cmd = encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
    cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
    cmd.src = src->buffer();
    cmd.src_offset = src_offset + srv.byte_offset;
    cmd.dst = readback;
    cmd.dst_offset = readback_offset;
    cmd.copy_length = length;
    FrameTraceReadback entry{frame_trace_.present(), 0, length / 4, false, mapped};
    entry.srv = true;
    entry.slot = srv.slot;
    entry.serial = serial;
    trace_readbacks_.push_back(entry);
  }
}

void
ArgumentEncodingContext::traceCbReadback(std::vector<TraceCbBuffer> &&buffers) {
  // spec 05 clarification 58; the same blit pass as traceSrvReadback, before the same draw
  if (!trace_active_ || !dump_active_)
    return;
  unsigned serial = trace_draw_serial_ + 1;
  for (auto &cb : buffers) {
    if (!cb.buffer.ptr())
      continue;
    unsigned length = std::min<uint32_t>(cb.byte_length, kFrameTraceCbReadbackMaxBytes) & ~3u;
    if (!length)
      continue;
    // at most 300 a frame (apart from the views'), out of the dump's 64 MB share
    if (!dump_ledger_.takeCbReadback(length))
      continue;
    auto [src, src_offset] = access(cb.buffer, cb.byte_offset, length, DXMT_ENCODER_RESOURCE_ACESS_READ);
    auto [mapped, readback, readback_offset] = queue_.AllocateArgumentBuffer(seq_id_, length);
    auto &cmd = encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
    cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
    cmd.src = src->buffer();
    cmd.src_offset = src_offset + cb.byte_offset;
    cmd.dst = readback;
    cmd.dst_offset = readback_offset;
    cmd.copy_length = length;
    FrameTraceReadback entry{frame_trace_.present(), 0, length / 4, false, mapped};
    entry.cb = true;
    entry.slot = cb.slot;
    entry.serial = serial;
    entry.bytes = length;
    entry.cpu_hash_valid = cb.cpu_hash_valid;
    entry.cpu_hash = cb.cpu_hash;
    trace_readbacks_.push_back(entry);
  }
}

void
ArgumentEncodingContext::traceIndirectReadback(Rc<Buffer> const &args, unsigned offset, unsigned length, bool indexed) {
  // spec 05 clarification 24; runs in the blit pass the context opened for it
  if (!trace_active_ || trace_readback_count_ >= kFrameTraceMaxReadbacks)
    return;
  trace_readback_count_++;
  unsigned k = frame_trace_.nextIndirect();
  trace_pending_indirect_ = k;
  auto [src, src_offset] = access(args, offset, length, DXMT_ENCODER_RESOURCE_ACESS_READ);
  auto [mapped, readback, readback_offset] = queue_.AllocateArgumentBuffer(seq_id_, 32);
  auto &cmd = encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
  cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
  cmd.src = src->buffer();
  cmd.src_offset = src_offset + offset;
  cmd.dst = readback;
  cmd.dst_offset = readback_offset;
  cmd.copy_length = length;
  trace_readbacks_.push_back({frame_trace_.present(), k, length / 4, indexed, mapped});
}

void
ArgumentEncodingContext::traceIndirect(
    const void *cmd, BufferAllocation *allocation, uint64_t buffer_bytes, unsigned offset, const char *what,
    bool dispatch
) {
  // spec 05 clarification 24
  if (!trace_active_)
    return;
  // clarification 48: an indirect draw already took its number in traceDraw
  unsigned k = 0;
  if (!dispatch && trace_draw_indirect_k_)
    k = trace_draw_indirect_k_;
  else if (trace_pending_indirect_)
    k = trace_pending_indirect_;
  else
    k = frame_trace_.nextIndirect();
  trace_pending_indirect_ = 0;
  if (!dispatch)
    trace_draw_indirect_k_ = 0;
  int written = -1;
  if (dispatch) {
    // DXMT's own view: an encoder of this command buffer (the open one included) whose write
    // set has the argument buffer's key. A Bloom filter, so "yes" may be a false positive;
    // GPU-read-only buffers are never tracked, so they read "no".
    EncoderDepSet probe;
    probe.add(allocation->depkey);
    written = 0;
    for (EncoderData *e = encoder_head.next; e && !written; e = e->next)
      written = !probe.isDisjointWith(e->buf_write);
    if (!written && encoder_current)
      written = !probe.isDisjointWith(encoder_current->buf_write);
  }
  trace_indirect_notes_[cmd] = TraceIndirect(k, what, buffer_bytes, offset, written);
}

void
ArgumentEncodingContext::traceEncoders(EncoderData **encoders, unsigned count) {
  std::vector<std::string> out;
  for (unsigned i = 0; i < count; i++) {
    auto current = encoders[i];
    switch (current->type) {
    case EncoderType::Render:
      traceRenderPass(static_cast<RenderEncoderData *>(current), out);
      break;
    case EncoderType::Compute:
      traceComputePass(static_cast<ComputeEncoderData *>(current), out);
      break;
    case EncoderType::Blit:
      traceBlitPass(static_cast<BlitEncoderData *>(current), out);
      break;
    case EncoderType::Clear: {
      // a clear not merged into the next render pass (merged ones show as its "clear" load)
      auto data = static_cast<ClearEncoderData *>(current);
      auto texture = data->attachment.texture();
      std::string what = data->clear_dsv ? "clear depth-stencil " : "clear color ";
      frame_trace_.line(
          what + TraceTexture(texture.pixelFormat(), data->width, data->height) + " slices " +
              std::to_string(data->array_length) + " rect " + TraceRect(0, 0, data->width, data->height),
          out
      );
      break;
    }
    case EncoderType::Resolve: {
      auto data = static_cast<ResolveEncoderData *>(current);
      auto src = data->src.texture();
      auto dst = data->dst.texture();
      std::string line = std::string("resolve ") + (data->is_depth ? "depth " : "") +
                         TraceTexture(src.pixelFormat(), src.width(), src.height()) + " -> " +
                         TraceTexture(dst.pixelFormat(), dst.width(), dst.height());
      if (data->src_rect)
        line += " rect " + TraceRect(data->src_rect->x, data->src_rect->y, data->src_rect->width, data->src_rect->height);
      frame_trace_.line(line, out);
      break;
    }
    case EncoderType::Present:
      frame_trace_.line("present", out);
      break;
    default:
      break; // merged (Null), events, upscales, stretch blits
    }
  }
  trace_dispatch_notes_.clear();
  trace_indirect_notes_.clear();
  trace_state_notes_.clear(); // spec 05 clarification 30
  trace_draw_notes_.clear(); // spec 05 clarification 44
  // spec 05 clarification 39: into the frame's trace.txt, not the log
  if (trace_record_)
    for (auto &line : out)
      trace_record_->lines.push_back(std::move(line));
}

static std::string
TraceTextureHandle(obj_handle_t texture) {
  if (!texture)
    return "none";
  return TraceTexture(MTLTexture_pixelFormat(texture), MTLTexture_width(texture), MTLTexture_height(texture));
}

void
ArgumentEncodingContext::traceRenderPass(RenderEncoderData *data, std::vector<std::string> &out) {
  std::vector<std::string> attachments, viewports, scissors;
  uint32_t width = data->render_target_width, height = data->render_target_height;
  // spec 05 clarification 44: each attachment's texture type, as its view sees it
  auto type_of = [](const TextureViewRef &ref) {
    auto *view = ref.ptr();
    return TraceTextureType(view->allocation->descriptor->textureType(view->key));
  };
  for (unsigned i = 0; i < data->colors.size(); i++) {
    auto &color = data->colors[i];
    if (!color.attachment)
      continue;
    auto texture = color.attachment.texture();
    if (!width) {
      width = texture.width();
      height = texture.height();
    }
    attachments.push_back(TraceAttachment(
        "c" + std::to_string(i), texture.pixelFormat(), texture.width(), texture.height(), color.level, color.slice,
        color.load_action, type_of(color.attachment)
    ));
  }
  if (data->depth.attachment) {
    auto texture = data->depth.attachment.texture();
    attachments.push_back(TraceAttachment(
        "depth", texture.pixelFormat(), texture.width(), texture.height(), data->depth.level, data->depth.slice,
        data->depth.load_action, type_of(data->depth.attachment)
    ));
  }
  if (data->stencil.attachment) {
    auto texture = data->stencil.attachment.texture();
    attachments.push_back(TraceAttachment(
        "stencil", texture.pixelFormat(), texture.width(), texture.height(), data->stencil.level, data->stencil.slice,
        data->stencil.load_action, type_of(data->stencil.attachment)
    ));
  }

  // the viewports and scissor rectangles in effect at each draw
  unsigned draws = 0;
  bool partial = false;
  std::vector<std::string> indirect_lines;
  std::vector<WMTViewport> current_viewports;
  std::vector<WMTScissorRect> current_scissors;
  for (auto cmd = (wmtcmd_base *)&data->cmd_head; cmd; cmd = (wmtcmd_base *)cmd->next.get()) {
    switch ((WMTRenderCommandType)cmd->type) {
    case WMTRenderCommandSetViewports: {
      auto body = (wmtcmd_render_setviewports *)cmd;
      auto first = (WMTViewport *)body->viewports.get();
      current_viewports.assign(first, first + body->viewport_count);
      break;
    }
    case WMTRenderCommandSetViewport:
      current_viewports.assign(1, ((wmtcmd_render_setviewport *)cmd)->viewport);
      break;
    case WMTRenderCommandSetScissorRects: {
      auto body = (wmtcmd_render_setscissorrects *)cmd;
      auto first = (WMTScissorRect *)body->scissor_rects.get();
      current_scissors.assign(first, first + body->rect_count);
      break;
    }
    case WMTRenderCommandSetScissorRect:
      current_scissors.assign(1, ((wmtcmd_render_setscissorrect *)cmd)->scissor_rect);
      break;
    case WMTRenderCommandDraw:
    case WMTRenderCommandDrawIndexed:
    case WMTRenderCommandDrawIndirect:
    case WMTRenderCommandDrawIndexedIndirect:
    case WMTRenderCommandDrawMeshThreadgroups:
    case WMTRenderCommandDrawMeshThreadgroupsIndirect:
    case WMTRenderCommandDXMTGeometryDraw:
    case WMTRenderCommandDXMTGeometryDrawIndexed:
    case WMTRenderCommandDXMTGeometryDrawIndirect:
    case WMTRenderCommandDXMTGeometryDrawIndexedIndirect:
    case WMTRenderCommandDXMTTessellationMeshDraw:
    case WMTRenderCommandDXMTTessellationMeshDrawIndexed:
    case WMTRenderCommandDXMTTessellationMeshDrawIndirect:
    case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect:
      draws++;
      if (auto note = trace_indirect_notes_.find(cmd); note != trace_indirect_notes_.end())
        indirect_lines.push_back(note->second); // spec 05 clarification 24
      for (auto &v : current_viewports) {
        TraceAddDistinct(viewports, TraceRect(v.originX, v.originY, v.width, v.height));
        if (!TraceCovers(v.originX, v.originY, v.width, v.height, width, height))
          partial = true;
      }
      for (auto &s : current_scissors) {
        TraceAddDistinct(scissors, TraceRect(s.x, s.y, s.width, s.height));
        if (!TraceCovers(s.x, s.y, s.width, s.height, width, height))
          partial = true;
      }
      break;
    default:
      break;
    }
  }
  unsigned index = frame_trace_.nextPass();
  // clarification 44: the layer count (0 when not layered)
  int layers = data->render_target_array_length > 1 ? int(data->render_target_array_length) : 0;
  frame_trace_.line(TraceRenderPass(index, attachments, draws, viewports, scissors, partial, layers), out);
  frame_trace_.countRenderPass(draws); // spec 05 clarification 39
  // spec 05 clarification 30: the distinct states the pass's draws used
  if (auto states = trace_state_notes_.find(data); states != trace_state_notes_.end())
    for (auto &state : states->second)
      frame_trace_.line("pass " + std::to_string(index) + " " + state, out);
  // clarifications 44 and 49: each draw, in order: its line, then its bindings' lines; 50: where
  // its block ends in the frame's lines, for its vertex-shader buffer read-backs
  if (auto drawn = trace_draw_notes_.find(data); drawn != trace_draw_notes_.end()) {
    for (size_t k = 0; k < drawn->second.size(); k++) {
      std::string name = "pass " + std::to_string(index) + " draw " + std::to_string(k);
      auto &text = drawn->second[k].lines;
      size_t begin = 0;
      bool first = true;
      while (begin <= text.size()) {
        size_t end = text.find('\n', begin);
        std::string part = text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        frame_trace_.line(name + (first ? ": " : " ") + part, out);
        first = false;
        if (end == std::string::npos)
          break;
        begin = end + 1;
      }
      if (trace_record_)
        trace_record_->draw_anchors[drawn->second[k].serial] = {trace_record_->lines.size() + out.size() - 1, name};
    }
  }
  for (auto &line : indirect_lines)
    frame_trace_.line(line, out);
}

void
ArgumentEncodingContext::traceComputePass(ComputeEncoderData *data, std::vector<std::string> &out) {
  frame_trace_.line("compute pass " + std::to_string(frame_trace_.nextPass()), out);
  WMTSize tg{0, 0, 0};
  for (auto cmd = (wmtcmd_base *)&data->cmd_head; cmd; cmd = (wmtcmd_base *)cmd->next.get()) {
    switch ((WMTComputeCommandType)cmd->type) {
    case WMTComputeCommandSetPSO:
      tg = ((wmtcmd_compute_setpso *)cmd)->threadgroup_size;
      break;
    case WMTComputeCommandDispatch:
    case WMTComputeCommandDispatchThreads:
    case WMTComputeCommandDispatchIndirect: {
      // a dispatch from a Direct3D 11 shader has a note; DXMT's own kernels (clears) don't
      auto note = trace_dispatch_notes_.find(cmd);
      uint32_t hash = note != trace_dispatch_notes_.end() ? note->second.hash : 0;
      std::string uavs = note != trace_dispatch_notes_.end() ? note->second.uavs : std::string();
      unsigned k = note != trace_dispatch_notes_.end() ? note->second.k : 0; // spec 05 clarification 27
      frame_trace_.countDispatch();                                        // spec 05 clarification 39
      if (cmd->type == WMTComputeCommandDispatchIndirect) {
        frame_trace_.line(TraceDispatch(k, hash, tg.width, tg.height, tg.depth, true, 0, 0, 0, uavs), out);
        if (auto indirect = trace_indirect_notes_.find(cmd); indirect != trace_indirect_notes_.end())
          frame_trace_.line(indirect->second, out); // spec 05 clarification 24
      } else {
        auto size = ((wmtcmd_compute_dispatch *)cmd)->size;
        frame_trace_.line(
            TraceDispatch(k, hash, tg.width, tg.height, tg.depth, false, size.width, size.height, size.depth, uavs), out
        );
      }
      // spec 05 clarification 30: the dispatch's samplers
      if (note != trace_dispatch_notes_.end())
        for (auto &sampler : note->second.samplers)
          frame_trace_.line("dispatch #" + std::to_string(k) + " " + sampler, out);
      break;
    }
    default:
      break;
    }
  }
}

void
ArgumentEncodingContext::traceBlitPass(BlitEncoderData *data, std::vector<std::string> &out) {
  for (auto cmd = (wmtcmd_base *)&data->cmd_head; cmd; cmd = (wmtcmd_base *)cmd->next.get()) {
    switch ((WMTBlitCommandType)cmd->type) {
    case WMTBlitCommandCopyFromBufferToBuffer: {
      auto body = (wmtcmd_blit_copy_from_buffer_to_buffer *)cmd;
      frame_trace_.line(
          "copy buffer -> buffer " + std::to_string(body->copy_length) + " bytes at " +
              std::to_string(body->src_offset) + " -> " + std::to_string(body->dst_offset),
          out
      );
      break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      // UpdateSubresource and copies from staging buffers
      auto body = (wmtcmd_blit_copy_from_buffer_to_texture *)cmd;
      frame_trace_.line(
          "copy buffer -> " + TraceTextureHandle(body->dst) + " mip " + std::to_string(body->level) + " slice " +
              std::to_string(body->slice) + " box " +
              TraceBox(body->origin.x, body->origin.y, body->origin.z, body->size.width, body->size.height,
                       body->size.depth),
          out
      );
      break;
    }
    case WMTBlitCommandCopyFromTextureToBuffer: {
      auto body = (wmtcmd_blit_copy_from_texture_to_buffer *)cmd;
      frame_trace_.line(
          "copy " + TraceTextureHandle(body->src) + " mip " + std::to_string(body->level) + " slice " +
              std::to_string(body->slice) + " box " +
              TraceBox(body->origin.x, body->origin.y, body->origin.z, body->size.width, body->size.height,
                       body->size.depth) +
              " -> buffer",
          out
      );
      break;
    }
    case WMTBlitCommandCopyFromTextureToTexture: {
      auto body = (wmtcmd_blit_copy_from_texture_to_texture *)cmd;
      frame_trace_.line(
          "copy " + TraceTextureHandle(body->src) + " mip " + std::to_string(body->src_level) + " slice " +
              std::to_string(body->src_slice) + " box " +
              TraceBox(body->src_origin.x, body->src_origin.y, body->src_origin.z, body->src_size.width,
                       body->src_size.height, body->src_size.depth) +
              " -> " + TraceTextureHandle(body->dst) + " mip " + std::to_string(body->dst_level) + " slice " +
              std::to_string(body->dst_slice) + " at " + std::to_string(body->dst_origin.x) + "," +
              std::to_string(body->dst_origin.y) + "," + std::to_string(body->dst_origin.z),
          out
      );
      break;
    }
    case WMTBlitCommandGenerateMipmaps:
      frame_trace_.line("generate mipmaps " + TraceTextureHandle(((wmtcmd_blit_generate_mipmaps *)cmd)->texture), out);
      break;
    case WMTBlitCommandFillBuffer: {
      auto body = (wmtcmd_blit_fillbuffer *)cmd;
      frame_trace_.line("fill buffer " + std::to_string(body->length) + " bytes", out);
      break;
    }
    default:
      break;
    }
  }
}

constexpr unsigned kEncoderOptimizerThreshold = 64;

/* MYIOSDECK spec 05 clarification 57, level 4: every encoder waits for the fence the encoder
   before it updated (all stages), so no two passes overlap on the GPU. Present, MetalFX and
   event encoders aren't DXMT's own encoders and take no fence. */
void
ArgumentEncodingContext::serialWait(WMT::RenderCommandEncoder &encoder) {
  if (serialize_fence_ != nullptr)
    encoder.waitForFence(
        serialize_fence_, WMTRenderStages(WMTRenderStageVertex | WMTRenderStageObject | WMTRenderStageMesh)
    );
}

void
ArgumentEncodingContext::serialWait(WMT::ComputeCommandEncoder &encoder) {
  if (serialize_fence_ != nullptr)
    encoder.waitForFence(serialize_fence_);
}

void
ArgumentEncodingContext::serialWait(WMT::BlitCommandEncoder &encoder) {
  if (serialize_fence_ != nullptr)
    encoder.waitForFence(serialize_fence_);
}

void
ArgumentEncodingContext::serialUpdate(WMT::RenderCommandEncoder &encoder) {
  if (serialize_fence_ != nullptr)
    encoder.updateFence(serialize_fence_, WMTRenderStageFragment);
}

void
ArgumentEncodingContext::serialUpdate(WMT::ComputeCommandEncoder &encoder) {
  if (serialize_fence_ != nullptr)
    encoder.updateFence(serialize_fence_);
}

void
ArgumentEncodingContext::serialUpdate(WMT::BlitCommandEncoder &encoder) {
  if (serialize_fence_ != nullptr)
    encoder.updateFence(serialize_fence_);
}

std::unique_ptr<VisibilityResultReadback>
ArgumentEncodingContext::flushCommands(WMT::CommandBuffer cmdbuf, uint64_t seqId, uint64_t event_seq_id) {
  assert(!encoder_current);

  unsigned encoder_count = encoder_count_;
  unsigned encoder_index = 0;
  EncoderData **encoders =
      reinterpret_cast<EncoderData **>(allocate_cpu_heap(sizeof(EncoderData *) * encoder_count, alignof(EncoderData *))
      );

  {
    EncoderData *current = encoder_head.next;
    while (current) {
      encoders[encoder_index++] = current;
      current = current->next;
    }
    assert(encoder_index == encoder_count);
  }

  // the upstream loop, moved into dxmt_serialize.hpp unchanged so a host test can drive it
  // (spec 05 clarification 57; checkEncoderRelation keeps the order from level 1 on)
  OptimizeEncoderList(
      encoders, encoder_count, kEncoderOptimizerThreshold,
      [this](EncoderData *former, EncoderData *latter) {
        return checkEncoderRelation(former, latter) == DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
      },
      [](EncoderData *encoder) { return encoder->type == EncoderType::Null; }
  );

  // MYIOSDECK spec 05 clarification 21: the traced frame's passes, in submission order, after
  // clears were merged into the passes they precede (so their load actions are final)
  if (trace_active_)
    traceEncoders(encoders, encoder_count);

  std::unique_ptr<VisibilityResultReadback> visibility_readback {};

  if (auto count = vro_state_.reset()) {
    visibility_readback = std::make_unique<VisibilityResultReadback>(
        device_, seqId, count, pending_queries_
    );
  }
#ifdef DXMT_IOS
  else {
    // Spec 05 H1 (DXMT_QUERY_SETTLE_EMPTY): a submission with no visibility
    // samples builds no readback, so a query that ends in it would never
    // complete. Attach an empty readback for those queries. Default on in
    // every build since spec 05 clarification 10 (steamac's DXMT patch 0004
    // completes such a query unconditionally); before, only in the i386 build.
    static const bool settle_empty = env::readSwitch("DXMT_QUERY_SETTLE_EMPTY", true);
    if (settle_empty) {
      auto ending = QueriesEndingAt(pending_queries_, seqId);
      if (!ending.empty())
        visibility_readback = std::make_unique<VisibilityResultReadback>(seqId, std::move(ending));
    }
  }
#endif
  std::erase_if(pending_queries_, [=](auto &query) -> bool { return query->queryEndAt() == seqId; });

  while (encoder_index) {
    auto current = encoders[encoder_count - encoder_index];
    switch (current->type) {
    case EncoderType::Render: {
      auto data = static_cast<RenderEncoderData *>(current);
      WMTRenderPassInfo render_pass_info;
      WMT::InitializeRenderPassInfo(render_pass_info);
      {
        for (unsigned i = 0; i < std::size(render_pass_info.colors); i++) {
          auto &color_data = data->colors[i];
          if (!color_data.attachment)
            continue;
          auto &color_info = render_pass_info.colors[i];
          color_info.texture = color_data.attachment.texture();
          color_info.load_action = color_data.load_action;
          color_info.store_action = color_data.store_action;
          color_info.level = color_data.level;
          color_info.slice = color_data.slice;
          color_info.depth_plane = color_data.depth_plane;
          color_info.clear_color = color_data.clear_color;
          color_info.resolve_texture = color_data.resolve_attachment.texture();
          color_info.resolve_level = color_data.resolve_level;
          color_info.resolve_slice = color_data.resolve_slice;
          color_info.resolve_depth_plane = color_data.resolve_depth_plane;
        }
        if (data->depth.attachment) {
          auto &depth_info = render_pass_info.depth;
          auto &depth_data = data->depth;
          depth_info.texture = depth_data.attachment.texture();
          depth_info.load_action = depth_data.load_action;
          depth_info.store_action = depth_data.store_action;
          depth_info.level = depth_data.level;
          depth_info.slice = depth_data.slice;
          depth_info.depth_plane = depth_data.depth_plane;
          depth_info.clear_depth = depth_data.clear_depth;
        }
        if (data->stencil.attachment) {
          auto &stencil_info = render_pass_info.stencil;
          auto &stencil_data = data->stencil;
          stencil_info.texture = stencil_data.attachment.texture();
          stencil_info.load_action = stencil_data.load_action;
          stencil_info.store_action = stencil_data.store_action;
          stencil_info.level = stencil_data.level;
          stencil_info.slice = stencil_data.slice;
          stencil_info.depth_plane = stencil_data.depth_plane;
          stencil_info.clear_stencil = stencil_data.clear_stencil;
        }
        render_pass_info.default_raster_sample_count = data->default_raster_sample_count;
        render_pass_info.render_target_array_length = data->render_target_array_length;
        render_pass_info.render_target_width = data->render_target_width;
        render_pass_info.render_target_height = data->render_target_height;
      }
      if (data->use_visibility_result) {
        assert(visibility_readback);
        render_pass_info.visibility_buffer = visibility_readback->visibility_result_heap;
      }
      auto gpu_buffer_ = data->allocated_argbuf;
      auto encoder = cmdbuf.renderCommandEncoder(render_pass_info);
      serialWait(encoder); // spec 05 clarification 57 (level 4)
      encoder.setVertexBuffer(gpu_buffer_, 0, 16);
      encoder.setVertexBuffer(gpu_buffer_, 0, 29);
      encoder.setVertexBuffer(gpu_buffer_, 0, 30);
      encoder.setFragmentBuffer(gpu_buffer_, 0, 29);
      encoder.setFragmentBuffer(gpu_buffer_, 0, 30);
      if (data->use_geometry || data->use_tessellation) {
        encoder.setObjectBuffer(gpu_buffer_, 0, 16);
        encoder.setObjectBuffer(gpu_buffer_, 0, 21); // draw arguments
        if (data->use_tessellation) {
          encoder.setObjectBuffer(gpu_buffer_, 0, 27);
          encoder.setObjectBuffer(gpu_buffer_, 0, 28);
        }
        encoder.setObjectBuffer(gpu_buffer_, 0, 29);
        encoder.setObjectBuffer(gpu_buffer_, 0, 30);
        encoder.setMeshBuffer(gpu_buffer_, 0, 29);
        encoder.setMeshBuffer(gpu_buffer_, 0, 30);
      }
      if (data->gs_arg_marshal_tasks.size()) {
        auto task_count = data->gs_arg_marshal_tasks.size();
        struct GS_MARSHAL_TASK {
          uint64_t draw_args;
          uint64_t dispatch_args_out;
          uint64_t max_object_threadgroups;
          uint32_t vertex_count_per_warp;
          uint32_t end_of_command;
        };
        auto [mapped_task_data, task_data_buffer, task_data_buffer_offset] =
            queue_.AllocateArgumentBuffer(seq_id_, sizeof(GS_MARSHAL_TASK) * task_count);
        auto tasks_data = (GS_MARSHAL_TASK *)mapped_task_data;
        for (unsigned i = 0; i<task_count; i++) {
          auto & task = data->gs_arg_marshal_tasks[i];
          tasks_data[i].draw_args = task.draw_arguments_va;
          tasks_data[i].dispatch_args_out = task.dispatch_arguments_va;
          tasks_data[i].max_object_threadgroups = task.max_object_threadgroups;
          tasks_data[i].vertex_count_per_warp = task.vertex_count_per_warp;
          tasks_data[i].end_of_command = 0;
          encoder.useResource(task.draw_arguments, WMTResourceUsageRead, WMTRenderStageVertex);
          encoder.useResource(task.dispatch_arguments_buffer, WMTResourceUsageWrite, WMTRenderStageVertex);
        }
        tasks_data[task_count - 1].end_of_command = 1;
        emulated_cmd.MarshalGSDispatchArguments(encoder, task_data_buffer, task_data_buffer_offset);
      }
      if (data->ts_arg_marshal_tasks.size()) {
        auto task_count = data->ts_arg_marshal_tasks.size();
        struct TS_MARSHAL_TASK {
          uint64_t draw_args;
          uint64_t dispatch_args_out;
          uint64_t max_object_threadgroups;
          uint16_t control_point_count;
          uint16_t patch_per_group;
          uint32_t end_of_command;
        };
        auto [mapped_task_data, task_data_buffer, task_data_buffer_offset] =
            queue_.AllocateArgumentBuffer(seq_id_, sizeof(TS_MARSHAL_TASK) * task_count);
        auto tasks_data = (TS_MARSHAL_TASK *)mapped_task_data;
        for (unsigned i = 0; i<task_count; i++) {
          auto & task = data->ts_arg_marshal_tasks[i];
          tasks_data[i].draw_args = task.draw_arguments_va;
          tasks_data[i].dispatch_args_out = task.dispatch_arguments_va;
          tasks_data[i].max_object_threadgroups = task.max_object_threadgroups;
          tasks_data[i].control_point_count = task.control_point_count;
          tasks_data[i].patch_per_group = task.patch_per_group;
          tasks_data[i].end_of_command = 0;
          encoder.useResource(task.draw_arguments, WMTResourceUsageRead, WMTRenderStageVertex);
          encoder.useResource(task.dispatch_arguments_buffer, WMTResourceUsageWrite, WMTRenderStageVertex);
        }
        tasks_data[task_count - 1].end_of_command = 1;
        emulated_cmd.MarshalTSDispatchArguments(encoder, task_data_buffer, task_data_buffer_offset);
      }
      if (data->gs_arg_marshal_tasks.size() > 0 || data->ts_arg_marshal_tasks.size() > 0) {
        encoder.memoryBarrier(
            WMTBarrierScopeBuffers, WMTRenderStageVertex,
            WMTRenderStageVertex | WMTRenderStageMesh | WMTRenderStageObject
        );
      }
      encoder.encodeCommands(&data->cmd_head);
      serialUpdate(encoder);
      encoder.endEncoding();
      data->~RenderEncoderData();
      break;
    }
    case EncoderType::Compute: {
      auto data = static_cast<ComputeEncoderData *>(current);
      auto encoder = cmdbuf.computeCommandEncoder(false);
      serialWait(encoder); // spec 05 clarification 57 (level 4)
      struct wmtcmd_compute_setbuffer setcmd;
      setcmd.type = WMTComputeCommandSetBuffer;
      setcmd.next.set(nullptr);
      setcmd.buffer = data->allocated_argbuf;
      setcmd.offset = 0;
      setcmd.index = 29;
      encoder.encodeCommands((const wmtcmd_compute_nop *)&setcmd);
      setcmd.index = 30;
      encoder.encodeCommands((const wmtcmd_compute_nop *)&setcmd);
      encoder.encodeCommands(&data->cmd_head);
      serialUpdate(encoder);
      encoder.endEncoding();
      data->~ComputeEncoderData();
      break;
    }
    case EncoderType::Blit: {
      auto data = static_cast<BlitEncoderData *>(current);
      auto encoder = cmdbuf.blitCommandEncoder();
      serialWait(encoder); // spec 05 clarification 57 (level 4)
      encoder.encodeCommands(&data->cmd_head);
      serialUpdate(encoder);
      encoder.endEncoding();
      data->~BlitEncoderData();
      break;
    }
    case EncoderType::Present: {
      auto data = static_cast<PresentData *>(current);
      auto t0 = clock::now();
      auto drawable = data->presenter->encodeCommands(cmdbuf, {}, data->backbuffer, data->metadata);
      auto t1 = clock::now();
      currentFrameStatistics().drawable_blocking_interval += (t1 - t0);
#ifdef DXMT_IOS
      // Spec 05 E2.1: a plain presentDrawable:. presentDrawable:afterMinimumDuration:
      // for the game's sync interval waits for the previous frame's on-screen
      // presentation on iOS; pacing is done in the unix half (E3).
      // E2.2: no drawable (RAW pacing, or a timeout): no blit, no present; the
      // frame still counts as presented (PresentData's state is released below).
      if (drawable.handle != NULL_OBJECT_HANDLE)
        cmdbuf.presentDrawable(drawable);
#else
      if (data->after > 0)
        cmdbuf.presentDrawableAfterMinimumDuration(drawable, data->after);
      else
        cmdbuf.presentDrawable(drawable);
#endif
      data->~PresentData();
      break;
    }
    case EncoderType::Clear: {
      auto data = static_cast<ClearEncoderData *>(current);
      {
        WMTRenderPassInfo info;
        WMT::InitializeRenderPassInfo(info);
        if (data->clear_dsv) {
          if (data->clear_dsv & 1) {
            info.depth.clear_depth = data->depth_stencil.first;
            info.depth.texture = data->attachment.texture();
            info.depth.load_action = WMTLoadActionClear;
            info.depth.store_action = WMTStoreActionStore;
          }
          if (data->clear_dsv & 2) {
            info.stencil.clear_stencil = data->depth_stencil.second;
            info.stencil.texture = data->attachment.texture();
            info.stencil.load_action = WMTLoadActionClear;
            info.stencil.store_action = WMTStoreActionStore;
          }
          info.render_target_width = data->width;
          info.render_target_height = data->height;
        } else {
          info.colors[0].clear_color = data->color;
          info.colors[0].texture = data->attachment.texture();
          info.colors[0].load_action = WMTLoadActionClear;
          info.colors[0].store_action = WMTStoreActionStore;
        }
        info.render_target_array_length = data->array_length;
        auto encoder = cmdbuf.renderCommandEncoder(info);
        serialWait(encoder); // spec 05 clarification 57 (level 4)
        encoder.setLabel(WMT::String::string("ClearPass", WMTUTF8StringEncoding));
        serialUpdate(encoder);
        encoder.endEncoding();
      }
      data->~ClearEncoderData();
      break;
    }
    case EncoderType::Resolve: {
      auto data = static_cast<ResolveEncoderData *>(current);
      /* ---- spec 07: LGPL-2.1-or-later block, from dxmt-d3d9 v0.4-d3d9 (e8dd4c6)
         dxmt_context.cpp (without the tag's fences) ---- */
      if (data->is_depth) {
        // Point resolve of a multisampled depth surface: a depth-only render pass
        // whose fragment shader reads sample 0 of the source (bound as a fragment
        // depth texture) and writes the destination depth attachment. Only the
        // depth aspect is resolved, matching wined3d and DXVK.
        auto src_texture = data->src.texture();
        auto dst_texture = data->dst.texture();
        if (!src_texture || !dst_texture || !data->pso) {
          WARN("skipped a depth resolve with a missing attachment or pipeline");
          data->~ResolveEncoderData();
          break;
        }
        auto *dst_allocation = data->dst ? data->dst->allocation : nullptr;
        auto *dst_descriptor = dst_allocation ? dst_allocation->descriptor : nullptr;

        WMTRenderPassInfo info;
        WMT::InitializeRenderPassInfo(info);
        info.depth.texture = dst_texture;
        info.depth.load_action = WMTLoadActionDontCare;
        info.depth.store_action = WMTStoreActionStore;
        if (dst_descriptor) {
          info.render_target_width = dst_descriptor->width(data->dst->key);
          info.render_target_height = dst_descriptor->height(data->dst->key);
        }
        info.render_target_array_length = 1;
        info.default_raster_sample_count = 1;

        if (!depth_resolve_dss_) {
          WMTDepthStencilInfo ds_info = {};
          ds_info.depth_compare_function = WMTCompareFunctionAlways;
          ds_info.depth_write_enabled = true;
          depth_resolve_dss_ = device_.newDepthStencilState(ds_info);
        }

        auto encoder = cmdbuf.renderCommandEncoder(info);
        serialWait(encoder); // spec 05 clarification 57 (level 4)
        encoder.setLabel(WMT::String::string("DepthResolvePass", WMTUTF8StringEncoding));
        struct ResolveMetadata {
          uint32_t src_origin[2];
          uint32_t dst_origin[2];
          uint32_t size[2];
        } metadata = {};
        metadata.src_origin[0] = data->src_rect ? data->src_rect->x : 0;
        metadata.src_origin[1] = data->src_rect ? data->src_rect->y : 0;
        metadata.dst_origin[0] = data->dst_origin.x;
        metadata.dst_origin[1] = data->dst_origin.y;
        metadata.size[0] = data->resolve_size.width ? data->resolve_size.width : info.render_target_width;
        metadata.size[1] = data->resolve_size.height ? data->resolve_size.height : info.render_target_height;
        encoder.setRenderPipelineState(data->pso);
        encoder.setDepthStencilState(depth_resolve_dss_);
        encoder.setFragmentTexture(src_texture, 0);
        encoder.setFragmentBytes(&metadata, sizeof(metadata), 0);
        encoder.setViewport(
            {double(metadata.dst_origin[0]), double(metadata.dst_origin[1]), double(metadata.size[0]),
             double(metadata.size[1]), 0.0, 1.0}
        );
        encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
        serialUpdate(encoder);
        encoder.endEncoding();
        data->~ResolveEncoderData();
        break;
      }
      /* ---- end of LGPL block ---- */
      {
        WMTRenderPassInfo info;
        WMT::InitializeRenderPassInfo(info);
        info.colors[0].texture = data->src.texture();
        info.colors[0].load_action = WMTLoadActionLoad;
        info.colors[0].store_action = WMTStoreActionStoreAndMultisampleResolve;
        info.colors[0].resolve_texture = data->dst.texture();

        auto encoder = cmdbuf.renderCommandEncoder(info);
        serialWait(encoder); // spec 05 clarification 57 (level 4)
        encoder.setLabel(WMT::String::string("ResolvePass", WMTUTF8StringEncoding));
        serialUpdate(encoder);
        encoder.endEncoding();
      }
      data->~ResolveEncoderData();
      break;
    }
    /* ---- spec 07: LGPL-2.1-or-later block, from dxmt-d3d9 v0.4-d3d9 (e8dd4c6)
       dxmt_context.cpp (without the tag's fences) ---- */
    case EncoderType::StretchBlit: {
      auto data = static_cast<StretchBlitEncoderData *>(current);
      {
        auto *dst_allocation = data->dst ? data->dst->allocation : nullptr;
        auto *dst_descriptor = dst_allocation ? dst_allocation->descriptor : nullptr;

        WMTRenderPassInfo info;
        WMT::InitializeRenderPassInfo(info);
        info.colors[0].texture = data->dst.texture();
        info.colors[0].load_action = WMTLoadActionLoad;
        info.colors[0].store_action = WMTStoreActionStore;
        if (dst_descriptor) {
          info.render_target_width = dst_descriptor->width(data->dst->key);
          info.render_target_height = dst_descriptor->height(data->dst->key);
          info.render_target_array_length = 1;
          // A single-sample -> multisample StretchRect broadcasts the sampled
          // source into every sample, so the pass raster count follows the
          // destination.
          info.default_raster_sample_count = dst_descriptor->sampleCount();
        } else {
          info.default_raster_sample_count = 1;
        }

        auto encoder = cmdbuf.renderCommandEncoder(info);
        serialWait(encoder); // spec 05 clarification 57 (level 4)
        encoder.setLabel(WMT::String::string("StretchBlitPass", WMTUTF8StringEncoding));
        if (data->pso && data->sampler) {
          struct {
            float src_uv_origin[2];
            float src_uv_size[2];
          } metadata{};
          metadata.src_uv_origin[0] = data->src_uv_origin[0];
          metadata.src_uv_origin[1] = data->src_uv_origin[1];
          metadata.src_uv_size[0] = data->src_uv_size[0];
          metadata.src_uv_size[1] = data->src_uv_size[1];
          encoder.setRenderPipelineState(data->pso);
          encoder.setFragmentTexture(data->src.texture(), 0);
          encoder.setFragmentSamplerState(data->sampler, 0);
          encoder.setFragmentBytes(&metadata, sizeof(metadata), 0);
          encoder.setViewport(
              {double(data->dst_origin.x), double(data->dst_origin.y), double(data->dst_size.width),
               double(data->dst_size.height), 0.0, 1.0}
          );
          encoder.drawPrimitives(WMTPrimitiveTypeTriangle, 0, 3);
        }
        serialUpdate(encoder);
        encoder.endEncoding();
      }
      data->~StretchBlitEncoderData();
      break;
    }
    /* ---- end of LGPL block ---- */
    case EncoderType::SpatialUpscale: {
      auto data = static_cast<SpatialUpscaleData *>(current);
      cmdbuf.encodeSpatialScale(data->scaler, data->backbuffer, data->upscaled, {});
      data->~SpatialUpscaleData();
      break;
    }
    case EncoderType::SignalEvent: {
      auto data = static_cast<SignalEventData *>(current);
      cmdbuf.encodeSignalEvent(data->event, data->value);
      data->~SignalEventData();
      break;
    }
    case EncoderType::WaitForEvent: {
      auto data = static_cast<WaitForEventData *>(current);
      cmdbuf.encodeWaitForEvent(data->event, data->value);
      data->~WaitForEventData();
      break;
    }
    case EncoderType::TemporalUpscale: {
      auto data = static_cast<TemporalUpscaleData *>(current);
      cmdbuf.encodeTemporalScale(data->scaler, data->input, data->output, data->depth, data->motion_vector, data->exposure, {}, data->props);
      data->~TemporalUpscaleData();
      break;
    }
    default:
      break;
    }
    encoder_index--;
  }
  encoder_head.next = nullptr;
  encoder_last = &encoder_head;
  encoder_count_ = 0;

  cmdbuf.encodeSignalEvent(queue_.event, event_seq_id);

  for (size_t i = cpu_buffer_chunks_.size() - 1; i > current_buffer_chunk_; i--) {
    if (++cpu_buffer_chunks_[i].underused_times > kEncodingContextCPUHeapLifetime) {
      cpu_buffer_chunks_.pop_back();
    }
  }

  return visibility_readback;
}

DXMT_ENCODER_LIST_OP
ArgumentEncodingContext::checkEncoderRelation(EncoderData *former, EncoderData *latter) {
  if (former->type == EncoderType::Null)
    return DXMT_ENCODER_LIST_OP_SWAP;
  if (latter->type == EncoderType::Null)
    return DXMT_ENCODER_LIST_OP_SWAP;
  // MYIOSDECK spec 05 clarification 57: from PG_DXMT_SERIALIZE=1 on, nothing is merged or
  // moved (a clear stays its own pass)
  if (SerializeKeepsEncoderOrder(serialize_level_))
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  if (former->type == EncoderType::SignalEvent)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  if (latter->type == EncoderType::SignalEvent)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  if (former->type == EncoderType::WaitForEvent)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
  if (latter->type == EncoderType::WaitForEvent)
    return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;

  while (former->type != latter->type) {
    if (former->type == EncoderType::Clear && latter->type == EncoderType::Render) {
      auto render = reinterpret_cast<RenderEncoderData *>(latter);
      auto clear = reinterpret_cast<ClearEncoderData *>(former);

      if (render->render_target_array_length != clear->array_length)
        break;

      if (clear->clear_dsv) {
        if (auto depth_attachment = isClearDepthSignatureMatched(clear, render)) {
          if (depth_attachment->load_action == WMTLoadActionLoad) {
            depth_attachment->clear_depth = clear->depth_stencil.first;
            depth_attachment->load_action = WMTLoadActionClear;
            depth_attachment->store_action = WMTStoreActionStore;
            render->tex_write.merge(clear->tex_write);
          }
          clear->clear_dsv &= ~1;
        }
        if (auto stencil_attachment = isClearStencilSignatureMatched(clear, render)) {
          if (stencil_attachment->load_action == WMTLoadActionLoad) {
            stencil_attachment->clear_stencil = clear->depth_stencil.second;
            stencil_attachment->load_action = WMTLoadActionClear;
            stencil_attachment->store_action = WMTStoreActionStore;
            render->tex_write.merge(clear->tex_write);
          }
          clear->clear_dsv &= ~2;
        }
        if (clear->clear_dsv == 0) {
          currentFrameStatistics().clear_pass_optimized++;
          clear->~ClearEncoderData();
          clear->next = nullptr;
          clear->type = EncoderType::Null;
          return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
        }
      } else {
        if (auto attachment = isClearColorSignatureMatched(clear, render)) {
          if (attachment->load_action == WMTLoadActionLoad) {
            attachment->load_action = WMTLoadActionClear;
            attachment->clear_color = clear->color;
            if (attachment->store_action != WMTStoreActionDontCare)
              render->tex_write.merge(clear->tex_write);
          }

          currentFrameStatistics().clear_pass_optimized++;
          clear->~ClearEncoderData();
          clear->next = nullptr;
          clear->type = EncoderType::Null;
          return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
        }
      }
      break;
    }
    if (latter->type == EncoderType::Clear && former->type == EncoderType::Render) {
      auto render = reinterpret_cast<RenderEncoderData *>(former);
      auto clear = reinterpret_cast<ClearEncoderData *>(latter);

      // DontCare can be used because it's going to be cleared anyway
      // just keep in mind DontCare != DontStore
      if (clear->clear_dsv & 1 && render->depth.attachment == clear->attachment)
        render->depth.store_action = WMTStoreActionDontCare;
      if (clear->clear_dsv & 2 && render->stencil.attachment == clear->attachment)
        render->stencil.store_action = WMTStoreActionDontCare;
    }
    if (former->type == EncoderType::Render && latter->type == EncoderType::Resolve) {
      auto render = reinterpret_cast<RenderEncoderData *>(former);
      auto resolve = reinterpret_cast<ResolveEncoderData *>(latter);
      auto result = isResolveSignatureMatched(render, resolve);
      if (result.src) {
        result.src->store_action = WMTStoreActionStoreAndMultisampleResolve;
        result.src->resolve_attachment = result.dst;
        render->tex_write.merge(resolve->tex_write);

        currentFrameStatistics().resolve_pass_optimized++;
        resolve->~ResolveEncoderData();
        resolve->next = nullptr;
        resolve->type = EncoderType::Null;
        return DXMT_ENCODER_LIST_OP_SWAP; // carry on (RENDER -> RESOLVE -> RESOLVE -> ...)
      }
    }
    return hasDataDependency(latter, former) ? DXMT_ENCODER_LIST_OP_SYNCHRONIZE : DXMT_ENCODER_LIST_OP_SWAP;
  }

  if (former->type == EncoderType::Render) {
    auto r1 = reinterpret_cast<RenderEncoderData *>(latter);
    auto r0 = reinterpret_cast<RenderEncoderData *>(former);

    if (isEncoderSignatureMatched(r0, r1)) {
      for (unsigned i = 0; i < r0->render_target_count; i++) {
        auto &a0 = r0->colors[i];
        auto &a1 = r1->colors[i];
        a1.load_action = a0.load_action;
        a1.clear_color = a0.clear_color;
      }

      r1->depth.load_action = r0->depth.load_action;
      r1->depth.clear_depth = r0->depth.clear_depth;
      r1->depth.store_action = r0->depth.store_action;
      r1->stencil.load_action = r0->stencil.load_action;
      r1->stencil.clear_stencil = r0->stencil.clear_stencil;
      r1->stencil.store_action = r0->stencil.store_action;

      if ((void *)r0->cmd_tail != &r0->cmd_head) {
        r0->cmd_tail->next.set(r1->cmd_head.next.get());
        r1->cmd_head.next.set(r0->cmd_head.next.get());
        r0->cmd_head.next.set(nullptr);
        r0->cmd_tail = (wmtcmd_base *)&r0->cmd_head;
      }
      r1->use_tessellation = r0->use_tessellation || r1->use_tessellation;
      r1->use_geometry = r0->use_geometry || r1->use_geometry;
      std::move(
        r1->gs_arg_marshal_tasks.begin(),
        r1->gs_arg_marshal_tasks.end(),
        std::back_inserter(r0->gs_arg_marshal_tasks)
      );
      std::move(
        r1->ts_arg_marshal_tasks.begin(),
        r1->ts_arg_marshal_tasks.end(),
        std::back_inserter(r0->ts_arg_marshal_tasks)
      );
      r1->gs_arg_marshal_tasks = std::move(r0->gs_arg_marshal_tasks);
      r1->ts_arg_marshal_tasks = std::move(r0->ts_arg_marshal_tasks);
      r1->use_visibility_result = r0->use_visibility_result || r1->use_visibility_result;

      r1->buf_read.merge(r0->buf_read);
      r1->buf_write.merge(r0->buf_write);
      r1->tex_read.merge(r0->tex_read);
      r1->tex_write.merge(r0->tex_write);

      currentFrameStatistics().render_pass_optimized++;
      r0->~RenderEncoderData();
      r0->next = nullptr;
      r0->type = EncoderType::Null;

      return DXMT_ENCODER_LIST_OP_SYNCHRONIZE;
    }
  }

  return hasDataDependency(latter, former) ? DXMT_ENCODER_LIST_OP_SYNCHRONIZE : DXMT_ENCODER_LIST_OP_SWAP;
}

bool
ArgumentEncodingContext::hasDataDependency(EncoderData *latter, EncoderData *former) {
  if (latter->type == EncoderType::Clear && former->type == EncoderType::Clear) {
    // FIXME: prove it's safe to return false
    return false;
  }
  // read-after-write
  if (!former->buf_write.isDisjointWith(latter->buf_read))
    return true;
  if (!former->tex_write.isDisjointWith(latter->tex_read))
    return true;
  // write-after-write
  if (!former->buf_write.isDisjointWith(latter->buf_write))
    return true;
  if (!former->tex_write.isDisjointWith(latter->tex_write))
    return true;
  // write-after-read
  if (!former->buf_read.isDisjointWith(latter->buf_write))
    return true;
  if (!former->tex_read.isDisjointWith(latter->tex_write))
    return true;
  return false;
}

bool
ArgumentEncodingContext::isEncoderSignatureMatched(RenderEncoderData *r0, RenderEncoderData *r1) {
  // FIXME: it can be different?
  if (r0->render_target_count != r1->render_target_count)
    return false;
  if (r0->dsv_planar_flags != r1->dsv_planar_flags)
    return false;
  if (r0->dsv_readonly_flags != r1->dsv_readonly_flags)
    return false;
  if (r0->render_target_array_length != r1->render_target_array_length)
    return false;
  /**
  In case two encoder has different argument buffer
  It can be further optimized by inserting extra setBuffer() calls
  but let's just simplify it, as it's a rather rare case.
  */
  if (r0->allocated_argbuf != r1->allocated_argbuf)
    return false;
  if (r0->dsv_planar_flags & 1) {
    if (r0->depth.attachment != r1->depth.attachment)
      return false;
    if (r0->dsv_readonly_flags & 1) {
      if (r1->depth.load_action == WMTLoadActionClear)
        return false;
    } else {
      if (r0->depth.store_action != WMTStoreActionStore)
        return false;
      if (r1->depth.load_action != WMTLoadActionLoad)
        return false;
    }
  }
  if (r0->dsv_planar_flags & 2) {
    if (r0->stencil.attachment != r1->stencil.attachment)
      return false;
    if (r0->dsv_readonly_flags & 2) {
      if (r1->stencil.load_action == WMTLoadActionClear)
        return false;
    } else {
      if (r0->stencil.store_action != WMTStoreActionStore)
        return false;
      if (r1->stencil.load_action != WMTLoadActionLoad)
        return false;
    }
  }
  for (unsigned i = 0; i < r0->render_target_count; i++) {
    auto &a0 = r0->colors[i];
    auto &a1 = r1->colors[i];
    if (a0.attachment != a1.attachment)
      return false;
    if (a0.depth_plane != a1.depth_plane)
      return false;
    if (!a0.attachment)
      continue;
    if (a0.store_action != WMTStoreActionStore)
      return false;
    if (a1.load_action != WMTLoadActionLoad)
      return false;
  }
  return true;
}

RenderEncoderColorAttachmentData *
ArgumentEncodingContext::isClearColorSignatureMatched(ClearEncoderData *clear, RenderEncoderData *render) {
  for (unsigned i = 0; i < render->render_target_count; i++) {
    auto &attachment = render->colors[i];
    if (attachment.attachment == clear->attachment) {
      return &attachment;
    }
  }
  return nullptr;
}

RenderEncoderDepthAttachmentData *
ArgumentEncodingContext::isClearDepthSignatureMatched(ClearEncoderData *clear, RenderEncoderData *render) {
  if ((clear->clear_dsv & 1) == 0)
    return nullptr;
  if (render->depth.attachment != clear->attachment)
    return nullptr;
  return &render->depth;
}

RenderEncoderStencilAttachmentData *
ArgumentEncodingContext::isClearStencilSignatureMatched(ClearEncoderData *clear, RenderEncoderData *render) {
  if ((clear->clear_dsv & 2) == 0)
    return nullptr;
  if (render->stencil.attachment != clear->attachment)
    return nullptr;
  return &render->stencil;
}

ArgumentEncodingContext::ResolveSignatureMatchResult
ArgumentEncodingContext::isResolveSignatureMatched(RenderEncoderData *render, ResolveEncoderData *resolve) {
  ResolveSignatureMatchResult ret{};
  for (unsigned i = 0; i < render->render_target_count; i++) {
    auto &color = render->colors[i];
    if (!color.attachment)
      continue;
    if (color.store_action != WMTStoreActionStore)
      continue;
    if (color.resolve_attachment)
      continue;
    if (color.attachment->allocation != resolve->src->allocation)
      continue;
    if (color.attachment->key == resolve->src->key) {
      ret.src = &color;
      ret.dst = resolve->dst;
      break;
    };
    auto &descriptor_src = resolve->src->allocation->descriptor;
    auto &descriptor_dst = resolve->dst->allocation->descriptor;
    auto color_format = descriptor_src->pixelFormat(color.attachment->key);
    auto view_src_in_color_format = descriptor_src->checkViewUseFormat(resolve->src->key, color_format);
    if (color.attachment->key == view_src_in_color_format) {
      auto view_dst_in_color_format = descriptor_dst->checkViewUseFormat(resolve->dst->key, color_format);
      ret.src = &color;
      ret.dst = descriptor_dst->view(view_dst_in_color_format, resolve->dst->allocation);
      break;
    }
  }
  return ret;
}

} // namespace dxmt