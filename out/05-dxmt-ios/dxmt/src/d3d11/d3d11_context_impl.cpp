// Changed for MYIOSDECK (spec 05 B, C, D; spec 05 clarification 10: LGPL-2.1-or-later blocks from
// steamac's DXMT patches 0005, 0007 and 0014, marked inline, see LICENSE-NOTES.md; spec 05
// clarifications 19-21: constant-buffer ranges, compute threadgroup size, frame trace notes;
// 24-25: indirect read-backs, the barrier before an indirect dispatch; 27-28: the blit pass
// after a traced dispatch; 30: state lines; 44-58: draw lines, bindings, read-backs, the serialize switch;
// 63-73: call statistics and the round-17 cuts).
// Upstream: DXMT a3a6e8f, MIT (see LICENSE).

/**
This file is included by d3d11_context_{imm|def}.cpp
and should be not used as a compilation unit

since it is for internal use only
(and I don't want to deal with several thousands line of code)
*/
#include "Metal.hpp"
#include "d3d11_annotation.hpp"
#include "d3d11_context.hpp"
#include "d3d11_device_child.hpp"
#include "d3d11_enumerable.hpp"
#include "d3d11_interfaces.hpp"
#include "d3d11_private.h"
#include "d3d11_context_state.hpp"
#include "d3d11_device.hpp"
#include "d3d11_pipeline.hpp"
#include "d3d11_query.hpp"
#include "dxmt_buffer.hpp"
#include "dxmt_context.hpp"
#include "dxmt_format.hpp"
#include "dxmt_ring_bump_allocator.hpp"
#include "dxmt_staging.hpp"
#include "d3d11_resource.hpp"
#include "dxmt_texture.hpp"
#include "util_flags.hpp"
#include "util_math.hpp"
#include "util_win32_compat.h"
#ifdef DXMT_IOS
#include "dxmt_bc_emulation.hpp"
#include "dxmt_mip_clamp.hpp"
#include "util_log_rare.hpp"
#include "d3d11_constant_buffer_bind.hpp" // spec 05 clarification 19
#include "dxmt_frame_trace.hpp"           // spec 05 clarification 21
#include "dxmt_indirect.hpp"              // spec 05 clarification 25
#include "d3d11_trace_state.hpp"          // spec 05 clarification 30
#include "d3d11_call_stats.hpp"           // spec 05 clarification 63
#include "dxmt_cuts.hpp"                  // spec 05 clarification 73
#include "d3d11_state_filter.hpp"         // spec 05 clarification 65
#include "d3d11_discard_bound.hpp"        // spec 05 clarification 66
#include "d3d11_pass_cuts.hpp"            // spec 05 clarifications 69, 70, 72
#include <cstdlib>
#endif

// Changed for MYIOSDECK (spec 05 B8, C, D2, D5, E2). Upstream: DXMT a3a6e8f, MIT (see LICENSE).

namespace dxmt {

template<typename Object> Rc<Object> forward_rc(Rc<Object>& obj);

/**
Spec 05 B8: a copy between a buffer and a combined depth-stencil texture moves
one plane, which Metal must be told with a blit option. The plane follows the
copy's format: a stencil-only format selects stencil, anything else depth.
Returns 0 for textures that aren't combined depth-stencil.
*/
inline uint64_t
DepthStencilPlaneBlitOption(WMTPixelFormat texture_format, uint32_t copy_format_flags) {
#ifdef DXMT_IOS
  if (DepthStencilPlanarFlags((WMTPixelFormat)ORIGINAL_FORMAT(texture_format)) != 3)
    return WMTBlitOptionNone;
  if ((copy_format_flags & MTL_DXGI_FORMAT_STENCIL_PLANER) && !(copy_format_flags & MTL_DXGI_FORMAT_DEPTH_PLANER))
    return WMTBlitOptionStencilFromDepthStencil;
  return WMTBlitOptionDepthFromDepthStencil;
#else
  return WMTBlitOptionNone;
#endif
}

inline bool
to_metal_primitive_type(D3D11_PRIMITIVE_TOPOLOGY topo, WMTPrimitiveType& primitive, uint32_t& control_point_num) {
  control_point_num = 0;
  switch (topo) {
  case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
    primitive = WMTPrimitiveTypePoint;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
    primitive = WMTPrimitiveTypeLine;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    primitive = WMTPrimitiveTypeLineStrip;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
    primitive = WMTPrimitiveTypeTriangle;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    primitive = WMTPrimitiveTypeTriangleStrip;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
    // geometry
    primitive = WMTPrimitiveTypePoint;
    break;
  case D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_5_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_6_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_7_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_8_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_9_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_10_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_11_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_12_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_13_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_14_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_15_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_16_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_17_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_18_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_19_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_20_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_21_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_22_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_23_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_24_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_25_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_26_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_27_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_28_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_29_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_30_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_31_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST:
    primitive = WMTPrimitiveTypePoint;
    control_point_num = topo - 32;
    break;
  default:
    return false;
  }
  return true;
}

inline WMTPrimitiveTopologyClass
to_metal_primitive_topology(D3D11_PRIMITIVE_TOPOLOGY topo) {
  switch (topo) {
  case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
    return WMTPrimitiveTopologyClassPoint;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    return WMTPrimitiveTopologyClassLine;
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    return WMTPrimitiveTopologyClassTriangle;
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
    break;
  case D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_5_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_6_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_7_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_8_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_9_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_10_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_11_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_12_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_13_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_14_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_15_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_16_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_17_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_18_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_19_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_20_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_21_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_22_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_23_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_24_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_25_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_26_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_27_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_28_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_29_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_30_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_31_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST:
    break;
  case D3D_PRIMITIVE_TOPOLOGY_UNDEFINED:
    D3D11_ASSERT(0 && "Invalid topology");
  }
  return WMTPrimitiveTopologyClassUnspecified;
}

inline bool is_strip_topology(D3D11_PRIMITIVE_TOPOLOGY topo) {
  switch (topo) {
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
    return true;
  default:
    break;
  }
  return false;
}

inline std::pair<uint32_t, uint32_t>
get_gs_vertex_count(D3D11_PRIMITIVE_TOPOLOGY primitive) {
  switch (primitive) {
  case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
    return {32, 32};
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
    return {32, 32};
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
    return {32, 31};
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
    return {30, 30};
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
    return {32, 30};
  case D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ:
    return {32, 32};
  case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP_ADJ:
    return {32, 29};
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ:
    return {30, 30};
  case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP_ADJ:
    return {32, 28};
  case D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST:
  return {32, 32};
  case D3D_PRIMITIVE_TOPOLOGY_2_CONTROL_POINT_PATCHLIST:
  return {32, 32};
  case D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST:
  return {30, 30};
  case D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST:
  return {32, 32};
  case D3D_PRIMITIVE_TOPOLOGY_5_CONTROL_POINT_PATCHLIST:
  return {30, 30};
  case D3D_PRIMITIVE_TOPOLOGY_6_CONTROL_POINT_PATCHLIST:
  return {30, 30};
  case D3D_PRIMITIVE_TOPOLOGY_7_CONTROL_POINT_PATCHLIST:
  return {28, 28};
  case D3D_PRIMITIVE_TOPOLOGY_8_CONTROL_POINT_PATCHLIST:
  return {32, 32};
  case D3D_PRIMITIVE_TOPOLOGY_9_CONTROL_POINT_PATCHLIST:
  return {27, 27};
  case D3D_PRIMITIVE_TOPOLOGY_10_CONTROL_POINT_PATCHLIST:
  return {30, 30};
  case D3D_PRIMITIVE_TOPOLOGY_11_CONTROL_POINT_PATCHLIST:
  return {22, 22};
  case D3D_PRIMITIVE_TOPOLOGY_12_CONTROL_POINT_PATCHLIST:
  return {24, 24};
  case D3D_PRIMITIVE_TOPOLOGY_13_CONTROL_POINT_PATCHLIST:
  return {26, 26};
  case D3D_PRIMITIVE_TOPOLOGY_14_CONTROL_POINT_PATCHLIST:
  return {28, 28};
  case D3D_PRIMITIVE_TOPOLOGY_15_CONTROL_POINT_PATCHLIST:
  return {30, 30};
  case D3D_PRIMITIVE_TOPOLOGY_16_CONTROL_POINT_PATCHLIST:
  return {32, 32};
  case D3D_PRIMITIVE_TOPOLOGY_17_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_18_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_19_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_20_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_21_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_22_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_23_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_24_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_25_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_26_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_27_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_28_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_29_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_30_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_31_CONTROL_POINT_PATCHLIST:
  case D3D_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST:
    return {
      uint32_t(primitive) - uint32_t(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST) + 1, 
      uint32_t(primitive) - uint32_t(D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST) + 1
    };
  default:
    break;
  }
  DXMT_UNREACHABLE
}

struct Subresource {
  DXGI_FORMAT Format;
  uint32_t MipLevel;
  uint32_t ArraySlice;
  uint32_t Width;
  uint32_t Height;
  uint32_t Depth;
};

class BlitObject {
public:
  ID3D11Resource *pResource;
  MTL_DXGI_FORMAT_DESC FormatDescription;
  D3D11_RESOURCE_DIMENSION Dimension;
  union {
    D3D11_TEXTURE1D_DESC Texture1DDesc;
    D3D11_TEXTURE2D_DESC1 Texture2DDesc;
    D3D11_TEXTURE3D_DESC1 Texture3DDesc;
    D3D11_BUFFER_DESC BufferDesc;
  };

  BlitObject(MTLD3D11Device *pDevice, ID3D11Resource *pResource) : pResource(pResource) {
    pResource->GetType(&Dimension);
    switch (Dimension) {
    case D3D11_RESOURCE_DIMENSION_UNKNOWN:
      break;
    case D3D11_RESOURCE_DIMENSION_BUFFER:
      reinterpret_cast<ID3D11Buffer *>(pResource)->GetDesc(&BufferDesc);
      break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D:
      reinterpret_cast<ID3D11Texture1D *>(pResource)->GetDesc(&Texture1DDesc);
      MTLQueryDXGIFormat(pDevice->GetMTLDevice(), Texture1DDesc.Format, FormatDescription);
      break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D:
      reinterpret_cast<ID3D11Texture2D1 *>(pResource)->GetDesc1(&Texture2DDesc);
      MTLQueryDXGIFormat(pDevice->GetMTLDevice(), Texture2DDesc.Format, FormatDescription);
      break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D:
      reinterpret_cast<ID3D11Texture3D1 *>(pResource)->GetDesc1(&Texture3DDesc);
      MTLQueryDXGIFormat(pDevice->GetMTLDevice(), Texture3DDesc.Format, FormatDescription);
      break;
    }
  };
};

class TextureCopyCommand {
public:
  ID3D11Resource *pSrc;
  ID3D11Resource *pDst;
  uint32_t SrcSubresource;
  uint32_t DstSubresource;

  Subresource Src;
  Subresource Dst;
  WMTOrigin SrcOrigin;
  WMTSize SrcSize;
  WMTOrigin DstOrigin;

  MTL_DXGI_FORMAT_DESC SrcFormat;
  MTL_DXGI_FORMAT_DESC DstFormat;

  bool Invalid = true;

  TextureCopyCommand(
      BlitObject &Dst_, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, BlitObject &Src_, UINT SrcSubresource,
      const D3D11_BOX *pSrcBox
  ) :
      pSrc(Src_.pResource),
      pDst(Dst_.pResource),
      SrcSubresource(SrcSubresource),
      DstSubresource(DstSubresource),
      SrcFormat(Src_.FormatDescription),
      DstFormat(Dst_.FormatDescription) {
    if (Dst_.Dimension != Src_.Dimension)
      return;

    if (SrcFormat.PixelFormat == WMTPixelFormatInvalid)
      return;
    if (DstFormat.PixelFormat == WMTPixelFormatInvalid)
      return;

    switch (Dst_.Dimension) {
    default: {
      return;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      D3D11_TEXTURE1D_DESC &dst_desc = Dst_.Texture1DDesc;
      D3D11_TEXTURE1D_DESC &src_desc = Src_.Texture1DDesc;

      Src.Format = src_desc.Format;
      Src.MipLevel = SrcSubresource % src_desc.MipLevels;
      Src.ArraySlice = SrcSubresource / src_desc.MipLevels;
      Src.Width = std::max(1u, src_desc.Width >> Src.MipLevel);
      Src.Height = 1;
      Src.Depth = 1;

      Dst.Format = dst_desc.Format;
      Dst.MipLevel = DstSubresource % dst_desc.MipLevels;
      Dst.ArraySlice = DstSubresource / dst_desc.MipLevels;
      Dst.Width = std::max(1u, dst_desc.Width >> Dst.MipLevel);
      Dst.Height = 1;
      Dst.Depth = 1;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      D3D11_TEXTURE2D_DESC1 &dst_desc = Dst_.Texture2DDesc;
      D3D11_TEXTURE2D_DESC1 &src_desc = Src_.Texture2DDesc;

      Src.Format = src_desc.Format;
      Src.MipLevel = SrcSubresource % src_desc.MipLevels;
      Src.ArraySlice = SrcSubresource / src_desc.MipLevels;
      Src.Width = std::max(1u, src_desc.Width >> Src.MipLevel);
      Src.Height = std::max(1u, src_desc.Height >> Src.MipLevel);
      Src.Depth = 1;

      Dst.Format = dst_desc.Format;
      Dst.MipLevel = DstSubresource % dst_desc.MipLevels;
      Dst.ArraySlice = DstSubresource / dst_desc.MipLevels;
      Dst.Width = std::max(1u, dst_desc.Width >> Dst.MipLevel);
      Dst.Height = std::max(1u, dst_desc.Height >> Dst.MipLevel);
      Dst.Depth = 1;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      D3D11_TEXTURE3D_DESC1 &dst_desc = Dst_.Texture3DDesc;
      D3D11_TEXTURE3D_DESC1 &src_desc = Src_.Texture3DDesc;

      Src.Format = src_desc.Format;
      Src.MipLevel = SrcSubresource;
      Src.ArraySlice = 0;
      Src.Width = std::max(1u, src_desc.Width >> Src.MipLevel);
      Src.Height = std::max(1u, src_desc.Height >> Src.MipLevel);
      Src.Depth = std::max(1u, src_desc.Depth >> Src.MipLevel);

      Dst.Format = dst_desc.Format;
      Dst.MipLevel = DstSubresource;
      Dst.ArraySlice = 0;
      Dst.Width = std::max(1u, dst_desc.Width >> Dst.MipLevel);
      Dst.Height = std::max(1u, dst_desc.Height >> Dst.MipLevel);
      Dst.Depth = std::max(1u, dst_desc.Depth >> Dst.MipLevel);

      break;
    }
    }

    D3D11_BOX SrcBox;

    if (pSrcBox) {
      if (pSrcBox->left >= pSrcBox->right)
        return;
      if (pSrcBox->top >= pSrcBox->bottom)
        return;
      if (pSrcBox->front >= pSrcBox->back)
        return;
      SrcBox = *pSrcBox;
    } else {
      SrcBox = {0, 0, 0, Src.Width, Src.Height, Src.Depth};
    }

    // Discard, if source box does't overlap with source size at all
    if (SrcBox.left >= Src.Width)
      return;
    if (SrcBox.top >= Src.Height)
      return;
    if (SrcBox.front >= Src.Depth)
      return;

    // Discard, if dest origin is not even within dest
    if (DstX >= Dst.Width)
      return;
    if (DstY >= Dst.Height)
      return;
    if (DstZ >= Dst.Depth)
      return;

    // Clip source box, if it's larger than source size
    // It is useful for copying between BC textures
    // since Metal takes virtual size
    // while D3D11 requires block size aligned (typically 4x)
    SrcBox.right = std::min(SrcBox.right, Src.Width);
    SrcBox.bottom = std::min(SrcBox.bottom, Src.Height);
    SrcBox.back = std::min(SrcBox.back, Src.Depth);

    if ((DstFormat.Flag & MTL_DXGI_FORMAT_BC) == (SrcFormat.Flag & MTL_DXGI_FORMAT_BC)) {
      // Discard, if the copy overflow
      if ((DstX + SrcBox.right - SrcBox.left) > Dst.Width)
        return;
      if ((DstY + SrcBox.bottom - SrcBox.top) > Dst.Height)
        return;
      if ((DstZ + SrcBox.back - SrcBox.front) > Dst.Depth)
        return;
    }

    DstOrigin = {DstX, DstY, DstZ};
    SrcOrigin = {SrcBox.left, SrcBox.top, SrcBox.front};
    SrcSize = {SrcBox.right - SrcBox.left, SrcBox.bottom - SrcBox.top, SrcBox.back - SrcBox.front};

    Invalid = false;
  }
};

class TextureUpdateCommand {
public:
  ID3D11Resource *pDst;
  UINT DstSubresource;
  Subresource Dst;
  WMTOrigin DstOrigin;
  WMTSize DstSize;
  uint32_t EffectiveBytesPerRow;
  uint32_t EffectiveRows;

  MTL_DXGI_FORMAT_DESC DstFormat;

  bool Invalid = true;

  TextureUpdateCommand(BlitObject &Dst_, UINT DstSubresource, const D3D11_BOX *pDstBox) :
      pDst(Dst_.pResource),
      DstSubresource(DstSubresource),
      DstFormat(Dst_.FormatDescription) {

    if (DstFormat.PixelFormat == WMTPixelFormatInvalid || DstFormat.BytesPerTexel == 0)
      return;

    switch (Dst_.Dimension) {
    default: {
      return;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      D3D11_TEXTURE1D_DESC &dst_desc = Dst_.Texture1DDesc;

      if (DstSubresource >= dst_desc.MipLevels * dst_desc.ArraySize)
        return;

      Dst.Format = dst_desc.Format;
      Dst.MipLevel = DstSubresource % dst_desc.MipLevels;
      Dst.ArraySlice = DstSubresource / dst_desc.MipLevels;
      Dst.Width = std::max(1u, dst_desc.Width >> Dst.MipLevel);
      Dst.Height = 1;
      Dst.Depth = 1;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      D3D11_TEXTURE2D_DESC1 dst_desc = Dst_.Texture2DDesc;

      if (DstSubresource >= dst_desc.MipLevels * dst_desc.ArraySize)
        return;

      Dst.Format = dst_desc.Format;
      Dst.MipLevel = DstSubresource % dst_desc.MipLevels;
      Dst.ArraySlice = DstSubresource / dst_desc.MipLevels;
      Dst.Width = std::max(1u, dst_desc.Width >> Dst.MipLevel);
      Dst.Height = std::max(1u, dst_desc.Height >> Dst.MipLevel);
      Dst.Depth = 1;
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      D3D11_TEXTURE3D_DESC1 dst_desc = Dst_.Texture3DDesc;

      if (DstSubresource >= dst_desc.MipLevels)
        return;

      Dst.Format = dst_desc.Format;
      Dst.MipLevel = DstSubresource;
      Dst.ArraySlice = 0;
      Dst.Width = std::max(1u, dst_desc.Width >> Dst.MipLevel);
      Dst.Height = std::max(1u, dst_desc.Height >> Dst.MipLevel);
      Dst.Depth = std::max(1u, dst_desc.Depth >> Dst.MipLevel);
      break;
    }
    }

    D3D11_BOX DstBox;

    if (pDstBox) {
      if (pDstBox->left >= pDstBox->right)
        return;
      if (pDstBox->top >= pDstBox->bottom)
        return;
      if (pDstBox->front >= pDstBox->back)
        return;
      DstBox = *pDstBox;
    } else {
      DstBox = {0, 0, 0, Dst.Width, Dst.Height, Dst.Depth};
    }

    // Discard, if box does't overlap with the texure at all
    if (DstBox.left >= Dst.Width)
      return;
    if (DstBox.top >= Dst.Height)
      return;
    if (DstBox.front >= Dst.Depth)
      return;

    // It is useful for copying between BC textures
    // since Metal takes virtual size
    // while D3D11 requires block size aligned (typically 4x)
    DstBox.right = std::min(DstBox.right, Dst.Width);
    DstBox.bottom = std::min(DstBox.bottom, Dst.Height);
    DstBox.back = std::min(DstBox.back, Dst.Depth);

    DstOrigin = {DstBox.left, DstBox.top, DstBox.front};
    DstSize = {DstBox.right - DstBox.left, DstBox.bottom - DstBox.top, DstBox.back - DstBox.front};

    if (DstFormat.Flag & MTL_DXGI_FORMAT_BC) {
      EffectiveBytesPerRow = (align(DstSize.width, 4u) >> 2) * DstFormat.BytesPerTexel;
      EffectiveRows = align(DstSize.height, 4u) >> 2;
    } else {
      EffectiveBytesPerRow = DstSize.width * DstFormat.BytesPerTexel;
      EffectiveRows = DstSize.height;
    }

    Invalid = false;
  }
};

struct DXMT_DRAW_ARGUMENTS {
  uint32_t VertexCount;
  uint32_t InstanceCount;
  uint32_t StartVertex;
  uint32_t StartInstance;
};

struct DXMT_DRAW_INDEXED_ARGUMENTS {
  uint32_t IndexCount;
  uint32_t InstanceCount;
  uint32_t StartIndex;
  int32_t BaseVertex;
  uint32_t StartInstance;
};

struct DXMT_DISPATCH_ARGUMENTS {
  uint32_t X;
  uint32_t Y;
  uint32_t Z;
};

template <typename ContextInternalState>
class MTLD3D11ContextExt;

enum class DrawCallStatus { Invalid, Ordinary, Tessellation, Geometry };

template <typename ContextInternalState> class MTLD3D11DeviceContextImplBase : public MTLD3D11DeviceContextBase {
  template<typename ContextInternalState_>
  friend class MTLD3D11ContextExt;

  using mutex_t = ContextInternalState::device_mutex_t;
public:
  /* MYIOSDECK spec 05 clarification 63: the call statistics' range of this kind of context */
  static constexpr unsigned kD11csBase =
      ContextInternalState::kD11csDeferred ? d11cs::kD11IdDeferred : d11cs::kD11IdImmediate;

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    D11_CTX_CALL(QueryInterface);
    return QueryInterfaceUncounted(riid, ppvObject);
  }

  /* ID3D11DeviceChild's methods, counted as this context's (spec 05 clarification 63) */
  void STDMETHODCALLTYPE
  GetDevice(ID3D11Device **ppDevice) override {
    D11_CTX_CALL(GetDevice);
    MTLD3D11DeviceContextBase::GetDevice(ppDevice);
  }

  HRESULT STDMETHODCALLTYPE
  GetPrivateData(REFGUID guid, UINT *pDataSize, void *pData) override {
    D11_CTX_CALL(GetPrivateData);
    return MTLD3D11DeviceContextBase::GetPrivateData(guid, pDataSize, pData);
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateData(REFGUID guid, UINT DataSize, const void *pData) override {
    D11_CTX_CALL(SetPrivateData);
    return MTLD3D11DeviceContextBase::SetPrivateData(guid, DataSize, pData);
  }

  HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID guid, const IUnknown *pUnknown) override {
    D11_CTX_CALL(SetPrivateDataInterface);
    return MTLD3D11DeviceContextBase::SetPrivateDataInterface(guid, pUnknown);
  }

  /* spec 05 clarification 66: after Map(WRITE_DISCARD) of a dynamic buffer, mark dirty the
     vertex-buffer, constant-buffer and shader-resource slots that hold it (DXMT_D11_DISCARD_BOUND),
     or every slot of each kind it can be bound as (switch off, as before) */
  void
  DirtyDiscardedBuffer(ID3D11Resource *pResource, UINT bind_flag) {
    const bool only = CutSettingsOnce()[CutSwitch::DiscardBound];
    auto *resource = reinterpret_cast<D3D11ResourceCommon *>(pResource);
    if (bind_flag & D3D11_BIND_VERTEX_BUFFER)
      DirtyDiscardedSlots(state_.InputAssembler.VertexBuffers, only, [resource](const auto &entry) {
        return entry.Buffer.ptr() == resource;
      });
    if (bind_flag & D3D11_BIND_CONSTANT_BUFFER)
      for (auto &stage : state_.ShaderStages)
        DirtyDiscardedSlots(stage.ConstantBuffers, only, [resource](const auto &entry) {
          return entry.Buffer.ptr() == resource;
        });
    if (bind_flag & D3D11_BIND_SHADER_RESOURCE) {
      Buffer *buffer = only ? resource->buffer().ptr() : nullptr;
      for (auto &stage : state_.ShaderStages)
        DirtyDiscardedSlots(stage.SRVs, only, [buffer](const auto &entry) {
          return entry.SRV->buffer().ptr() == buffer;
        });
    }
  }

  /* the same for a dynamic texture: the shader-resource slots that hold a view of it */
  void
  DirtyDiscardedTexture(ID3D11Resource *pResource) {
    const bool only = CutSettingsOnce()[CutSwitch::DiscardBound];
    Texture *texture = only ? reinterpret_cast<D3D11ResourceCommon *>(pResource)->texture().ptr() : nullptr;
    for (auto &stage : state_.ShaderStages)
      DirtyDiscardedSlots(stage.SRVs, only, [texture](const auto &entry) {
        return entry.SRV->texture().ptr() == texture;
      });
  }

  /* QueryInterface without counting it, for the immediate context's own QueryInterface */
  HRESULT
  QueryInterfaceUncounted(REFIID riid, void **ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(ID3D11DeviceContext) ||
        riid == __uuidof(ID3D11DeviceContext1) || riid == __uuidof(ID3D11DeviceContext2) ||
        riid == __uuidof(ID3D11DeviceContext3) || riid == __uuidof(ID3D11DeviceContext4) ||
        riid == __uuidof(IMTLD3D11DeviceContext)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (riid == __uuidof(IMTLD3D11ContextExt) || riid == __uuidof(IMTLD3D11ContextExt1)) {
      *ppvObject = ref(&ext_);
      return S_OK;
    }

    if (riid == __uuidof(ID3DUserDefinedAnnotation)) {
      *ppvObject = ref(&annotation_);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D11DeviceContext4), riid)) {
      WARN("D3D11DeviceContext: Unknown interface query ", str::format(riid));
    }
    return E_NOINTERFACE;
  }

  void
  STDMETHODCALLTYPE
  SetResourceMinLOD(ID3D11Resource *pResource, FLOAT MinLOD) override {
    D11_CTX_CALL(SetResourceMinLOD);
    if (auto expected = com_cast<IMTLMinLODClampable>(pResource)) {
      expected->SetMinLOD(MinLOD);
    }
  }

  FLOAT
  STDMETHODCALLTYPE
  GetResourceMinLOD(ID3D11Resource *pResource) override {
    D11_CTX_CALL(GetResourceMinLOD);
    if (auto expected = com_cast<IMTLMinLODClampable>(pResource)) {
      return expected->GetMinLOD();
    }
    return 0.0f;
  }

#pragma region Resource Manipulation

  void
  STDMETHODCALLTYPE
  ClearRenderTargetView(ID3D11RenderTargetView *pRenderTargetView, const FLOAT ColorRGBA[4]) override {
    D11_CTX_CALL(ClearRenderTargetView);
    NotePassReason(d11cs::PassStart::Clear); // spec 05 clarification 63
    D11_WORK(work_.clears++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    ClearRenderTargetView(static_cast<D3D11RenderTargetView *>(pRenderTargetView), ColorRGBA);
  }

  void
  STDMETHODCALLTYPE
  ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView *pUnorderedAccessView, const UINT Values[4]) override {
    D11_CTX_CALL(ClearUnorderedAccessViewUint);
    NotePassReason(d11cs::PassStart::Clear); // spec 05 clarification 63
    D11_WORK(work_.clears++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    ClearUnorderedAccessViewUint(static_cast<D3D11UnorderedAccessView *>(pUnorderedAccessView), Values);
  }

  void
  STDMETHODCALLTYPE
  ClearUnorderedAccessViewUint(D3D11UnorderedAccessView *pUAV, const UINT Values[4]) {
    InvalidateCurrentPass(true);
    SwitchToComputeEncoder();
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc;
    pUAV->GetDesc(&desc);
    if (desc.ViewDimension == D3D11_UAV_DIMENSION_BUFFER)
      EmitOP([=, buffer = pUAV->buffer(), viewId = pUAV->viewId(), slice = pUAV->bufferSlice(),
            value = std::array<uint32_t, 4>({Values[0], Values[1], Values[2], Values[3]}),
            is_raw = desc.Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW,
            format = desc.Format](ArgumentEncodingContext &enc) {
        if (is_raw) {
          auto [buffer_alloc, offset] = enc.access(buffer, slice.byteOffset, slice.byteLength, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          enc.emulated_cmd.ClearBufferUint(buffer_alloc->buffer(), slice.byteOffset + offset, slice.byteLength >> 2, value);
        } else {
          if (format == DXGI_FORMAT_UNKNOWN) {
            auto [buffer_alloc, offset] = enc.access(buffer, slice.byteOffset, slice.byteLength, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
            enc.emulated_cmd.ClearBufferUint(buffer_alloc->buffer(), slice.byteOffset + offset, slice.byteLength >> 2, value);
          } else {
            auto [view, element_offset] = enc.access(buffer, viewId, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
            enc.emulated_cmd.ClearTextureBufferUint(view.texture, slice.firstElement + element_offset, slice.elementCount, value);
          }
        }
      });
    else
      EmitOP([=, texture = pUAV->texture(), viewId = pUAV->viewId(),
            value = std::array<uint32_t, 4>({Values[0], Values[1], Values[2], Values[3]}),
            dimension = desc.ViewDimension](ArgumentEncodingContext &enc) mutable {
        auto view_format = texture->pixelFormat(viewId);
        auto uint_format = MTLGetUnsignedIntegerFormat((WMTPixelFormat)view_format);
        /* RG11B10Float is a special case */
        if (view_format == WMTPixelFormatRG11B10Float) {
          uint_format = WMTPixelFormatR32Uint;
          value[0] = ((value[0] & 0x7FF) << 0) | ((value[1] & 0x7FF) << 11) | ((value[2] & 0x3FF) << 22);
        }

        if (uint_format == WMTPixelFormatInvalid) {
          WARN("ClearUnorderedAccessViewUint: unhandled format ", view_format);
          return;
        }
        auto viewChecked = texture->checkViewUseFormat(viewId, uint_format);
        WMT::Texture texture_handle = enc.access(texture, viewChecked, DXMT_ENCODER_RESOURCE_ACESS_WRITE).texture;
        switch (dimension) {
        default:
          break;
        case D3D11_UAV_DIMENSION_TEXTURE1D:
        case D3D11_UAV_DIMENSION_TEXTURE2D:
          enc.emulated_cmd.ClearTexture2DUint(texture_handle, value);
          break;
        case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
        case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
          enc.emulated_cmd.ClearTexture2DArrayUint(texture_handle, value);
          break;
        case D3D11_UAV_DIMENSION_TEXTURE3D:
          enc.emulated_cmd.ClearTexture3DUint(texture_handle, value);
          break;
        }
      });
    InvalidateCurrentPass();
  }

  void
  STDMETHODCALLTYPE
  ClearUnorderedAccessViewFloat(ID3D11UnorderedAccessView *pUnorderedAccessView, const FLOAT Values[4]) override {
    D11_CTX_CALL(ClearUnorderedAccessViewFloat);
    NotePassReason(d11cs::PassStart::Clear); // spec 05 clarification 63
    D11_WORK(work_.clears++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    ClearUnorderedAccessViewFloat(static_cast<D3D11UnorderedAccessView *>(pUnorderedAccessView), Values);
  }

  void
  STDMETHODCALLTYPE
  ClearUnorderedAccessViewFloat(D3D11UnorderedAccessView *pUAV, const FLOAT Values[4])  {
    InvalidateCurrentPass(true);
    SwitchToComputeEncoder();
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc;
    pUAV->GetDesc(&desc);
    if (desc.ViewDimension == D3D11_UAV_DIMENSION_BUFFER)
      EmitOP([=, buffer = pUAV->buffer(), viewId = pUAV->viewId(), slice = pUAV->bufferSlice(),
            value = std::array<float, 4>({Values[0], Values[1], Values[2], Values[3]}),
            is_raw = desc.Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW,
            format = desc.Format](ArgumentEncodingContext &enc) {
        if (is_raw) {
          auto [buffer_alloc, offset] = enc.access(buffer, slice.byteOffset, slice.byteLength, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          enc.emulated_cmd.ClearBufferFloat(buffer_alloc->buffer(), slice.byteOffset + offset, slice.byteLength >> 2, value);
        } else {
          if (format == DXGI_FORMAT_UNKNOWN) {
            auto [buffer_alloc, offset] = enc.access(buffer, slice.byteOffset, slice.byteLength, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
            enc.emulated_cmd.ClearBufferFloat(buffer_alloc->buffer(), slice.byteOffset + offset, slice.byteLength >> 2, value);
          } else {
            auto [view, element_offset] = enc.access(buffer, viewId, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
            enc.emulated_cmd.ClearTextureBufferFloat(view.texture, slice.firstElement + element_offset, slice.elementCount, value);
          }
        }
      });
    else
      EmitOP([=, texture = pUAV->texture(), viewId = pUAV->viewId(), dimension = desc.ViewDimension,
            value = std::array<float, 4>({Values[0], Values[1], Values[2], Values[3]})](ArgumentEncodingContext &enc) {
        WMT::Texture texture_handle = enc.access(texture, viewId, DXMT_ENCODER_RESOURCE_ACESS_WRITE).texture;
        switch (dimension) {
        default:
          break;
        case D3D11_UAV_DIMENSION_TEXTURE1D:
        case D3D11_UAV_DIMENSION_TEXTURE2D:
          enc.emulated_cmd.ClearTexture2DFloat(texture_handle, value);
          break;
        case D3D11_UAV_DIMENSION_TEXTURE1DARRAY:
        case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
          enc.emulated_cmd.ClearTexture2DArrayFloat(texture_handle, value);
          break;
        case D3D11_UAV_DIMENSION_TEXTURE3D:
          enc.emulated_cmd.ClearTexture3DFloat(texture_handle, value);
          break;
        }
      });
    InvalidateCurrentPass();
  }

  void
  STDMETHODCALLTYPE
  ClearDepthStencilView(ID3D11DepthStencilView *pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
      override {
    D11_CTX_CALL(ClearDepthStencilView);
    NotePassReason(d11cs::PassStart::Clear); // spec 05 clarification 63
    D11_WORK(work_.clears++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    ClearDepthStencilView(static_cast<D3D11DepthStencilView*>(pDepthStencilView), ClearFlags, Depth, Stencil);
  }

  void
  STDMETHODCALLTYPE
  ClearView(ID3D11View *pView, const FLOAT Color[4], const D3D11_RECT *pRect, UINT NumRects) override {
    D11_CTX_CALL(ClearView);
    NotePassReason(d11cs::PassStart::Clear); // spec 05 clarification 63
    D11_WORK(work_.clears++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    if (NumRects && !pRect)
      return;
    if (!pView)
      return;

    auto color = std::array<float, 4>({Color[0], Color[1], Color[2], Color[3]});

    while (auto expected = com_cast<ID3D11RenderTargetView>(pView)) {
      auto rtv = static_cast<D3D11RenderTargetView *>(expected.ptr());
      // d3d11 spec: ClearView doesn’t support 3D textures.
      if (rtv->texture()->textureType(rtv->viewId()) == WMTTextureType3D)
        return;
      // check if rtv is fully cleared (which can be potentially optimized as a LoadActionClear)
      while (NumRects <= 1) {
        if (pRect) {
          if (pRect[0].top != 0 || pRect[0].left != 0)
            break;
          if (pRect[0].bottom < 0 || pRect[0].right < 0)
            break;
          if (uint32_t(pRect[0].right) != rtv->description().Width)
            break;
          if (uint32_t(pRect[0].bottom) != rtv->description().Height)
            break;
        }
        return ClearRenderTargetView(rtv, Color);
      }
      InvalidateCurrentPass(true);
      EmitST([texture = rtv->texture(), view = rtv->viewId()](ArgumentEncodingContext &enc) {
        enc.clear_rt_cmd.begin(texture, view);
      });
      for (unsigned i = 0; i < NumRects; i++) {
        auto rect = pRect[i];
        uint32_t rect_offset_x = std::max(rect.left, (LONG)0);
        uint32_t rect_offset_y = std::max(rect.top, (LONG)0);
        int32_t rect_width = rect.right - rect_offset_x;
        int32_t rect_height = rect.bottom - rect_offset_y;
        if (rect_height <= 0 || rect_width <= 0)
          continue;
        EmitOP([=](ArgumentEncodingContext &enc) {
          enc.clear_rt_cmd.clear(rect_offset_x, rect_offset_y, rect_width, rect_height, color);
        });
      }
      EmitST([](ArgumentEncodingContext &enc) { 
        enc.clear_rt_cmd.end();
      });
      return;
    }

    while (auto expected = com_cast<ID3D11DepthStencilView>(pView)) {
      auto dsv = static_cast<D3D11DepthStencilView *>(expected.ptr());
      // d3d11 spec: ClearView doesn’t support 3D textures.
      if (dsv->texture()->textureType(dsv->viewId()) == WMTTextureType3D)
        return;
      // check if rtv is fully cleared (which can be potentially optimized as a LoadActionClear)
      while (NumRects <= 1) {
        if (pRect) {
          if (pRect[0].top != 0 || pRect[0].left != 0)
            break;
          if (pRect[0].bottom < 0 || pRect[0].right < 0)
            break;
          if (uint32_t(pRect[0].right) != dsv->description().Width)
            break;
          if (uint32_t(pRect[0].bottom) != dsv->description().Height)
            break;
        }
        return ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, color[0], 0);
      }
      InvalidateCurrentPass(true);
      EmitST([texture = dsv->texture(), view = dsv->viewId()](ArgumentEncodingContext &enc) {
        enc.clear_rt_cmd.begin(texture, view);
      });
      for (unsigned i = 0; i < NumRects; i++) {
        auto rect = pRect[i];
        uint32_t rect_offset_x = std::max(rect.left, (LONG)0);
        uint32_t rect_offset_y = std::max(rect.top, (LONG)0);
        int32_t rect_width = rect.right - rect_offset_x;
        int32_t rect_height = rect.bottom - rect_offset_y;
        if (rect_height <= 0 || rect_width <= 0)
          continue;
        EmitOP([=](ArgumentEncodingContext &enc) {
          enc.clear_rt_cmd.clear(rect_offset_x, rect_offset_y, rect_width, rect_height, color);
        });
      }
      EmitST([](ArgumentEncodingContext &enc) { 
        enc.clear_rt_cmd.end();
      });
      return;
    }

    if (auto expected = com_cast<ID3D11UnorderedAccessView>(pView)) {
      InvalidateCurrentPass(true);
      auto uav = static_cast<D3D11UnorderedAccessView *>(expected.ptr());
      D3D11_UNORDERED_ACCESS_VIEW_DESC1 desc;
      uav->GetDesc1(&desc);
      if (desc.ViewDimension != D3D11_UAV_DIMENSION_BUFFER) {
        EmitST([=, texture = uav->texture(),
                view = uav->viewId()](ArgumentEncodingContext &enc) {
          enc.clear_res_cmd.begin(color, texture, view);
        });
      } else if (desc.Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW) {
        EmitST([=, buffer = uav->buffer(),
                view = uav->viewId()](ArgumentEncodingContext &enc) {
          enc.clear_res_cmd.begin(color, buffer, view);
        });
      } else {
        EmitST([=, buffer = uav->buffer()](ArgumentEncodingContext &enc) {
          /* FIXME: raw buffer always considered unsigned integer? */
          enc.clear_res_cmd.begin(color, buffer, true);
        });
      }
      for (unsigned i = 0; i < NumRects; i++) {
        auto rect = pRect[i];
        uint32_t rect_offset_x = std::max(rect.left, (LONG)0);
        uint32_t rect_offset_y = std::max(rect.top, (LONG)0);
        int32_t rect_width = rect.right - rect_offset_x;
        int32_t rect_height = rect.bottom - rect_offset_y;
        if (rect_height <= 0 || rect_width <= 0)
          continue;
        EmitOP([=](ArgumentEncodingContext &enc) {
          enc.clear_res_cmd.clear(rect_offset_x, rect_offset_y, rect_width, rect_height);
        });
      }
      EmitST([](ArgumentEncodingContext &enc) { enc.clear_res_cmd.end(); });
      return;
    }
  }

  void
  STDMETHODCALLTYPE
  GenerateMips(ID3D11ShaderResourceView *pShaderResourceView) override {
    D11_CTX_CALL(GenerateMips);
    NotePassReason(d11cs::PassStart::Copy); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    if (auto srv = static_cast<D3D11ShaderResourceView *>(pShaderResourceView)) {
      D3D11_SHADER_RESOURCE_VIEW_DESC desc;
      pShaderResourceView->GetDesc(&desc);
      if (desc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER || desc.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX) {
        return;
      }
      SwitchToBlitEncoder(CommandBufferState::BlitEncoderActive);
      EmitOP([tex = srv->texture(), viewId = srv->viewId()](ArgumentEncodingContext &enc) {
        // workaround: mipmap generation of a8unorm is borked, so use a r8unorm view
        auto fixedViewId = viewId;
        if (tex->pixelFormat(viewId) == WMTPixelFormatA8Unorm) {
          fixedViewId = tex->checkViewUseFormat(viewId, WMTPixelFormatR8Unorm);
        }
        WMT::Texture texture = enc.access(tex, fixedViewId, DXMT_ENCODER_RESOURCE_ACESS_READWRITE).texture;
        if (texture.mipmapLevelCount() > 1) {
          auto &cmd = enc.encodeBlitCommand<wmtcmd_blit_generate_mipmaps>();
          cmd.type = WMTBlitCommandGenerateMipmaps;
          cmd.texture = texture;
        }
      });
    }
  }

  void
  STDMETHODCALLTYPE
  ResolveSubresource(
      ID3D11Resource *pDstResource, UINT DstSubresource, ID3D11Resource *pSrcResource, UINT SrcSubresource,
      DXGI_FORMAT Format
  ) override {
    D11_CTX_CALL(ResolveSubresource);
    NotePassReason(d11cs::PassStart::Copy); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    if (!pDstResource || !pSrcResource)
      return;
    if (pDstResource == pSrcResource && DstSubresource == SrcSubresource)
      return;

    BlitObject Dst(device, pDstResource);
    if (Dst.Dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      return;
    if (Dst.Texture2DDesc.SampleDesc.Count != 1)
      return;
    if (DstSubresource >= Dst.Texture2DDesc.MipLevels * Dst.Texture2DDesc.ArraySize)
      return;

    BlitObject Src(device, pSrcResource);
    if (Src.Dimension != D3D11_RESOURCE_DIMENSION_TEXTURE2D)
      return;
    if (SrcSubresource >= Src.Texture2DDesc.MipLevels * Src.Texture2DDesc.ArraySize)
      return;

    if (Src.Texture2DDesc.SampleDesc.Count == 1)
      return CopyTexture(TextureCopyCommand(Dst, DstSubresource, 0, 0, 0, Src, SrcSubresource, nullptr));


    MTL_DXGI_FORMAT_DESC format_desc;
    if (FAILED(MTLQueryDXGIFormat(device->GetMTLDevice(), Format, format_desc))) {
      ERR("ResolveSubresource: invalid format ", Format);
      return;
    }
    InvalidateCurrentPass();
    EmitOP([src = static_cast<D3D11ResourceCommon *>(pSrcResource)->texture(),
            dst = static_cast<D3D11ResourceCommon *>(pDstResource)->texture(),
            dst_level = DstSubresource % Dst.Texture2DDesc.MipLevels,
            dst_slice = DstSubresource / Dst.Texture2DDesc.MipLevels, SrcSubresource,
            format = format_desc.PixelFormat](ArgumentEncodingContext &enc) mutable {
      TextureViewDescriptor src_desc;
      src_desc.format = format;
      src_desc.type = src->textureType();
      src_desc.arraySize = 1;
      src_desc.firstArraySlice = SrcSubresource; // src must be a MS(Array) texture which has exactly 1 mipmap level
      src_desc.miplevelCount = 1;
      src_desc.firstMiplevel = 0;

      TextureViewDescriptor dst_desc;
      dst_desc.format = format;
      dst_desc.type = WMTTextureType2D;
      dst_desc.arraySize = 1;
      dst_desc.firstArraySlice = dst_slice;
      dst_desc.miplevelCount = 1;
      dst_desc.firstMiplevel = dst_level;

      auto src_view = src->createView(src_desc);
      auto dst_view = dst->createView(dst_desc);

      enc.resolveTexture(forward_rc(src), src_view, forward_rc(dst), dst_view);
    });
  }

  void
  STDMETHODCALLTYPE
  CopyResource(ID3D11Resource *pDstResource, ID3D11Resource *pSrcResource) override {
    D11_CTX_CALL(CopyResource);
    NotePassReason(d11cs::PassStart::Copy); // spec 05 clarification 63
    D11_WORK(work_.copies++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    BlitObject Dst(device, pDstResource);
    BlitObject Src(device, pSrcResource);

    switch (Dst.Dimension) {
    case D3D11_RESOURCE_DIMENSION_UNKNOWN:
      break;
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      if (Src.Dimension != D3D11_RESOURCE_DIMENSION_BUFFER)
        return;
      CopyBuffer((ID3D11Buffer *)pDstResource, 0, 0, 0, 0, (ID3D11Buffer *)pSrcResource, 0, nullptr);
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
      D3D11_TEXTURE1D_DESC desc;
      ((ID3D11Texture1D *)pSrcResource)->GetDesc(&desc);
      for (auto sub : EnumerateSubresources(desc)) {
        CopyTexture(TextureCopyCommand(Dst, sub.SubresourceId, 0, 0, 0, Src, sub.SubresourceId, nullptr));
      }
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
      D3D11_TEXTURE2D_DESC1 desc;
      ((ID3D11Texture2D1 *)pSrcResource)->GetDesc1(&desc);
      for (auto sub : EnumerateSubresources(desc)) {
        CopyTexture(TextureCopyCommand(Dst, sub.SubresourceId, 0, 0, 0, Src, sub.SubresourceId, nullptr));
      }
      break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
      D3D11_TEXTURE3D_DESC1 desc;
      ((ID3D11Texture3D1 *)pSrcResource)->GetDesc1(&desc);
      for (auto sub : EnumerateSubresources(desc)) {
        CopyTexture(TextureCopyCommand(Dst, sub.SubresourceId, 0, 0, 0, Src, sub.SubresourceId, nullptr));
      }
      break;
    }
    }
  }

  void
  STDMETHODCALLTYPE
  CopySubresourceRegion(
      ID3D11Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource *pSrcResource,
      UINT SrcSubresource, const D3D11_BOX *pSrcBox
  ) override {
    D11_CTX_CALL(CopySubresourceRegion);
    CopySubresourceRegion1(pDstResource, DstSubresource, DstX, DstY, DstZ, pSrcResource, SrcSubresource, pSrcBox, 0);
  }

  void
  STDMETHODCALLTYPE
  CopySubresourceRegion1(
      ID3D11Resource *pDstResource, UINT DstSubresource, UINT DstX, UINT DstY, UINT DstZ, ID3D11Resource *pSrcResource,
      UINT SrcSubresource, const D3D11_BOX *pSrcBox, UINT CopyFlags
  ) override {
    D11_CTX_CALL(CopySubresourceRegion1);
    NotePassReason(d11cs::PassStart::Copy); // spec 05 clarification 63
    D11_WORK(work_.copies++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    if (!pDstResource)
      return;
    if (!pSrcResource)
      return;

    BlitObject Dst(device, pDstResource);
    BlitObject Src(device, pSrcResource);

    switch (Dst.Dimension) {
    case D3D11_RESOURCE_DIMENSION_UNKNOWN:
      break;
    case D3D11_RESOURCE_DIMENSION_BUFFER: {
      if (Src.Dimension != D3D11_RESOURCE_DIMENSION_BUFFER)
        return;
      CopyBuffer(
          (ID3D11Buffer *)pDstResource, DstSubresource, DstX, DstY, DstZ, (ID3D11Buffer *)pSrcResource, SrcSubresource,
          pSrcBox
      );
      break;
    }
    default:
      CopyTexture(TextureCopyCommand(Dst, DstSubresource, DstX, DstY, DstZ, Src, SrcSubresource, pSrcBox));
      break;
    }
  }

  void
  STDMETHODCALLTYPE
  CopyStructureCount(ID3D11Buffer *pDstBuffer, UINT DstAlignedByteOffset, ID3D11UnorderedAccessView *pSrcView)
      override {
    D11_CTX_CALL(CopyStructureCount);
    NotePassReason(d11cs::PassStart::Copy); // spec 05 clarification 63
    D11_WORK(work_.copies++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    if (auto dst_bind = reinterpret_cast<D3D11ResourceCommon *>(pDstBuffer)) {
      if (auto uav = static_cast<D3D11UnorderedAccessView *>(pSrcView)) {
        SwitchToBlitEncoder(CommandBufferState::BlitEncoderActive);
        EmitOP([=, dst = dst_bind->buffer(), counter = uav->counter()](ArgumentEncodingContext &enc) {
          auto [dst_buffer, dst_offset] = enc.access(dst, DstAlignedByteOffset, 4, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          auto [counter_buffer, counter_offset] = enc.access(counter, 0, 4, DXMT_ENCODER_RESOURCE_ACESS_READ);
          auto &cmd = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
          cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
          cmd.copy_length = 4;
          cmd.src = counter_buffer->buffer();
          cmd.src_offset = counter_offset;
          cmd.dst = dst_buffer->buffer();
          cmd.dst_offset = DstAlignedByteOffset + dst_offset;
        });
      }
    }
  }

  void
  STDMETHODCALLTYPE
  UpdateSubresource(
      ID3D11Resource *pDstResource, UINT DstSubresource, const D3D11_BOX *pDstBox, const void *pSrcData,
      UINT SrcRowPitch, UINT SrcDepthPitch
  ) override {
    D11_CTX_CALL(UpdateSubresource);
    UpdateSubresource1(pDstResource, DstSubresource, pDstBox, pSrcData, SrcRowPitch, SrcDepthPitch, 0);
  }

  void
  STDMETHODCALLTYPE
  UpdateSubresource1(
      ID3D11Resource *pDstResource, UINT DstSubresource, const D3D11_BOX *pDstBox, const void *pSrcData,
      UINT SrcRowPitch, UINT SrcDepthPitch, UINT CopyFlags
  ) override {
    D11_CTX_CALL(UpdateSubresource1);
    NotePassReason(d11cs::PassStart::Copy); // spec 05 clarification 63
    D11_WORK(work_.updates++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    if (!pDstResource)
      return;

    SerializeBeforeCpuWrite(); // spec 05 clarification 57 (level 3): the upload allocations

    BlitObject Dst(device, pDstResource);

    if (Dst.Dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
      D3D11_BUFFER_DESC desc;
      ((ID3D11Buffer *)pDstResource)->GetDesc(&desc);
      uint32_t copy_offset = 0;
      uint32_t copy_len = desc.ByteWidth;
      if (pDstBox) {
        if (pDstBox->right <= pDstBox->left) {
          return;
        }
        copy_offset = pDstBox->left;
        copy_len = pDstBox->right - copy_offset;
      }
      UINT buffer_len = 0;
      UINT unused_bind_flag = 0;
      if (auto dynamic = GetDynamicBuffer(pDstResource, &buffer_len, &unused_bind_flag)) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if ((CopyFlags & D3D11_COPY_DISCARD) || (copy_len == buffer_len && copy_offset == 0)) {
          Map(pDstResource, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
          auto [allocation, sub] = GetDynamicBufferAllocation(dynamic);
          allocation->updateContents(copy_offset, pSrcData, copy_len, sub);
          Unmap(pDstResource, 0);
          return;
        }
        if (CopyFlags & D3D11_COPY_NO_OVERWRITE) {
          Map(pDstResource, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped);
          auto [allocation, sub] = GetDynamicBufferAllocation(dynamic);
          allocation->updateContents(copy_offset, pSrcData, copy_len, sub);
          Unmap(pDstResource, 0);
          return;
        }
      }
      if (auto staging = GetStagingResource(pDstResource, DstSubresource)) {
        /* LGPL-2.1-or-later, from steamac's DXMT patch 0005 (osy): MSDN excludes only
           IMMUTABLE and DYNAMIC destinations, so a STAGING one is legal; order the write
           with the rest of the GPU timeline by blitting it into the staging buffer, like
           a copy into staging. */
        auto [upload, upload_offset] = AllocateStagingBuffer(copy_len, 16);
        upload.updateContents(upload_offset, pSrcData, copy_len);
        SwitchToBlitEncoder(CommandBufferState::UpdateBlitEncoderActive);
        UseCopyDestination(staging);
        EmitOP([upload, upload_offset, dst_ = std::move(staging), copy_offset,
                copy_len](ArgumentEncodingContext &enc) {
          auto [dst, dst_offset] = enc.access(dst_->buffer(), copy_offset, copy_len, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          auto &cmd = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
          cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
          cmd.copy_length = copy_len;
          cmd.src = upload;
          cmd.src_offset = upload_offset;
          cmd.dst = dst->buffer();
          cmd.dst_offset = copy_offset + dst_offset;
        });
        promote_flush = true;
        /* end of LGPL block */
      } else if (auto bindable = reinterpret_cast<D3D11ResourceCommon *>(pDstResource)) {
        // if (auto _ = UseImmediate(bindable.ptr())) {
        //   auto buffer = _.buffer();
        //   memcpy(((char *)buffer->contents()) + copy_offset, pSrcData, copy_len);
        //   buffer->didModifyRange(NS::Range::Make(copy_offset, copy_len));
        //   return;
        // }
        auto [staging_buffer, offset] = AllocateStagingBuffer(copy_len, 16);
        staging_buffer.updateContents(offset, pSrcData, copy_len);
        SwitchToBlitEncoder(CommandBufferState::UpdateBlitEncoderActive);
        EmitOP([staging_buffer, offset, dst = bindable->buffer(), copy_offset, copy_len](ArgumentEncodingContext &enc) {
          auto [dst_buffer, dst_offset] = enc.access(dst, copy_offset, copy_len, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          auto &cmd = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
          cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
          cmd.copy_length = copy_len;
          cmd.src = staging_buffer;
          cmd.src_offset = offset;
          cmd.dst = dst_buffer->buffer();
          cmd.dst_offset = copy_offset + dst_offset;
        });
      } else {
        UNIMPLEMENTED("UpdateSubresource1: TODO: staging?");
      }
      return;
    }

    UpdateTexture(TextureUpdateCommand(Dst, DstSubresource, pDstBox), pSrcData, SrcRowPitch, SrcDepthPitch, CopyFlags);
  }

  void
  STDMETHODCALLTYPE
  DiscardResource(ID3D11Resource *pResource) override {
    D11_CTX_CALL(DiscardResource);
    std::lock_guard<mutex_t> lock(mutex);
    // MYIOSDECK spec 05 clarification 71: every render-target or depth-stencil subresource of a
    // texture (DXMT_PASS_DISCARD; noted for the would-entries when counting or tracing)
    if (!pResource || !DiscardNoting())
      return;
    auto texture = static_cast<D3D11ResourceCommon *>(pResource)->texture();
    if (!texture.ptr())
      return; // a buffer: nothing to do
    DiscardSubresources(texture, 0, texture->mipLevelCount(), 0, texture->arrayLength(Texture::fullView));
  }

  void
  STDMETHODCALLTYPE
  DiscardView(ID3D11View *pResourceView) override {
    D11_CTX_CALL(DiscardView);
    DiscardViewUncounted(pResourceView, nullptr, 0);
  }

  void
  STDMETHODCALLTYPE
  DiscardView1(ID3D11View *pResourceView, const D3D11_RECT *pRects, UINT NumRects) override {
    D11_CTX_CALL(DiscardView1);
    DiscardViewUncounted(pResourceView, pRects, NumRects);
  }

  /* spec 05 clarification 71: a render-target or depth-stencil view's subresources; with
     rectangles, nothing, as before */
  void
  DiscardViewUncounted(ID3D11View *pResourceView, const D3D11_RECT *pRects, UINT NumRects) {
    std::lock_guard<mutex_t> lock(mutex);
    if (!pResourceView || DiscardHasRectangles(pRects, NumRects) || !DiscardNoting())
      return;
    if (auto rtv = com_cast<ID3D11RenderTargetView>(pResourceView)) {
      auto *view = static_cast<D3D11RenderTargetView *>(rtv.ptr());
      auto texture = view->texture();
      DiscardSubresources(
          texture, texture->firstMiplevel(view->viewId()), 1, texture->firstArraySlice(view->viewId()),
          texture->arrayLength(view->viewId())
      );
    } else if (auto dsv = com_cast<ID3D11DepthStencilView>(pResourceView)) {
      auto *view = static_cast<D3D11DepthStencilView *>(dsv.ptr());
      auto texture = view->texture();
      DiscardSubresources(
          texture, texture->firstMiplevel(view->viewId()), 1, texture->firstArraySlice(view->viewId()),
          texture->arrayLength(view->viewId())
      );
    }
  }

  bool
  DiscardNoting() {
    return CutSettingsOnce()[CutSwitch::Discard] || d11cs::g_level || TraceRecording();
  }

  void
  DiscardSubresources(Rc<Texture> &texture, unsigned level, unsigned levels, unsigned slice, unsigned slices) {
    if (!texture.ptr() || !levels || !slices)
      return;
    // with the switch on, an open pass that attaches the texture ends first, so that its store
    // can be dropped and the draws after the discard start a pass that doesn't load it
    const bool render_pass_open = cmdbuf_state == CommandBufferState::RenderEncoderActive ||
                                  cmdbuf_state == CommandBufferState::RenderPipelineReady ||
                                  cmdbuf_state == CommandBufferState::TessellationRenderPipelineReady ||
                                  cmdbuf_state == CommandBufferState::GeometryRenderPipelineReady;
    if (CutSettingsOnce()[CutSwitch::Discard] && render_pass_open) { // pass_targets_ is that pass's
      bool attached = pass_targets_.dsv &&
                      static_cast<D3D11DepthStencilView *>(const_cast<void *>(pass_targets_.dsv))->texture().ptr() ==
                          texture.ptr();
      for (unsigned i = 0; i < pass_targets_.num_rtvs && i < 8; i++)
        attached |= pass_targets_.rtvs[i] &&
                    static_cast<D3D11RenderTargetView *>(const_cast<void *>(pass_targets_.rtvs[i]))->texture().ptr() ==
                        texture.ptr();
      if (attached)
        InvalidateCurrentPass();
    }
    EmitST([texture, level, levels, slice, slices](ArgumentEncodingContext &enc) {
      enc.discardTexture(texture, level, levels, slice, slices);
    });
  }
#pragma endregion

#pragma region DrawCall

  void
  STDMETHODCALLTYPE
  Draw(UINT VertexCount, UINT StartVertexLocation) override {
    D11_CTX_CALL(Draw);
    D11_WORK(work_.draws++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    WMTPrimitiveType Primitive;
    uint32_t ControlPointCount;
    if(!to_metal_primitive_type(state_.InputAssembler.Topology, Primitive, ControlPointCount))
      return;
    TraceVSBufferReadback(); // spec 05 clarification 50
    DrawCallStatus status = PreDraw<false>();
    if (status == DrawCallStatus::Invalid)
      return TraceDrawDropped();
    TraceDraw("draw", false, false, VertexCount, StartVertexLocation, 0, 1, 0); // spec 05 clarification 44
    if (status == DrawCallStatus::Geometry) {
      return GeometryDraw(VertexCount, 1, StartVertexLocation, 0);
    }
    if (status == DrawCallStatus::Tessellation) {
      return TessellationDraw(ControlPointCount, VertexCount, 1, StartVertexLocation, 0);
    }
    EmitOP([Primitive, StartVertexLocation, VertexCount](ArgumentEncodingContext& enc) {
      enc.bumpVisibilityResultOffset();
      auto &cmd = enc.encodeRenderCommand<wmtcmd_render_draw>();
      cmd.type = WMTRenderCommandDraw;
      cmd.primitive_type = Primitive;
      cmd.vertex_start = StartVertexLocation;
      cmd.vertex_count = VertexCount;
      cmd.base_instance = 0;
      cmd.instance_count = 1;
    });
  }

  void
  STDMETHODCALLTYPE
  DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation) override {
    D11_CTX_CALL(DrawIndexed);
    D11_WORK(work_.draws++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    if (!IndexCount)
      return;
    WMTPrimitiveType Primitive;
    uint32_t ControlPointCount;
    if(!to_metal_primitive_type(state_.InputAssembler.Topology, Primitive, ControlPointCount))
      return;
    TraceVSBufferReadback(); // spec 05 clarification 50
    DrawCallStatus status = PreDraw<true>();
    if (status == DrawCallStatus::Invalid)
      return TraceDrawDropped();
    TraceDraw("draw indexed", true, false, IndexCount, StartIndexLocation, BaseVertexLocation, 1, 0); // spec 05 cl. 44
    if (status == DrawCallStatus::Geometry) {
      return GeometryDrawIndexed(IndexCount, StartIndexLocation, BaseVertexLocation, 1, 0);
    }
    if (status == DrawCallStatus::Tessellation) {
      return TessellationDrawIndexed(ControlPointCount, IndexCount, StartIndexLocation, BaseVertexLocation, 1, 0);
    };
    auto IndexType =
        state_.InputAssembler.IndexBufferFormat == DXGI_FORMAT_R32_UINT ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16;
    auto IndexBufferOffset =
        state_.InputAssembler.IndexBufferOffset +
        StartIndexLocation * (state_.InputAssembler.IndexBufferFormat == DXGI_FORMAT_R32_UINT ? 4 : 2);
    EmitOP([IndexType, IndexBufferOffset, Primitive, IndexCount, BaseVertexLocation](ArgumentEncodingContext &enc) {
      enc.bumpVisibilityResultOffset();
      auto [index_buffer, index_sub_offset] = enc.currentIndexBuffer();
      auto &cmd = enc.encodeRenderCommand<wmtcmd_render_draw_indexed>();
      cmd.type = WMTRenderCommandDrawIndexed;
      cmd.primitive_type = Primitive;
      cmd.index_count = IndexCount;
      cmd.index_type = IndexType;
      cmd.index_buffer = index_buffer;
      cmd.index_buffer_offset = IndexBufferOffset + index_sub_offset;
      cmd.base_vertex = BaseVertexLocation;
      cmd.base_instance = 0;
      cmd.instance_count = 1;
    });
  }

  void
  STDMETHODCALLTYPE
  DrawInstanced(UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation)
      override {
    D11_CTX_CALL(DrawInstanced);
    D11_WORK(work_.draws++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    WMTPrimitiveType Primitive;
    uint32_t ControlPointCount;
    if (!to_metal_primitive_type(state_.InputAssembler.Topology, Primitive, ControlPointCount))
      return;
    TraceVSBufferReadback(); // spec 05 clarification 50
    DrawCallStatus status = PreDraw<false>();
    if (status == DrawCallStatus::Invalid)
      return TraceDrawDropped();
    TraceDraw(
        "draw instanced", false, false, VertexCountPerInstance, StartVertexLocation, 0, InstanceCount,
        StartInstanceLocation
    ); // spec 05 clarification 44
    if (status == DrawCallStatus::Geometry) {
      return GeometryDraw(VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation);
    }
    if (status == DrawCallStatus::Tessellation) {
      return TessellationDraw(
          ControlPointCount, VertexCountPerInstance, InstanceCount, StartVertexLocation, StartInstanceLocation
      );
    }
    EmitOP([Primitive, StartVertexLocation, VertexCountPerInstance, InstanceCount,
          StartInstanceLocation](ArgumentEncodingContext &enc) {
      enc.bumpVisibilityResultOffset();
      auto &cmd = enc.encodeRenderCommand<wmtcmd_render_draw>();
      cmd.type = WMTRenderCommandDraw;
      cmd.primitive_type = Primitive;
      cmd.vertex_start = StartVertexLocation;
      cmd.vertex_count = VertexCountPerInstance;
      cmd.instance_count = InstanceCount;
      cmd.base_instance = StartInstanceLocation;
    });
  }

  void
  STDMETHODCALLTYPE
  DrawIndexedInstanced(
      UINT IndexCountPerInstance, UINT InstanceCount, UINT StartIndexLocation, INT BaseVertexLocation,
      UINT StartInstanceLocation
  ) override {
    D11_CTX_CALL(DrawIndexedInstanced);
    D11_WORK(work_.draws++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    if (!IndexCountPerInstance)
      return;
    WMTPrimitiveType Primitive;
    uint32_t ControlPointCount;
    if(!to_metal_primitive_type(state_.InputAssembler.Topology, Primitive, ControlPointCount))
      return;
    TraceVSBufferReadback(); // spec 05 clarification 50
    DrawCallStatus status = PreDraw<true>();
    if (status == DrawCallStatus::Invalid)
      return TraceDrawDropped();
    TraceDraw(
        "draw indexed instanced", true, false, IndexCountPerInstance, StartIndexLocation, BaseVertexLocation,
        InstanceCount, StartInstanceLocation
    ); // spec 05 clarification 44
    if (status == DrawCallStatus::Geometry) {
      return GeometryDrawIndexed(
          IndexCountPerInstance, StartIndexLocation, BaseVertexLocation, InstanceCount, StartInstanceLocation
      );
    }
    if (status == DrawCallStatus::Tessellation) {
      return TessellationDrawIndexed(
          ControlPointCount, IndexCountPerInstance, StartIndexLocation, BaseVertexLocation, InstanceCount,
          StartInstanceLocation
      );
    }
    auto IndexType =
        state_.InputAssembler.IndexBufferFormat == DXGI_FORMAT_R32_UINT ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16;
    auto IndexBufferOffset =
        state_.InputAssembler.IndexBufferOffset +
        StartIndexLocation * (state_.InputAssembler.IndexBufferFormat == DXGI_FORMAT_R32_UINT ? 4 : 2);
    EmitOP([IndexType, IndexBufferOffset, Primitive, InstanceCount, BaseVertexLocation, StartInstanceLocation,
          IndexCountPerInstance](ArgumentEncodingContext &enc) {
      enc.bumpVisibilityResultOffset();
      auto [index_buffer, index_sub_offset] = enc.currentIndexBuffer();
      auto &cmd = enc.encodeRenderCommand<wmtcmd_render_draw_indexed>();
      cmd.type = WMTRenderCommandDrawIndexed;
      cmd.primitive_type = Primitive;
      cmd.index_count = IndexCountPerInstance;
      cmd.index_type = IndexType;
      cmd.index_buffer = index_buffer;
      cmd.index_buffer_offset = IndexBufferOffset + index_sub_offset;
      cmd.base_vertex = BaseVertexLocation;
      cmd.base_instance = StartInstanceLocation;
      cmd.instance_count = InstanceCount;
    });
  }

  void
  TessellationDraw(
      UINT NumControlPoint, UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation,
      UINT StartInstanceLocation
  ) {
    auto draw_arguments_offset = PreAllocateArgumentBuffer(sizeof(DXMT_DRAW_ARGUMENTS), 32);
    auto max_object_threadgroups = max_object_threadgroups_;
    EmitOP([=](ArgumentEncodingContext &enc) {
      DXMT_DRAW_ARGUMENTS *draw_arugment = enc.getMappedArgumentBuffer<DXMT_DRAW_ARGUMENTS>(draw_arguments_offset);
      draw_arugment->StartVertex = StartVertexLocation;
      draw_arugment->VertexCount = VertexCountPerInstance;
      draw_arugment->InstanceCount = InstanceCount;
      draw_arugment->StartInstance = StartInstanceLocation;

      auto PatchCountPerInstance = VertexCountPerInstance / NumControlPoint;
      auto PatchPerGroup = 32 / enc.tess_threads_per_patch;
      auto ThreadsPerPatch = enc.tess_threads_per_patch;
      auto PatchPerObjectInstance = (PatchCountPerInstance - 1) / PatchPerGroup + 1;

      if (PatchPerObjectInstance * InstanceCount > max_object_threadgroups) {
        WARN(
            "Omitted mesh draw (TS) because of too many object threadgroups (", PatchPerObjectInstance, "x",
            InstanceCount, ")"
        );
        return;
      }

      enc.bumpVisibilityResultOffset();
      auto &cmd = enc.encodeRenderCommand<wmtcmd_render_dxmt_tessellation_mesh_draw>();
      cmd.type = WMTRenderCommandDXMTTessellationMeshDraw;
      cmd.draw_arguments_offset = enc.getFinalArgumentBufferOffset(draw_arguments_offset);
      cmd.instance_count = InstanceCount;
      cmd.threads_per_patch = ThreadsPerPatch;
      cmd.patch_per_group = PatchPerGroup;
      cmd.patch_per_mesh_instance = PatchPerObjectInstance;
    });
  }

  void
  TessellationDrawIndexed(
      UINT NumControlPoint, UINT IndexCountPerInstance, UINT StartIndexLocation, INT BaseVertexLocation,
      UINT InstanceCount, UINT BaseInstance
  ) {
    auto IndexBufferOffset = state_.InputAssembler.IndexBufferOffset;
    auto draw_arguments_offset = PreAllocateArgumentBuffer(sizeof(DXMT_DRAW_INDEXED_ARGUMENTS), 32);
    auto max_object_threadgroups = max_object_threadgroups_;
    EmitOP([=](ArgumentEncodingContext &enc) {
      DXMT_DRAW_INDEXED_ARGUMENTS *draw_arugment = enc.getMappedArgumentBuffer<DXMT_DRAW_INDEXED_ARGUMENTS>(draw_arguments_offset);
      draw_arugment->BaseVertex = BaseVertexLocation;
      draw_arugment->IndexCount = IndexCountPerInstance;
      draw_arugment->StartIndex = StartIndexLocation;
      draw_arugment->InstanceCount = InstanceCount;
      draw_arugment->StartInstance = BaseInstance;

      auto PatchCountPerInstance = IndexCountPerInstance / NumControlPoint;
      auto PatchPerGroup = 32 / enc.tess_threads_per_patch;
      auto ThreadsPerPatch = enc.tess_threads_per_patch;
      auto PatchPerObjectInstance = (PatchCountPerInstance - 1) / PatchPerGroup + 1;

      if (PatchPerObjectInstance * InstanceCount > max_object_threadgroups) {
        WARN(
            "Omitted mesh draw (TS) because of too many object threadgroups (", PatchPerObjectInstance, "x",
            InstanceCount, ")"
        );
        return;
      }

      auto [index_buffer, index_sub_offset] = enc.currentIndexBuffer();
      enc.bumpVisibilityResultOffset();
      auto &cmd = enc.encodeRenderCommand<wmtcmd_render_dxmt_tessellation_mesh_draw_indexed>();
      cmd.type = WMTRenderCommandDXMTTessellationMeshDrawIndexed;
      cmd.draw_arguments_offset = enc.getFinalArgumentBufferOffset(draw_arguments_offset);
      cmd.instance_count = InstanceCount;
      cmd.threads_per_patch = ThreadsPerPatch;
      cmd.patch_per_group = PatchPerGroup;
      cmd.patch_per_mesh_instance = PatchPerObjectInstance;
      cmd.index_buffer = index_buffer;
      cmd.index_buffer_offset = IndexBufferOffset + index_sub_offset;
    });
  }

  void
  GeometryDraw(
      UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation,
      UINT StartInstanceLocation
  ) {
    auto draw_arguments_offset = PreAllocateArgumentBuffer(sizeof(DXMT_DRAW_ARGUMENTS), 32);
    auto max_object_threadgroups = max_object_threadgroups_;
    EmitOP([=, topo = state_.InputAssembler.Topology](ArgumentEncodingContext &enc) {
      DXMT_DRAW_ARGUMENTS *draw_arugment = enc.getMappedArgumentBuffer<DXMT_DRAW_ARGUMENTS>(draw_arguments_offset);
      draw_arugment->StartVertex = StartVertexLocation;
      draw_arugment->VertexCount = VertexCountPerInstance;
      draw_arugment->InstanceCount = InstanceCount;
      draw_arugment->StartInstance = StartInstanceLocation;

      auto [vertex_per_warp, vertex_increment_per_wrap] = get_gs_vertex_count(topo);
      auto warp_count = (VertexCountPerInstance - 1) / vertex_increment_per_wrap + 1;

      if (warp_count * InstanceCount > max_object_threadgroups) {
        WARN("Omitted mesh draw (GS) because of too many object threadgroups(", warp_count, "x", InstanceCount, ")");
        return;
      }

      enc.bumpVisibilityResultOffset();
      auto &cmd = enc.encodeRenderCommand<wmtcmd_render_dxmt_geometry_draw>();
      cmd.type = WMTRenderCommandDXMTGeometryDraw;
      cmd.draw_arguments_offset = enc.getFinalArgumentBufferOffset(draw_arguments_offset);
      cmd.instance_count = InstanceCount;
      cmd.warp_count = warp_count;
      cmd.vertex_per_warp = vertex_per_warp;
    });
  }

  void
  GeometryDrawIndexed(
      UINT IndexCountPerInstance, UINT StartIndexLocation, INT BaseVertexLocation,
      UINT InstanceCount, UINT BaseInstance
  ) {
    auto IndexBufferOffset = state_.InputAssembler.IndexBufferOffset;
    auto draw_arguments_offset = PreAllocateArgumentBuffer(sizeof(DXMT_DRAW_INDEXED_ARGUMENTS), 32);
    auto max_object_threadgroups = max_object_threadgroups_;
    EmitOP([=, topo = state_.InputAssembler.Topology](ArgumentEncodingContext &enc) {
      DXMT_DRAW_INDEXED_ARGUMENTS *draw_arugment = enc.getMappedArgumentBuffer<DXMT_DRAW_INDEXED_ARGUMENTS>(draw_arguments_offset);
      draw_arugment->BaseVertex = BaseVertexLocation;
      draw_arugment->IndexCount = IndexCountPerInstance;
      draw_arugment->StartIndex = StartIndexLocation;
      draw_arugment->InstanceCount = InstanceCount;
      draw_arugment->StartInstance = BaseInstance;

      auto [index_buffer, index_sub_offset] = enc.currentIndexBuffer();
      auto [vertex_per_warp, vertex_increment_per_wrap] = get_gs_vertex_count(topo);
      auto warp_count = (IndexCountPerInstance - 1) / vertex_increment_per_wrap + 1;

      if (warp_count * InstanceCount > max_object_threadgroups) {
        WARN("Omitted mesh draw (GS) because of too many object threadgroups(", warp_count, "x", InstanceCount, ")");
        return;
      }

      enc.bumpVisibilityResultOffset();
      auto &cmd = enc.encodeRenderCommand<wmtcmd_render_dxmt_geometry_draw_indexed>();
      cmd.type = WMTRenderCommandDXMTGeometryDrawIndexed;
      cmd.draw_arguments_offset = enc.getFinalArgumentBufferOffset(draw_arguments_offset);
      cmd.instance_count = InstanceCount;
      cmd.warp_count = warp_count;
      cmd.vertex_per_warp = vertex_per_warp;
      cmd.index_buffer = index_buffer;
      cmd.index_buffer_offset = IndexBufferOffset + index_sub_offset;
    });
  }

  void
  STDMETHODCALLTYPE
  DrawIndexedInstancedIndirect(ID3D11Buffer *pBufferForArgs, UINT AlignedByteOffsetForArgs) override {
    D11_CTX_CALL(DrawIndexedInstancedIndirect);
    D11_WORK(work_.draws++; work_.draws_indirect++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    WMTPrimitiveType Primitive;
    uint32_t ControlPointCount;
    if(!to_metal_primitive_type(state_.InputAssembler.Topology, Primitive, ControlPointCount))
      return;
    TraceIndirectReadback(pBufferForArgs, AlignedByteOffsetForArgs, 20, true); // spec 05 clarification 24
    TraceVSBufferReadback(); // spec 05 clarification 50
    DrawCallStatus status = PreDraw<true>();
    if (status == DrawCallStatus::Invalid)
      return TraceDrawDropped();
    TraceDraw("draw indexed instanced", true, true, 0, 0, 0, 0, 0); // spec 05 clarification 44
    if (status == DrawCallStatus::Geometry) {
      return GeometryDrawIndexedIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
    }
    if (status == DrawCallStatus::Tessellation) {
      return TessellationDrawIndexedIndirect(ControlPointCount ,pBufferForArgs, AlignedByteOffsetForArgs);
    }
    auto IndexType =
        state_.InputAssembler.IndexBufferFormat == DXGI_FORMAT_R32_UINT ? WMTIndexTypeUInt32 : WMTIndexTypeUInt16;
    auto IndexBufferOffset = state_.InputAssembler.IndexBufferOffset;
    if (auto bindable = reinterpret_cast<D3D11ResourceCommon *>(pBufferForArgs)) {
      EmitOP([IndexType, IndexBufferOffset, Primitive, ArgBuffer = bindable->buffer(),
            AlignedByteOffsetForArgs](ArgumentEncodingContext &enc) {
        auto [buffer, buffer_offset] = enc.access<true>(ArgBuffer, AlignedByteOffsetForArgs, 20, DXMT_ENCODER_RESOURCE_ACESS_READ);
        enc.bumpVisibilityResultOffset();
        auto [index_buffer, index_sub_offset] = enc.currentIndexBuffer();
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_draw_indexed_indirect>();
        cmd.type = WMTRenderCommandDrawIndexedIndirect;
        cmd.primitive_type = Primitive;
        cmd.index_type = IndexType;
        cmd.indirect_args_buffer = buffer->buffer();
        cmd.indirect_args_offset = AlignedByteOffsetForArgs + buffer_offset;
        cmd.index_buffer = index_buffer;
        cmd.index_buffer_offset = IndexBufferOffset + index_sub_offset;
        enc.traceIndirect(&cmd, buffer, ArgBuffer->length(), AlignedByteOffsetForArgs, "draw indexed", false); // spec 05 clarification 24
      });
    }
  }

  void
  STDMETHODCALLTYPE
  DrawInstancedIndirect(ID3D11Buffer *pBufferForArgs, UINT AlignedByteOffsetForArgs) override {
    D11_CTX_CALL(DrawInstancedIndirect);
    D11_WORK(work_.draws++; work_.draws_indirect++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    WMTPrimitiveType Primitive;
    uint32_t ControlPointCount;
    if(!to_metal_primitive_type(state_.InputAssembler.Topology, Primitive, ControlPointCount))
      return;
    TraceIndirectReadback(pBufferForArgs, AlignedByteOffsetForArgs, 16, false); // spec 05 clarification 24
    TraceVSBufferReadback(); // spec 05 clarification 50
    DrawCallStatus status = PreDraw<false>();
    if (status == DrawCallStatus::Invalid)
      return TraceDrawDropped();
    TraceDraw("draw instanced", false, true, 0, 0, 0, 0, 0); // spec 05 clarification 44
    if (status == DrawCallStatus::Geometry) {
      return GeometryDrawIndirect(pBufferForArgs, AlignedByteOffsetForArgs);
    }
    if (status == DrawCallStatus::Tessellation) {
      return TessellationDrawIndirect(ControlPointCount, pBufferForArgs, AlignedByteOffsetForArgs);
    }
    if (auto bindable = reinterpret_cast<D3D11ResourceCommon *>(pBufferForArgs)) {
      EmitOP([Primitive, ArgBuffer = bindable->buffer(), AlignedByteOffsetForArgs](ArgumentEncodingContext &enc) {
        auto [buffer, buffer_offset] = enc.access<true>(ArgBuffer, AlignedByteOffsetForArgs, 20, DXMT_ENCODER_RESOURCE_ACESS_READ);
        enc.bumpVisibilityResultOffset();
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_draw_indirect>();
        cmd.type = WMTRenderCommandDrawIndirect;
        cmd.primitive_type = Primitive;
        cmd.indirect_args_buffer = buffer->buffer();;
        cmd.indirect_args_offset = AlignedByteOffsetForArgs + buffer_offset;
        enc.traceIndirect(&cmd, buffer, ArgBuffer->length(), AlignedByteOffsetForArgs, "draw", false); // spec 05 clarification 24
      });
    }
  }

  void
  GeometryDrawIndirect(
    ID3D11Buffer *pBufferForArgs, UINT AlignedByteOffsetForArgs
  ) {
    auto max_object_threadgroups = max_object_threadgroups_;
    if (auto bindable = reinterpret_cast<D3D11ResourceCommon *>(pBufferForArgs)) {
      EmitOP([=, topo = state_.InputAssembler.Topology, ArgBuffer = bindable->buffer()](ArgumentEncodingContext &enc) {
        auto [buffer, buffer_offset] = enc.access<true>(ArgBuffer, AlignedByteOffsetForArgs, 20, DXMT_ENCODER_RESOURCE_ACESS_READ);
        auto dispatch_arg = enc.allocateTempBuffer1(sizeof(DXMT_DISPATCH_ARGUMENTS), 4);
  
        auto [vertex_per_warp, vertex_increment_per_wrap] = get_gs_vertex_count(topo);
  
        enc.bumpVisibilityResultOffset();
        enc.encodeGSDispatchArgumentsMarshal(
          buffer->buffer(), buffer->gpuAddress() + buffer_offset, AlignedByteOffsetForArgs, vertex_increment_per_wrap,
          dispatch_arg.gpu_buffer, dispatch_arg.gpu_address, dispatch_arg.offset, max_object_threadgroups
        );
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_dxmt_geometry_draw_indirect>();
        cmd.type = WMTRenderCommandDXMTGeometryDrawIndirect;
        cmd.dispatch_args_buffer = dispatch_arg.gpu_buffer;
        cmd.dispatch_args_offset = dispatch_arg.offset;
        cmd.vertex_per_warp = vertex_per_warp;
        cmd.indirect_args_buffer = buffer->buffer();
        cmd.indirect_args_offset = buffer_offset + AlignedByteOffsetForArgs;
        cmd.imm_draw_arguments = enc.getFinalArgumentBuffer();
      });
    }
  }

  void
  GeometryDrawIndexedIndirect(
    ID3D11Buffer *pBufferForArgs, UINT AlignedByteOffsetForArgs
  ) {
    auto IndexBufferOffset = state_.InputAssembler.IndexBufferOffset;
    auto max_object_threadgroups = max_object_threadgroups_;

    if (auto bindable = reinterpret_cast<D3D11ResourceCommon *>(pBufferForArgs)) {
      EmitOP([=, topo = state_.InputAssembler.Topology, ArgBuffer = bindable->buffer()](ArgumentEncodingContext &enc) {
        auto [buffer, buffer_offset] = enc.access<true>(ArgBuffer, AlignedByteOffsetForArgs, 20, DXMT_ENCODER_RESOURCE_ACESS_READ);
        auto dispatch_arg = enc.allocateTempBuffer1(sizeof(DXMT_DISPATCH_ARGUMENTS), 4);
  
        auto [vertex_per_warp, vertex_increment_per_wrap] = get_gs_vertex_count(topo);
        auto [index_buffer, index_sub_offset] = enc.currentIndexBuffer();
  
        enc.bumpVisibilityResultOffset();
        enc.encodeGSDispatchArgumentsMarshal(
          buffer->buffer(), buffer->gpuAddress() + buffer_offset, AlignedByteOffsetForArgs, vertex_increment_per_wrap,
          dispatch_arg.gpu_buffer, dispatch_arg.gpu_address, dispatch_arg.offset, max_object_threadgroups
        );
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_dxmt_geometry_draw_indexed_indirect>();
        cmd.type = WMTRenderCommandDXMTGeometryDrawIndexedIndirect;
        cmd.dispatch_args_buffer = dispatch_arg.gpu_buffer;
        cmd.dispatch_args_offset = dispatch_arg.offset;
        cmd.vertex_per_warp = vertex_per_warp;
        cmd.indirect_args_buffer = buffer->buffer();
        cmd.indirect_args_offset = AlignedByteOffsetForArgs + buffer_offset;
        cmd.index_buffer = index_buffer;
        cmd.index_buffer_offset = IndexBufferOffset + index_sub_offset;
        cmd.imm_draw_arguments = enc.getFinalArgumentBuffer();
      });
    }
  }

  void
  TessellationDrawIndirect(
    UINT NumControlPoint, ID3D11Buffer *pBufferForArgs, UINT AlignedByteOffsetForArgs
  ) {
    auto max_object_threadgroups = max_object_threadgroups_;
    if (auto bindable = reinterpret_cast<D3D11ResourceCommon *>(pBufferForArgs)) {
      EmitOP([=, ArgBuffer = bindable->buffer()](ArgumentEncodingContext &enc) {
        auto [buffer, buffer_offset] = enc.access<true>(ArgBuffer, AlignedByteOffsetForArgs, 20, DXMT_ENCODER_RESOURCE_ACESS_READ);
        auto dispatch_arg = enc.allocateTempBuffer1(sizeof(DXMT_DISPATCH_ARGUMENTS), 4);
  
        auto PatchPerGroup = 32 / enc.tess_threads_per_patch;
        auto ThreadsPerPatch = enc.tess_threads_per_patch;
  
        enc.bumpVisibilityResultOffset();
        enc.encodeTSDispatchArgumentsMarshal(
            buffer->buffer(), buffer->gpuAddress() + buffer_offset,
            AlignedByteOffsetForArgs, NumControlPoint, PatchPerGroup,
            dispatch_arg.gpu_buffer, dispatch_arg.gpu_address,
            dispatch_arg.offset, max_object_threadgroups);
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_dxmt_tessellation_mesh_draw_indirect>();
        cmd.type = WMTRenderCommandDXMTTessellationMeshDrawIndirect;
        cmd.dispatch_args_buffer = dispatch_arg.gpu_buffer;
        cmd.dispatch_args_offset = dispatch_arg.offset;
        cmd.patch_per_group = PatchPerGroup;
        cmd.threads_per_patch = ThreadsPerPatch;
        cmd.indirect_args_buffer = buffer->buffer();
        cmd.indirect_args_offset = buffer_offset + AlignedByteOffsetForArgs;
        cmd.imm_draw_arguments = enc.getFinalArgumentBuffer();
      });
    }
  }

  void
  TessellationDrawIndexedIndirect(
    UINT NumControlPoint, ID3D11Buffer *pBufferForArgs, UINT AlignedByteOffsetForArgs
  ) {
    auto IndexBufferOffset = state_.InputAssembler.IndexBufferOffset;
    auto max_object_threadgroups = max_object_threadgroups_;

    if (auto bindable = reinterpret_cast<D3D11ResourceCommon *>(pBufferForArgs)) {
      EmitOP([=, ArgBuffer = bindable->buffer()](ArgumentEncodingContext &enc) {
        auto [buffer, buffer_offset] = enc.access<true>(ArgBuffer, AlignedByteOffsetForArgs, 20, DXMT_ENCODER_RESOURCE_ACESS_READ);
        auto dispatch_arg = enc.allocateTempBuffer1(sizeof(DXMT_DISPATCH_ARGUMENTS), 4);
  
        auto PatchPerGroup = 32 / enc.tess_threads_per_patch;
        auto ThreadsPerPatch = enc.tess_threads_per_patch;
        auto [index_buffer, index_sub_offset] = enc.currentIndexBuffer();
  
        enc.bumpVisibilityResultOffset();
        enc.encodeTSDispatchArgumentsMarshal(
            buffer->buffer(), buffer->gpuAddress() + buffer_offset,
            AlignedByteOffsetForArgs, NumControlPoint, PatchPerGroup,
            dispatch_arg.gpu_buffer, dispatch_arg.gpu_address,
            dispatch_arg.offset, max_object_threadgroups);
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect>();
        cmd.type = WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect;
        cmd.dispatch_args_buffer = dispatch_arg.gpu_buffer;
        cmd.dispatch_args_offset = dispatch_arg.offset;
        cmd.patch_per_group = PatchPerGroup;
        cmd.threads_per_patch = ThreadsPerPatch;
        cmd.indirect_args_buffer = buffer->buffer();
        cmd.indirect_args_offset = AlignedByteOffsetForArgs + buffer_offset;
        cmd.index_buffer = index_buffer;
        cmd.index_buffer_offset = IndexBufferOffset + index_sub_offset;
        cmd.imm_draw_arguments = enc.getFinalArgumentBuffer();
      });
    }
  }

  void
  STDMETHODCALLTYPE
  DrawAuto() override {
    D11_CTX_CALL(DrawAuto);
    D11_WORK(work_.draws++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    EmitST([](ArgumentEncodingContext &enc) { enc.setCompatibilityFlag(FeatureCompatibility::UnsupportedDrawAuto); });
  }

  void
  STDMETHODCALLTYPE
  Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ) override {
    D11_CTX_CALL(Dispatch);
    NotePassReason(d11cs::PassStart::Dispatch); // spec 05 clarification 63
    D11_WORK(work_.dispatches++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    if (!PreDispatch())
      return;
    EmitOP([ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ](ArgumentEncodingContext &enc) {
      auto &cmd = enc.encodeComputeCommand<wmtcmd_compute_dispatch>();
      cmd.type = WMTComputeCommandDispatch;
      cmd.size = {ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ};
      enc.traceDispatch(&cmd); // spec 05 clarification 21
    });
    TraceAfterDispatch(); // spec 05 clarifications 27 and 28
  }

  /* MYIOSDECK spec 05 clarifications 27 and 28: in a traced frame of the immediate context, a
     blit pass right after the dispatch reads back its buffer UAVs (and, in the dumped frame,
     copies its 2D texture UAVs). The next dispatch opens a compute pass again. */
  void
  TraceAfterDispatch() {
    if (!FrameTraceSwitch() || !FrameTraced(RecordingPresent()))
      return;
    SwitchToBlitEncoder(CommandBufferState::BlitEncoderActive);
    EmitOP([](ArgumentEncodingContext &enc) { enc.traceAfterDispatch(); });
  }

  void
  STDMETHODCALLTYPE
  DispatchIndirect(ID3D11Buffer *pBufferForArgs, UINT AlignedByteOffsetForArgs) override {
    D11_CTX_CALL(DispatchIndirect);
    NotePassReason(d11cs::PassStart::Dispatch); // spec 05 clarification 63
    D11_WORK(work_.dispatches++); // spec 05 clarification 63
    std::lock_guard<mutex_t> lock(mutex);

    TraceIndirectReadback(pBufferForArgs, AlignedByteOffsetForArgs, 12, false); // spec 05 clarification 24
    if (!PreDispatch())
      return;
    if (auto bindable = reinterpret_cast<D3D11ResourceCommon *>(pBufferForArgs)) {
      EmitOP([AlignedByteOffsetForArgs, ArgBuffer = bindable->buffer()](ArgumentEncodingContext &enc) {
        auto [buffer, buffer_offset] = enc.access(ArgBuffer, AlignedByteOffsetForArgs, 12, DXMT_ENCODER_RESOURCE_ACESS_READ);
        // spec 05 clarification 25: a buffers-and-textures memory barrier, then the dispatch
        auto &cmd = EncodeIndirectDispatch(enc, buffer->buffer(), AlignedByteOffsetForArgs + buffer_offset);
        enc.traceIndirect(&cmd, buffer, ArgBuffer->length(), AlignedByteOffsetForArgs, "dispatch", true);
        enc.traceDispatch(&cmd); // spec 05 clarification 21
      });
      TraceAfterDispatch(); // spec 05 clarifications 27 and 28
    }
  }

  /* MYIOSDECK spec 05 clarification 24: in a traced frame of the immediate context, a blit of
     the indirect command's `length` argument bytes into a read-back slot, right before it (the
     command's own encoder is opened again after it, as after a copy). */
  void
  TraceIndirectReadback(ID3D11Buffer *pBufferForArgs, UINT offset, unsigned length, bool indexed) {
    auto bindable = reinterpret_cast<D3D11ResourceCommon *>(pBufferForArgs);
    if (!bindable || !FrameTraceSwitch() || !FrameTraced(RecordingPresent()))
      return;
    SwitchToBlitEncoder(CommandBufferState::BlitEncoderActive);
    EmitOP([ArgBuffer = bindable->buffer(), offset, length, indexed](ArgumentEncodingContext &enc) {
      enc.traceIndirectReadback(ArgBuffer, offset, length, indexed);
    });
  }
#pragma endregion

#pragma region State API

  void
  STDMETHODCALLTYPE
  GetPredication(ID3D11Predicate **ppPredicate, BOOL *pPredicateValue) override {
    D11_CTX_CALL(GetPredication);
    std::lock_guard<mutex_t> lock(mutex);


    if (ppPredicate) {
      *ppPredicate = state_.predicate.ref();
    }
    if (pPredicateValue) {
      *pPredicateValue = state_.predicate_value;
    }

  }

  void
  STDMETHODCALLTYPE
  SetPredication(ID3D11Predicate *pPredicate, BOOL PredicateValue) override {
    D11_CTX_CALL(SetPredication);
    std::lock_guard<mutex_t> lock(mutex);


    state_.predicate = pPredicate;
    state_.predicate_value = PredicateValue;

    if (pPredicate)
      EmitST([](ArgumentEncodingContext &enc) { enc.setCompatibilityFlag(FeatureCompatibility::UnsupportedPredication); });
  }

  //-----------------------------------------------------------------------------
  // State Machine
  //-----------------------------------------------------------------------------

  void
  STDMETHODCALLTYPE
  SwapDeviceContextState(ID3DDeviceContextState *pState, ID3DDeviceContextState **ppPreviousState) override {
    D11_CTX_CALL(SwapDeviceContextState);
    /* LGPL-2.1-or-later, from steamac's DXMT patch 0014 (hanzpo) */
    std::lock_guard<mutex_t> lock(mutex);

    if (ppPreviousState)
      *ppPreviousState = nullptr;

    if (!pState)
      return;

    // the context starts out with an implicit state object holding its current state
    if (!active_context_state_)
      active_context_state_ = new MTLD3D11DeviceContextState(device);

    if (ppPreviousState)
      *ppPreviousState = ref(active_context_state_.ptr());

    auto next = static_cast<MTLD3D11DeviceContextState *>(pState);
    if (next == active_context_state_.ptr())
      return;

    // end the current pass while state_ still describes it
    ResetEncodingContextState();

    // binding sets are move-only; moving marks every bound slot dirty, and pipeline state is rebuilt on the next
    // draw/dispatch. the incoming object's copy is left stale while it is active (see MTLD3D11DeviceContextState)
    active_context_state_->state = std::move(state_);
    state_ = std::move(next->state);
    active_context_state_ = next;

    RestoreEncodingContextState();
    /* end of LGPL block */
  }

  void
  STDMETHODCALLTYPE
  ClearState() override {
    D11_CTX_CALL(ClearState);
    std::lock_guard<mutex_t> lock(mutex);

    ResetEncodingContextState();
    ResetD3D11ContextState();
  }

#pragma region InputAssembler

  void
  STDMETHODCALLTYPE
  IASetInputLayout(ID3D11InputLayout *pInputLayout) override {
    D11_CTX_CALL(IASetInputLayout);
    std::lock_guard<mutex_t> lock(mutex);

    auto expected = com_cast<IMTLD3D11InputLayout>(pInputLayout);
    // spec 05 clarification 65: the layout already set doesn't invalidate the pipeline
    if (expected.ptr() == state_.InputAssembler.InputLayout.ptr()) {
      d11cs::NoteRepeated();
      if (StateFilterSkips(CutSettingsOnce()[CutSwitch::StateFilter], true))
        return;
    }
    if (expected) {
      state_.InputAssembler.InputLayout = std::move(expected);
    } else {
      state_.InputAssembler.InputLayout = nullptr;
    }
    InvalidateRenderPipeline();
  }
  void
  STDMETHODCALLTYPE
  IAGetInputLayout(ID3D11InputLayout **ppInputLayout) override {
    D11_CTX_CALL(IAGetInputLayout);
    std::lock_guard<mutex_t> lock(mutex);

    if (ppInputLayout) {
      if (state_.InputAssembler.InputLayout) {
        state_.InputAssembler.InputLayout->QueryInterface(IID_PPV_ARGS(ppInputLayout));
      } else {
        *ppInputLayout = nullptr;
      }
    }
  }

  void
  STDMETHODCALLTYPE
  IASetVertexBuffers(
      UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppVertexBuffers, const UINT *pStrides, const UINT *pOffsets
  ) override {
    D11_CTX_CALL(IASetVertexBuffers);
    std::lock_guard<mutex_t> lock(mutex);

    SetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
  }

  void
  STDMETHODCALLTYPE
  IAGetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppVertexBuffers, UINT *pStrides, UINT *pOffsets)
      override {
    D11_CTX_CALL(IAGetVertexBuffers);
    std::lock_guard<mutex_t> lock(mutex);

    GetVertexBuffers(StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
  }

  void
  STDMETHODCALLTYPE
  IASetIndexBuffer(ID3D11Buffer *pIndexBuffer, DXGI_FORMAT Format, UINT Offset) override {
    D11_CTX_CALL(IASetIndexBuffer);
    std::lock_guard<mutex_t> lock(mutex);

    // spec 05 clarification 65: the buffer already set records no command (the format and
    // offset are stored either way)
    auto *requested = reinterpret_cast<D3D11ResourceCommon *>(pIndexBuffer);
    bool same = SameIndexBuffer(state_.InputAssembler.IndexBuffer.ptr(), requested);
    if (same && state_.InputAssembler.IndexBufferFormat == Format && state_.InputAssembler.IndexBufferOffset == Offset)
      d11cs::NoteRepeated();
    if (StateFilterSkips(CutSettingsOnce()[CutSwitch::StateFilter], same)) {
      // nothing to record
    } else if (auto expected = requested) {
      state_.InputAssembler.IndexBuffer = expected;
      EmitST([buffer = state_.InputAssembler.IndexBuffer->buffer()](ArgumentEncodingContext &enc) mutable {
        enc.bindIndexBuffer(forward_rc(buffer));
      });
    } else {
      state_.InputAssembler.IndexBuffer = nullptr;
      EmitST([](ArgumentEncodingContext &enc) { enc.bindIndexBuffer({}); });
    }
    state_.InputAssembler.IndexBufferFormat = Format;
    state_.InputAssembler.IndexBufferOffset = Offset;
  }
  void
  STDMETHODCALLTYPE
  IAGetIndexBuffer(ID3D11Buffer **pIndexBuffer, DXGI_FORMAT *Format, UINT *Offset) override {
    D11_CTX_CALL(IAGetIndexBuffer);
    std::lock_guard<mutex_t> lock(mutex);

    if (pIndexBuffer) {
      if (state_.InputAssembler.IndexBuffer) {
        state_.InputAssembler.IndexBuffer->QueryInterface(IID_PPV_ARGS(pIndexBuffer));
      } else {
        *pIndexBuffer = nullptr;
      }
    }
    if (Format != NULL)
      *Format = state_.InputAssembler.IndexBufferFormat;
    if (Offset != NULL)
      *Offset = state_.InputAssembler.IndexBufferOffset;
  }
  void
  STDMETHODCALLTYPE
  IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY Topology) override {
    D11_CTX_CALL(IASetPrimitiveTopology);
    std::lock_guard<mutex_t> lock(mutex);

    if (state_.InputAssembler.Topology != Topology) {
      state_.InputAssembler.Topology = Topology;
      InvalidateRenderPipeline();
    } else {
      d11cs::NoteRepeated(); // spec 05 clarification 63
    }
  }
  void
  STDMETHODCALLTYPE
  IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY *pTopology) override {
    D11_CTX_CALL(IAGetPrimitiveTopology);
    std::lock_guard<mutex_t> lock(mutex);

    if (pTopology) {
      *pTopology = state_.InputAssembler.Topology;
    }
  }
#pragma endregion

#pragma region VertexShader

  void
  STDMETHODCALLTYPE
  VSSetShader(ID3D11VertexShader *pVertexShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances)
      override {
    D11_CTX_CALL(VSSetShader);
    SetShader<PipelineStage::Vertex, ID3D11VertexShader>(pVertexShader, ppClassInstances, NumClassInstances);
  }

  void
  STDMETHODCALLTYPE
  VSGetShader(ID3D11VertexShader **ppVertexShader, ID3D11ClassInstance **ppClassInstances, UINT *pNumClassInstances)
      override {
    D11_CTX_CALL(VSGetShader);
    GetShader<PipelineStage::Vertex, ID3D11VertexShader>(ppVertexShader, ppClassInstances, pNumClassInstances);
  }

  void
  STDMETHODCALLTYPE
  VSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) override {
    D11_CTX_CALL(VSSetShaderResources);
    SetShaderResource<PipelineStage::Vertex>(StartSlot, NumViews, ppShaderResourceViews);
  }

  void
  STDMETHODCALLTYPE
  VSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView **ppShaderResourceViews) override {
    D11_CTX_CALL(VSGetShaderResources);
    GetShaderResource<PipelineStage::Vertex>(StartSlot, NumViews, ppShaderResourceViews);
  }

  void
  STDMETHODCALLTYPE
  VSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) override {
    D11_CTX_CALL(VSSetSamplers);
    SetSamplers<PipelineStage::Vertex>(StartSlot, NumSamplers, ppSamplers);
  }

  void
  STDMETHODCALLTYPE
  VSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState **ppSamplers) override {
    D11_CTX_CALL(VSGetSamplers);
    GetSamplers<PipelineStage::Vertex>(StartSlot, NumSamplers, ppSamplers);
  }

  void
  STDMETHODCALLTYPE
  VSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) override {
    D11_CTX_CALL(VSSetConstantBuffers);
    VSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, NULL, NULL);
  }

  void
  STDMETHODCALLTYPE
  VSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers) override {
    D11_CTX_CALL(VSGetConstantBuffers);
    VSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, NULL, NULL);
  }

  void
  STDMETHODCALLTYPE
  VSSetConstantBuffers1(
      UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers, const UINT *pFirstConstant,
      const UINT *pNumConstants
  ) override {
    D11_CTX_CALL(VSSetConstantBuffers1);
    SetConstantBuffer<PipelineStage::Vertex>(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
  }

  void
  STDMETHODCALLTYPE
  VSGetConstantBuffers1(
      UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers, UINT *pFirstConstant, UINT *pNumConstants
  ) override {
    D11_CTX_CALL(VSGetConstantBuffers1);
    GetConstantBuffer<PipelineStage::Vertex>(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
  }

#pragma endregion

#pragma region PixelShader

  void
  STDMETHODCALLTYPE
  PSSetShader(ID3D11PixelShader *pPixelShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances)
      override {
    D11_CTX_CALL(PSSetShader);
    SetShader<PipelineStage::Pixel, ID3D11PixelShader>(pPixelShader, ppClassInstances, NumClassInstances);
  }

  void
  STDMETHODCALLTYPE
  PSGetShader(ID3D11PixelShader **ppPixelShader, ID3D11ClassInstance **ppClassInstances, UINT *pNumClassInstances)
      override {
    D11_CTX_CALL(PSGetShader);
    GetShader<PipelineStage::Pixel, ID3D11PixelShader>(ppPixelShader, ppClassInstances, pNumClassInstances);
  }

  void
  STDMETHODCALLTYPE
  PSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) override {
    D11_CTX_CALL(PSSetShaderResources);
    SetShaderResource<PipelineStage::Pixel>(StartSlot, NumViews, ppShaderResourceViews);
  }

  void
  STDMETHODCALLTYPE
  PSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView **ppShaderResourceViews) override {
    D11_CTX_CALL(PSGetShaderResources);
    GetShaderResource<PipelineStage::Pixel>(StartSlot, NumViews, ppShaderResourceViews);
  }

  void
  STDMETHODCALLTYPE
  PSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) override {
    D11_CTX_CALL(PSSetSamplers);
    SetSamplers<PipelineStage::Pixel>(StartSlot, NumSamplers, ppSamplers);
  }

  void
  STDMETHODCALLTYPE
  PSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState **ppSamplers) override {
    D11_CTX_CALL(PSGetSamplers);

    GetSamplers<PipelineStage::Pixel>(StartSlot, NumSamplers, ppSamplers);
  }

  void
  STDMETHODCALLTYPE
  PSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) override {
    D11_CTX_CALL(PSSetConstantBuffers);
    PSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, 0, 0);
  }

  void
  STDMETHODCALLTYPE
  PSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers) override {
    D11_CTX_CALL(PSGetConstantBuffers);
    PSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, 0, 0);
  }

  void
  STDMETHODCALLTYPE
  PSSetConstantBuffers1(
      UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers, const UINT *pFirstConstant,
      const UINT *pNumConstants
  ) override {
    D11_CTX_CALL(PSSetConstantBuffers1);
    SetConstantBuffer<PipelineStage::Pixel>(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
  }

  void
  STDMETHODCALLTYPE
  PSGetConstantBuffers1(
      UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers, UINT *pFirstConstant, UINT *pNumConstants
  ) override {
    D11_CTX_CALL(PSGetConstantBuffers1);
    GetConstantBuffer<PipelineStage::Pixel>(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
  }

#pragma endregion

#pragma region GeometryShader

  void
  STDMETHODCALLTYPE
  GSSetShader(ID3D11GeometryShader *pShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances)
      override {
    D11_CTX_CALL(GSSetShader);
    SetShader<PipelineStage::Geometry>(pShader, ppClassInstances, NumClassInstances);
  }

  void
  STDMETHODCALLTYPE
  GSGetShader(ID3D11GeometryShader **ppGeometryShader, ID3D11ClassInstance **ppClassInstances, UINT *pNumClassInstances)
      override {
    D11_CTX_CALL(GSGetShader);
    GetShader<PipelineStage::Geometry>(ppGeometryShader, ppClassInstances, pNumClassInstances);
  }

  void
  STDMETHODCALLTYPE
  GSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) override {
    D11_CTX_CALL(GSSetConstantBuffers);
    GSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, NULL, NULL);
  }

  void
  STDMETHODCALLTYPE
  GSSetConstantBuffers1(
      UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers, const UINT *pFirstConstant,
      const UINT *pNumConstants
  ) override {
    D11_CTX_CALL(GSSetConstantBuffers1);
    SetConstantBuffer<PipelineStage::Geometry>(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
  }

  void
  STDMETHODCALLTYPE
  GSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers) override {
    D11_CTX_CALL(GSGetConstantBuffers);
    GSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, NULL, NULL);
  }

  void
  STDMETHODCALLTYPE
  GSGetConstantBuffers1(
      UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers, UINT *pFirstConstant, UINT *pNumConstants
  ) override {
    D11_CTX_CALL(GSGetConstantBuffers1);
    GetConstantBuffer<PipelineStage::Geometry>(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
  }

  void
  STDMETHODCALLTYPE
  GSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) override {
    D11_CTX_CALL(GSSetShaderResources);
    SetShaderResource<PipelineStage::Geometry>(StartSlot, NumViews, ppShaderResourceViews);
  }

  void
  STDMETHODCALLTYPE
  GSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView **ppShaderResourceViews) override {
    D11_CTX_CALL(GSGetShaderResources);
    GetShaderResource<PipelineStage::Geometry>(StartSlot, NumViews, ppShaderResourceViews);
  }

  void
  STDMETHODCALLTYPE
  GSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) override {
    D11_CTX_CALL(GSSetSamplers);
    SetSamplers<PipelineStage::Geometry>(StartSlot, NumSamplers, ppSamplers);
  }

  void
  STDMETHODCALLTYPE
  GSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState **ppSamplers) override {
    D11_CTX_CALL(GSGetSamplers);
    GetSamplers<PipelineStage::Geometry>(StartSlot, NumSamplers, ppSamplers);
  }

  void
  STDMETHODCALLTYPE
  SOSetTargets(UINT NumBuffers, ID3D11Buffer *const *ppSOTargets, const UINT *pOffsets) override {
    D11_CTX_CALL(SOSetTargets);
    std::lock_guard<mutex_t> lock(mutex);

    if (NumBuffers == 0) {
      NumBuffers = 4; // see msdn description of SOSetTargets
    }
    for (unsigned slot = 0; slot < NumBuffers; slot++) {
      auto pBuffer = ppSOTargets ? ppSOTargets[slot] : nullptr;
      if (pBuffer) {
        bool replaced = false;
        auto &entry = state_.StreamOutput.Targets.bind(slot, {pBuffer}, replaced);
        if (!replaced) {
          auto offset = pOffsets ? pOffsets[slot] : 0;
          if (offset != entry.Offset) {
            state_.StreamOutput.Targets.set_dirty(slot);
            entry.Offset = offset;
          }
          continue;
        }
        entry.Buffer = reinterpret_cast<D3D11ResourceCommon *>(pBuffer);
        entry.Offset = pOffsets ? pOffsets[slot] : 0;
      } else {
        state_.StreamOutput.Targets.unbind(slot);
      }
    }
  }

  void
  STDMETHODCALLTYPE
  SOGetTargets(UINT NumBuffers, ID3D11Buffer **ppSOTargets) override {
    D11_CTX_CALL(SOGetTargets);
    std::lock_guard<mutex_t> lock(mutex);

    if (!ppSOTargets)
      return;
    for (unsigned i = 0; i < std::min(4u, NumBuffers); i++) {
      if (state_.StreamOutput.Targets.test_bound(i)) {
        state_.StreamOutput.Targets[i].Buffer->QueryInterface(IID_PPV_ARGS(&ppSOTargets[i]));
      } else {
        ppSOTargets[i] = nullptr;
      }
    }
  }

  void SOGetTargetsWithOffsets(UINT NumBuffers, REFIID riid, void **ppSOTargets,
                               UINT *pOffsets) override {
    std::lock_guard<mutex_t> lock(mutex);

    for (unsigned i = 0; i < std::min(4u, NumBuffers); i++) {
      if (state_.StreamOutput.Targets.test_bound(i)) {
        if (ppSOTargets)
          state_.StreamOutput.Targets[i].Buffer->QueryInterface(riid, &ppSOTargets[i]);
        if (pOffsets)
          pOffsets[i] = state_.StreamOutput.Targets[i].Offset;
      } else {
        if (ppSOTargets)
          ppSOTargets[i] = nullptr;
        if (pOffsets)
          pOffsets[i] = 0;
      }
    }
  }

#pragma endregion

#pragma region HullShader

  void
  STDMETHODCALLTYPE
  HSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView **ppShaderResourceViews) override {
    D11_CTX_CALL(HSGetShaderResources);
    GetShaderResource<PipelineStage::Hull>(StartSlot, NumViews, ppShaderResourceViews);
  }

  void
  STDMETHODCALLTYPE
  HSGetShader(ID3D11HullShader **ppHullShader, ID3D11ClassInstance **ppClassInstances, UINT *pNumClassInstances)
      override {
    D11_CTX_CALL(HSGetShader);
    GetShader<PipelineStage::Hull>(ppHullShader, ppClassInstances, pNumClassInstances);
  }

  void
  STDMETHODCALLTYPE
  HSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState **ppSamplers) override {
    D11_CTX_CALL(HSGetSamplers);
    GetSamplers<PipelineStage::Hull>(StartSlot, NumSamplers, ppSamplers);
  }

  void
  STDMETHODCALLTYPE
  HSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers) override {
    D11_CTX_CALL(HSGetConstantBuffers);
    HSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, NULL, NULL);
  }

  void
  STDMETHODCALLTYPE
  HSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) override {
    D11_CTX_CALL(HSSetShaderResources);
    SetShaderResource<PipelineStage::Hull>(StartSlot, NumViews, ppShaderResourceViews);
  }

  void
  STDMETHODCALLTYPE
  HSSetShader(ID3D11HullShader *pHullShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances)
      override {
    D11_CTX_CALL(HSSetShader);
    SetShader<PipelineStage::Hull>(pHullShader, ppClassInstances, NumClassInstances);
  }

  void
  STDMETHODCALLTYPE
  HSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) override {
    D11_CTX_CALL(HSSetSamplers);
    SetSamplers<PipelineStage::Hull>(StartSlot, NumSamplers, ppSamplers);
  }

  void
  STDMETHODCALLTYPE
  HSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) override {
    D11_CTX_CALL(HSSetConstantBuffers);
    HSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, NULL, NULL);
  }

  void
  STDMETHODCALLTYPE
  HSSetConstantBuffers1(
      UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers, const UINT *pFirstConstant,
      const UINT *pNumConstants
  ) override {
    D11_CTX_CALL(HSSetConstantBuffers1);
    SetConstantBuffer<PipelineStage::Hull>(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
  }

  void
  STDMETHODCALLTYPE
  HSGetConstantBuffers1(
      UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers, UINT *pFirstConstant, UINT *pNumConstants
  ) override {
    D11_CTX_CALL(HSGetConstantBuffers1);
    GetConstantBuffer<PipelineStage::Hull>(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
  }

#pragma endregion

#pragma region DomainShader

  void
  STDMETHODCALLTYPE
  DSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) override {
    D11_CTX_CALL(DSSetShaderResources);
    SetShaderResource<PipelineStage::Domain>(StartSlot, NumViews, ppShaderResourceViews);
  }

  void
  STDMETHODCALLTYPE
  DSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView **ppShaderResourceViews) override {
    D11_CTX_CALL(DSGetShaderResources);
    GetShaderResource<PipelineStage::Domain>(StartSlot, NumViews, ppShaderResourceViews);
  }

  void
  STDMETHODCALLTYPE
  DSSetShader(ID3D11DomainShader *pDomainShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances)
      override {
    D11_CTX_CALL(DSSetShader);
    SetShader<PipelineStage::Domain, ID3D11DomainShader>(pDomainShader, ppClassInstances, NumClassInstances);
  }

  void
  STDMETHODCALLTYPE
  DSGetShader(ID3D11DomainShader **ppDomainShader, ID3D11ClassInstance **ppClassInstances, UINT *pNumClassInstances)
      override {
    D11_CTX_CALL(DSGetShader);
    GetShader<PipelineStage::Domain, ID3D11DomainShader>(ppDomainShader, ppClassInstances, pNumClassInstances);
  }

  void
  STDMETHODCALLTYPE
  DSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState **ppSamplers) override {
    D11_CTX_CALL(DSGetSamplers);
    GetSamplers<PipelineStage::Domain>(StartSlot, NumSamplers, ppSamplers);
  }

  void
  STDMETHODCALLTYPE
  DSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) override {
    D11_CTX_CALL(DSSetSamplers);
    SetSamplers<PipelineStage::Domain>(StartSlot, NumSamplers, ppSamplers);
  }

  void
  STDMETHODCALLTYPE
  DSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) override {
    D11_CTX_CALL(DSSetConstantBuffers);
    return DSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, NULL, NULL);
  }

  void
  STDMETHODCALLTYPE
  DSSetConstantBuffers1(
      UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers, const UINT *pFirstConstant,
      const UINT *pNumConstants
  ) override {
    D11_CTX_CALL(DSSetConstantBuffers1);
    SetConstantBuffer<PipelineStage::Domain>(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
  }

  void
  STDMETHODCALLTYPE
  DSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers) override {
    D11_CTX_CALL(DSGetConstantBuffers);
    UINT *pFirstConstant = 0, *pNumConstants = 0;
    return DSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
  }

  void
  STDMETHODCALLTYPE
  DSGetConstantBuffers1(
      UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers, UINT *pFirstConstant, UINT *pNumConstants
  ) override {
    D11_CTX_CALL(DSGetConstantBuffers1);
    GetConstantBuffer<PipelineStage::Domain>(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
  }

#pragma endregion

#pragma region ComputeShader

  void
  STDMETHODCALLTYPE
  CSGetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView **ppShaderResourceViews) override {
    D11_CTX_CALL(CSGetShaderResources);
    GetShaderResource<PipelineStage::Compute>(StartSlot, NumViews, ppShaderResourceViews);
  }

  void
  STDMETHODCALLTYPE
  CSSetShaderResources(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) override {
    D11_CTX_CALL(CSSetShaderResources);
    SetShaderResource<PipelineStage::Compute>(StartSlot, NumViews, ppShaderResourceViews);
  }

  void
  STDMETHODCALLTYPE
  CSSetUnorderedAccessViews(
      UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews,
      const UINT *pUAVInitialCounts
  ) override {
    D11_CTX_CALL(CSSetUnorderedAccessViews);
    std::lock_guard<mutex_t> lock(mutex);

    SetUnorderedAccessView<PipelineStage::Compute>(StartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
  }

  void
  STDMETHODCALLTYPE
  CSGetUnorderedAccessViews(UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView **ppUnorderedAccessViews) override {
    D11_CTX_CALL(CSGetUnorderedAccessViews);
    std::lock_guard<mutex_t> lock(mutex);

    for (auto i = 0u; i < NumUAVs; i++) {
      if (state_.ComputeStageUAV.UAVs.test_bound(StartSlot + i)) {
        state_.ComputeStageUAV.UAVs[StartSlot + i].View->QueryInterface(
            IID_PPV_ARGS(&ppUnorderedAccessViews[i])
        );
      } else {
        ppUnorderedAccessViews[i] = nullptr;
      }
    }
  }

  void
  STDMETHODCALLTYPE
  CSSetShader(ID3D11ComputeShader *pComputeShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances)
      override {
    D11_CTX_CALL(CSSetShader);
    SetShader<PipelineStage::Compute>(pComputeShader, ppClassInstances, NumClassInstances);
  }

  void
  STDMETHODCALLTYPE
  CSGetShader(ID3D11ComputeShader **ppComputeShader, ID3D11ClassInstance **ppClassInstances, UINT *pNumClassInstances)
      override {
    D11_CTX_CALL(CSGetShader);
    GetShader<PipelineStage::Compute>(ppComputeShader, ppClassInstances, pNumClassInstances);
  }

  void
  STDMETHODCALLTYPE
  CSSetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) override {
    D11_CTX_CALL(CSSetSamplers);
    SetSamplers<PipelineStage::Compute>(StartSlot, NumSamplers, ppSamplers);
  }

  void
  STDMETHODCALLTYPE
  CSGetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState **ppSamplers) override {
    D11_CTX_CALL(CSGetSamplers);
    GetSamplers<PipelineStage::Compute>(StartSlot, NumSamplers, ppSamplers);
  }

  void
  STDMETHODCALLTYPE
  CSSetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers) override {
    D11_CTX_CALL(CSSetConstantBuffers);
    CSSetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, NULL, NULL);
  }

  void
  STDMETHODCALLTYPE
  CSGetConstantBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers) override {
    D11_CTX_CALL(CSGetConstantBuffers);
    CSGetConstantBuffers1(StartSlot, NumBuffers, ppConstantBuffers, NULL, NULL);
  }

  void
  STDMETHODCALLTYPE
  CSSetConstantBuffers1(
      UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers, const UINT *pFirstConstant,
      const UINT *pNumConstants
  ) override {
    D11_CTX_CALL(CSSetConstantBuffers1);
    SetConstantBuffer<PipelineStage::Compute>(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
  }

  void
  STDMETHODCALLTYPE
  CSGetConstantBuffers1(
      UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers, UINT *pFirstConstant, UINT *pNumConstants
  ) override {
    D11_CTX_CALL(CSGetConstantBuffers1);
    GetConstantBuffer<PipelineStage::Compute>(StartSlot, NumBuffers, ppConstantBuffers, pFirstConstant, pNumConstants);
  }

#pragma endregion

#pragma region OutputMerger

  void
  STDMETHODCALLTYPE
  OMSetRenderTargets(
      UINT NumViews, ID3D11RenderTargetView *const *ppRenderTargetViews, ID3D11DepthStencilView *pDepthStencilView
  ) override {
    D11_CTX_CALL(OMSetRenderTargets);
    NotePassReason(d11cs::PassStart::Targets); // spec 05 clarification 63
    SetRenderTargetsUncounted( // spec 05 clarification 76: counted once, as OMSetRenderTargets
        NumViews, ppRenderTargetViews, pDepthStencilView, 0, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, NULL, NULL
    );
  }

  void
  STDMETHODCALLTYPE
  OMGetRenderTargets(
      UINT NumViews, ID3D11RenderTargetView **ppRenderTargetViews, ID3D11DepthStencilView **ppDepthStencilView
  ) override {
    D11_CTX_CALL(OMGetRenderTargets);
    OMGetRenderTargetsAndUnorderedAccessViews(NumViews, ppRenderTargetViews, ppDepthStencilView, 0, 0, NULL);
  }

  bool
  ValidateSetRenderTargets(
      UINT NumRTVs, ID3D11RenderTargetView *const *ppRenderTargetViews, ID3D11DepthStencilView *pDepthStencilView
  ) {
    MTL_RENDER_PASS_ATTACHMENT_DESC *ref = nullptr;

    auto dsv = static_cast<D3D11DepthStencilView *>(pDepthStencilView);
    UINT render_target_array_length = 0, sample_count = 1, width = 0, height = 0;

    if (dsv) {
      ref = &dsv->description();
      render_target_array_length = ref->RenderTargetArrayLength;
      sample_count = ref->SampleCount;
      width = ref->Width;
      height = ref->Height;
    }

    for (unsigned i = 0; i < NumRTVs; i++) {
      auto rtv = static_cast<D3D11RenderTargetView *>(ppRenderTargetViews[i]);
      if (rtv) {
        // TODO: render target type should be checked as well
        if (ref) {
          auto &props = rtv->description();
          if (props.SampleCount != sample_count)
            return false;
          if (props.RenderTargetArrayLength != render_target_array_length) {
            // array length can be different only if either is 0 or 1
            if (std::max(props.RenderTargetArrayLength, render_target_array_length) != 1)
              return false;
          }
          // TODO: check all RTVs have the same size
          if (width < props.Width || height < props.Height)
            return false;
          // render_target_array_length will be 1 only if all render targets are array
          render_target_array_length = std::min(render_target_array_length, props.RenderTargetArrayLength);
          width = std::min(width, props.Width);
          height = std::min(height, props.Height);
        } else {
          ref = &rtv->description();
          render_target_array_length = ref->RenderTargetArrayLength;
          sample_count = ref->SampleCount;
          width = ref->Width;
          height = ref->Height;
        }
      }
    }

    state_.OutputMerger.SampleCount = sample_count;
    state_.OutputMerger.ArrayLength = render_target_array_length;
    state_.OutputMerger.RenderTargetWidth = width;
    state_.OutputMerger.RenderTargetHeight = height;

    return true;
  };

  void
  STDMETHODCALLTYPE
  OMSetRenderTargetsAndUnorderedAccessViews(
      UINT NumRTVs, ID3D11RenderTargetView *const *ppRenderTargetViews, ID3D11DepthStencilView *pDepthStencilView,
      UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews,
      const UINT *pUAVInitialCounts
  ) override {
    D11_CTX_CALL(OMSetRenderTargetsAndUnorderedAccessViews);
    NotePassReason(d11cs::PassStart::Targets); // spec 05 clarification 63
    SetRenderTargetsUncounted(
        NumRTVs, ppRenderTargetViews, pDepthStencilView, UAVStartSlot, NumUAVs, ppUnorderedAccessViews,
        pUAVInitialCounts
    );
  }

  /* both OMSetRenderTargets calls, counted by the one the application made (spec 05
     clarifications 63 and 76: a repeat counts against it) */
  void
  SetRenderTargetsUncounted(
      UINT NumRTVs, ID3D11RenderTargetView *const *ppRenderTargetViews, ID3D11DepthStencilView *pDepthStencilView,
      UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews,
      const UINT *pUAVInitialCounts
  ) {
    std::lock_guard<mutex_t> lock(mutex);


    bool should_invalidate_pass = false;

    if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) {
      if (!ValidateSetRenderTargets(NumRTVs, ppRenderTargetViews, pDepthStencilView)) {
        WARN("OMSetRenderTargets: invalid render targets");
        return;
      }
      auto &BoundRTVs = state_.OutputMerger.RTVs;
      constexpr unsigned RTVSlotCount = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
      for (unsigned rtv_index = 0; rtv_index < RTVSlotCount; rtv_index++) {
        if (rtv_index < NumRTVs && ppRenderTargetViews[rtv_index]) {
          auto rtv = static_cast<D3D11RenderTargetView *>(ppRenderTargetViews[rtv_index]);
          if (BoundRTVs[rtv_index].ptr() == rtv)
            continue;
          BoundRTVs[rtv_index] = rtv;
          should_invalidate_pass = true;
        } else {
          if (BoundRTVs[rtv_index]) {
            should_invalidate_pass = true;
          }
          BoundRTVs[rtv_index] = nullptr;
        }
      }
      state_.OutputMerger.NumRTVs = NumRTVs;

      if (auto dsv = static_cast<D3D11DepthStencilView *>(pDepthStencilView)) {
        if (state_.OutputMerger.DSV.ptr() != dsv) {
          state_.OutputMerger.DSV = dsv;
          should_invalidate_pass = true;
        }
      } else {
        if (state_.OutputMerger.DSV) {
          should_invalidate_pass = true;
        }
        state_.OutputMerger.DSV = nullptr;
      }
    }

    bool uavs_changed = false;
    if (NumUAVs != D3D11_KEEP_UNORDERED_ACCESS_VIEWS) {
      uavs_changed =
          SetUnorderedAccessView<PipelineStage::Pixel>(UAVStartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
    }

    // spec 05 clarifications 63 and 76: the targets and UAVs already set
    if (!should_invalidate_pass && !uavs_changed)
      d11cs::NoteRepeated();

    // spec 05 clarification 69: the depth-stencil view stays attached while the application has
    // it unbound with the same render targets (DXMT_PASS_KEEP_DSV), or the front end notes where
    // that would have happened (switch off)
    const bool render_pass_open = cmdbuf_state == CommandBufferState::RenderEncoderActive ||
                                  cmdbuf_state == CommandBufferState::RenderPipelineReady ||
                                  cmdbuf_state == CommandBufferState::TessellationRenderPipelineReady ||
                                  cmdbuf_state == CommandBufferState::GeometryRenderPipelineReady;
    if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL && (keep_dsv_.active() || KeepDsvNoting())) {
      KeepDsvTargets call;
      call.num_rtvs = state_.OutputMerger.NumRTVs;
      for (unsigned i = 0; i < call.num_rtvs && i < 8; i++)
        call.rtvs[i] = state_.OutputMerger.RTVs[i].ptr();
      call.dsv = state_.OutputMerger.DSV.ptr();
      call.array_length = state_.OutputMerger.ArrayLength; // item 77: the layout the call leaves
      call.sample_count = state_.OutputMerger.SampleCount;
      call.width = state_.OutputMerger.RenderTargetWidth;
      call.height = state_.OutputMerger.RenderTargetHeight;
      auto *pass_dsv = render_pass_open && !keep_dsv_.active()
                           ? static_cast<D3D11DepthStencilView *>(const_cast<void *>(keep_dsv_.pass().dsv))
                           : nullptr;
      bool srv_views_dsv = pass_dsv && !call.dsv && AnyShaderResourceOf(pass_dsv->texture().ptr());
      const bool on = CutSettingsOnce()[CutSwitch::KeepDsv];
      auto r = keep_dsv_.setTargets(render_pass_open, call, uavs_changed, srv_views_dsv, on, KeepDsvNoting());
      if (r.have_note) {
        EmitKeepDsvNote(r.note); // into the open pass, before it ends (if it does)
        if (r.note.applied && !r.keep_pass_open)
          should_invalidate_pass = true;
      }
      // the kept view is held while any keep lasts (a would-keep's too: the hook in
      // SetShaderResource looks at its texture)
      if (!keep_dsv_.active()) {
        keep_dsv_view_ = nullptr;
      } else if (!keep_dsv_view_) {
        keep_dsv_view_ = pass_dsv; // a keep started
        if (keep_dsv_.applied())
          dirty_state.set(DirtyState::DepthStencilState); // the pass goes on with it: depth off
      }
      if (r.keep_pass_open)
        should_invalidate_pass = false;
      if (r.end_pass_first && should_invalidate_pass) {
        InvalidateCurrentPass(); // as before; the would-keep starts with the next pass
        should_invalidate_pass = false;
      }
    }

    if (should_invalidate_pass) {
      InvalidateCurrentPass();
    }
  }

  void
  STDMETHODCALLTYPE
  OMGetRenderTargetsAndUnorderedAccessViews(
      UINT NumRTVs, ID3D11RenderTargetView **ppRenderTargetViews, ID3D11DepthStencilView **ppDepthStencilView,
      UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView **ppUnorderedAccessViews
  ) override {
    D11_CTX_CALL(OMGetRenderTargetsAndUnorderedAccessViews);
    std::lock_guard<mutex_t> lock(mutex);

    if (ppRenderTargetViews) {
      for (unsigned i = 0; i < NumRTVs; i++) {
        if (i < state_.OutputMerger.NumRTVs && state_.OutputMerger.RTVs[i]) {
          state_.OutputMerger.RTVs[i]->QueryInterface(IID_PPV_ARGS(&ppRenderTargetViews[i]));
        } else {
          ppRenderTargetViews[i] = nullptr;
        }
      }
    }
    if (ppDepthStencilView) {
      if (state_.OutputMerger.DSV) {
        state_.OutputMerger.DSV->QueryInterface(IID_PPV_ARGS(ppDepthStencilView));
      } else {
        *ppDepthStencilView = nullptr;
      }
    }
    if (ppUnorderedAccessViews) {
      for (unsigned i = 0; i < NumUAVs; i++) {
        if (state_.OutputMerger.UAVs.test_bound(i + UAVStartSlot)) {
          state_.OutputMerger.UAVs[i + UAVStartSlot].View->QueryInterface(IID_PPV_ARGS(&ppUnorderedAccessViews[i]));
        } else {
          ppUnorderedAccessViews[i] = nullptr;
        }
      }
    }
  }

  void
  STDMETHODCALLTYPE
  OMSetBlendState(ID3D11BlendState *pBlendState, const FLOAT BlendFactor[4], UINT SampleMask) override {
    D11_CTX_CALL(OMSetBlendState);
    std::lock_guard<mutex_t> lock(mutex);

    // spec 05 clarification 65: the same state, factors and sample mask mark nothing dirty (item
    // 76: the state's interface is asked for once)
    auto expected = com_cast<IMTLD3D11BlendState>(pBlendState);
    if (SameBlendCall(
            state_.OutputMerger.BlendState, state_.OutputMerger.BlendFactor, state_.OutputMerger.SampleMask,
            expected.ptr(), BlendFactor, SampleMask
        )) {
      d11cs::NoteRepeated();
      if (StateFilterSkips(CutSettingsOnce()[CutSwitch::StateFilter], true))
        return;
    }

    bool should_invalidate_pipeline = false;
    if (expected) {
      if (expected.ptr() != state_.OutputMerger.BlendState) {
        state_.OutputMerger.BlendState = expected.ptr();
        should_invalidate_pipeline = true;
      }
    } else {
      if (state_.OutputMerger.BlendState) {
        state_.OutputMerger.BlendState = nullptr;
        should_invalidate_pipeline = true;
      }
    }
    if (BlendFactor) {
      memcpy(state_.OutputMerger.BlendFactor, BlendFactor, sizeof(float[4]));
    } else {
      state_.OutputMerger.BlendFactor[0] = 1.0f;
      state_.OutputMerger.BlendFactor[1] = 1.0f;
      state_.OutputMerger.BlendFactor[2] = 1.0f;
      state_.OutputMerger.BlendFactor[3] = 1.0f;
    }
    if (state_.OutputMerger.SampleMask != SampleMask) {
      state_.OutputMerger.SampleMask = SampleMask;
      should_invalidate_pipeline = true;
    }
    if (should_invalidate_pipeline) {
      InvalidateRenderPipeline();
    }
    dirty_state.set(DirtyState::BlendFactorAndStencilRef);
  }
  void
  STDMETHODCALLTYPE
  OMGetBlendState(ID3D11BlendState **ppBlendState, FLOAT BlendFactor[4], UINT *pSampleMask) override {
    D11_CTX_CALL(OMGetBlendState);
    std::lock_guard<mutex_t> lock(mutex);

    if (ppBlendState) {
      if (state_.OutputMerger.BlendState) {
        state_.OutputMerger.BlendState->QueryInterface(IID_PPV_ARGS(ppBlendState));
      } else {
        *ppBlendState = nullptr;
      }
    }
    if (BlendFactor) {
      memcpy(BlendFactor, state_.OutputMerger.BlendFactor, sizeof(float[4]));
    }
    if (pSampleMask) {
      *pSampleMask = state_.OutputMerger.SampleMask;
    }
  }

  void
  STDMETHODCALLTYPE
  OMSetDepthStencilState(ID3D11DepthStencilState *pDepthStencilState, UINT StencilRef) override {
    D11_CTX_CALL(OMSetDepthStencilState);
    std::lock_guard<mutex_t> lock(mutex);

    auto expected = com_cast<IMTLD3D11DepthStencilState>(pDepthStencilState);
    // spec 05 clarification 65: the same state (or none both times) and stencil reference mark
    // nothing dirty
    if (SameDepthStencilCall(
            state_.OutputMerger.DepthStencilState, state_.OutputMerger.StencilRef, expected.ptr(), StencilRef
        )) {
      d11cs::NoteRepeated();
      if (StateFilterSkips(CutSettingsOnce()[CutSwitch::StateFilter], true))
        return;
    }
    if (expected) {
      state_.OutputMerger.DepthStencilState = expected.ptr();
    } else {
      state_.OutputMerger.DepthStencilState = nullptr;
    }
    state_.OutputMerger.StencilRef = StencilRef;
    dirty_state.set(DirtyState::DepthStencilState);
  }

  void
  STDMETHODCALLTYPE
  OMGetDepthStencilState(ID3D11DepthStencilState **ppDepthStencilState, UINT *pStencilRef) override {
    D11_CTX_CALL(OMGetDepthStencilState);
    std::lock_guard<mutex_t> lock(mutex);

    if (ppDepthStencilState) {
      if (state_.OutputMerger.DepthStencilState) {
        state_.OutputMerger.DepthStencilState->QueryInterface(IID_PPV_ARGS(ppDepthStencilState));
      } else {
        *ppDepthStencilState = nullptr;
      }
    }
    if (pStencilRef) {
      *pStencilRef = state_.OutputMerger.StencilRef;
    }
  }

#pragma endregion

#pragma region Rasterizer

  void
  STDMETHODCALLTYPE
  RSSetState(ID3D11RasterizerState *pRasterizerState) override {
    D11_CTX_CALL(RSSetState);
    std::lock_guard<mutex_t> lock(mutex);

    if (pRasterizerState) {
      if (auto expected = com_cast<IMTLD3D11RasterizerState>(pRasterizerState)) {
        auto current_rs =
            state_.Rasterizer.RasterizerState ? state_.Rasterizer.RasterizerState : default_rasterizer_state;
        if (current_rs == expected.ptr()) {
          d11cs::NoteRepeated(); // spec 05 clarification 63
          return;
        }
        if (current_rs->IsScissorEnabled() != expected->IsScissorEnabled()) {
          dirty_state.set(DirtyState::Scissors);
        }
        state_.Rasterizer.RasterizerState = expected.ptr();
        dirty_state.set(DirtyState::RasterizerState);
      } else {
        ERR("RSSetState: invalid ID3D11RasterizerState object.");
      }
    } else {
      if (state_.Rasterizer.RasterizerState) {
        if (state_.Rasterizer.RasterizerState->IsScissorEnabled() != default_rasterizer_state->IsScissorEnabled()) {
          dirty_state.set(DirtyState::Scissors);
        }
      }
      state_.Rasterizer.RasterizerState = nullptr;
      dirty_state.set(DirtyState::RasterizerState);
    }
  }

  void
  STDMETHODCALLTYPE
  RSGetState(ID3D11RasterizerState **ppRasterizerState) override {
    D11_CTX_CALL(RSGetState);
    std::lock_guard<mutex_t> lock(mutex);

    if (ppRasterizerState) {
      if (state_.Rasterizer.RasterizerState) {
        state_.Rasterizer.RasterizerState->QueryInterface(IID_PPV_ARGS(ppRasterizerState));
      } else {
        *ppRasterizerState = NULL;
      }
    }
  }

  void
  STDMETHODCALLTYPE
  RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT *pViewports) override {
    D11_CTX_CALL(RSSetViewports);
    std::lock_guard<mutex_t> lock(mutex);

    if (NumViewports > 16)
      return;
    if (state_.Rasterizer.NumViewports == NumViewports &&
        memcmp(state_.Rasterizer.viewports, pViewports, NumViewports * sizeof(D3D11_VIEWPORT)) == 0) {
      d11cs::NoteRepeated(); // spec 05 clarifications 63 and 76
      return;
    }
    state_.Rasterizer.NumViewports = NumViewports;
    for (auto i = 0u; i < NumViewports; i++) {
      state_.Rasterizer.viewports[i] = pViewports[i];
    }
    dirty_state.set(DirtyState::Viewport);
    IMTLD3D11RasterizerState *current_rs =
        state_.Rasterizer.RasterizerState ? state_.Rasterizer.RasterizerState : default_rasterizer_state;
    if (!current_rs->IsScissorEnabled()) {
      dirty_state.set(DirtyState::Scissors);
    }
  }

  void
  STDMETHODCALLTYPE
  RSGetViewports(UINT *pNumViewports, D3D11_VIEWPORT *pViewports) override {
    D11_CTX_CALL(RSGetViewports);
    std::lock_guard<mutex_t> lock(mutex);

    uint32_t num_viewports_available = state_.Rasterizer.NumViewports;
    uint32_t num_viewports_requested = *pNumViewports;

    if (pViewports) {
      for (unsigned i = 0; i < num_viewports_requested; i++) {
        if (i < num_viewports_available)
          pViewports[i] = state_.Rasterizer.viewports[i];
        else
          pViewports[i] = {0, 0, 0, 0, 0, 0};
      };
      *pNumViewports = std::min(num_viewports_available, num_viewports_requested);
    } else {
      *pNumViewports = num_viewports_available;
    }
  }

  void
  STDMETHODCALLTYPE
  RSSetScissorRects(UINT NumRects, const D3D11_RECT *pRects) override {
    D11_CTX_CALL(RSSetScissorRects);
    std::lock_guard<mutex_t> lock(mutex);

    if (NumRects > 16)
      return;
    if (state_.Rasterizer.NumScissorRects == NumRects &&
        memcmp(state_.Rasterizer.scissor_rects, pRects, NumRects * sizeof(D3D11_RECT)) == 0) {
      d11cs::NoteRepeated(); // spec 05 clarifications 63 and 76
      return;
    }
    state_.Rasterizer.NumScissorRects = NumRects;
    for (unsigned i = 0; i < NumRects; i++) {
      state_.Rasterizer.scissor_rects[i] = pRects[i];
    }
    IMTLD3D11RasterizerState *current_rs =
        state_.Rasterizer.RasterizerState ? state_.Rasterizer.RasterizerState : default_rasterizer_state;
    if (current_rs->IsScissorEnabled()) {
      dirty_state.set(DirtyState::Scissors);
      // otherwise no need to update scissor because it's either already dirty
      // or duplicates viewports
    }
  }

  void
  STDMETHODCALLTYPE
  RSGetScissorRects(UINT *pNumRects, D3D11_RECT *pRects) override {
    D11_CTX_CALL(RSGetScissorRects);
    std::lock_guard<mutex_t> lock(mutex);

    uint32_t num_rects_available = state_.Rasterizer.NumScissorRects;
    uint32_t num_rects_requested = *pNumRects;

    if (pRects) {
      for (unsigned i = 0; i < num_rects_requested; i++) {
        if (i < num_rects_available)
          pRects[i] = state_.Rasterizer.scissor_rects[i];
        else
          pRects[i] = {0, 0, 0, 0};
      };
      *pNumRects = std::min(num_rects_available, num_rects_requested);
    } else {
      *pNumRects = num_rects_available;
    }
  }
#pragma endregion

#pragma region D3D11DeviceContext2

  HRESULT STDMETHODCALLTYPE UpdateTileMappings(
      ID3D11Resource *resource, UINT region_count, const D3D11_TILED_RESOURCE_COORDINATE *region_start_coordinates,
      const D3D11_TILE_REGION_SIZE *region_sizes, ID3D11Buffer *pool, UINT range_count, const UINT *range_flags,
      const UINT *pool_start_offsets, const UINT *range_tile_counts, UINT flags
  ) override {
    D11_CTX_CALL(UpdateTileMappings);
    IMPLEMENT_ME
  };

  HRESULT STDMETHODCALLTYPE CopyTileMappings(
      ID3D11Resource *dst_resource, const D3D11_TILED_RESOURCE_COORDINATE *dst_start_coordinate,
      ID3D11Resource *src_resource, const D3D11_TILED_RESOURCE_COORDINATE *src_start_coordinate,
      const D3D11_TILE_REGION_SIZE *region_size, UINT flags
  ) override {
    D11_CTX_CALL(CopyTileMappings);
    IMPLEMENT_ME
  };

  void STDMETHODCALLTYPE CopyTiles(
      ID3D11Resource *resource, const D3D11_TILED_RESOURCE_COORDINATE *start_coordinate,
      const D3D11_TILE_REGION_SIZE *size, ID3D11Buffer *buffer, UINT64 start_offset, UINT flags
  ) override {
    D11_CTX_CALL(CopyTiles);
    IMPLEMENT_ME
  };

  void STDMETHODCALLTYPE UpdateTiles(
      ID3D11Resource *dst_resource, const D3D11_TILED_RESOURCE_COORDINATE *dst_start_coordinate,
      const D3D11_TILE_REGION_SIZE *dst_region_size, const void *src_data, UINT flags
  ) override {
    D11_CTX_CALL(UpdateTiles);
    IMPLEMENT_ME
  };

  HRESULT STDMETHODCALLTYPE ResizeTilePool(ID3D11Buffer *pool, UINT64 size) override {
    D11_CTX_CALL(ResizeTilePool);
    IMPLEMENT_ME
  }

  void STDMETHODCALLTYPE
  TiledResourceBarrier(ID3D11DeviceChild *before_barrier, ID3D11DeviceChild *after_barrier) override {
    D11_CTX_CALL(TiledResourceBarrier);
    IMPLEMENT_ME
  };

  WINBOOL STDMETHODCALLTYPE IsAnnotationEnabled() override {
    D11_CTX_CALL(IsAnnotationEnabled);
    IMPLEMENT_ME
  }

  void STDMETHODCALLTYPE SetMarkerInt(const WCHAR *label, int data) override {
    D11_CTX_CALL(SetMarkerInt);
    IMPLEMENT_ME
  }

  void STDMETHODCALLTYPE BeginEventInt(const WCHAR *label, int data) override {
    D11_CTX_CALL(BeginEventInt);
    IMPLEMENT_ME
  }

  void STDMETHODCALLTYPE EndEvent() override {
    D11_CTX_CALL(EndEvent);
    IMPLEMENT_ME
  }

#pragma endregion

#pragma region 11.3

  void STDMETHODCALLTYPE
  Flush1(D3D11_CONTEXT_TYPE type, HANDLE event) override {
    D11_CTX_CALL(Flush1);
    IMPLEMENT_ME;
  }

  void STDMETHODCALLTYPE
  SetHardwareProtectionState(WINBOOL enable) override {
    D11_CTX_CALL(SetHardwareProtectionState);
    WARN("SetHardwareProtectionState: stub");
  }

  void STDMETHODCALLTYPE
  GetHardwareProtectionState(WINBOOL *enable) override {
    D11_CTX_CALL(GetHardwareProtectionState);
    *enable = false;
    WARN("GetHardwareProtectionState: stub");
  }

#pragma endregion

#pragma region Internal

  template <CommandWithContext<ArgumentEncodingContext> cmd> void EmitST(cmd &&fn);
  template <CommandWithContext<ArgumentEncodingContext> cmd> void EmitOP(cmd &&fn);

  template <typename T> moveonly_list<T> AllocateCommandData(size_t n = 1);

  std::tuple<WMT::Buffer, uint64_t> AllocateStagingBuffer(size_t size, size_t alignment);
  /* spec 05 clarification 24: the number of the present that ends the frame being recorded
     (immediate context), or 0 (a deferred context records for later) */
  uint64_t RecordingPresent();
  /* spec 05 clarification 57: at PG_DXMT_SERIALIZE=3 and above, before the CPU is handed memory
     to write, commit and wait for the GPU (immediate context; a deferred one does nothing) */
  void SerializeBeforeCpuWrite();
  void UseCopyDestination(Rc<StagingResource> &);
  void UseCopySource(Rc<StagingResource> &);

  std::pair<BufferAllocation *, uint32_t>
  GetDynamicBufferAllocation(Rc<DynamicBuffer> &dynamic);

  uint64_t *allocated_encoder_argbuf_size_ = nullptr;

  uint64_t PreAllocateArgumentBuffer(size_t size, size_t alignment) {
    assert(allocated_encoder_argbuf_size_ && "otherwise invalid call");
    std::size_t adjustment = align_forward_adjustment((void *)*allocated_encoder_argbuf_size_, alignment);
    auto aligned = *allocated_encoder_argbuf_size_ + adjustment;
    *allocated_encoder_argbuf_size_ = aligned + size;
    return aligned;
  }

  template <PipelineStage stage, PipelineKind kind>
  void
  UploadShaderStageResourceBinding() {
    auto &ShaderStage = state_.ShaderStages[stage];
    if (!ShaderStage.Shader) {
      return;
    }
    auto &UAVBindingSet = stage == PipelineStage::Compute ? state_.ComputeStageUAV.UAVs : state_.OutputMerger.UAVs;

    auto managed_shader = ShaderStage.Shader->GetManagedShader();
    const MTL_SHADER_REFLECTION *reflection = &managed_shader->reflection();

    bool dirty_cbuffer = ShaderStage.ConstantBuffers.any_dirty_masked(reflection->ConstantBufferSlotMask);
    bool dirty_sampler = ShaderStage.Samplers.any_dirty_masked(reflection->SamplerSlotMask);
    bool dirty_srv = ShaderStage.SRVs.any_dirty_masked(reflection->SRVSlotMaskHi, reflection->SRVSlotMaskLo);
    bool dirty_uav = UAVBindingSet.any_dirty_masked(reflection->UAVSlotMask);
    if (!dirty_cbuffer && !dirty_sampler && !dirty_srv && !dirty_uav)
      return;

    if (reflection->NumConstantBuffers && dirty_cbuffer) {
      auto ConstantBufferCount = reflection->NumConstantBuffers;
      auto offset = PreAllocateArgumentBuffer(ConstantBufferCount << 3, 32);
      EmitST([=, cb = managed_shader->constant_buffers_info()](ArgumentEncodingContext &enc) {
        enc.encodeConstantBuffers<stage, kind>(reflection, cb, offset);
      });
      D11_WORK(work_.tables_cb++); // spec 05 clarification 63
      ShaderStage.ConstantBuffers.clear_dirty();
    }

    if (reflection->NumArguments && (dirty_sampler || dirty_srv || dirty_uav)) {
      auto ArgumentTableQwords = reflection->ArgumentTableQwords;
      auto offset = PreAllocateArgumentBuffer(ArgumentTableQwords << 3, 32);
      EmitST([=, arg = managed_shader->arguments_info()](ArgumentEncodingContext &enc) {
        enc.encodeShaderResources<stage, kind>(reflection, arg, offset);
      });
      D11_WORK(work_.tables_srv++); // spec 05 clarification 63
      ShaderStage.Samplers.clear_dirty();
      ShaderStage.SRVs.clear_dirty();
      if (stage == PipelineStage::Pixel || stage == PipelineStage::Compute) {
        UAVBindingSet.clear_dirty();
      }
    }
  }

  void
  UpdateVertexBuffer() {
    if (!state_.InputAssembler.InputLayout)
      return;

    uint32_t slot_mask = state_.InputAssembler.InputLayout->GetManagedInputLayout()->input_slot_mask();
    if (slot_mask == 0) // effectively empty input layout
      return;

    auto &VertexBuffers = state_.InputAssembler.VertexBuffers;
    if (!VertexBuffers.any_dirty_masked((uint64_t)slot_mask))
      return;

    uint32_t num_slots = __builtin_popcount(slot_mask);
    auto offset = PreAllocateArgumentBuffer(16 * num_slots, 32);
    D11_WORK(work_.tables_vb++); // spec 05 clarification 63

    if (cmdbuf_state == CommandBufferState::TessellationRenderPipelineReady) {
      EmitST([=](ArgumentEncodingContext &enc) { enc.encodeVertexBuffers<PipelineKind::Tessellation>(slot_mask, offset); });
    }
    if (cmdbuf_state == CommandBufferState::GeometryRenderPipelineReady) {
      EmitST([=](ArgumentEncodingContext &enc) { enc.encodeVertexBuffers<PipelineKind::Geometry>(slot_mask, offset); });
    }
    if (cmdbuf_state == CommandBufferState::RenderPipelineReady) {
      EmitST([=](ArgumentEncodingContext &enc) { enc.encodeVertexBuffers<PipelineKind::Ordinary>(slot_mask, offset); });
    }

    VertexBuffers.clear_dirty_mask(slot_mask);
  }

  /**
  Valid transition:
  * -> Idle
  Idle -> RenderEncoderActive
  Idle -> ComputeEncoderActive
  Idle -> (Upload|Readback)BlitEncoderActive
  RenderEncoderActive <-> RenderPipelineReady
  ComputeEncoderActive <-> CoputePipelineReady
  */
  enum class CommandBufferState {
    Idle,
    RenderEncoderActive,
    RenderPipelineReady,
    TessellationRenderPipelineReady,
    GeometryRenderPipelineReady,
    ComputeEncoderActive,
    ComputePipelineReady,
    BlitEncoderActive,
    UpdateBlitEncoderActive,
    ReadbackBlitEncoderActive
  };

#pragma region ShaderCommon

  template <PipelineStage stage, typename IShader>
  void
  SetShader(IShader *pShader, ID3D11ClassInstance *const *ppClassInstances, UINT NumClassInstances) {
    std::lock_guard<mutex_t> lock(mutex);

    auto &ShaderStage = state_.ShaderStages[stage];

    if (pShader) {
      if (auto expected = com_cast<IMTLD3D11Shader>(pShader)) {
        if (ShaderStage.Shader.ptr() == expected.ptr()) {
          d11cs::NoteRepeated(); // spec 05 clarification 63
          return;
        }
        ShaderStage.Shader = std::move(expected);
        ShaderStage.ConstantBuffers.set_dirty();
        ShaderStage.SRVs.set_dirty();
        ShaderStage.Samplers.set_dirty();
        if constexpr (stage == PipelineStage::Vertex) {
          state_.InputAssembler.VertexBuffers.set_dirty();
        }
        if constexpr (stage == PipelineStage::Compute) {
          state_.ComputeStageUAV.UAVs.set_dirty();
        } else {
          state_.OutputMerger.UAVs.set_dirty();
        }
      } else {
        D3D11_ASSERT(0 && "unexpected shader object");
      }
    } else {
      if (!ShaderStage.Shader)
        d11cs::NoteRepeated(); // spec 05 clarification 63
      ShaderStage.Shader = nullptr;
    }

    if (NumClassInstances)
      ERR("Class instances not supported");

    if constexpr (stage == PipelineStage::Compute) {
      InvalidateComputePipeline();
    } else {
      InvalidateRenderPipeline();
    }
  }

  template <PipelineStage Type, typename IShader>
  void
  GetShader(IShader **ppShader, ID3D11ClassInstance **ppClassInstances, UINT *pNumClassInstances) {
    std::lock_guard<mutex_t> lock(mutex);

    auto &ShaderStage = state_.ShaderStages[Type];

    if (ppShader) {
      if (ShaderStage.Shader) {
        ShaderStage.Shader->QueryInterface(IID_PPV_ARGS(ppShader));
      } else {
        *ppShader = NULL;
      }
    }
    if (ppClassInstances) {
      *ppClassInstances = NULL;
    }
    if (pNumClassInstances) {
      *pNumClassInstances = 0;
    }
  }

  template <PipelineStage Stage>
  void
  SetConstantBuffer(
      UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppConstantBuffers, const UINT *pFirstConstant,
      const UINT *pNumConstants
  ) {
    std::lock_guard<mutex_t> lock(mutex);

    auto &ShaderStage = state_.ShaderStages[Stage];

    // MYIOSDECK spec 05 clarification 19: once per process, the first non-zero FirstConstant
    if (FirstConstantBufferOffsetInUse(pFirstConstant, NumBuffers, ConstantBufferOffsetsSeen()))
      Logger::info("dxmt: constant buffer offsets in use");

    bool changed = false; // spec 05 clarification 63
    for (unsigned slot = StartSlot; slot < StartSlot + NumBuffers; slot++) {
      auto pConstantBuffer = ppConstantBuffers[slot - StartSlot];
      if (pConstantBuffer) {
        // MYIOSDECK spec 05 clarification 19: a NULL pFirstConstant / pNumConstants means the
        // buffer's start / its whole size (at most 4096 constants), also when the same buffer
        // stays bound after a *SetConstantBuffers1 with an offset.
        D3D11_BUFFER_DESC desc;
        pConstantBuffer->GetDesc(&desc);
        auto range = ConstantBufferBindRange(pFirstConstant, pNumConstants, slot - StartSlot, desc.ByteWidth);
        if (range.FirstConstant != 0 && FrameTraceSwitch()) // spec 05 clarification 21
          EmitST([](ArgumentEncodingContext &enc) { enc.traceConstantBufferOffsetBind(); });
        bool replaced = false;
        auto &entry = ShaderStage.ConstantBuffers.bind(slot, {pConstantBuffer}, replaced);
        if (!replaced) {
          auto changes = ConstantBufferRebindChanges({entry.FirstConstant, entry.NumConstants}, range);
          changed |= changes.offset_changed || changes.size_changed;
          if (changes.offset_changed) {
            ShaderStage.ConstantBuffers.set_dirty(slot);
            entry.FirstConstant = range.FirstConstant;
            EmitST([=, offset = entry.FirstConstant
                                << 4](ArgumentEncodingContext &enc) mutable {
              enc.bindConstantBufferOffset<Stage>(slot, offset);
            });
          }
          if (changes.size_changed) {
            ShaderStage.ConstantBuffers.set_dirty(slot);
            entry.NumConstants = range.NumConstants;
          }
          continue;
        }
        changed = true;
        entry.FirstConstant = range.FirstConstant;
        entry.NumConstants = range.NumConstants;
        entry.Buffer = reinterpret_cast<D3D11ResourceCommon *>(pConstantBuffer);
        EmitST([=, buffer = entry.Buffer->buffer(), offset = entry.FirstConstant << 4](ArgumentEncodingContext &enc
             ) mutable { enc.bindConstantBuffer<Stage>(slot, offset, forward_rc(buffer)); });
      } else {
        // BIND NULL
        if (ShaderStage.ConstantBuffers.unbind(slot)) {
          changed = true;
          EmitST([=](ArgumentEncodingContext &enc) { enc.bindConstantBuffer<Stage>(slot, 0, {}); });
        }
      }
    }
    if (!changed)
      d11cs::NoteRepeated(); // spec 05 clarification 63
  }

  template <PipelineStage Stage>
  void
  GetConstantBuffer(
      UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppConstantBuffers, UINT *pFirstConstant, UINT *pNumConstants
  ) {
    std::lock_guard<mutex_t> lock(mutex);

    auto &ShaderStage = state_.ShaderStages[Stage];

    for (auto i = 0u; i < NumBuffers; i++) {
      if (ShaderStage.ConstantBuffers.test_bound(StartSlot + i)) {
        auto &cb = ShaderStage.ConstantBuffers.at(StartSlot + i);
        if (ppConstantBuffers) {
          cb.Buffer->QueryInterface(IID_PPV_ARGS(&ppConstantBuffers[i]));
        }
        if (pFirstConstant) {
          pFirstConstant[i] = cb.FirstConstant;
        }
        if (pNumConstants) {
          pNumConstants[i] = cb.NumConstants;
        }
      } else {
        if (ppConstantBuffers) {
          ppConstantBuffers[i] = nullptr;
        }
      }
    }
  }

  template <PipelineStage Stage>
  void
  SetShaderResource(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView *const *ppShaderResourceViews) {
    std::lock_guard<mutex_t> lock(mutex);


    auto &ShaderStage = state_.ShaderStages[Stage];
    bool changed = false; // spec 05 clarification 63
    for (unsigned slot = StartSlot; slot < StartSlot + NumViews; slot++) {
      auto pView = static_cast<D3D11ShaderResourceView *>(ppShaderResourceViews[slot - StartSlot]);
      // spec 05 clarification 69: a view of the kept depth-stencil texture ends the keep, and the
      // pass with it (switch on)
      if (pView && unlikely(keep_dsv_.active()) && keep_dsv_view_ &&
          pView->texture().ptr() == keep_dsv_view_->texture().ptr()) {
        KeepDsvTracker::Note note;
        if (keep_dsv_.keptTextureBound(note)) {
          EmitKeepDsvNote(note);
          if (note.applied)
            InvalidateCurrentPass();
        }
      }
      if (pView) {
        bool replaced = false;
        auto &entry = ShaderStage.SRVs.bind(slot, {pView}, replaced);
        if (!replaced)
          continue;
        changed = true;
        entry.SRV = pView;
        if (auto buffer = entry.SRV->buffer()) {
          EmitST([=, buffer = std::move(buffer), viewId = pView->viewId(),
                slice = pView->bufferSlice()](ArgumentEncodingContext &enc) mutable {
            enc.bindBuffer<Stage>(slot, forward_rc(buffer), viewId, slice);
          });
        } else {
          EmitST([=, texture = pView->texture(), viewId = pView->viewId()](ArgumentEncodingContext &enc) mutable {
            enc.bindTexture<Stage>(slot, forward_rc(texture), viewId);
          });
        }
      } else {
        // BIND NULL
        if (ShaderStage.SRVs.unbind(slot)) {
          changed = true;
          EmitST([=](ArgumentEncodingContext& enc) {
            enc.bindBuffer<Stage>(slot, {}, 0, {});
          });
        }
      }
    }
    if (!changed)
      d11cs::NoteRepeated(); // spec 05 clarification 63
  }

  template <PipelineStage Stage>
  void
  GetShaderResource(UINT StartSlot, UINT NumViews, ID3D11ShaderResourceView **ppShaderResourceViews) {
    std::lock_guard<mutex_t> lock(mutex);

    auto &ShaderStage = state_.ShaderStages[Stage];

    if (!ppShaderResourceViews)
      return;
    for (auto i = 0u; i < NumViews; i++) {
      if (ShaderStage.SRVs.test_bound(StartSlot + i)) {
        ShaderStage.SRVs[StartSlot + i].SRV->QueryInterface(IID_PPV_ARGS(&ppShaderResourceViews[i]));
      } else {
        ppShaderResourceViews[i] = nullptr;
      }
    }
  }

  template <PipelineStage Stage>
  void
  SetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState *const *ppSamplers) {
    std::lock_guard<mutex_t> lock(mutex);

    auto &ShaderStage = state_.ShaderStages[Stage];
    bool changed = false; // spec 05 clarification 63
    for (unsigned Slot = StartSlot; Slot < StartSlot + NumSamplers; Slot++) {
      auto pSampler = static_cast<D3D11SamplerState *>(ppSamplers[Slot - StartSlot]);
      if (pSampler) {
        bool replaced = false;
        auto &entry = ShaderStage.Samplers.bind(Slot, {pSampler}, replaced);
        if (!replaced)
          continue;
        changed = true;
        entry.Sampler = pSampler;
        EmitST([=, sampler = pSampler->sampler()](ArgumentEncodingContext &enc) mutable {
          enc.bindSampler<Stage>(Slot, forward_rc(sampler));
        });
      } else {
        // BIND NULL
        if (ShaderStage.Samplers.unbind(Slot)) {
          changed = true;
          EmitST([=](ArgumentEncodingContext& enc) {
            enc.bindSampler<Stage>(Slot, {});
          });
        }
      }
    }
    if (!changed)
      d11cs::NoteRepeated(); // spec 05 clarification 63
  }

  template <PipelineStage Stage>
  void
  GetSamplers(UINT StartSlot, UINT NumSamplers, ID3D11SamplerState **ppSamplers) {
    std::lock_guard<mutex_t> lock(mutex);

    auto &ShaderStage = state_.ShaderStages[Stage];
    if (ppSamplers) {
      for (unsigned Slot = StartSlot; Slot < StartSlot + NumSamplers; Slot++) {
        if (ShaderStage.Samplers.test_bound(Slot)) {
          ShaderStage.Samplers[Slot].Sampler->QueryInterface(IID_PPV_ARGS(&ppSamplers[Slot - StartSlot]));
        } else {
          ppSamplers[Slot - StartSlot] = nullptr;
        }
      }
    }
  }

  void
  UpdateUAVCounter(D3D11UnorderedAccessView *uav, uint32_t value) {
    EmitST([counter = uav->counter(), value](ArgumentEncodingContext &enc) {
      if (!counter.ptr())
        return;
      /* TODO: suballocate uav counter */
      auto new_counter = counter->allocate(BufferAllocationFlag::GpuManaged);
      new_counter->updateContents(0, &value, 4);
      new_counter->buffer().didModifyRange(0, 4);
      auto old = counter->rename(std::move(new_counter));
      // TODO: reused discarded buffer
    });
  }

  template <PipelineStage Stage>
  bool
  SetUnorderedAccessView(
      UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView *const *ppUnorderedAccessViews,
      const UINT *pUAVInitialCounts
  ) {
    bool changed = false; // spec 05 clarification 69: returned
    auto &binding_set = Stage == PipelineStage::Compute ? state_.ComputeStageUAV.UAVs : state_.OutputMerger.UAVs;

    // std::erase_if(state_.ComputeStageUAV.UAVs, [&](const auto &item) -> bool
    // {
    //   auto &[slot, bound_uav] = item;
    //   if (slot < StartSlot || slot >= (StartSlot + NumUAVs))
    //     return false;
    //   for (auto i = 0u; i < NumUAVs; i++) {
    //     if (auto uav = static_cast<MTLD3D11UnorderedAccessView *>(
    //             ppUnorderedAccessViews[i])) {
    //       // if (bound_uav.View->GetViewRange().CheckOverlap(
    //       //         uav->GetViewRange())) {
    //       //   return true;
    //       // }
    //     }
    //   }
    //   return false;
    // });

    for (unsigned slot = StartSlot; slot < StartSlot + NumUAVs; slot++) {
      auto pUAV = static_cast<D3D11UnorderedAccessView*>(ppUnorderedAccessViews[slot - StartSlot]);
      auto InitialCount = pUAVInitialCounts ? pUAVInitialCounts[slot - StartSlot] : ~0u;
      if (pUAV) {
        bool replaced = false;
        auto &entry = binding_set.bind(slot, {pUAV}, replaced);
        if (InitialCount != ~0u) {
          UpdateUAVCounter(pUAV, InitialCount);
        }
        if (!replaced) {
          continue;
        }
        changed = true;
        entry.View = pUAV;
        if (auto buffer = pUAV->buffer()) {
          EmitST([=, buffer = std::move(buffer), viewId = pUAV->viewId(), counter = pUAV->counter(),
                slice = pUAV->bufferSlice()](ArgumentEncodingContext &enc) mutable {
            enc.bindOutputBuffer<Stage>(slot, forward_rc(buffer), viewId, forward_rc(counter), slice);
          });
        } else {
          EmitST([=, texture = pUAV->texture(), viewId = pUAV->viewId()](ArgumentEncodingContext &enc) mutable {
            enc.bindOutputTexture<Stage>(slot, forward_rc(texture), viewId);
          });
        }
        // FIXME: resolve srv hazard: unbind any cs srv that share the resource
        // std::erase_if(state_.ShaderStages[5].SRVs,
        //               [&](const auto &item) -> bool {
        //                 // auto &[slot, bound_srv] = item;
        //                 // if srv conflict with uav, return true
        //                 return false;
        //               });
      } else {
        if (binding_set.unbind(slot)) {
          changed = true;
          EmitST([=](ArgumentEncodingContext &enc) { enc.bindOutputBuffer<Stage>(slot, {}, 0, {}, {}); });
        }
      }
    }
    return changed;
  }

#pragma endregion

#pragma region InputAssembler
  void
  SetVertexBuffers(
      UINT StartSlot, UINT NumBuffers, ID3D11Buffer *const *ppVertexBuffers, const UINT *pStrides, const UINT *pOffsets
  ) {
    std::lock_guard<mutex_t> lock(mutex);

    auto &VertexBuffers = state_.InputAssembler.VertexBuffers;
    bool changed = false; // spec 05 clarification 63
    for (unsigned slot = StartSlot; slot < StartSlot + NumBuffers; slot++) {
      auto pVertexBuffer = ppVertexBuffers[slot - StartSlot];
      if (pVertexBuffer) {
        bool replaced = false;
        auto &entry = VertexBuffers.bind(slot, {pVertexBuffer}, replaced);
        if (!replaced) {
          if (pStrides && pStrides[slot - StartSlot] != entry.Stride) {
            VertexBuffers.set_dirty(slot);
            entry.Stride = pStrides[slot - StartSlot];
            changed = true;
          }
          if (pOffsets && pOffsets[slot - StartSlot] != entry.Offset) {
            VertexBuffers.set_dirty(slot);
            entry.Offset = pOffsets[slot - StartSlot];
            changed = true;
          }
          EmitST([=, offset = entry.Offset,
                stride = entry.Stride](ArgumentEncodingContext &enc) mutable {
            enc.bindVertexBufferOffset(slot, offset, stride);
          });
          continue;
        }
        if (pStrides) {
          entry.Stride = pStrides[slot - StartSlot];
        } else {
          ERR("SetVertexBuffers: stride is null");
          entry.Stride = 0;
        }
        if (pOffsets) {
          entry.Offset = pOffsets[slot - StartSlot];
        } else {
          ERR("SetVertexBuffers: offset is null");
          entry.Stride = 0;
        }
        entry.Buffer = reinterpret_cast<D3D11ResourceCommon *>(pVertexBuffer);
        changed = true;
          EmitST([=, buffer = entry.Buffer->buffer(), offset = entry.Offset,
                stride = entry.Stride](ArgumentEncodingContext &enc) mutable {
            enc.bindVertexBuffer(slot, offset, stride, forward_rc(buffer));
        });
      } else {
        if (VertexBuffers.unbind(slot)) {
          changed = true;
          EmitST([=](ArgumentEncodingContext& enc) {
            enc.bindVertexBuffer(slot, 0, 0, {});
          });
        }
      }
    }
    if (!changed)
      d11cs::NoteRepeated(); // spec 05 clarification 63
  }

  void
  GetVertexBuffers(UINT StartSlot, UINT NumBuffers, ID3D11Buffer **ppVertexBuffers, UINT *pStrides, UINT *pOffsets) {
    std::lock_guard<mutex_t> lock(mutex);

    auto &VertexBuffers = state_.InputAssembler.VertexBuffers;
    for (unsigned i = 0; i < NumBuffers; i++) {
      if (!VertexBuffers.test_bound(i + StartSlot)) {
        if (ppVertexBuffers != NULL)
          ppVertexBuffers[i] = nullptr;
        if (pStrides != NULL)
          pStrides[i] = 0;
        if (pOffsets != NULL)
          pOffsets[i] = 0;
        continue;
      }
      auto &VertexBuffer = state_.InputAssembler.VertexBuffers[i + StartSlot];
      if (ppVertexBuffers != NULL) {
        VertexBuffer.Buffer->QueryInterface(IID_PPV_ARGS(&ppVertexBuffers[i]));
      }
      if (pStrides != NULL)
        pStrides[i] = VertexBuffer.Stride;
      if (pOffsets != NULL)
        pOffsets[i] = VertexBuffer.Offset;
    }
  }
#pragma endregion

#pragma region CopyResource

  void
  CopyBuffer(
      ID3D11Buffer *pDstResource, uint32_t DstSubresource, uint32_t DstX, uint32_t DstY, uint32_t DstZ,
      ID3D11Buffer *pSrcResource, uint32_t SrcSubresource, const D3D11_BOX *pSrcBox
  ) {
    D3D11_BUFFER_DESC dst_desc;
    D3D11_BUFFER_DESC src_desc;
    pDstResource->GetDesc(&dst_desc);
    pSrcResource->GetDesc(&src_desc);
    D3D11_ASSERT(SrcSubresource == 0);
    D3D11_ASSERT(DstSubresource == 0);
    D3D11_BOX SrcBox;
    if (pSrcBox) {
      SrcBox = *pSrcBox;
    } else {
      SrcBox.left = 0;
      SrcBox.right = src_desc.ByteWidth;
    }
    if (SrcBox.right <= SrcBox.left)
      return;
    if (auto staging_dst = GetStagingResource(pDstResource, DstSubresource)) {
      if (auto staging_src = GetStagingResource(pSrcResource, SrcSubresource)) {
        SwitchToBlitEncoder(CommandBufferState::BlitEncoderActive);
        UseCopyDestination(staging_dst);
        UseCopySource(staging_src);
        EmitOP([dst_ = std::move(staging_dst), src_ = std::move(staging_src), DstX,
                SrcX = SrcBox.left, Size = SrcBox.right - SrcBox.left](ArgumentEncodingContext& enc) {
          auto [src, src_offset] = enc.access(src_->buffer(), SrcX, Size, DXMT_ENCODER_RESOURCE_ACESS_READ);
          auto [dst, dst_offset] = enc.access(dst_->buffer(), DstX, Size, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          auto &cmd = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
          cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
          cmd.copy_length = Size;
          cmd.src = src->buffer();
          cmd.src_offset = SrcX + src_offset;
          cmd.dst = dst->buffer();
          cmd.dst_offset = DstX + dst_offset;
        });
        promote_flush = true;
      } else if (auto src = reinterpret_cast<D3D11ResourceCommon *>(pSrcResource)) {
        // copy from device to staging
        SwitchToBlitEncoder(CommandBufferState::ReadbackBlitEncoderActive);
        UseCopyDestination(staging_dst);
        EmitOP([src_ = src->buffer(), dst_ = std::move(staging_dst), DstX, SrcBox](ArgumentEncodingContext &enc) {
          auto [src, src_offset] = enc.access(src_, SrcBox.left, SrcBox.right - SrcBox.left, DXMT_ENCODER_RESOURCE_ACESS_READ);
          auto [dst, dst_offset] = enc.access(dst_->buffer(), DstX, SrcBox.right - SrcBox.left, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          auto &cmd = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
          cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
          cmd.copy_length = SrcBox.right - SrcBox.left;
          cmd.src = src->buffer();;
          cmd.src_offset = SrcBox.left + src_offset;
          cmd.dst = dst->buffer();
          cmd.dst_offset = DstX + dst_offset;
        });
        promote_flush = true;
      } else {
        UNIMPLEMENTED("todo");
      }
    } else if (auto dst = reinterpret_cast<D3D11ResourceCommon *>(pDstResource)) {
      if (auto staging_src = GetStagingResource(pSrcResource, SrcSubresource)) {
        SwitchToBlitEncoder(CommandBufferState::UpdateBlitEncoderActive);
        UseCopySource(staging_src);
        EmitOP([dst_ = dst->buffer(), src_ = std::move(staging_src), DstX, SrcBox](ArgumentEncodingContext &enc) {
          auto [src, src_offset] = enc.access(src_->buffer(), SrcBox.left, SrcBox.right - SrcBox.left, DXMT_ENCODER_RESOURCE_ACESS_READ);
          auto [dst, dst_offset] = enc.access(dst_, DstX, SrcBox.right - SrcBox.left, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          auto &cmd = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
          cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
          cmd.copy_length = SrcBox.right - SrcBox.left;
          cmd.src = src->buffer();
          cmd.src_offset = SrcBox.left + src_offset;
          cmd.dst = dst->buffer();;
          cmd.dst_offset = DstX + dst_offset;
        });
      } else if (auto src = reinterpret_cast<D3D11ResourceCommon *>(pSrcResource)) {
        // on-device copy
        SwitchToBlitEncoder(CommandBufferState::BlitEncoderActive);
        EmitOP([dst_ = dst->buffer(), src_ = src->buffer(), DstX,
                               SrcBox](ArgumentEncodingContext& enc) {
          auto [src, src_offset] = enc.access(src_, SrcBox.left, SrcBox.right - SrcBox.left, DXMT_ENCODER_RESOURCE_ACESS_READ);
          auto [dst, dst_offset] = enc.access(dst_, DstX, SrcBox.right - SrcBox.left, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          auto &cmd = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
          cmd.type = WMTBlitCommandCopyFromBufferToBuffer;
          cmd.copy_length = SrcBox.right - SrcBox.left;
          cmd.src = src->buffer();;
          cmd.src_offset = SrcBox.left + src_offset;
          cmd.dst = dst->buffer();;
          cmd.dst_offset = DstX + dst_offset;
        });
      } else {
        UNIMPLEMENTED("todo");
      }
    } else {
      UNIMPLEMENTED("todo");
    }
  }

#ifdef DXMT_IOS
  /**
  Spec 05 Part C: decode `width` x `height` (x `depth`) texels of block data
  (rows of blocks `block_row_pitch` apart, depth slices `block_depth_pitch`
  apart) on the calling thread and upload them into a BC texture whose Metal
  texture is uncompressed. A 3D texture is decoded slice by slice, each depth
  slice an independent 2D BC image (clarification 3). Raw block bytes never
  reach such a texture: if the data can't be decoded or the decode buffer
  can't be allocated, the upload is skipped (and logged).
  */
  void
  UploadEmulatedBC(
      Rc<Texture> &&dst, int32_t level, uint32_t slice, WMTOrigin origin, uint32_t width, uint32_t height,
      uint32_t depth, uint32_t level_width, uint32_t level_height, const void *blocks, size_t block_row_pitch,
      size_t block_depth_pitch
  ) {
    WMTPixelFormat format = dst->pixelFormat();
    if (dst->textureType() != WMTTextureType3D)
      depth = 1;
    if (depth == 0)
      return;
    /* Spec 05 D5: level is physical; -1 is the logical level just above the
       clamp, which isn't uploaded but may refill physical level 0. */
    if (level < -1 || (level == -1 && !dst->isLevelLost(0)))
      return;
    size_t row_bytes = size_t(width) * BCPhysicalBytesPerTexel(format);
    size_t total_bytes = row_bytes * height * depth;
    void *decoded = malloc(total_bytes);
    if (!decoded) {
      WARN_RARE("BC emulation: no memory to decode a ", width, "x", height, "x", depth, " upload, skipped");
      return;
    }
    if (!BCDecodeVolumeToPhysical(format, blocks, block_row_pitch, block_depth_pitch, decoded, width, height, depth)) {
      WARN_RARE(
          "BC emulation: upload with row pitch ", block_row_pitch, " / depth pitch ", block_depth_pitch,
          " can't be decoded, skipped"
      );
      free(decoded);
      return;
    }
    bool full_level = origin.x == 0 && origin.y == 0 && width == level_width && height == level_height;
    if (level >= 0) {
      UploadPhysicalTexels(dst, level, slice, origin, width, height, decoded, row_bytes, depth);
      if (full_level)
        dst->clearLevelLost(level);
    }
    /* D5 mip clamp (and so lost levels) only exists for 2D textures */
    if (full_level && depth == 1)
      RefillLostLevels(dst, level + 1, slice, decoded, width, height);
    free(decoded);
  }

  /**
  Spec 05 D5 refill: `texels` is a full write of the level above `level`
  (physical). While `level` is lost, box-filter 2x2 into it, upload, clear the
  mark and continue down. Done on the calling thread (so there is no stale
  result to skip). BC6H levels get the grey fill instead.
  */
  void
  RefillLostLevels(
      Rc<Texture> &dst, int32_t level, uint32_t slice, const void *texels, uint32_t width, uint32_t height
  ) {
    WMTPixelFormat format = dst->pixelFormat();
    uint32_t bpp = BCPhysicalBytesPerTexel(format);
    bool bc6h = format == WMTPixelFormatBC6H_RGBFloat || format == WMTPixelFormatBC6H_RGBUfloat;
    bool is_signed = format == WMTPixelFormatBC4_RSnorm || format == WMTPixelFormatBC5_RGSnorm;
    const void *current = texels;
    void *owned = nullptr;
    for (uint32_t q = uint32_t(level); q < dst->mipLevelCount() && dst->isLevelLost(q); q++) {
      uint32_t next_w = std::max(1u, width / 2), next_h = std::max(1u, height / 2);
      size_t next_row = size_t(next_w) * bpp;
      void *next = malloc(next_row * next_h);
      if (!next) {
        WARN_RARE("mip clamp: no memory to refill level ", q);
        break;
      }
      if (bc6h)
        BCDecodeToPhysical(format, nullptr, 0, next, next_row, next_w, next_h);
      else
        mipclamp::BoxFilter2x2(
            (const uint8_t *)current, size_t(width) * bpp, width, height, (uint8_t *)next, next_row, bpp, is_signed
        );
      UploadPhysicalTexels(dst, q, slice, {0, 0, 0}, next_w, next_h, next, next_row);
      dst->clearLevelLost(q);
      free(owned);
      owned = next;
      current = next;
      width = next_w;
      height = next_h;
    }
    free(owned);
  }

  /* Upload `width` x `height` x `depth` texels of a texture's physical format
     (tightly packed slices). */
  void
  UploadPhysicalTexels(
      Rc<Texture> dst, uint32_t level, uint32_t slice, WMTOrigin origin, uint32_t width, uint32_t height,
      const void *texels, size_t row_bytes, uint32_t depth = 1
  ) {
    size_t total_bytes = row_bytes * height * depth;
    auto [staging_buffer, offset] = AllocateStagingBuffer(total_bytes, 16);
    staging_buffer.updateContents(offset, texels, total_bytes);
    SwitchToBlitEncoder(CommandBufferState::UpdateBlitEncoderActive);
    EmitOP([staging_buffer, offset, dst = std::move(dst), level, slice, origin, width, height, depth,
            row_bytes](ArgumentEncodingContext &enc) {
      auto texture = enc.access(dst, level, slice, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
      auto &cmd_cptex = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
      cmd_cptex.type = WMTBlitCommandCopyFromBufferToTexture;
      cmd_cptex.src = staging_buffer;
      cmd_cptex.src_offset = offset;
      cmd_cptex.bytes_per_row = row_bytes;
      cmd_cptex.bytes_per_image = row_bytes * height;
      cmd_cptex.size = {width, height, depth};
      cmd_cptex.dst = texture;
      cmd_cptex.slice = slice;
      cmd_cptex.level = level;
      cmd_cptex.origin = origin;
    });
  }

  /**
  Spec 05 Part C: copies that involve a BC texture while BC is emulated.
  - staging (block bytes) -> BC texture: decoded on the CPU now, from the
    staging resource's CPU memory;
  - BC texture -> BC texture with the same physical format (sRGB aside): a
    plain texture copy, the physical texels are what the game expects;
  - staging -> staging: raw bytes, as upstream;
  - anything else would move block bytes into (or physical texels out as)
    block data, and is skipped.
  */
  void
  CopyTextureEmulatedBC(TextureCopyCommand &&cmd) {
    bool src_bc = cmd.SrcFormat.Flag & MTL_DXGI_FORMAT_BC;
    bool dst_bc = cmd.DstFormat.Flag & MTL_DXGI_FORMAT_BC;
    auto staging_src = GetStagingResource(cmd.pSrc, cmd.SrcSubresource);
    auto staging_dst = GetStagingResource(cmd.pDst, cmd.DstSubresource);
    if (src_bc && dst_bc) {
      if (staging_src && staging_dst)
        return CopyTextureBitcast(std::move(cmd));
      if (staging_src) {
        if (auto dst = GetTexture(cmd.pDst)) {
          return UploadEmulatedBCFromStaging(cmd, staging_src, std::move(dst), int32_t(cmd.Dst.MipLevel));
        }
      }
      if (!staging_src && !staging_dst) {
        auto src = GetTexture(cmd.pSrc);
        auto dst = GetTexture(cmd.pDst);
        if (src && dst &&
            Forget_sRGB(BCPhysicalFormat(src->pixelFormat())) == Forget_sRGB(BCPhysicalFormat(dst->pixelFormat()))) {
          if (cmd.DstOrigin.x == 0 && cmd.DstOrigin.y == 0 && cmd.SrcSize.width == cmd.Dst.Width &&
              cmd.SrcSize.height == cmd.Dst.Height)
            dst->clearLevelLost(cmd.Dst.MipLevel); // D5: written in full
          SwitchToBlitEncoder(CommandBufferState::BlitEncoderActive);
          EmitOP([dst_ = std::move(dst), src_ = std::move(src), cmd = std::move(cmd)](ArgumentEncodingContext &enc) {
            auto src = enc.access(src_, cmd.Src.MipLevel, cmd.Src.ArraySlice, DXMT_ENCODER_RESOURCE_ACESS_READ);
            auto dst = enc.access(dst_, cmd.Dst.MipLevel, cmd.Dst.ArraySlice, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
            auto &cmd_cptex = enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
            cmd_cptex.type = WMTBlitCommandCopyFromTextureToTexture;
            cmd_cptex.src = src;
            cmd_cptex.src_slice = cmd.Src.ArraySlice;
            cmd_cptex.src_level = cmd.Src.MipLevel;
            cmd_cptex.src_origin = cmd.SrcOrigin;
            cmd_cptex.src_size = cmd.SrcSize;
            cmd_cptex.dst = dst;
            cmd_cptex.dst_slice = cmd.Dst.ArraySlice;
            cmd_cptex.dst_level = cmd.Dst.MipLevel;
            cmd_cptex.dst_origin = cmd.DstOrigin;
          });
          return;
        }
      }
    }
    WARN_RARE(
        "BC emulation: copy between formats ", cmd.Src.Format, " and ", cmd.Dst.Format,
        " can't be done on uncompressed textures, skipped"
    );
  }

  /* Part C4 (+ D5): decode a staging resource's block data into `dst` at
     physical level `level` (-1: the dropped logical level above the clamp). */
  void
  UploadEmulatedBCFromStaging(TextureCopyCommand &cmd, Rc<StagingResource> &staging_src, Rc<Texture> &&dst, int32_t level) {
    uint32_t block_bytes = BCBlockBytes(dst->pixelFormat());
    size_t offset = size_t(cmd.SrcOrigin.z) * staging_src->bytesPerImage +
                    size_t(cmd.SrcOrigin.y >> 2) * staging_src->bytesPerRow + size_t(cmd.SrcOrigin.x >> 2) * block_bytes;
    const void *blocks = ptr_add(staging_src->mappedImmediateMemory(), offset);
    UploadEmulatedBC(
        std::move(dst), level, cmd.Dst.ArraySlice, cmd.DstOrigin, cmd.SrcSize.width, cmd.SrcSize.height,
        cmd.SrcSize.depth, cmd.Dst.Width, cmd.Dst.Height, blocks, staging_src->bytesPerRow, staging_src->bytesPerImage
    );
  }

  /**
  Spec 05 D5: copies name logical levels; translate them to the physical levels
  of mip-clamped textures. Returns false if the copy is dropped:
  - a write to a level above the destination's clamp is dropped (a full write
    from a staging resource to the level just above may refill physical
    level 0 if it is lost);
  - when the source's level doesn't exist physically (the source is clamped
    more), the destination level is marked lost.
  */
  bool
  TranslateClampedCopy(TextureCopyCommand &cmd) {
    auto dst_tex = GetTexture(cmd.pDst);
    auto src_tex = GetTexture(cmd.pSrc);
    uint32_t dst_bias = dst_tex ? dst_tex->mipBias() : 0;
    uint32_t src_bias = src_tex ? src_tex->mipBias() : 0;
    if (!dst_bias && !src_bias)
      return true;
    if (cmd.Dst.MipLevel < dst_bias) {
      if (cmd.Dst.MipLevel + 1 == dst_bias && (cmd.SrcFormat.Flag & MTL_DXGI_FORMAT_BC) &&
          IsEmulatedBCFormat(device->GetMTLDevice(), dst_tex->pixelFormat())) {
        if (auto staging_src = GetStagingResource(cmd.pSrc, cmd.SrcSubresource))
          UploadEmulatedBCFromStaging(cmd, staging_src, std::move(dst_tex), -1);
      }
      return false;
    }
    cmd.Dst.MipLevel -= dst_bias;
    if (cmd.Src.MipLevel < src_bias) {
      if (dst_tex)
        dst_tex->markLevelLost(cmd.Dst.MipLevel);
      WARN_RARE("mip clamp: copy source level isn't in the clamped source texture, destination level marked lost");
      return false;
    }
    cmd.Src.MipLevel -= src_bias;
    if (dst_tex && !(src_tex && BCEmulationActive(device->GetMTLDevice())) && cmd.DstOrigin.x == 0 &&
        cmd.DstOrigin.y == 0 && cmd.SrcSize.width == cmd.Dst.Width && cmd.SrcSize.height == cmd.Dst.Height)
      dst_tex->clearLevelLost(cmd.Dst.MipLevel); // written in full (GPU copy)
    return true;
  }
#endif

  void
  CopyTexture(TextureCopyCommand &&cmd) {
    if (cmd.Invalid)
      return;
#ifdef DXMT_IOS
    if (!TranslateClampedCopy(cmd))
      return;
    if (((cmd.SrcFormat.Flag | cmd.DstFormat.Flag) & MTL_DXGI_FORMAT_BC) && BCEmulationActive(device->GetMTLDevice()))
      return CopyTextureEmulatedBC(std::move(cmd));
#endif
    if ((cmd.SrcFormat.Flag & MTL_DXGI_FORMAT_BC) != (cmd.DstFormat.Flag & MTL_DXGI_FORMAT_BC)) {
      if (cmd.SrcFormat.Flag & MTL_DXGI_FORMAT_BC) {
        return CopyTextureFromCompressed(std::move(cmd));
      } else {
        return CopyTextureToCompressed(std::move(cmd));
      }
    }
    return CopyTextureBitcast(std::move(cmd));
  }

  void
  CopyTextureBitcast(TextureCopyCommand &&cmd) {
    if (auto staging_dst = GetStagingResource(cmd.pDst, cmd.DstSubresource)) {
      if (auto staging_src = GetStagingResource(cmd.pSrc, cmd.SrcSubresource)) {
        SwitchToBlitEncoder(CommandBufferState::BlitEncoderActive);
        UseCopyDestination(staging_dst);
        UseCopySource(staging_src);
        EmitOP([dst_ = std::move(staging_dst), src_ = std::move(staging_src),
                cmd = std::move(cmd)](ArgumentEncodingContext &enc) {
          auto [src, src_sub_offset] = enc.access(src_->buffer(), 0, src_->length, DXMT_ENCODER_RESOURCE_ACESS_READ);
          auto [dst, dst_sub_offset] = enc.access(dst_->buffer(), 0, dst_->length, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          if (cmd.SrcFormat.Flag & MTL_DXGI_FORMAT_BC) {
            ERR("copy between staging BC texture");
            return;
          }
          for (unsigned offset_z = 0; offset_z < cmd.SrcSize.depth; offset_z++) {
            for (unsigned offset_y = 0; offset_y < cmd.SrcSize.height; offset_y++) {
              auto src_offset = (cmd.SrcOrigin.z + offset_z) * src_->bytesPerImage +
                                (cmd.SrcOrigin.y + offset_y) * src_->bytesPerRow +
                                cmd.SrcOrigin.x * cmd.SrcFormat.BytesPerTexel;
              auto dst_offset = (cmd.DstOrigin.z + offset_z) * dst_->bytesPerImage +
                                (cmd.DstOrigin.y + offset_y) * dst_->bytesPerRow +
                                cmd.DstOrigin.x * cmd.DstFormat.BytesPerTexel;
              auto &cmdcp = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
              cmdcp.type = WMTBlitCommandCopyFromBufferToBuffer;
              cmdcp.copy_length = cmd.SrcSize.width * cmd.DstFormat.BytesPerTexel;
              cmdcp.src = src->buffer();
              cmdcp.src_offset = src_sub_offset + src_offset;
              cmdcp.dst = dst->buffer();
              cmdcp.dst_offset = dst_sub_offset + dst_offset;
            }
          }
        });
        promote_flush = true;
      } else if (auto src = GetTexture(cmd.pSrc)) {
        if (cmd.DstFormat.Flag & MTL_DXGI_FORMAT_EMULATED_LINEAR_DEPTH_STENCIL) {
          InvalidateCurrentPass(true);
          UseCopyDestination(staging_dst);
          EmitOP([src_ = std::move(src), dst_ = std::move(staging_dst),
                  cmd = std::move(cmd)](ArgumentEncodingContext &enc) {
            enc.blit_depth_stencil_cmd.copyFromTexture(
                src_, cmd.Src.MipLevel, cmd.Src.ArraySlice, dst_->buffer(), 0, dst_->length, dst_->bytesPerRow,
                dst_->bytesPerImage, cmd.DstFormat.Flag & MTL_DXGI_FORMAT_EMULATED_D24
            );
          });
          promote_flush = true;
          return;
        }
        SwitchToBlitEncoder(CommandBufferState::ReadbackBlitEncoderActive);
        UseCopyDestination(staging_dst);
        EmitOP([src_ = std::move(src), dst_ = std::move(staging_dst),
              cmd = std::move(cmd)](ArgumentEncodingContext &enc) {
          auto src = enc.access(src_, cmd.Src.MipLevel, cmd.Src.ArraySlice, DXMT_ENCODER_RESOURCE_ACESS_READ);
          auto [dst, dst_offset] = enc.access(dst_->buffer(), 0, dst_->length, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          auto offset = cmd.DstOrigin.z * dst_->bytesPerImage + cmd.DstOrigin.y * dst_->bytesPerRow +
                        cmd.DstOrigin.x * cmd.DstFormat.BytesPerTexel;
          auto &cmd_cpbuf = enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
          cmd_cpbuf.type = WMTBlitCommandCopyFromTextureToBuffer;
          cmd_cpbuf.src = src;
          cmd_cpbuf.slice = cmd.Src.ArraySlice;
          cmd_cpbuf.level = cmd.Src.MipLevel;
          cmd_cpbuf.origin = cmd.SrcOrigin;
          cmd_cpbuf.size = cmd.SrcSize;
          cmd_cpbuf.dst = dst->buffer();
          cmd_cpbuf.offset = offset + dst_offset;
          cmd_cpbuf.bytes_per_row = dst_->bytesPerRow;
          cmd_cpbuf.bytes_per_image = dst_->bytesPerImage;
          cmd_cpbuf.options = DepthStencilPlaneBlitOption(src_->pixelFormat(), cmd.SrcFormat.Flag);
        });
        promote_flush = true;
      } else {
        UNREACHABLE
      }
    } else if (auto dst = GetTexture(cmd.pDst)) {
      if (auto staging_src = GetStagingResource(cmd.pSrc, cmd.SrcSubresource)) {
        if (cmd.SrcFormat.Flag & MTL_DXGI_FORMAT_EMULATED_LINEAR_DEPTH_STENCIL) {
          InvalidateCurrentPass(true);
          UseCopySource(staging_src);
          EmitOP([dst_ = std::move(dst), src_ = std::move(staging_src),
                  cmd = std::move(cmd)](ArgumentEncodingContext &enc) {
            enc.blit_depth_stencil_cmd.copyFromBuffer(
                src_->buffer(), 0, src_->length, src_->bytesPerRow, src_->bytesPerImage, dst_, cmd.Dst.MipLevel,
                cmd.Dst.ArraySlice, cmd.SrcFormat.Flag & MTL_DXGI_FORMAT_EMULATED_D24
            );
          });
          return;
        }
        // copy from staging to default
        SwitchToBlitEncoder(CommandBufferState::UpdateBlitEncoderActive);
        UseCopySource(staging_src);
        EmitOP([dst_ = std::move(dst), src_ =std::move(staging_src),
              cmd = std::move(cmd)](ArgumentEncodingContext &enc) {
          auto [src, src_offset] = enc.access(src_->buffer(), 0, src_->length, DXMT_ENCODER_RESOURCE_ACESS_READ);
          auto dst = enc.access(dst_, cmd.Dst.MipLevel, cmd.Dst.ArraySlice, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          uint32_t offset;
          if (cmd.SrcFormat.Flag & MTL_DXGI_FORMAT_BC) {
            offset = cmd.SrcOrigin.z * src_->bytesPerImage + (cmd.SrcOrigin.y >> 2) * src_->bytesPerRow +
                     (cmd.SrcOrigin.x >> 2) * cmd.SrcFormat.BytesPerTexel;
          } else {
            offset = cmd.SrcOrigin.z * src_->bytesPerImage + cmd.SrcOrigin.y * src_->bytesPerRow +
                     cmd.SrcOrigin.x * cmd.SrcFormat.BytesPerTexel;
          }
          auto &cmd_cptex = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
          cmd_cptex.type = WMTBlitCommandCopyFromBufferToTexture;
          cmd_cptex.src = src->buffer();
          cmd_cptex.src_offset = offset + src_offset;
          cmd_cptex.bytes_per_row =  src_->bytesPerRow;
          cmd_cptex.bytes_per_image = src_->bytesPerImage;
          cmd_cptex.size = cmd.SrcSize;
          cmd_cptex.dst = dst;
          cmd_cptex.slice = cmd.Dst.ArraySlice;
          cmd_cptex.level = cmd.Dst.MipLevel;
          cmd_cptex.origin = cmd.DstOrigin;
          cmd_cptex.options = DepthStencilPlaneBlitOption(dst_->pixelFormat(), cmd.DstFormat.Flag);
        });
      } else if (auto src = GetTexture(cmd.pSrc)) {
        // on-device copy
        SwitchToBlitEncoder(CommandBufferState::BlitEncoderActive);
        EmitOP([dst_ = std::move(dst), src_ = std::move(src), cmd = std::move(cmd)](ArgumentEncodingContext &enc) {
          auto src_format = src_->pixelFormat();
          auto dst_format = dst_->pixelFormat();
          auto src = enc.access(src_, cmd.Src.MipLevel, cmd.Src.ArraySlice, DXMT_ENCODER_RESOURCE_ACESS_READ);
          auto dst = enc.access(dst_, cmd.Dst.MipLevel, cmd.Dst.ArraySlice, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          if (Forget_sRGB(dst_format) != Forget_sRGB(src_format)) {

            // bitcast, using a temporary buffer
            size_t bytes_per_row, bytes_per_image, bytes_total;
            if (cmd.SrcFormat.Flag & MTL_DXGI_FORMAT_BC) {
              bytes_per_row = (align(cmd.SrcSize.width, 4u) >> 2) * cmd.SrcFormat.BytesPerTexel;
              bytes_per_image = bytes_per_row * (align(cmd.SrcSize.height, 4u) >> 2);
            } else {
              bytes_per_row = cmd.SrcSize.width * cmd.SrcFormat.BytesPerTexel;
              bytes_per_image = bytes_per_row * cmd.SrcSize.height;
            }
            bytes_total = bytes_per_image * cmd.SrcSize.depth;

            auto [buffer, offset] = enc.allocateTempBuffer(bytes_total, 256);
            auto &cmd_cpbuf = enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
            cmd_cpbuf.type = WMTBlitCommandCopyFromTextureToBuffer;
            cmd_cpbuf.src = src;
            cmd_cpbuf.slice = cmd.Src.ArraySlice;
            cmd_cpbuf.level = cmd.Src.MipLevel;
            cmd_cpbuf.origin = cmd.SrcOrigin;
            cmd_cpbuf.size = cmd.SrcSize;
            cmd_cpbuf.dst = buffer;
            cmd_cpbuf.offset = offset;
            cmd_cpbuf.bytes_per_row = bytes_per_row;
            cmd_cpbuf.bytes_per_image = bytes_per_image;
  
            auto &cmd_cptex = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
            cmd_cptex.type = WMTBlitCommandCopyFromBufferToTexture;
            cmd_cptex.src = buffer;
            cmd_cptex.src_offset = offset;
            cmd_cptex.bytes_per_row = bytes_per_row;
            cmd_cptex.bytes_per_image = bytes_per_image;
            cmd_cptex.size = cmd.SrcSize;
            cmd_cptex.dst = dst;
            cmd_cptex.slice = cmd.Dst.ArraySlice;
            cmd_cptex.level = cmd.Dst.MipLevel;
            cmd_cptex.origin = cmd.DstOrigin;
            return;
          }
          auto &cmd_cptex = enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_texture>();
          cmd_cptex.type = WMTBlitCommandCopyFromTextureToTexture;
          cmd_cptex.src = src;
          cmd_cptex.src_slice = cmd.Src.ArraySlice;
          cmd_cptex.src_level = cmd.Src.MipLevel;
          cmd_cptex.src_origin = cmd.SrcOrigin;
          cmd_cptex.src_size = cmd.SrcSize;
          cmd_cptex.dst = dst;
          cmd_cptex.dst_slice = cmd.Dst.ArraySlice;
          cmd_cptex.dst_level = cmd.Dst.MipLevel;
          cmd_cptex.dst_origin = cmd.DstOrigin;
        });
      } else {
        UNREACHABLE
      }
    } else {
      UNREACHABLE
    }
  }

  void
  CopyTextureFromCompressed(TextureCopyCommand &&cmd) {
    if (auto staging_dst = GetStagingResource(cmd.pDst, cmd.DstSubresource)) {
      IMPLEMENT_ME
    } else if (auto dst = GetTexture(cmd.pDst)) {
      if (auto staging_src = GetStagingResource(cmd.pSrc, cmd.SrcSubresource)) {
        // copy from staging to default
        UNIMPLEMENTED("copy from compressed staging to default");
      } else if (auto src = GetTexture(cmd.pSrc)) {
        // on-device copy
        SwitchToBlitEncoder(CommandBufferState::BlitEncoderActive);
        EmitOP([dst_ = std::move(dst), src_ = std::move(src), cmd = std::move(cmd)](ArgumentEncodingContext &enc) {
          auto src = enc.access(src_, cmd.Src.MipLevel, cmd.Src.ArraySlice, DXMT_ENCODER_RESOURCE_ACESS_READ);
          auto dst = enc.access(dst_, cmd.Dst.MipLevel, cmd.Dst.ArraySlice, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          auto block_w = (align(cmd.SrcSize.width, 4u) >> 2);
          auto block_h = (align(cmd.SrcSize.height, 4u) >> 2);
          auto bytes_per_row = block_w * cmd.SrcFormat.BytesPerTexel;
          auto bytes_per_image = bytes_per_row * block_h;
          auto [buffer, offset] = enc.allocateTempBuffer(bytes_per_image * cmd.SrcSize.depth, 256);
          auto &cmd_cpbuf = enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
          cmd_cpbuf.type = WMTBlitCommandCopyFromTextureToBuffer;
          cmd_cpbuf.src = src;
          cmd_cpbuf.slice = cmd.Src.ArraySlice;
          cmd_cpbuf.level = cmd.Src.MipLevel;
          cmd_cpbuf.origin = cmd.SrcOrigin;
          cmd_cpbuf.size = cmd.SrcSize;
          cmd_cpbuf.dst = buffer;
          cmd_cpbuf.offset = offset;
          cmd_cpbuf.bytes_per_row = bytes_per_row;
          cmd_cpbuf.bytes_per_image = bytes_per_image;

          auto &cmd_cptex = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
          cmd_cptex.type = WMTBlitCommandCopyFromBufferToTexture;
          cmd_cptex.src = buffer;
          cmd_cptex.src_offset = offset;
          cmd_cptex.bytes_per_row = bytes_per_row;
          cmd_cptex.bytes_per_image = bytes_per_image;
          cmd_cptex.size = {block_w, block_h, cmd.SrcSize.depth};
          cmd_cptex.dst = dst;
          cmd_cptex.slice = cmd.Dst.ArraySlice;
          cmd_cptex.level = cmd.Dst.MipLevel;
          cmd_cptex.origin = cmd.DstOrigin;
        });
      } else {
        UNREACHABLE
      }
    } else {
      UNREACHABLE
    }
  }

  void
  CopyTextureToCompressed(TextureCopyCommand &&cmd) {
    if (auto staging_dst = GetStagingResource(cmd.pDst, cmd.DstSubresource)) {
      IMPLEMENT_ME
    } else if (auto dst = GetTexture(cmd.pDst)) {
      if (auto staging_src = GetStagingResource(cmd.pSrc, cmd.SrcSubresource)) {
        // copy from staging to default
        UNIMPLEMENTED("copy from staging to compressed default");
      } else if (auto src = GetTexture(cmd.pSrc)) {
        // on-device copy
        SwitchToBlitEncoder(CommandBufferState::BlitEncoderActive);
        EmitOP([dst_ = std::move(dst), src_ = std::move(src), cmd = std::move(cmd)](ArgumentEncodingContext &enc) {
          auto src = enc.access(src_, cmd.Src.MipLevel, cmd.Src.ArraySlice, DXMT_ENCODER_RESOURCE_ACESS_READ);
          auto dst = enc.access(dst_, cmd.Dst.MipLevel, cmd.Dst.ArraySlice, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          auto bytes_per_row = cmd.SrcSize.width * cmd.SrcFormat.BytesPerTexel;
          auto bytes_per_image = cmd.SrcSize.height * bytes_per_row;
          auto [buffer, offset] = enc.allocateTempBuffer(bytes_per_image * cmd.SrcSize.depth, 256);
          auto clamped_src_width = std::min(
              cmd.SrcSize.width << 2, std::max<uint32_t>(dst.width() >> cmd.Dst.MipLevel, 1u) - cmd.DstOrigin.x
          );
          auto clamped_src_height = std::min(
              cmd.SrcSize.height << 2, std::max<uint32_t>(dst.height() >> cmd.Dst.MipLevel, 1u) - cmd.DstOrigin.y
          );
          auto &cmd_cpbuf = enc.encodeBlitCommand<wmtcmd_blit_copy_from_texture_to_buffer>();
          cmd_cpbuf.type = WMTBlitCommandCopyFromTextureToBuffer;
          cmd_cpbuf.src = src;
          cmd_cpbuf.slice = cmd.Src.ArraySlice;
          cmd_cpbuf.level = cmd.Src.MipLevel;
          cmd_cpbuf.origin = cmd.SrcOrigin;
          cmd_cpbuf.size = cmd.SrcSize;
          cmd_cpbuf.dst = buffer;
          cmd_cpbuf.offset = offset;
          cmd_cpbuf.bytes_per_row = bytes_per_row;
          cmd_cpbuf.bytes_per_image = bytes_per_image;

          auto &cmd_cptex = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
          cmd_cptex.type = WMTBlitCommandCopyFromBufferToTexture;
          cmd_cptex.src = buffer;
          cmd_cptex.src_offset = offset;
          cmd_cptex.bytes_per_row = bytes_per_row;
          cmd_cptex.bytes_per_image = bytes_per_image;
          cmd_cptex.size = {clamped_src_width, clamped_src_height, cmd.SrcSize.depth};
          cmd_cptex.dst = dst;
          cmd_cptex.slice = cmd.Dst.ArraySlice;
          cmd_cptex.level = cmd.Dst.MipLevel;
          cmd_cptex.origin = cmd.DstOrigin;
        });
      } else {
        UNREACHABLE
      }
    } else {
      UNREACHABLE
    }
  }

  void
  UpdateTexture(
      TextureUpdateCommand &&cmd, const void *pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch, UINT CopyFlags
  ) {
    if (cmd.Invalid)
      return;

    std::lock_guard<mutex_t> lock(mutex);

#ifdef DXMT_IOS
    // Spec 05 D5: the subresource's level is logical; on a mip-clamped texture
    // the physical level is lower by the bias (negative: above the clamp).
    int32_t physical_level = int32_t(cmd.Dst.MipLevel);
    if (auto dst = GetTexture(cmd.pDst))
      physical_level -= int32_t(dst->mipBias());
    // Spec 05 C3: UpdateSubresource into an emulated BC texture is decoded here.
    // A STAGING destination keeps D3D's block rows (spec 05 clarification 10,
    // steamac's DXMT patch 0005): it goes on to the staging path below.
    if ((cmd.DstFormat.Flag & MTL_DXGI_FORMAT_BC) && BCEmulationActive(device->GetMTLDevice()) &&
        !GetStagingResource(cmd.pDst, cmd.DstSubresource)) {
      if (auto dst = GetTexture(cmd.pDst)) {
        UploadEmulatedBC(
            std::move(dst), physical_level, cmd.Dst.ArraySlice, cmd.DstOrigin, cmd.DstSize.width,
            cmd.DstSize.height, cmd.DstSize.depth, cmd.Dst.Width, cmd.Dst.Height, pSrcData, SrcRowPitch, SrcDepthPitch
        );
      }
      return;
    }
    if (physical_level < 0)
      return; // written level isn't in the clamped texture: dropped
    if (physical_level != int32_t(cmd.Dst.MipLevel)) {
      cmd.Dst.MipLevel = uint32_t(physical_level);
      if (auto dst = GetTexture(cmd.pDst)) {
        if (cmd.DstOrigin.x == 0 && cmd.DstOrigin.y == 0 && cmd.DstSize.width == cmd.Dst.Width &&
            cmd.DstSize.height == cmd.Dst.Height)
          dst->clearLevelLost(cmd.Dst.MipLevel);
      }
    }
#endif

    if (auto dst = GetTexture(cmd.pDst)) {
      auto bytes_per_depth_slice = cmd.EffectiveRows * cmd.EffectiveBytesPerRow;
      auto [staging_buffer, offset] = AllocateStagingBuffer(bytes_per_depth_slice * cmd.DstSize.depth, 16);
      if (cmd.EffectiveBytesPerRow == SrcRowPitch) {
        for (unsigned depthSlice = 0; depthSlice < cmd.DstSize.depth; depthSlice++) {
          auto dst_offset = offset + depthSlice * bytes_per_depth_slice;
          const char *src = ((const char *)pSrcData) + depthSlice * SrcDepthPitch;
          staging_buffer.updateContents(dst_offset, src, bytes_per_depth_slice);
        }
      } else {
        for (unsigned depthSlice = 0; depthSlice < cmd.DstSize.depth; depthSlice++) {
          for (unsigned row = 0; row < cmd.EffectiveRows; row++) {
            auto dst_offset = offset + row * cmd.EffectiveBytesPerRow + depthSlice * bytes_per_depth_slice;
            const char *src = ((const char *)pSrcData) + row * SrcRowPitch + depthSlice * SrcDepthPitch;
            staging_buffer.updateContents(dst_offset, src,  cmd.EffectiveBytesPerRow);
          }
        }
      }
      SwitchToBlitEncoder(CommandBufferState::UpdateBlitEncoderActive);
      EmitOP([staging_buffer, offset, dst = std::move(dst), cmd = std::move(cmd),
            bytes_per_depth_slice](ArgumentEncodingContext &enc) {
        auto texture = enc.access(dst, cmd.Dst.MipLevel, cmd.Dst.ArraySlice, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
        auto &cmd_cptex = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
        cmd_cptex.type = WMTBlitCommandCopyFromBufferToTexture;
        cmd_cptex.src = staging_buffer;
        cmd_cptex.src_offset = offset;
        cmd_cptex.bytes_per_row = cmd.EffectiveBytesPerRow;
        cmd_cptex.bytes_per_image = bytes_per_depth_slice;
        cmd_cptex.size = cmd.DstSize;
        cmd_cptex.dst = texture;
        cmd_cptex.slice = cmd.Dst.ArraySlice;
        cmd_cptex.level = cmd.Dst.MipLevel;
        cmd_cptex.origin = cmd.DstOrigin;
        cmd_cptex.options = DepthStencilPlaneBlitOption(dst->pixelFormat(), cmd.DstFormat.Flag);
      });
    } else if (auto staging_dst = GetStagingResource(cmd.pDst, cmd.DstSubresource)) {
      /* LGPL-2.1-or-later, from steamac's DXMT patch 0005 (osy): a STAGING texture is a
         buffer in D3D's own layout (block rows for BC, also under BC emulation): pack the
         rows, then blit each one to its place, ordered like any other copy into staging. */
      const uint32_t row_bytes = cmd.EffectiveBytesPerRow, rows = cmd.EffectiveRows, depth = cmd.DstSize.depth;
      const bool bc = cmd.DstFormat.Flag & MTL_DXGI_FORMAT_BC;
      const uint32_t dst_x = (bc ? cmd.DstOrigin.x >> 2 : cmd.DstOrigin.x) * cmd.DstFormat.BytesPerTexel;
      const uint32_t dst_y = bc ? cmd.DstOrigin.y >> 2 : cmd.DstOrigin.y;
      auto [upload, upload_offset] = AllocateStagingBuffer(row_bytes * rows * depth, 16);
      for (uint32_t z = 0; z < depth; z++)
        for (uint32_t row = 0; row < rows; row++)
          upload.updateContents(
              upload_offset + (z * rows + row) * row_bytes,
              (const char *)pSrcData + z * SrcDepthPitch + row * SrcRowPitch, row_bytes
          );
      SwitchToBlitEncoder(CommandBufferState::UpdateBlitEncoderActive);
      UseCopyDestination(staging_dst);
      EmitOP([upload, upload_offset, dst_ = std::move(staging_dst), row_bytes, rows, depth, dst_x, dst_y,
              dst_z = cmd.DstOrigin.z](ArgumentEncodingContext &enc) {
        auto [dst, dst_sub_offset] = enc.access(dst_->buffer(), 0, dst_->length, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
        for (uint32_t z = 0; z < depth; z++) {
          for (uint32_t row = 0; row < rows; row++) {
            auto &cp = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_buffer>();
            cp.type = WMTBlitCommandCopyFromBufferToBuffer;
            cp.copy_length = row_bytes;
            cp.src = upload;
            cp.src_offset = upload_offset + (z * rows + row) * row_bytes;
            cp.dst = dst->buffer();
            cp.dst_offset =
                dst_sub_offset + (dst_z + z) * dst_->bytesPerImage + (dst_y + row) * dst_->bytesPerRow + dst_x;
          }
        }
      });
      promote_flush = true;
      /* end of LGPL block */
    } else {
      UNIMPLEMENTED("unknown texture");
    }
  }

  void
  UpdateTexture(
      TextureUpdateCommand &&cmd, Rc<Buffer> &&src, UINT SrcRowPitch, UINT SrcDepthPitch
  ) {
    if (cmd.Invalid)
      return;

    std::lock_guard<mutex_t> lock(mutex);

#ifdef DXMT_IOS
    // Spec 05 Part C: a dynamic BC texture's buffer holds block bytes; decode
    // them from its CPU memory instead of copying them into the texture.
    if ((cmd.DstFormat.Flag & MTL_DXGI_FORMAT_BC) && BCEmulationActive(device->GetMTLDevice())) {
      if (auto dst = GetTexture(cmd.pDst)) {
        auto allocation = src->current();
        const void *blocks = ptr_add(allocation->mappedMemory(0), allocation->currentSuballocationOffset());
        // dynamic textures are never mip-clamped
        UploadEmulatedBC(
            std::move(dst), int32_t(cmd.Dst.MipLevel), cmd.Dst.ArraySlice, cmd.DstOrigin, cmd.DstSize.width,
            cmd.DstSize.height, cmd.DstSize.depth, cmd.Dst.Width, cmd.Dst.Height, blocks, SrcRowPitch, SrcDepthPitch
        );
      }
      return;
    }
#endif

    if (auto dst = GetTexture(cmd.pDst)) {

      SwitchToBlitEncoder(CommandBufferState::UpdateBlitEncoderActive);
      EmitOP([=, src = std::move(src), dst = std::move(dst), cmd = std::move(cmd)](ArgumentEncodingContext &enc) {
        auto [src_buffer, src_offset] = enc.access(src, 0, src->length(), DXMT_ENCODER_RESOURCE_ACESS_READ);
        auto texture = enc.access(dst, cmd.Dst.MipLevel, cmd.Dst.ArraySlice, DXMT_ENCODER_RESOURCE_ACESS_WRITE);
        auto &cmd_cptex = enc.encodeBlitCommand<wmtcmd_blit_copy_from_buffer_to_texture>();
        cmd_cptex.type = WMTBlitCommandCopyFromBufferToTexture;
        cmd_cptex.src = src_buffer->buffer();
        cmd_cptex.src_offset = src_offset;
        cmd_cptex.bytes_per_row = SrcRowPitch;
        cmd_cptex.bytes_per_image = SrcDepthPitch;
        cmd_cptex.size = cmd.DstSize;
        cmd_cptex.dst = texture;
        cmd_cptex.slice = cmd.Dst.ArraySlice;
        cmd_cptex.level = cmd.Dst.MipLevel;
        cmd_cptex.origin = cmd.DstOrigin;
        cmd_cptex.options = DepthStencilPlaneBlitOption(dst->pixelFormat(), cmd.DstFormat.Flag);
      });
    }
  }

#pragma endregion

#pragma region CommandEncoder Maintain State

  bool promote_flush = false;

  /**
  Render pass can be invalidated by reasons:
  - render target changes (including depth stencil)
  All pass can be invalidated by reasons:
  - a encoder type switch
  - flush/present
  Return value indicates if a commit happens
  */
  bool
  InvalidateCurrentPass(bool defer_commit = false) {
    // spec 05 clarification 69: a keep ends with its pass (a pass, if one is open: this is also
    // called while idle as a pass starts; item 77)
    {
      KeepDsvTracker::Note note;
      if (keep_dsv_.passEnding(cmdbuf_state != CommandBufferState::Idle, note))
        EmitKeepDsvNote(note);
    }
    NotePassEnded(); // spec 05 clarification 63
    switch (cmdbuf_state) {
    case CommandBufferState::Idle:
      break;
    case CommandBufferState::RenderEncoderActive:
    case CommandBufferState::RenderPipelineReady:
    case CommandBufferState::TessellationRenderPipelineReady:
    case CommandBufferState::GeometryRenderPipelineReady: {
      // EmitCommand([](CommandChunk::context &ctx) {
      //   ctx.render_encoder->endEncoding();
      //   ctx.render_encoder = nullptr;
      //   ctx.dsv_planar_flags = 0;
      // });
      // vro_state.endEncoder();
      EmitST([](ArgumentEncodingContext& enc) {
        enc.endPass();
      });
      allocated_encoder_argbuf_size_ = nullptr;
      break;
    }
    case CommandBufferState::ComputeEncoderActive:
    case CommandBufferState::ComputePipelineReady:
      EmitST([](ArgumentEncodingContext& enc) {
        enc.endPass();
      });
      allocated_encoder_argbuf_size_ = nullptr;
      break;
    case CommandBufferState::UpdateBlitEncoderActive:
    case CommandBufferState::ReadbackBlitEncoderActive:
    case CommandBufferState::BlitEncoderActive:
      EmitST([](ArgumentEncodingContext& enc) {
        enc.endPass();
      });
      break;
    }

    cmdbuf_state = CommandBufferState::Idle;
    if (promote_flush && !defer_commit) {
      Commit();
      return true;
    }

    return false;
  }

  /**
  Render pipeline can be invalidate by reasons:
  - shader program changes
  - input layout changes (if using metal vertex descriptor)
  - blend state changes
  - tessellation state changes (not implemented yet)
  - (when layered rendering) input primitive topology changes
  -
  */
  void
  InvalidateRenderPipeline() {
    if (cmdbuf_state != CommandBufferState::RenderPipelineReady &&
        cmdbuf_state != CommandBufferState::TessellationRenderPipelineReady&&
        cmdbuf_state != CommandBufferState::GeometryRenderPipelineReady)
      return;
    previous_render_pipeline_state = cmdbuf_state;
    cmdbuf_state = CommandBufferState::RenderEncoderActive;
  }

  /**
  Compute pipeline can be invalidate by reasons:
  - shader program changes
  TOOD: maybe we don't need this, since compute shader can be pre-compiled
  */
  void
  InvalidateComputePipeline() {
    if (cmdbuf_state != CommandBufferState::ComputePipelineReady)
      return;
    cmdbuf_state = CommandBufferState::ComputeEncoderActive;
  }

  void
  ClearRenderTargetView(D3D11RenderTargetView *pRenderTargetView, const FLOAT ColorRGBA[4]) {
    InvalidateCurrentPass();
    auto clear_color = WMTClearColor{ColorRGBA[0], ColorRGBA[1], ColorRGBA[2], ColorRGBA[3]};
    auto &props = pRenderTargetView->description();

    EmitOP([texture = pRenderTargetView->texture(), view = pRenderTargetView->viewId(),
          clear_color = std::move(clear_color), array_length = props.RenderTargetArrayLength](ArgumentEncodingContext &enc) mutable {
      enc.clearColor(forward_rc(texture), view, array_length, clear_color);
    });
  }

  void
  ClearDepthStencilView(D3D11DepthStencilView *pDepthStencilView, UINT ClearFlags, FLOAT Depth, UINT8 Stencil) {
    if (ClearFlags == 0)
      return;
    InvalidateCurrentPass();
    auto &props = pDepthStencilView->description();

    EmitOP([texture = pDepthStencilView->texture(), view = pDepthStencilView->viewId(),
          renamable = pDepthStencilView->renamable(), array_length = props.RenderTargetArrayLength,
          ClearFlags = ClearFlags & 0b11, Depth, Stencil](ArgumentEncodingContext &enc) mutable {
      if (renamable.ptr() && ClearFlags == DepthStencilPlanarFlags(texture->pixelFormat())) {
        texture->rename(renamable->getNext(enc.currentSeqId()));
      }
      enc.clearDepthStencil(forward_rc(texture), view, array_length, ClearFlags, Depth, Stencil);
    });
  }

  /**
  Switch to render encoder and set all states (expect for pipeline state)
  */
  bool
  SwitchToRenderEncoder() {
    if (cmdbuf_state == CommandBufferState::RenderPipelineReady)
      return true;
    if (cmdbuf_state == CommandBufferState::TessellationRenderPipelineReady)
      return true;
    if (cmdbuf_state == CommandBufferState::GeometryRenderPipelineReady)
      return true;
    if (cmdbuf_state == CommandBufferState::RenderEncoderActive)
      return true;
    InvalidateCurrentPass();

    // set dirty state
    state_.ShaderStages[PipelineStage::Vertex].ConstantBuffers.set_dirty();
    state_.ShaderStages[PipelineStage::Vertex].Samplers.set_dirty();
    state_.ShaderStages[PipelineStage::Vertex].SRVs.set_dirty();
    state_.ShaderStages[PipelineStage::Pixel].ConstantBuffers.set_dirty();
    state_.ShaderStages[PipelineStage::Pixel].Samplers.set_dirty();
    state_.ShaderStages[PipelineStage::Pixel].SRVs.set_dirty();
    state_.ShaderStages[PipelineStage::Hull].ConstantBuffers.set_dirty();
    state_.ShaderStages[PipelineStage::Hull].Samplers.set_dirty();
    state_.ShaderStages[PipelineStage::Hull].SRVs.set_dirty();
    state_.ShaderStages[PipelineStage::Domain].ConstantBuffers.set_dirty();
    state_.ShaderStages[PipelineStage::Domain].Samplers.set_dirty();
    state_.ShaderStages[PipelineStage::Domain].SRVs.set_dirty();
    state_.ShaderStages[PipelineStage::Geometry].ConstantBuffers.set_dirty();
    state_.ShaderStages[PipelineStage::Geometry].Samplers.set_dirty();
    state_.ShaderStages[PipelineStage::Geometry].SRVs.set_dirty();
    state_.InputAssembler.VertexBuffers.set_dirty();
    state_.OutputMerger.UAVs.set_dirty();
    dirty_state.set(
        DirtyState::BlendFactorAndStencilRef, DirtyState::RasterizerState, DirtyState::DepthStencilState,
        DirtyState::Viewport, DirtyState::Scissors
    );

    // should assume: render target is properly set
    {
      /* Setup RenderCommandEncoder */
      struct RENDER_TARGET_STATE {
        Rc<Texture> Texture;
        unsigned viewId;
        UINT RenderTargetIndex;
        UINT DepthPlane;
        WMTPixelFormat PixelFormat = WMTPixelFormatInvalid;
        WMTLoadAction LoadAction{WMTLoadActionLoad};
      };

      uint32_t effective_render_target = 0;
      // FIXME: is this value always valid?
      uint32_t render_target_array = state_.OutputMerger.ArrayLength;
      auto rtvs = AllocateCommandData<RENDER_TARGET_STATE>(state_.OutputMerger.NumRTVs);
      for (unsigned i = 0; i < state_.OutputMerger.NumRTVs; i++) {
        auto &rtv = state_.OutputMerger.RTVs[i];
        if (rtv) {
          auto props = rtv->description();
          rtvs[i] = {rtv->texture(), rtv->viewId(), i, props.DepthPlane, rtv->pixelFormat()};
          D3D11_ASSERT(rtv->pixelFormat() != WMTPixelFormatInvalid);
          effective_render_target++;
        } else {
          rtvs[i].RenderTargetIndex = i;
        }
      }
      struct DEPTH_STENCIL_STATE {
        Rc<Texture> Texture{};
        unsigned viewId{};
        WMTPixelFormat PixelFormat = WMTPixelFormatInvalid;
        WMTLoadAction DepthLoadAction{WMTLoadActionLoad};
        WMTLoadAction StencilLoadAction{WMTLoadActionLoad};
        unsigned ReadOnlyFlags{};
      };
      // auto &dsv = state_.OutputMerger.DSV;
      DEPTH_STENCIL_STATE dsv_info;
      uint32_t render_target_width = state_.OutputMerger.RenderTargetWidth;
      uint32_t render_target_height = state_.OutputMerger.RenderTargetHeight;
      bool uav_only = false;
      uint32_t uav_only_sample_count = 0;
      if (state_.OutputMerger.DSV) {
        dsv_info.Texture = state_.OutputMerger.DSV->texture();
        dsv_info.viewId = state_.OutputMerger.DSV->viewId();
        dsv_info.PixelFormat = state_.OutputMerger.DSV->pixelFormat();
        dsv_info.ReadOnlyFlags =  state_.OutputMerger.DSV->readonlyFlags();
      } else if (effective_render_target == 0) {
        if (!state_.OutputMerger.UAVs.any_bound()) {
          DEBUG("No rendering attachment or uav is bounded");
          return false;
        }
        D3D11_ASSERT(state_.Rasterizer.NumViewports);
        IMTLD3D11RasterizerState *state =
            state_.Rasterizer.RasterizerState ? state_.Rasterizer.RasterizerState : default_rasterizer_state;
        auto &viewport = state_.Rasterizer.viewports[0];
        render_target_width = viewport.Width;
        render_target_height = viewport.Height;
        uav_only_sample_count = state->UAVOnlySampleCount();
        if (!(render_target_width && render_target_height)) {
          ERR("uav only rendering is enabled but viewport is empty");
          return false;
        }
        uav_only = true;
      }

      NotePassStarted(); // spec 05 clarification 63
      // spec 05 clarification 69: what this pass attaches, for a later unbinding of its view
      pass_targets_ = {};
      pass_targets_.num_rtvs = state_.OutputMerger.NumRTVs;
      for (unsigned i = 0; i < state_.OutputMerger.NumRTVs && i < 8; i++)
        pass_targets_.rtvs[i] = state_.OutputMerger.RTVs[i].ptr();
      pass_targets_.dsv = state_.OutputMerger.DSV.ptr();
      pass_targets_.array_length = state_.OutputMerger.ArrayLength; // item 77: the layout
      pass_targets_.sample_count = state_.OutputMerger.SampleCount;
      pass_targets_.width = state_.OutputMerger.RenderTargetWidth;
      pass_targets_.height = state_.OutputMerger.RenderTargetHeight;
      keep_dsv_.passStarted(pass_targets_); // a would-keep's pass begins
      auto allocated_encoder_argbuf_size = std::make_unique<uint64_t>(0);
      allocated_encoder_argbuf_size_ = allocated_encoder_argbuf_size.get();

      EmitST([rtvs = std::move(rtvs), dsv = std::move(dsv_info), effective_render_target, uav_only,
            render_target_height, render_target_width, uav_only_sample_count,
            render_target_array, encoder_argbuf_size = std::move(allocated_encoder_argbuf_size)](ArgumentEncodingContext &ctx) {
        auto pool = WMT::MakeAutoreleasePool();
        uint32_t dsv_planar_flags = DepthStencilPlanarFlags(dsv.PixelFormat);
        auto& info = *ctx.startRenderPass(dsv_planar_flags, dsv.ReadOnlyFlags, rtvs.size(), *encoder_argbuf_size.get());

        for (auto &rtv : rtvs.span()) {
          if (rtv.PixelFormat == WMTPixelFormatInvalid) {
            continue;
          }
          auto &color = info.colors[rtv.RenderTargetIndex];
          // spec 05 clarification 71: a discarded subresource isn't loaded (before the access,
          // which counts as a write and takes the mark away)
          int discarded = rtv.LoadAction == WMTLoadActionLoad
                              ? ctx.discardedLoad(rtv.Texture.ptr(), rtv.Texture->firstMiplevel(rtv.viewId),
                                                  rtv.Texture->firstArraySlice(rtv.viewId))
                              : 0;
          color.attachment = ctx.access(rtv.Texture, rtv.viewId, DXMT_ENCODER_RESOURCE_ACESS_READWRITE);
          color.depth_plane = rtv.DepthPlane;
          color.load_action = discarded == 1 ? WMTLoadActionDontCare : rtv.LoadAction;
          color.store_action = WMTStoreActionStore;
          ctx.noteDiscardLoad(info, rtv.RenderTargetIndex, color.attachment, 0, discarded);
        };

        if (dsv.Texture.ptr()) {
          auto access_flag = ((dsv.ReadOnlyFlags & dsv_planar_flags) == dsv_planar_flags)
                                 ? DXMT_ENCODER_RESOURCE_ACESS_READ
                                 : DXMT_ENCODER_RESOURCE_ACESS_READWRITE;
          // spec 05 clarification 71: the discard marks, checked once for both planes
          int discarded = dsv.DepthLoadAction == WMTLoadActionLoad || dsv.StencilLoadAction == WMTLoadActionLoad
                              ? ctx.discardedLoad(dsv.Texture.ptr(), dsv.Texture->firstMiplevel(dsv.viewId),
                                                  dsv.Texture->firstArraySlice(dsv.viewId))
                              : 0;
          if (dsv_planar_flags & 1) {
            auto &depth = info.depth;
            depth.attachment = ctx.access(dsv.Texture, dsv.viewId, access_flag);
            int d = dsv.DepthLoadAction == WMTLoadActionLoad ? discarded : 0;
            depth.load_action = d == 1 ? WMTLoadActionDontCare : dsv.DepthLoadAction;
            depth.store_action = WMTStoreActionStore;
            ctx.noteDiscardLoad(info, 8, depth.attachment, 1, d);
          }

          if (dsv_planar_flags & 2) {
            auto &stencil = info.stencil;
            stencil.attachment = ctx.access(dsv.Texture, dsv.viewId, access_flag);
            int d = dsv.StencilLoadAction == WMTLoadActionLoad ? discarded : 0;
            stencil.load_action = d == 1 ? WMTLoadActionDontCare : dsv.StencilLoadAction;
            stencil.store_action = WMTStoreActionStore;
            ctx.noteDiscardLoad(info, 9, stencil.attachment, 2, d);
          }
        }
        if (effective_render_target == 0) {
          if (uav_only) {
            info.default_raster_sample_count = uav_only_sample_count;
          }
        }

        info.render_target_height = render_target_height;
        info.render_target_width = render_target_width;
        info.render_target_array_length = render_target_array;
      });
    }

    cmdbuf_state = CommandBufferState::RenderEncoderActive;
    previous_render_pipeline_state = cmdbuf_state;
    return true;
  }

  /**
  Switch to blit encoder
  */
  void
  SwitchToBlitEncoder(CommandBufferState BlitKind) {
    if (cmdbuf_state == BlitKind)
      return;

    /**
    Don't flush if it's a readback/upload blit pass
     */
    InvalidateCurrentPass(BlitKind != CommandBufferState::BlitEncoderActive);

    NotePassStarted(); // spec 05 clarification 63
    EmitST([](ArgumentEncodingContext &enc) { enc.startBlitPass(); });

    cmdbuf_state = BlitKind;
  }

  /**
  Switch to compute encoder and set all states (expect for pipeline state)
  */
  void
  SwitchToComputeEncoder() {
    if (cmdbuf_state == CommandBufferState::ComputePipelineReady)
      return;
    if (cmdbuf_state == CommandBufferState::ComputeEncoderActive)
      return;
    InvalidateCurrentPass();

    // set dirty state
    state_.ShaderStages[PipelineStage::Compute].ConstantBuffers.set_dirty();
    state_.ShaderStages[PipelineStage::Compute].Samplers.set_dirty();
    state_.ShaderStages[PipelineStage::Compute].SRVs.set_dirty();
    state_.ComputeStageUAV.UAVs.set_dirty();

    auto allocated_encoder_argbuf_size = std::make_unique<uint64_t>(0);
    allocated_encoder_argbuf_size_ = allocated_encoder_argbuf_size.get();

    NotePassStarted(); // spec 05 clarification 63
    EmitST([encoder_argbuf_size = std::move(allocated_encoder_argbuf_size)](
               ArgumentEncodingContext &enc) {
      enc.startComputePass(*encoder_argbuf_size.get());
    });

    cmdbuf_state = CommandBufferState::ComputeEncoderActive;
  }

  template <PipelineStage Type>
  ManagedShader
  GetManagedShader() {
    auto ptr = state_.ShaderStages[Type].Shader.ptr();
    return ptr ? ptr->GetManagedShader() : nullptr;
  };

  template <bool IndexedDraw>
  void
  InitializeGraphicsPipelineDesc(MTL_GRAPHICS_PIPELINE_DESC &Desc) {
    // TODO: reduce branching
    auto PS = GetManagedShader<PipelineStage::Pixel>();
    auto GS = GetManagedShader<PipelineStage::Geometry>();
    Desc.VertexShader = GetManagedShader<PipelineStage::Vertex>();
    Desc.PixelShader = PS;
    // FIXME: ensure valid state: hull and domain shader none or both are bound
    Desc.HullShader = GetManagedShader<PipelineStage::Hull>();
    Desc.DomainShader = GetManagedShader<PipelineStage::Domain>();
    Desc.GeometryShader = GetManagedShader<PipelineStage::Geometry>();
    if (state_.InputAssembler.InputLayout) {
      Desc.InputLayout = state_.InputAssembler.InputLayout->GetManagedInputLayout();
    } else {
      Desc.InputLayout = nullptr;
    }
    auto so_layout = com_cast<IMTLD3D11StreamOutputLayout>(state_.ShaderStages[PipelineStage::Geometry].Shader.ptr());
    if (unlikely(so_layout)) {
      Desc.SOLayout = so_layout.ptr();
    } else {
      Desc.SOLayout = nullptr;
    }
    Desc.NumColorAttachments = state_.OutputMerger.NumRTVs;
    for (unsigned i = 0; i < ARRAYSIZE(state_.OutputMerger.RTVs); i++) {
      auto &rtv = state_.OutputMerger.RTVs[i];
      if (rtv && i < Desc.NumColorAttachments) {
        Desc.ColorAttachmentFormats[i] = state_.OutputMerger.RTVs[i]->pixelFormat();
      } else {
        Desc.ColorAttachmentFormats[i] = WMTPixelFormatInvalid;
      }
    }
    Desc.BlendState = state_.OutputMerger.BlendState ? state_.OutputMerger.BlendState : default_blend_state;
    // spec 05 clarification 69: the attached view's format, the kept one while it is kept
    Desc.DepthStencilFormat = AttachedDsv() ? AttachedDsv()->pixelFormat() : WMTPixelFormatInvalid;
    Desc.TopologyClass = to_metal_primitive_topology(state_.InputAssembler.Topology);
    bool ds_enabled =
        (state_.OutputMerger.DepthStencilState ? state_.OutputMerger.DepthStencilState : default_depth_stencil_state)
            ->IsEnabled();
    // FIXME: corner case: DSV is not bound or missing planar
    Desc.RasterizationEnabled =
        (PS || ds_enabled) && (!Desc.SOLayout || Desc.SOLayout->RasterizedStream() != D3D11_SO_NO_RASTERIZED_STREAM);
    if (!Desc.RasterizationEnabled)
      Desc.PixelShader = nullptr; // Even rasterization is disabled, Metal still checks if VS-PS signatures match.
    Desc.SampleMask = state_.OutputMerger.SampleMask;
    Desc.GSPassthrough = GS ? GS->reflection().GeometryShader.GSPassThrough : ~0u;
    if (unlikely(Desc.GSPassthrough == ~0u && Desc.GeometryShader != nullptr)) {
      Desc.GSStripTopology = is_strip_topology(state_.InputAssembler.Topology);
    } else {
      Desc.GSStripTopology = false;
    }
    if constexpr (IndexedDraw) {
      Desc.IndexBufferFormat = state_.InputAssembler.IndexBufferFormat == DXGI_FORMAT_R32_UINT
                                   ? SM50_INDEX_BUFFER_FORMAT_UINT32
                                   : SM50_INDEX_BUFFER_FORMAT_UINT16;
    } else {
      Desc.IndexBufferFormat = SM50_INDEX_BUFFER_FORMAT_NONE;
    }
    Desc.SampleCount = state_.OutputMerger.SampleCount;
  }

  template <bool IndexedDraw>
  DrawCallStatus
  FinalizeTessellationRenderPipeline() {
    if (cmdbuf_state == CommandBufferState::TessellationRenderPipelineReady)
      return DrawCallStatus::Tessellation;
    auto HS = GetManagedShader<PipelineStage::Hull>();
    if (!HS) {
      return DrawCallStatus::Invalid;
    }
    switch (HS->reflection().Tessellator.OutputPrimitive) {
    case MTL_TESSELLATOR_OUTPUT_LINE:
      EmitST([](ArgumentEncodingContext &enc) {
        enc.setCompatibilityFlag(FeatureCompatibility::UnsupportedTessellationOutputPrimitive);
      });
      return DrawCallStatus::Invalid;
    case MTL_TESSELLATOR_OUTPUT_POINT:
      EmitST([](ArgumentEncodingContext &enc) {
        enc.setCompatibilityFlag(FeatureCompatibility::UnsupportedTessellationOutputPrimitive);
      });
      return DrawCallStatus::Invalid;
    default:
      break;
    }
    if (!state_.ShaderStages[PipelineStage::Domain].Shader) {
      return DrawCallStatus::Invalid;
    }
    auto GS = GetManagedShader<PipelineStage::Geometry>();
    if (GS && GS->reflection().GeometryShader.GSPassThrough == ~0u) {
      EmitST([](ArgumentEncodingContext &enc) {
        enc.setCompatibilityFlag(FeatureCompatibility::UnsupportedGeometryTessellationDraw);
      });
      return DrawCallStatus::Invalid;
    }

    if (!SwitchToRenderEncoder()) {
      return DrawCallStatus::Invalid;
    }

    MTLCompiledTessellationMeshPipeline *pipeline;

    MTL_GRAPHICS_PIPELINE_DESC pipelineDesc;
    InitializeGraphicsPipelineDesc<IndexedDraw>(pipelineDesc);

    D11_WORK(work_.pipeline_lookups++); // spec 05 clarification 63
    if (FAILED(device->CreateTessellationMeshPipeline(&pipelineDesc, &pipeline))) {
      return DrawCallStatus::Invalid;
    }

    EmitST([pso = std::move(pipeline)](ArgumentEncodingContext &enc) {
      auto render_encoder = enc.currentRenderEncoder();
      render_encoder->use_tessellation = 1;
      MTL_COMPILED_TESSELLATION_MESH_PIPELINE GraphicsPipeline{};
      pso->GetPipeline(&GraphicsPipeline); // may block
      enc.tess_num_output_control_point_element = GraphicsPipeline.NumControlPointOutputElement;
      enc.tess_threads_per_patch = GraphicsPipeline.ThreadsPerPatch;
      /* LGPL-2.1-or-later, from steamac's DXMT patch 0007 (Holly Hamilton): a null
         pipeline (compilation failed) is encoded anyway; it tells winemetal to drop the
         draws queued for it rather than run them with whatever pipeline the encoder had
         bound before. */
      auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setpso>();
      cmd.type = WMTRenderCommandSetPSO;
      cmd.pso = GraphicsPipeline.PipelineState;
    });

    cmdbuf_state = CommandBufferState::TessellationRenderPipelineReady;

    if (previous_render_pipeline_state != CommandBufferState::TessellationRenderPipelineReady) {
      state_.InputAssembler.VertexBuffers.set_dirty();
      state_.ShaderStages[PipelineStage::Vertex].ConstantBuffers.set_dirty();
      state_.ShaderStages[PipelineStage::Vertex].SRVs.set_dirty();
      state_.ShaderStages[PipelineStage::Vertex].Samplers.set_dirty();
      state_.ShaderStages[PipelineStage::Hull].ConstantBuffers.set_dirty();
      state_.ShaderStages[PipelineStage::Hull].SRVs.set_dirty();
      state_.ShaderStages[PipelineStage::Hull].Samplers.set_dirty();
      state_.ShaderStages[PipelineStage::Domain].ConstantBuffers.set_dirty();
      state_.ShaderStages[PipelineStage::Domain].SRVs.set_dirty();
      state_.ShaderStages[PipelineStage::Domain].Samplers.set_dirty();
      previous_render_pipeline_state = CommandBufferState::TessellationRenderPipelineReady;
    }

    return DrawCallStatus::Tessellation;
  }

  template <bool IndexedDraw>
  DrawCallStatus
  FinalizeGeometryRenderPipeline() {
    if (cmdbuf_state == CommandBufferState::GeometryRenderPipelineReady)
      return DrawCallStatus::Geometry;
    if (!SwitchToRenderEncoder()) {
      return DrawCallStatus::Invalid;
    }

    MTLCompiledGeometryPipeline *pipeline;

    MTL_GRAPHICS_PIPELINE_DESC pipelineDesc;
    InitializeGraphicsPipelineDesc<IndexedDraw>(pipelineDesc);
    D11_WORK(work_.pipeline_lookups++); // spec 05 clarification 63
    device->CreateGeometryPipeline(&pipelineDesc, &pipeline);
    EmitST([pso = std::move(pipeline)](ArgumentEncodingContext& enc) {
      auto render_encoder = enc.currentRenderEncoder();
      render_encoder->use_geometry = 1;
      MTL_COMPILED_GRAPHICS_PIPELINE GraphicsPipeline{};
      pso->GetPipeline(&GraphicsPipeline); // may block
      /* steamac's DXMT patch 0007 (LGPL, Holly Hamilton): a null pipeline is encoded too */
      auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setpso>();
      cmd.type = WMTRenderCommandSetPSO;
      cmd.pso = GraphicsPipeline.PipelineState;
    });

    cmdbuf_state = CommandBufferState::GeometryRenderPipelineReady;

    if (previous_render_pipeline_state != CommandBufferState::GeometryRenderPipelineReady) {
      state_.InputAssembler.VertexBuffers.set_dirty();
      state_.ShaderStages[PipelineStage::Vertex].ConstantBuffers.set_dirty();
      state_.ShaderStages[PipelineStage::Vertex].SRVs.set_dirty();
      state_.ShaderStages[PipelineStage::Vertex].Samplers.set_dirty();
      state_.ShaderStages[PipelineStage::Geometry].ConstantBuffers.set_dirty();
      state_.ShaderStages[PipelineStage::Geometry].SRVs.set_dirty();
      state_.ShaderStages[PipelineStage::Geometry].Samplers.set_dirty();
      previous_render_pipeline_state = CommandBufferState::GeometryRenderPipelineReady;
    }

    return DrawCallStatus::Geometry;
  }

  /**
  Assume we have all things needed to build PSO
  If the current encoder is not a render encoder, switch to it.
  */
  template <bool IndexedDraw>
  DrawCallStatus
  FinalizeCurrentRenderPipeline() {
    if (state_.ShaderStages[PipelineStage::Hull].Shader) {
      return FinalizeTessellationRenderPipeline<IndexedDraw>();
    }
    if (cmdbuf_state == CommandBufferState::RenderPipelineReady)
      return DrawCallStatus::Ordinary;
    auto GS = GetManagedShader<PipelineStage::Geometry>();
    if (GS) {
      if (GS->reflection().GeometryShader.GSPassThrough == ~0u) {
        return FinalizeGeometryRenderPipeline<IndexedDraw>();
      }
    }

    if (!SwitchToRenderEncoder()) {
      return DrawCallStatus::Invalid;
    }

    MTLCompiledGraphicsPipeline *pipeline;

    MTL_GRAPHICS_PIPELINE_DESC pipelineDesc;
    InitializeGraphicsPipelineDesc<IndexedDraw>(pipelineDesc);

    D11_WORK(work_.pipeline_lookups++); // spec 05 clarification 63
    device->CreateGraphicsPipeline(&pipelineDesc, &pipeline);
    EmitST([pso = std::move(pipeline)](ArgumentEncodingContext& enc) {
      MTL_COMPILED_GRAPHICS_PIPELINE GraphicsPipeline{};
      pso->GetPipeline(&GraphicsPipeline); // may block
      /* steamac's DXMT patch 0007 (LGPL, Holly Hamilton): a null pipeline is encoded too */
      auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setpso>();
      cmd.type = WMTRenderCommandSetPSO;
      cmd.pso = GraphicsPipeline.PipelineState;
    });

    cmdbuf_state = CommandBufferState::RenderPipelineReady;

    if (previous_render_pipeline_state != CommandBufferState::RenderPipelineReady) {
      state_.InputAssembler.VertexBuffers.set_dirty();
      state_.ShaderStages[PipelineStage::Vertex].ConstantBuffers.set_dirty();
      state_.ShaderStages[PipelineStage::Vertex].SRVs.set_dirty();
      state_.ShaderStages[PipelineStage::Vertex].Samplers.set_dirty();
      previous_render_pipeline_state = CommandBufferState::RenderPipelineReady;
    }

    return DrawCallStatus::Ordinary;
  }

  template <bool IndexedDraw>
  DrawCallStatus
  PreDraw() {
    DrawCallStatus status;
    if constexpr (IndexedDraw) {
      if (!state_.InputAssembler.IndexBuffer)
        return DrawCallStatus::Invalid;
    }
    if (status = FinalizeCurrentRenderPipeline<IndexedDraw>(); status == DrawCallStatus::Invalid) {
      return status;
    }
    UpdateVertexBuffer();
    UpdateSOTargets();
    if (dirty_state.any(DirtyState::DepthStencilState)) {
      IMTLD3D11DepthStencilState *state =
          state_.OutputMerger.DepthStencilState ? state_.OutputMerger.DepthStencilState : default_depth_stencil_state;
      // spec 05 clarification 69: while the view is kept, depth and stencil off (Direct3D 11's
      // behaviour with no view bound)
      if (keep_dsv_.applied())
        if (auto off = KeepDsvOffState())
          state = off;
      // spec 05 clarification 70: which planes a draw with this state can write, worked out and
      // sent only while item 70 is on or noted (item 77); without them a pass keeps its stores
      const bool note_writes = CutSettingsOnce()[CutSwitch::RoStore] || d11cs::g_level || TraceRecording();
      uint8_t writes = 0;
      if (note_writes) {
        D3D11_DEPTH_STENCIL_DESC ds_desc;
        state->GetDesc(&ds_desc);
        writes = DepthStencilWrites(ds_desc);
      }
      EmitST([state, stencil_ref = state_.OutputMerger.StencilRef, writes, note_writes](ArgumentEncodingContext& enc) {
        auto encoder = enc.currentRenderEncoder();
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setdsso>();
        cmd.type = WMTRenderCommandSetDSSO;
        cmd.dsso = state->GetDepthStencilState(encoder->dsv_planar_flags);
        cmd.stencil_ref = stencil_ref;
        if (note_writes)
          enc.noteDepthStencilWrites(writes);
      });
    }
    if (unlikely(keep_dsv_.active()))
      keep_dsv_.draw(); // spec 05 clarification 69
    if (dirty_state.any(DirtyState::RasterizerState)) {
      IMTLD3D11RasterizerState *state =
          state_.Rasterizer.RasterizerState ? state_.Rasterizer.RasterizerState : default_rasterizer_state;
      EmitST([state](ArgumentEncodingContext& enc) {
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setrasterizerstate>();
        cmd.type = WMTRenderCommandSetRasterizerState;
        state->SetupRasterizerState(cmd);
      });
    }
    if (dirty_state.any(DirtyState::BlendFactorAndStencilRef)) {
      EmitST([r = state_.OutputMerger.BlendFactor[0], g = state_.OutputMerger.BlendFactor[1],
            b = state_.OutputMerger.BlendFactor[2], a = state_.OutputMerger.BlendFactor[3],
            stencil_ref = state_.OutputMerger.StencilRef](ArgumentEncodingContext &enc) {
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setblendcolor>();
        cmd.type = WMTRenderCommandSetBlendFactorAndStencilRef;
        cmd.red = r;
        cmd.green = g;
        cmd.blue = b;
        cmd.alpha = a;
        cmd.stencil_ref = stencil_ref;
      });
    }
    IMTLD3D11RasterizerState *current_rs =
        state_.Rasterizer.RasterizerState ? state_.Rasterizer.RasterizerState : default_rasterizer_state;
    bool allow_scissor = current_rs->IsScissorEnabled();
    if (dirty_state.any(DirtyState::Viewport)) {
      auto viewports = AllocateCommandData<WMTViewport>(state_.Rasterizer.NumViewports);
      for (unsigned i = 0; i < state_.Rasterizer.NumViewports; i++) {
        auto &d3dViewport = state_.Rasterizer.viewports[i];
        viewports[i] = {d3dViewport.TopLeftX, d3dViewport.TopLeftY, d3dViewport.Width,
                        d3dViewport.Height,   d3dViewport.MinDepth, d3dViewport.MaxDepth};
      }
      EmitST([viewports = std::move(viewports)](ArgumentEncodingContext& enc) {
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setviewports>();
        cmd.type = WMTRenderCommandSetViewports;
        cmd.viewports.set(viewports.data());
        cmd.viewport_count = viewports.size();
      });
    }
    if (dirty_state.any(DirtyState::Scissors)) {
      auto render_target_width = state_.OutputMerger.RenderTargetWidth == 0
                                     ? (UINT)state_.Rasterizer.viewports[0].Width
                                     : state_.OutputMerger.RenderTargetWidth;
      auto render_target_height = state_.OutputMerger.RenderTargetHeight == 0
                                      ? (UINT)state_.Rasterizer.viewports[0].Height
                                      : state_.OutputMerger.RenderTargetHeight;
      auto scissors = AllocateCommandData<WMTScissorRect>(state_.Rasterizer.NumViewports);
      for (unsigned i = 0; i < state_.Rasterizer.NumViewports; i++) {
        if (allow_scissor) {
          if (i < state_.Rasterizer.NumScissorRects) {
            auto &d3d_rect = state_.Rasterizer.scissor_rects[i];
            LONG left = std::clamp(d3d_rect.left, (LONG)0, (LONG)render_target_width);
            LONG top = std::clamp(d3d_rect.top, (LONG)0, (LONG)render_target_height);
            LONG right = std::clamp(d3d_rect.right, left, (LONG)render_target_width);
            LONG bottom = std::clamp(d3d_rect.bottom, top, (LONG)render_target_height);
            scissors[i] = {uint32_t(left), uint32_t(top), uint32_t(right - left), uint32_t(bottom - top)};
          } else {
            scissors[i] = {0, 0, 0, 0};
          }
        } else {
          scissors[i] = {0, 0, render_target_width, render_target_height};
        }
      }
      EmitST([scissors = std::move(scissors)](ArgumentEncodingContext& enc) {
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setscissorrects>();
        cmd.type = WMTRenderCommandSetScissorRects;
        cmd.scissor_rects.set(scissors.data());
        cmd.rect_count = scissors.size();
      });
    }
    TraceDrawState(); // spec 05 clarification 30
    dirty_state.clrAll();
    if (cmdbuf_state == CommandBufferState::TessellationRenderPipelineReady) {
      UploadShaderStageResourceBinding<PipelineStage::Vertex, PipelineKind::Tessellation>();
      UploadShaderStageResourceBinding<PipelineStage::Pixel, PipelineKind::Tessellation>();
      UploadShaderStageResourceBinding<PipelineStage::Hull, PipelineKind::Tessellation>();
      UploadShaderStageResourceBinding<PipelineStage::Domain, PipelineKind::Tessellation>();
    } else if (cmdbuf_state == CommandBufferState::GeometryRenderPipelineReady) {
      UploadShaderStageResourceBinding<PipelineStage::Vertex, PipelineKind::Geometry>();
      UploadShaderStageResourceBinding<PipelineStage::Pixel, PipelineKind::Geometry>();
      UploadShaderStageResourceBinding<PipelineStage::Geometry, PipelineKind::Geometry>();
    } else {
      UploadShaderStageResourceBinding<PipelineStage::Vertex, PipelineKind::Ordinary>();
      UploadShaderStageResourceBinding<PipelineStage::Pixel, PipelineKind::Ordinary>();
    }
    return status;
  }

  /**
  Assume we have all things needed to build PSO
  If the current encoder is not a compute encoder, switch to it.
  */
  bool
  FinalizeCurrentComputePipeline() {
    if (cmdbuf_state == CommandBufferState::ComputePipelineReady)
      return true;

    SwitchToComputeEncoder();

    auto CS = GetManagedShader<PipelineStage::Compute>();

    if (!CS) {
      ERR("Shader not found?");
      return false;
    }
    MTLCompiledComputePipeline *pipeline;
    MTL_COMPUTE_PIPELINE_DESC desc{CS};
    D11_WORK(work_.pipeline_lookups++); // spec 05 clarification 63
    device->CreateComputePipeline(&desc, &pipeline);

    // spec 05 clarification 21: the shader's hash for the frame trace (0 when it's off)
    uint32_t trace_hash = FrameTraceSwitch() ? FrameTraceShaderHash(CS->sha1().data) : 0;
    EmitST([pso = std::move(pipeline),
            tg_size = WMTSize{CS->reflection().ThreadgroupSize[0],
                              CS->reflection().ThreadgroupSize[1],
                              CS->reflection().ThreadgroupSize[2]}, trace_hash](
               ArgumentEncodingContext &enc) {
      enc.traceComputeShader(trace_hash);
      MTL_COMPILED_COMPUTE_PIPELINE ComputePipeline;
      pso->GetPipeline(&ComputePipeline); // may block
      /* LGPL-2.1-or-later, from steamac's DXMT patch 0007 (Holly Hamilton): a null
         pipeline (compilation failed) is encoded anyway, with a zero threadgroup size:
         winemetal then drops the dispatches queued for it instead of running them on
         whatever kernel was bound before. */
      auto &cmd = enc.encodeComputeCommand<wmtcmd_compute_setpso>();
      cmd.type = WMTComputeCommandSetPSO;
      cmd.pso = ComputePipeline.PipelineState;
      cmd.threadgroup_size = ComputePipeline.PipelineState ? tg_size : WMTSize{0, 0, 0};
    });

    cmdbuf_state = CommandBufferState::ComputePipelineReady;
    return true;
  }

  /* MYIOSDECK spec 05 clarification 30: in a traced frame of the immediate context, the
     descriptions of the state a draw uses (its vertex and pixel shaders' samplers, blend,
     depth-stencil, rasterizer; the device's defaults for NULL ones), kept distinct per render
     pass by the encoder. Geometry, hull and domain shader samplers aren't listed. */
  void
  TraceDrawState() {
    if (!TraceRecording()) // spec 05 clarification 64: deferred contexts too
      return;
    std::vector<std::string> lines;
    TraceStageSamplers<PipelineStage::Vertex>("vs", lines);
    TraceStageSamplers<PipelineStage::Pixel>("ps", lines);
    D3D11_BLEND_DESC1 blend;
    (state_.OutputMerger.BlendState ? state_.OutputMerger.BlendState : default_blend_state)->GetDesc1(&blend);
    lines.push_back(TraceBlendState(blend, state_.OutputMerger.NumRTVs));
    D3D11_DEPTH_STENCIL_DESC depth_stencil;
    (state_.OutputMerger.DepthStencilState ? state_.OutputMerger.DepthStencilState : default_depth_stencil_state)
        ->GetDesc(&depth_stencil);
    lines.push_back(TraceDepthStencilState(depth_stencil, state_.OutputMerger.StencilRef));
    D3D11_RASTERIZER_DESC rasterizer;
    (state_.Rasterizer.RasterizerState ? state_.Rasterizer.RasterizerState : default_rasterizer_state)
        ->GetDesc(&rasterizer);
    lines.push_back(TraceRasterizerState(rasterizer));
    EmitST([lines = std::move(lines)](ArgumentEncodingContext &enc) mutable { enc.traceState(std::move(lines)); });
  }

  /* MYIOSDECK spec 05 clarification 44: in a traced frame of the immediate context, one line per
     draw: its arguments (indirect ones get their number from the encoder), the vertex shader's
     hash and whether its outputs include SV_RenderTargetArrayIndex / SV_ViewportArrayIndex, and
     any geometry shader, folded into the vertex stage (pass-through) or emulated. */
  void
  TraceDraw(
      const char *kind, bool indexed, bool indirect, uint32_t count, uint32_t start, int32_t base, uint32_t instances,
      uint32_t start_instance
  ) {
    NoteCutDraw(indirect, count, instances); // spec 05 clarification 72
    // spec 05 clarification 64: deferred contexts record their lines too
    if (!TraceRecording())
      return;
    TraceDrawArgs draw;
    draw.kind = kind;
    draw.indexed = indexed;
    draw.indirect = indirect;
    draw.count = count;
    draw.start = start;
    draw.base = base;
    draw.instances = instances;
    draw.start_instance = start_instance;
    if (auto vs = GetManagedShader<PipelineStage::Vertex>()) {
      draw.vs_hash = FrameTraceShaderHash(vs->sha1().data);
      draw.vs_writes = TraceShaderOutputWrites(vs);
    }
    if (auto gs = GetManagedShader<PipelineStage::Geometry>()) {
      draw.gs = true;
      draw.gs_hash = FrameTraceShaderHash(gs->sha1().data);
      draw.gs_writes = TraceShaderOutputWrites(gs);
      draw.gs_folded = gs->reflection().GeometryShader.GSPassThrough != ~0u;
    }
    // clarification 49: the vertex and pixel shaders' declared views and constant buffers, one
    // line each after the draw's own
    std::string lines = TraceDrawLine(draw);
    TraceStageBindings<PipelineStage::Vertex>("vs", lines);
    TraceStageBindings<PipelineStage::Pixel>("ps", lines);
    EmitST([lines = std::move(lines), indirect](ArgumentEncodingContext &enc) mutable {
      enc.traceDraw(std::move(lines), indirect);
    });
  }

  /* spec 05 clarification 49: "\nsrv vs t<slot> ..." per declared shader resource slot and
     "\ncb vs b<slot> ..." per declared constant buffer slot, `null` for an empty one */
  template <PipelineStage Stage>
  void
  TraceStageBindings(const char *stage_name, std::string &lines) {
    auto shader = GetManagedShader<Stage>();
    if (!shader)
      return;
    auto &reflection = shader->reflection();
    auto &stage = state_.ShaderStages[Stage];
    for (unsigned slot = 0; slot < 128; slot++) {
      uint64_t mask = slot < 64 ? reflection.SRVSlotMaskLo : reflection.SRVSlotMaskHi;
      if (!(mask & (1ull << (slot & 63))))
        continue;
      D3D11ShaderResourceView *view = stage.SRVs.test_bound(slot) ? stage.SRVs.at(slot).SRV.ptr() : nullptr;
      if (!view) {
        lines += "\n" + TraceSrvNull(stage_name, slot);
        continue;
      }
      D3D11_SHADER_RESOURCE_VIEW_DESC desc;
      view->GetDesc(&desc);
      TraceSrvResource res;
      if (auto buffer = view->buffer(); buffer.ptr()) {
        auto slice = view->bufferSlice();
        unsigned count = desc.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX ? desc.BufferEx.NumElements
                                                                             : desc.Buffer.NumElements;
        res.stride = count ? slice.byteLength / count : 0;
      } else if (auto texture = view->texture(); texture.ptr()) {
        res.width = texture->width();
        res.height = texture->height();
        res.depth = texture->depth();
        res.mips = texture->mipLevelCount();
      }
      lines += "\n" + TraceSrvLine(stage_name, slot, desc, res);
    }
    for (unsigned slot = 0; slot < 14; slot++) {
      if (!(reflection.ConstantBufferSlotMask & (1u << slot)))
        continue;
      if (!stage.ConstantBuffers.test_bound(slot) || !stage.ConstantBuffers.at(slot).Buffer.ptr()) {
        lines += "\n" + TraceConstantBufferNull(stage_name, slot);
        continue;
      }
      auto &cb = stage.ConstantBuffers.at(slot);
      lines += "\n" + TraceConstantBufferLine(
                          stage_name, slot, cb.Buffer->buffer()->length(), cb.FirstConstant, cb.NumConstants
                      );
    }
  }

  /* MYIOSDECK spec 05 clarification 50: in a dumped frame of the immediate context, a blit pass
     right before the draw copies the first 64 uints of each vertex-shader buffer view at its
     first element. The encoder keeps to 300 a frame and to the dump's first-write share.
     Clarification 58 adds the vertex shader's constant buffers to the same blit pass. */
  void
  TraceVSBufferReadback() {
    if (!FrameDumped(RecordingPresent()))
      return;
    auto shader = GetManagedShader<PipelineStage::Vertex>();
    if (!shader)
      return;
    auto &reflection = shader->reflection();
    auto &srvs = state_.ShaderStages[PipelineStage::Vertex].SRVs;
    std::vector<ArgumentEncodingContext::TraceSrvBuffer> buffers;
    for (unsigned slot = 0; slot < 128; slot++) {
      uint64_t mask = slot < 64 ? reflection.SRVSlotMaskLo : reflection.SRVSlotMaskHi;
      if (!(mask & (1ull << (slot & 63))) || !srvs.test_bound(slot))
        continue;
      auto view = srvs.at(slot).SRV.ptr();
      if (!view)
        continue;
      auto buffer = view->buffer();
      if (!buffer.ptr())
        continue;
      auto slice = view->bufferSlice();
      buffers.push_back({slot, std::move(buffer), slice.byteOffset, slice.byteLength});
    }
    // clarification 58: each constant buffer the vertex shader declares and has bound, from the
    // binding's offset, its size, at most 4096 bytes; a dynamic buffer's same bytes hashed from
    // the CPU's view now, when the draw is recorded
    std::vector<ArgumentEncodingContext::TraceCbBuffer> cbs;
    auto &cbuffers = state_.ShaderStages[PipelineStage::Vertex].ConstantBuffers;
    for (unsigned slot = 0; slot < 14; slot++) {
      if (!(reflection.ConstantBufferSlotMask & (1u << slot)) || !cbuffers.test_bound(slot))
        continue;
      auto &cb = cbuffers.at(slot);
      if (!cb.Buffer.ptr())
        continue;
      auto buffer = cb.Buffer->buffer();
      if (!buffer.ptr())
        continue;
      uint64_t total = buffer->length();
      uint64_t offset = uint64_t(cb.FirstConstant) * 16;
      if (offset >= total)
        continue;
      uint64_t bound = std::min<uint64_t>(uint64_t(cb.NumConstants) * 16, total - offset);
      uint32_t length = uint32_t(std::min<uint64_t>(bound, kFrameTraceCbReadbackMaxBytes));
      if (!length)
        continue;
      bool cpu_hash_valid = false;
      uint64_t cpu_hash = 0;
      UINT dynamic_length = 0, dynamic_bind = 0;
      if (auto dynamic = cb.Buffer->dynamicBuffer(&dynamic_length, &dynamic_bind)) {
        cpu_hash = TraceFnv1a64(static_cast<const char *>(dynamic->immediateMappedMemory()) + offset, length);
        cpu_hash_valid = true;
      }
      cbs.push_back({slot, std::move(buffer), uint32_t(offset), length, cpu_hash_valid, cpu_hash});
    }
    if (buffers.empty() && cbs.empty())
      return;
    SwitchToBlitEncoder(CommandBufferState::BlitEncoderActive);
    if (!buffers.empty())
      EmitOP([buffers = std::move(buffers)](ArgumentEncodingContext &enc) mutable {
        enc.traceSrvReadback(std::move(buffers));
      });
    if (!cbs.empty())
      EmitOP([cbs = std::move(cbs)](ArgumentEncodingContext &enc) mutable { enc.traceCbReadback(std::move(cbs)); });
  }

  /* spec 05 clarification 50: a draw that PreDraw turned down still takes its number, so a
     read-back made for it isn't put under the next draw */
  void
  TraceDrawDropped() {
    if (!TraceRecording()) // spec 05 clarification 64: deferred contexts too
      return;
    EmitST([](ArgumentEncodingContext &enc) { enc.traceDrawDropped(); });
  }

  /* MYIOSDECK spec 05 clarification 72: whether the draw overwrites each colour target whole, for
     the encoder (noted when the switch is on, when counting, or in a traced frame) */
  void
  NoteCutDraw(bool indirect, uint32_t count, uint32_t instances) {
    if (!CutSettingsOnce()[CutSwitch::OverwriteLoad] && !d11cs::g_level && !TraceRecording())
      return;
    OverwriteDrawFacts f;
    f.instanced = instances != 1;
    f.indirect = indirect;
    f.geometry_or_tessellation = state_.ShaderStages[PipelineStage::Geometry].Shader ||
                                 state_.ShaderStages[PipelineStage::Hull].Shader ||
                                 state_.ShaderStages[PipelineStage::Domain].Shader;
    auto topology = state_.InputAssembler.Topology;
    f.triangle_list_of_3 = topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST && count == 3;
    f.triangle_strip_of_4 = topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP && count == 4;
    D3D11_BLEND_DESC1 blend;
    (state_.OutputMerger.BlendState ? state_.OutputMerger.BlendState : default_blend_state)->GetDesc1(&blend);
    f.alpha_to_coverage = blend.AlphaToCoverageEnable;
    f.sample_mask = state_.OutputMerger.SampleMask;
    for (unsigned i = 0; i < 8; i++) {
      auto &target = blend.IndependentBlendEnable ? blend.RenderTarget[i] : blend.RenderTarget[0];
      f.target_plain[i] = !target.BlendEnable && !target.LogicOpEnable &&
                          (target.RenderTargetWriteMask & D3D11_COLOR_WRITE_ENABLE_ALL) == D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    const float width = float(state_.OutputMerger.RenderTargetWidth),
                height = float(state_.OutputMerger.RenderTargetHeight);
    if (state_.Rasterizer.NumViewports == 1) {
      auto &vp = state_.Rasterizer.viewports[0];
      f.one_viewport_covers = vp.TopLeftX <= 0 && vp.TopLeftY <= 0 && vp.TopLeftX + vp.Width >= width &&
                              vp.TopLeftY + vp.Height >= height;
    }
    D3D11_RASTERIZER_DESC rs;
    (state_.Rasterizer.RasterizerState ? state_.Rasterizer.RasterizerState : default_rasterizer_state)->GetDesc(&rs);
    f.solid_fill = rs.FillMode == D3D11_FILL_SOLID;
    if (!rs.ScissorEnable) {
      f.scissor_covers = true;
    } else if (state_.Rasterizer.NumScissorRects >= 1) {
      auto &r = state_.Rasterizer.scissor_rects[0];
      f.scissor_covers = r.left <= 0 && r.top <= 0 && float(r.right) >= width && float(r.bottom) >= height;
    }
    f.pixel_shader_may_discard = PixelShaderMayDiscard();
    EmitST([mask = OverwriteTargetMask(f)](ArgumentEncodingContext &enc) { enc.noteCutDraw(mask); });
  }

  /* clarification 72: the bound pixel shader has a discard (or can't be read, or there is none:
     then nothing is overwritten); scanned once per shader */
  const void *ps_discard_shader_ = nullptr;
  uint32_t ps_discard_hash_ = 0;
  bool ps_may_discard_ = true;

  bool
  PixelShaderMayDiscard() {
    auto ps = GetManagedShader<PipelineStage::Pixel>();
    if (!ps)
      return true;
    const void *key = &*ps;
    uint32_t hash = FrameTraceShaderHash(ps->sha1().data); // a new shader at the same address
    if (key != ps_discard_shader_ || hash != ps_discard_hash_) {
      ps_discard_shader_ = key;
      ps_discard_hash_ = hash;
      ps_may_discard_ = true;
#ifdef DXMT_IOS
      auto &bytecode = ps->bytecode();
      if (!bytecode.empty())
        ps_may_discard_ = DxbcMayDiscard(bytecode.data(), bytecode.size());
#endif
    }
    return ps_may_discard_;
  }

  /* spec 05 clarification 44: from the shader's DXBC output signature, kept on iOS (-1 when not) */
  static int
  TraceShaderOutputWrites(ManagedShader shader) {
#ifdef DXMT_IOS
    auto &bytecode = shader->bytecode();
    if (!bytecode.empty())
      return DxbcOutputWrites(bytecode.data(), bytecode.size());
#endif
    (void)shader;
    return -1;
  }

  /* spec 05 clarification 30: one line per sampler slot the stage's shader declares */
  template <PipelineStage Stage>
  void
  TraceStageSamplers(const char *stage_name, std::vector<std::string> &lines) {
    auto shader = GetManagedShader<Stage>();
    if (!shader)
      return;
    uint32_t mask = shader->reflection().SamplerSlotMask;
    auto &samplers = state_.ShaderStages[Stage].Samplers;
    for (unsigned slot = 0; slot < 16; slot++) {
      if (!(mask & (1u << slot)))
        continue;
      if (samplers.test_bound(slot) && samplers.at(slot).Sampler) {
        D3D11_SAMPLER_DESC desc;
        samplers.at(slot).Sampler->GetDesc(&desc);
        lines.push_back(TraceSamplerState(stage_name, slot, desc));
      } else {
        lines.push_back(TraceSamplerNull(stage_name, slot));
      }
    }
  }

  bool
  PreDispatch() {
    if (!FinalizeCurrentComputePipeline()) {
      return false;
    }
    if (TraceRecording()) { // spec 05 clarifications 30 and 64
      std::vector<std::string> lines;
      TraceStageSamplers<PipelineStage::Compute>("cs", lines);
      EmitST([lines = std::move(lines)](ArgumentEncodingContext &enc) mutable {
        enc.traceComputeSamplers(std::move(lines));
      });
    }
    UploadShaderStageResourceBinding<PipelineStage::Compute, PipelineKind::Ordinary>();
    return true;
  }

  void
  UpdateSOTargets() {
    if (!state_.ShaderStages[PipelineStage::Geometry].Shader) {
      return;
    }
    /**
    FIXME: support all slots
     */
    if (!state_.StreamOutput.Targets.test_dirty(0)) {
      return;
    }
    if (state_.StreamOutput.Targets.test_bound(0)) {
      auto &so_slot0 = state_.StreamOutput.Targets[0];
      if (so_slot0.Offset == 0xFFFFFFFF) {
        EmitST([slot0 = so_slot0.Buffer->buffer()](ArgumentEncodingContext &enc) {
          auto [buffer, buffer_offset] = enc.access<true>(slot0, 0, slot0->length(), DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setbuffer>();
          cmd.type = WMTRenderCommandSetVertexBuffer;
          cmd.buffer = buffer->buffer();;
          cmd.offset = buffer_offset;
          cmd.index = 20;
          enc.makeResident<PipelineStage::Vertex, PipelineKind::Ordinary>(slot0.ptr(), false, true);
          enc.setCompatibilityFlag(FeatureCompatibility::UnsupportedStreamOutputAppending);
        });
      } else {
        EmitST([slot0 = so_slot0.Buffer->buffer(), offset = so_slot0.Offset](ArgumentEncodingContext &enc) {
          auto [buffer, buffer_offset] = enc.access<true>(slot0, 0, slot0->length(), DXMT_ENCODER_RESOURCE_ACESS_WRITE);
          auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setbuffer>();
          cmd.type = WMTRenderCommandSetVertexBuffer;
          cmd.buffer = buffer->buffer();;
          cmd.offset = offset + buffer_offset;
          cmd.index = 20;
          enc.makeResident<PipelineStage::Vertex, PipelineKind::Ordinary>(slot0.ptr(), false, true);
        });
      }
    } else {
      EmitST([](ArgumentEncodingContext &enc) {
        auto &cmd = enc.encodeRenderCommand<wmtcmd_render_setbuffer>();
        cmd.type = WMTRenderCommandSetVertexBuffer;
        cmd.buffer = NULL_OBJECT_HANDLE;
        cmd.offset = 0;
        cmd.index = 20;
      });
    }
    state_.StreamOutput.Targets.clear_dirty(0);
    if (state_.StreamOutput.Targets.any_dirty()) {
      EmitST([](ArgumentEncodingContext &enc) {
        enc.setCompatibilityFlag(FeatureCompatibility::UnsupportedMultipleStreamOutput);
      });
    }
  }

  template <PipelineStage Stage>
  void RestoreEncodingContextStageState() {
      auto &ShaderStage = state_.ShaderStages[Stage];
      for (const auto &[slot, entry] : ShaderStage.ConstantBuffers) {
        EmitST([=, buffer = entry.Buffer->buffer(), offset = entry.FirstConstant << 4](ArgumentEncodingContext &enc
             ) mutable { enc.bindConstantBuffer<Stage>(slot, offset, forward_rc(buffer)); });
      }
      for (const auto &[slot, entry] : ShaderStage.Samplers) {
        EmitST([=, sampler = entry.Sampler->sampler()](ArgumentEncodingContext &enc) mutable {
          enc.bindSampler<Stage>(slot, forward_rc(sampler));
        });
      }
      for (const auto &[slot, entry] : ShaderStage.SRVs) {
        auto pView = entry.SRV.ptr();
        if (auto buffer = pView->buffer()) {
          EmitST([=, buffer = std::move(buffer), viewId = pView->viewId(),
                slice = pView->bufferSlice()](ArgumentEncodingContext &enc) mutable {
            enc.bindBuffer<Stage>(slot, forward_rc(buffer), viewId, slice);
          });
        } else {
          EmitST([=, texture = pView->texture(), viewId = pView->viewId()](ArgumentEncodingContext &enc) mutable {
            enc.bindTexture<Stage>(slot, forward_rc(texture), viewId);
          });
        }
      }
  }

  void
  RestoreEncodingContextState() {
    for (const auto &[slot, element] : state_.InputAssembler.VertexBuffers) {
      EmitST([=, buffer = element.Buffer->buffer(), offset = element.Offset,
            stride = element.Stride](ArgumentEncodingContext &enc) mutable {
        enc.bindVertexBuffer(slot, offset, stride, forward_rc(buffer));
      });
    }

    if (state_.InputAssembler.IndexBuffer) {
      EmitST([buffer = state_.InputAssembler.IndexBuffer->buffer()](ArgumentEncodingContext &enc) mutable {
        enc.bindIndexBuffer(forward_rc(buffer));
      });
    }

    {
      RestoreEncodingContextStageState<PipelineStage::Vertex>();
      RestoreEncodingContextStageState<PipelineStage::Pixel>();
      RestoreEncodingContextStageState<PipelineStage::Geometry>();
      RestoreEncodingContextStageState<PipelineStage::Hull>();
      RestoreEncodingContextStageState<PipelineStage::Domain>();
      RestoreEncodingContextStageState<PipelineStage::Compute>();
    }

    for (const auto &[slot, entry] : state_.OutputMerger.UAVs) {
      auto pUAV = entry.View.ptr();
      if (auto buffer = pUAV->buffer()) {
        EmitST([=, buffer = std::move(buffer), viewId = pUAV->viewId(), counter = pUAV->counter(),
              slice = pUAV->bufferSlice()](ArgumentEncodingContext &enc) mutable {
          enc.bindOutputBuffer<PipelineStage::Pixel>(slot, forward_rc(buffer), viewId, forward_rc(counter), slice);
        });
      } else {
        EmitST([=, texture = pUAV->texture(), viewId = pUAV->viewId()](ArgumentEncodingContext &enc) mutable {
          enc.bindOutputTexture<PipelineStage::Pixel>(slot, forward_rc(texture), viewId);
        });
      }
    }
    for (const auto &[slot, entry] : state_.OutputMerger.UAVs) {
      auto pUAV = entry.View.ptr();
      if (auto buffer = pUAV->buffer()) {
        EmitST([=, buffer = std::move(buffer), viewId = pUAV->viewId(), counter = pUAV->counter(),
              slice = pUAV->bufferSlice()](ArgumentEncodingContext &enc) mutable {
          enc.bindOutputBuffer<PipelineStage::Compute>(slot, forward_rc(buffer), viewId, forward_rc(counter), slice);
        });
      } else {
        EmitST([=, texture = pUAV->texture(), viewId = pUAV->viewId()](ArgumentEncodingContext &enc) mutable {
          enc.bindOutputTexture<PipelineStage::Compute>(slot, forward_rc(texture), viewId);
        });
      }
    }
  }

  void ResetEncodingContextState() {
    InvalidateCurrentPass(true);
    // TODO: optimize by clearing only bound resource
    EmitST([](ArgumentEncodingContext& enc) {
      enc.clearState();
    });
  }

  void ResetD3D11ContextState() {
    state_ = {};
  }

protected:
  /* spec 05 clarification 63: what is ending the open pass (set by the calls that end passes),
     and what ended the last one, counted when the next pass starts */
  d11cs::PassStart pass_reason_ = d11cs::PassStart::Other;
  d11cs::PassStart pass_ended_by_ = d11cs::PassStart::Other;

  void
  NotePassReason(d11cs::PassStart reason) {
    if (unlikely(d11cs::g_level))
      pass_reason_ = reason;
  }

  void
  NotePassEnded() {
    if (unlikely(d11cs::g_level) && cmdbuf_state != CommandBufferState::Idle) {
      pass_ended_by_ = pass_reason_;
      pass_reason_ = d11cs::PassStart::Other;
    }
  }

  void
  NotePassStarted() {
    if (unlikely(d11cs::g_level)) {
      d11cs::Work().pass_starts[unsigned(pass_ended_by_)]++;
      pass_ended_by_ = d11cs::PassStart::Other;
    }
  }

  void
  NotePassEndedByCommit() {
    if (unlikely(d11cs::g_level))
      pass_ended_by_ = d11cs::PassStart::Commit;
  }

  /* ---- spec 05 clarification 69: the depth-stencil view kept while it is unbound ---- */

  /* what the open render pass attached when it started */
  KeepDsvTargets pass_targets_;

  /* the keep's state machine (d3d11_pass_cuts.hpp), and the kept view, held while kept */
  KeepDsvTracker keep_dsv_;
  Com<D3D11DepthStencilView> keep_dsv_view_;

  Com<IMTLD3D11DepthStencilState> keep_dsv_off_state_; // depth and stencil off

  /* whether the keep is noted at all: the switch on, or counting, or a traced frame */
  bool
  KeepDsvNoting() {
    return CutSettingsOnce()[CutSwitch::KeepDsv] || d11cs::g_level || TraceRecording();
  }

  /* a keep ended: its draws and boundaries go to the open pass (before it ends, if it does) */
  void
  EmitKeepDsvNote(const KeepDsvTracker::Note &note) {
    if (note.applied)
      dirty_state.set(DirtyState::DepthStencilState); // the application's state again
    keep_dsv_view_ = nullptr;
    EmitST([note](ArgumentEncodingContext &enc) { enc.noteKeepDsv(note.draws, note.applied, note.boundaries); });
  }

  /* the view the pass has: the kept one while the switch keeps it, else the bound one */
  D3D11DepthStencilView *
  AttachedDsv() {
    return keep_dsv_.applied() ? keep_dsv_view_.ptr() : state_.OutputMerger.DSV.ptr();
  }

  /* depth test and write off and stencil off, for the draws of a keep */
  IMTLD3D11DepthStencilState *
  KeepDsvOffState() {
    if (!keep_dsv_off_state_) {
      D3D11_DEPTH_STENCIL_DESC desc = {};
      desc.DepthEnable = FALSE;
      desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
      desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
      desc.StencilEnable = FALSE;
      desc.StencilReadMask = D3D11_DEFAULT_STENCIL_READ_MASK;
      desc.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
      desc.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
      desc.BackFace = desc.FrontFace;
      Com<ID3D11DepthStencilState> state;
      if (SUCCEEDED(device->CreateDepthStencilState(&desc, &state)))
        keep_dsv_off_state_ = com_cast<IMTLD3D11DepthStencilState>(state.ptr());
    }
    return keep_dsv_off_state_.ptr();
  }

  /* a view of `texture` bound as a shader resource in any stage */
  bool
  AnyShaderResourceOf(Texture *texture) {
    for (auto &stage : state_.ShaderStages)
      for (const auto &[slot, entry] : stage.SRVs)
        if (entry.SRV->texture().ptr() == texture)
          return true;
    return false;
  }

  /* spec 05 clarification 64: whether this context records a traced frame's draw and state lines:
     the immediate context in a traced frame; a deferred one while the present the immediate
     context is recording, or the next one, is traced (its lines are dropped unless the command
     list runs in a traced frame) */
  bool
  TraceRecording() {
    if (!FrameTraceSwitch())
      return false;
    uint64_t present = ctx_state.cmd_queue.CurrentFrameSeq();
    if constexpr (ContextInternalState::kD11csDeferred)
      return FrameTraced(present) || FrameTraced(present + 1);
    else
      return FrameTraced(present);
  }

  MTLD3D11Device *device;
  CommandBufferState cmdbuf_state = CommandBufferState::Idle;
  CommandBufferState previous_render_pipeline_state = CommandBufferState::Idle;
  ContextInternalState &ctx_state;
  ContextInternalState::device_mutex_t &mutex;

  IMTLD3D11RasterizerState *default_rasterizer_state;
  IMTLD3D11DepthStencilState *default_depth_stencil_state;
  IMTLD3D11BlendState *default_blend_state;

public:
  enum class DirtyState {
    DepthStencilState,
    RasterizerState,
    BlendFactorAndStencilRef,
    Viewport,
    Scissors,
  };

  Flags<DirtyState> dirty_state = 0;

#pragma endregion

protected:
  D3D11ContextState state_;
  /* patch 0014 (LGPL, hanzpo): the active state object; a private reference, since a
     public one would keep the device alive */
  Com<MTLD3D11DeviceContextState, false> active_context_state_;
  D3D11UserDefinedAnnotation annotation_;
  MTLD3D11ContextExt<ContextInternalState> ext_;
  uint64_t max_object_threadgroups_;

public:
  MTLD3D11DeviceContextImplBase(MTLD3D11Device *pDevice, ContextInternalState &ctx_state, ContextInternalState::device_mutex_t &mutex) :
      MTLD3D11DeviceChild(pDevice),
      device(pDevice),
      ctx_state(ctx_state),
      mutex(mutex),
      state_(),
      annotation_(this),
      ext_(this) {
    pDevice->CreateRasterizerState2(&kDefaultRasterizerDesc, (ID3D11RasterizerState2 **)&default_rasterizer_state);
    pDevice->CreateBlendState1(&kDefaultBlendDesc, (ID3D11BlendState1 **)&default_blend_state);
    pDevice->CreateDepthStencilState(
        &kDefaultDepthStencilDesc, (ID3D11DepthStencilState **)&default_depth_stencil_state
    );
    /* we don't need the extra reference as they are always valid */
    default_rasterizer_state->Release();
    default_blend_state->Release();
    default_depth_stencil_state->Release();

    max_object_threadgroups_ = m_parent->GetDXMTDevice().maxObjectThreadgroups();
  }
};

template <typename ContextInternalState>
class MTLD3D11ContextExt : public IMTLD3D11ContextExt1 {
  class CachedTemporalScaler {
  public:
    WMTPixelFormat color_pixel_format;
    WMTPixelFormat output_pixel_format;
    WMTPixelFormat depth_pixel_format;
    WMTPixelFormat motion_vector_pixel_format;
    bool auto_exposure;
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    WMT::Reference<WMT::FXTemporalScaler> scaler;
    Rc<Texture> mv_downscaled;
  };
public:
  MTLD3D11ContextExt(MTLD3D11DeviceContextImplBase<ContextInternalState> *context) : ctx_(context){};

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void **ppvObject) final {
    return ctx_->QueryInterface(riid, ppvObject);
  }

  ULONG STDMETHODCALLTYPE AddRef() final { return ctx_->AddRef(); }

  ULONG STDMETHODCALLTYPE Release() final { return ctx_->Release(); }

  WMTPixelFormat GetCorrectMotionVectorFormat(WMTPixelFormat format) {
    switch (format) {
    case WMTPixelFormatRG16Uint:
    case WMTPixelFormatRG16Float:
    case WMTPixelFormatRG16Sint:
    case WMTPixelFormatRG16Snorm:
    case WMTPixelFormatRG16Unorm:
      return WMTPixelFormatRG16Float;
    case WMTPixelFormatRG32Uint:
    case WMTPixelFormatRG32Float:
    case WMTPixelFormatRG32Sint:
      return WMTPixelFormatRG32Float;
    case WMTPixelFormatRGBA16Sint:
    case WMTPixelFormatRGBA16Snorm:
    case WMTPixelFormatRGBA16Uint:
    case WMTPixelFormatRGBA16Unorm:
    case WMTPixelFormatRGBA16Float:
      return WMTPixelFormatRGBA16Float;
    default:
      break;
    }
    return WMTPixelFormatInvalid;
  }

  void STDMETHODCALLTYPE TemporalUpscale(const MTL_TEMPORAL_UPSCALE_D3D11_DESC *pDesc) final {
    Rc<Texture> input, output, depth, motion_vector, exposure;
    if (!(input = static_cast<D3D11ResourceCommon *>(pDesc->Color)->texture()))
      return;
    if (!(output = static_cast<D3D11ResourceCommon *>(pDesc->Output)->texture()))
      return;
    if (!(depth = static_cast<D3D11ResourceCommon *>(pDesc->Depth)->texture()))
      return;
    if (!(motion_vector = static_cast<D3D11ResourceCommon *>(pDesc->MotionVector)->texture()))
      return;
    if (pDesc->ExposureTexture && !(exposure = static_cast<D3D11ResourceCommon *>(pDesc->ExposureTexture)->texture()))
      return;

    WMTPixelFormat motion_vector_format = GetCorrectMotionVectorFormat(motion_vector->pixelFormat());
    if (motion_vector_format == WMTPixelFormatInvalid) {
      ERR("TemporalUpscale: invalid motion vector format ", motion_vector->pixelFormat());
      return;
    }

    WMT::Reference<WMT::FXTemporalScaler> scaler;
    Rc<Texture> mv_downscaled;

    for(CachedTemporalScaler& entry: scaler_cache_) {
      if(pDesc->AutoExposure != entry.auto_exposure) continue;
      if(input->width() != entry.input_width) continue;
      if(input->height() != entry.input_height) continue;
      if(output->width() != entry.output_width) continue;
      if(output->height() != entry.output_height) continue;
      if(input->pixelFormat() != entry.color_pixel_format) continue;
      if(output->pixelFormat() != entry.output_pixel_format) continue;
      if(depth->pixelFormat() != entry.depth_pixel_format) continue;
      if(motion_vector_format != entry.motion_vector_pixel_format) continue;

      scaler = entry.scaler;
      mv_downscaled = entry.mv_downscaled;
      break;
    }

    if (!scaler) {
      CachedTemporalScaler scaler_entry;
      scaler_entry.color_pixel_format = input->pixelFormat();
      scaler_entry.output_pixel_format = output->pixelFormat();
      scaler_entry.depth_pixel_format = depth->pixelFormat();
      scaler_entry.motion_vector_pixel_format = motion_vector_format;
      WMTFXTemporalScalerInfo info;
      info.color_format = scaler_entry.color_pixel_format;
      info.output_format = scaler_entry.output_pixel_format;
      info.depth_format = scaler_entry.depth_pixel_format;
      info.motion_format = motion_vector_format;
      scaler_entry.auto_exposure = pDesc->AutoExposure;
      scaler_entry.input_width = input->width();
      scaler_entry.input_height = input->height();
      scaler_entry.output_width = output->width();
      scaler_entry.output_height = output->height();
      info.auto_exposure = scaler_entry.auto_exposure;
      info.input_width = scaler_entry.input_width;
      info.input_height = scaler_entry.input_height;
      info.output_width = scaler_entry.output_width;
      info.output_height = scaler_entry.output_height;
      info.input_content_properties_enabled = true;
      info.input_content_min_scale = 1.0f;
      info.input_content_max_scale =  3.0f;
      info.requires_synchronous_initialization = true;
      scaler_entry.scaler = ctx_->device->GetMTLDevice().newTemporalScaler(info);
      if (pDesc->MotionVectorInDisplayRes) {
        WMTTextureInfo tex_info;
        tex_info.width = scaler_entry.input_width;
        tex_info.height = scaler_entry.input_height;
        tex_info.depth = 1;
        tex_info.array_length = 1;
        tex_info.mipmap_level_count = 1;
        tex_info.pixel_format = WMTPixelFormatRG32Float;
        tex_info.sample_count = 1;
        tex_info.type = WMTTextureType2D;
        tex_info.usage = WMTTextureUsageShaderRead | WMTTextureUsageShaderWrite;
        tex_info.options = WMTResourceStorageModePrivate;
        scaler_entry.mv_downscaled = new Texture(tex_info, this->ctx_->device->GetMTLDevice());
        mv_downscaled = scaler_entry.mv_downscaled;
        Flags<TextureAllocationFlag> flags;
        flags.set(TextureAllocationFlag::GpuPrivate);
        mv_downscaled->rename(mv_downscaled->allocate(flags));
      }
      scaler = scaler_entry.scaler;
      scaler_cache_.push_back(std::move(scaler_entry));
      // to simplify implementation, the created scalers are never destroyed
      // as they are not expected to be created frequently
    }

    ctx_->InvalidateCurrentPass();
    ctx_->EmitOP([input = std::move(input), output = std::move(output),
                  depth = std::move(depth),
                  motion_vector = std::move(motion_vector),
                  exposure = std::move(exposure), scaler = std::move(scaler),
                  props =
                      WMTFXTemporalScalerProps{
                          pDesc->InputContentWidth,
                          pDesc->InputContentHeight,
                          (bool)pDesc->InReset,
                          (bool)pDesc->DepthReversed,
                          pDesc->MotionVectorScaleX,
                          pDesc->MotionVectorScaleY,
                          pDesc->JitterOffsetX,
                          pDesc->JitterOffsetY,
                          pDesc->PreExposure,
                      },
                  motion_vector_format, mv_downscaled = std::move(mv_downscaled)
                ](ArgumentEncodingContext &enc) mutable {
      auto mv_view = motion_vector->createView(
          {.format = motion_vector_format,
           .type = WMTTextureType2D,
           .firstMiplevel = 0,
           .miplevelCount = 1,
           .firstArraySlice = 0,
           .arraySize = 1});
      auto &scaler_info = enc.currentFrameStatistics().last_scaler_info;
      scaler_info.type = ScalerType::Temporal;
      scaler_info.auto_exposure = exposure == nullptr;
      scaler_info.input_width = props.input_content_width;
      scaler_info.input_height = props.input_content_height;
      scaler_info.output_width = output->width();
      scaler_info.output_height = output->height();

      scaler_info.motion_vector_highres = mv_downscaled != nullptr;

      if (scaler_info.motion_vector_highres) {
        enc.mv_scale_cmd.dispatch(
            motion_vector, mv_view, mv_downscaled, 0, props.motion_vector_scale_x, props.motion_vector_scale_y
        );
        WMTFXTemporalScalerProps new_props = props;
        new_props.motion_vector_scale_x = 1.0;
        new_props.motion_vector_scale_y = 1.0;
        enc.upscaleTemporal(input, output, depth, mv_downscaled, 0, exposure, scaler, new_props);
      } else {
        enc.upscaleTemporal(input, output, depth, motion_vector, mv_view, exposure, scaler, props);
      }
    });
  }

  void STDMETHODCALLTYPE BeginUAVOverlap() final {
    // TODO
  }

  void STDMETHODCALLTYPE EndUAVOverlap() final {
    // TODO
  }

  virtual HRESULT STDMETHODCALLTYPE
  CheckFeatureSupport(MTL_FEATURE Feature, void *pFeatureSupportData, UINT FeatureSupportDataSize) final {
    if (!pFeatureSupportData)
      return E_INVALIDARG;
    switch (Feature) {
    case MTL_FEATURE_METALFX_TEMPORAL_SCALER: {
      if (FeatureSupportDataSize != sizeof(BOOL))
        return E_INVALIDARG;
      *reinterpret_cast<BOOL *>(pFeatureSupportData) = ctx_->device->GetMTLDevice().supportsFXTemporalScaler();
      return S_OK;
    }
    }
    return E_INVALIDARG;
  };

private:

  MTLD3D11DeviceContextImplBase<ContextInternalState> *ctx_;
  std::vector<CachedTemporalScaler> scaler_cache_;
};

struct used_dynamic_buffer {
  Rc<DynamicBuffer> buffer;
  Rc<BufferAllocation> allocation;
  uint32_t suballocation;
  bool latest;
};

struct used_dynamic_lineartexture {
  Rc<DynamicLinearTexture> texture;
  Rc<TextureAllocation> allocation;
  bool latest;
};

class MTLD3D11CommandList : public ManagedDeviceChild<ID3D11CommandList> {
public:
  MTLD3D11CommandList(MTLD3D11Device *pDevice, MTLD3D11CommandListPoolBase *pPool, UINT context_flag) :
      ManagedDeviceChild(pDevice),
      context_flag(context_flag),
      cmdlist_pool(pPool),
      staging_allocator({pDevice->GetMTLDevice(), WMTResourceOptionCPUCacheModeWriteCombined |
                                       WMTResourceHazardTrackingModeUntracked | WMTResourceStorageModeManaged, false
      }),
      cpu_command_allocator({}) {};

  ~MTLD3D11CommandList() {
    Reset();
  }

  void Recycle() final {
    Reset();
    cmdlist_pool->RecycleCommandList(this);
  }

  void
  Reset() {
    auto current_seq_id = m_parent->GetDXMTDevice().queue().CurrentSeqId();

    while (!used_dynamic_buffers.empty()) {
      auto &buffer = used_dynamic_buffers.back();
      buffer.buffer->recycle(current_seq_id, std::move(buffer.allocation));
      used_dynamic_buffers.pop_back();
    }
    while (!used_dynamic_lineartextures.empty()) {
      auto &texture = used_dynamic_lineartextures.back();
      texture.texture->recycle(current_seq_id, std::move(texture.allocation));
      used_dynamic_lineartextures.pop_back();
    }
    read_staging_resources.clear();
    written_staging_resources.clear();
    visibility_query_count = 0;
    issued_visibility_query.clear();
    issued_event_query.clear();
    list.reset();
    staging_allocator.free_blocks(~0uLL);
    cpu_command_allocator.free_blocks(~0uLL);

    promote_flush = false;
  };

  HRESULT
  STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) override {
    D11_CHILD_CALL(QueryInterface); // MYIOSDECK spec 05 clarification 63
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild) || riid == __uuidof(ID3D11CommandList)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    if (logQueryInterfaceError(__uuidof(ID3D11CommandList), riid)) {
      WARN("D3D11CommandList: Unknown interface query ", str::format(riid));
    }
    return E_NOINTERFACE;
  }

  UINT
  STDMETHODCALLTYPE
  GetContextFlags() override {
    return context_flag;
  };

#pragma region DeferredContext-related

  template <typename T>
  moveonly_list<T>
  AllocateCommandData(size_t n) {
    return moveonly_list<T>((T *)allocate_cpu_heap(sizeof(T) * n, alignof(T)), n);
  }

  std::tuple<WMT::Buffer, uint64_t>
  AllocateStagingBuffer(size_t size, size_t alignment) {
    auto [block, offset] = staging_allocator.allocate(1, 0, size, alignment);
    return {block.buffer, offset};
  }

  void *
  allocate_cpu_heap(size_t size, size_t alignment) {
    auto [block, offset] = cpu_command_allocator.allocate(1, 0, size, alignment);
    return ptr_add(block.ptr, offset);
  }

  template <typename T>
  T *
  allocate_cpu_heap() {
    return (T *)allocate_cpu_heap(sizeof(T), alignof(T));
  }

  template <CommandWithContext<ArgumentEncodingContext> cmd>
  void Emit(cmd &&fn) {
    list.emit(std::forward<cmd>(fn),
              allocate_cpu_heap(list.calculateCommandSize<cmd>(), 16));
  }

#pragma endregion

#pragma region ImmediateContext-related

  void Execute(ArgumentEncodingContext& ctx) {
    list.execute(ctx);
  };

#pragma endregion

  bool promote_flush = false;

  std::vector<used_dynamic_buffer> used_dynamic_buffers;
  std::vector<used_dynamic_lineartexture> used_dynamic_lineartextures;
  std::vector<Rc<StagingResource>> read_staging_resources;
  std::vector<Rc<StagingResource>> written_staging_resources;
  uint32_t visibility_query_count = 0;
  std::vector<std::pair<Com<IMTLD3DOcclusionQuery>, uint32_t>> issued_visibility_query;
  std::vector<Com<MTLD3D11EventQuery>> issued_event_query;

private:
  UINT context_flag;
  MTLD3D11CommandListPoolBase *cmdlist_pool;

  CommandList<ArgumentEncodingContext> list;

  RingBumpState<StagingBufferBlockAllocator, kStagingBlockSizeForDeferredContext> staging_allocator;
  RingBumpState<HostBufferBlockAllocator, kStagingBlockSizeForDeferredContext, dxmt::null_mutex> cpu_command_allocator; 

};

} // namespace dxmt