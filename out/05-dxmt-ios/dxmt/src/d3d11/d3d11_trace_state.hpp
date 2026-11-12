// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 30: the frame trace's state lines, from the Direct3D 11 descriptions
// of the state objects a traced draw or dispatch used (the device's defaults for NULL ones).
// Compact, one line per state; the encoder keeps the distinct ones of each pass. Plain code
// over d3d11.h's structs so the host tests check it.
//   sampler ps s0 addr wrap/wrap/clamp filter lin/lin/pt cmp never border 0,0,0,0 lod 0..max bias 0 aniso 1
//   blend a2c off dual off rt0 on src_alpha/inv_src_alpha add, one/zero add mask rgba
//   ds depth on write all less_equal; stencil off read ff write ff front keep/keep/keep always back keep/keep/keep always ref 0
//   rs cull back ccw off fill solid bias 0 slope 0 clamp 0 clip on scissor off

#pragma once

#include "d3d11_1.h" // D3D11_BLEND_DESC1 (and d3d11.h)

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>

namespace dxmt {

inline std::string
TraceStateNumber(double value) {
  char text[32];
  if (value >= 3.0e38)
    return "max";
  if (value <= -3.0e38)
    return "-max";
  std::snprintf(text, sizeof(text), "%g", value);
  return text;
}

inline std::string
TraceStateHex(unsigned value) {
  char text[16];
  std::snprintf(text, sizeof(text), "%x", value);
  return text;
}

inline const char *
TraceAddressMode(D3D11_TEXTURE_ADDRESS_MODE mode) {
  switch (mode) {
  case D3D11_TEXTURE_ADDRESS_WRAP: return "wrap";
  case D3D11_TEXTURE_ADDRESS_MIRROR: return "mirror";
  case D3D11_TEXTURE_ADDRESS_CLAMP: return "clamp";
  case D3D11_TEXTURE_ADDRESS_BORDER: return "border";
  case D3D11_TEXTURE_ADDRESS_MIRROR_ONCE: return "mirror_once";
  default: return "?";
  }
}

inline const char *
TraceComparison(D3D11_COMPARISON_FUNC func) {
  switch (func) {
  case D3D11_COMPARISON_NEVER: return "never";
  case D3D11_COMPARISON_LESS: return "less";
  case D3D11_COMPARISON_EQUAL: return "equal";
  case D3D11_COMPARISON_LESS_EQUAL: return "less_equal";
  case D3D11_COMPARISON_GREATER: return "greater";
  case D3D11_COMPARISON_NOT_EQUAL: return "not_equal";
  case D3D11_COMPARISON_GREATER_EQUAL: return "greater_equal";
  case D3D11_COMPARISON_ALWAYS: return "always";
  default: return "?";
  }
}

/* D3D11_FILTER's bits: mip linear 0x1, mag linear 0x4, min linear 0x10, anisotropic 0x40, and
   the reduction in bits 7-8 (0 standard, 1 comparison, 2 minimum, 3 maximum) */
inline std::string
TraceFilter(D3D11_FILTER filter) {
  unsigned bits = (unsigned)filter;
  std::string text = (bits & 0x40) ? std::string("aniso")
                                   : std::string(bits & 0x10 ? "lin" : "pt") + "/" + (bits & 0x4 ? "lin" : "pt") + "/" +
                                         (bits & 0x1 ? "lin" : "pt");
  switch ((bits >> 7) & 3) {
  case 1: text += " cmp"; break;
  case 2: text += " min"; break;
  case 3: text += " max"; break;
  default: break;
  }
  return text;
}

inline std::string
TraceSamplerState(const char *stage, unsigned slot, const D3D11_SAMPLER_DESC &desc) {
  return std::string("sampler ") + stage + " s" + std::to_string(slot) + " addr " +
         TraceAddressMode(desc.AddressU) + "/" + TraceAddressMode(desc.AddressV) + "/" +
         TraceAddressMode(desc.AddressW) + " filter " + TraceFilter(desc.Filter) + " cmp " +
         TraceComparison(desc.ComparisonFunc) + " border " + TraceStateNumber(desc.BorderColor[0]) + "," +
         TraceStateNumber(desc.BorderColor[1]) + "," + TraceStateNumber(desc.BorderColor[2]) + "," +
         TraceStateNumber(desc.BorderColor[3]) + " lod " + TraceStateNumber(desc.MinLOD) + ".." +
         TraceStateNumber(desc.MaxLOD) + " bias " + TraceStateNumber(desc.MipLODBias) + " aniso " +
         std::to_string(desc.MaxAnisotropy);
}

inline std::string
TraceSamplerNull(const char *stage, unsigned slot) {
  return std::string("sampler ") + stage + " s" + std::to_string(slot) + " null";
}

inline const char *
TraceBlendFactor(D3D11_BLEND blend) {
  switch (blend) {
  case D3D11_BLEND_ZERO: return "zero";
  case D3D11_BLEND_ONE: return "one";
  case D3D11_BLEND_SRC_COLOR: return "src_color";
  case D3D11_BLEND_INV_SRC_COLOR: return "inv_src_color";
  case D3D11_BLEND_SRC_ALPHA: return "src_alpha";
  case D3D11_BLEND_INV_SRC_ALPHA: return "inv_src_alpha";
  case D3D11_BLEND_DEST_ALPHA: return "dst_alpha";
  case D3D11_BLEND_INV_DEST_ALPHA: return "inv_dst_alpha";
  case D3D11_BLEND_DEST_COLOR: return "dst_color";
  case D3D11_BLEND_INV_DEST_COLOR: return "inv_dst_color";
  case D3D11_BLEND_SRC_ALPHA_SAT: return "src_alpha_sat";
  case D3D11_BLEND_BLEND_FACTOR: return "factor";
  case D3D11_BLEND_INV_BLEND_FACTOR: return "inv_factor";
  case D3D11_BLEND_SRC1_COLOR: return "src1_color";
  case D3D11_BLEND_INV_SRC1_COLOR: return "inv_src1_color";
  case D3D11_BLEND_SRC1_ALPHA: return "src1_alpha";
  case D3D11_BLEND_INV_SRC1_ALPHA: return "inv_src1_alpha";
  default: return "?";
  }
}

inline const char *
TraceBlendOp(D3D11_BLEND_OP op) {
  switch (op) {
  case D3D11_BLEND_OP_ADD: return "add";
  case D3D11_BLEND_OP_SUBTRACT: return "sub";
  case D3D11_BLEND_OP_REV_SUBTRACT: return "rev_sub";
  case D3D11_BLEND_OP_MIN: return "min";
  case D3D11_BLEND_OP_MAX: return "max";
  default: return "?";
  }
}

inline bool
TraceBlendIsDualSource(D3D11_BLEND blend) {
  return blend == D3D11_BLEND_SRC1_COLOR || blend == D3D11_BLEND_INV_SRC1_COLOR || blend == D3D11_BLEND_SRC1_ALPHA ||
         blend == D3D11_BLEND_INV_SRC1_ALPHA;
}

/* "rgba", "rg--", "----" */
inline std::string
TraceWriteMask(unsigned mask) {
  std::string text = "----";
  if (mask & D3D11_COLOR_WRITE_ENABLE_RED)
    text[0] = 'r';
  if (mask & D3D11_COLOR_WRITE_ENABLE_GREEN)
    text[1] = 'g';
  if (mask & D3D11_COLOR_WRITE_ENABLE_BLUE)
    text[2] = 'b';
  if (mask & D3D11_COLOR_WRITE_ENABLE_ALPHA)
    text[3] = 'a';
  return text;
}

/* The blend state for the first `render_targets` targets (one target when
   IndependentBlendEnable is off: all use RenderTarget[0]). */
inline std::string
TraceBlendState(const D3D11_BLEND_DESC1 &desc, unsigned render_targets) {
  bool dual = false;
  unsigned count = desc.IndependentBlendEnable ? std::max(render_targets, 1u) : 1u;
  if (count > 8)
    count = 8;
  std::string targets;
  for (unsigned i = 0; i < count; i++) {
    auto &rt = desc.RenderTarget[i];
    dual = dual || TraceBlendIsDualSource(rt.SrcBlend) || TraceBlendIsDualSource(rt.DestBlend) ||
           TraceBlendIsDualSource(rt.SrcBlendAlpha) || TraceBlendIsDualSource(rt.DestBlendAlpha);
    targets += (i ? "; rt" : " rt") + std::to_string(i);
    if (rt.BlendEnable)
      targets += std::string(" on ") + TraceBlendFactor(rt.SrcBlend) + "/" + TraceBlendFactor(rt.DestBlend) + " " +
                 TraceBlendOp(rt.BlendOp) + ", " + TraceBlendFactor(rt.SrcBlendAlpha) + "/" +
                 TraceBlendFactor(rt.DestBlendAlpha) + " " + TraceBlendOp(rt.BlendOpAlpha);
    else
      targets += " off";
    if (rt.LogicOpEnable)
      targets += " logic " + std::to_string((unsigned)rt.LogicOp);
    targets += " mask " + TraceWriteMask(rt.RenderTargetWriteMask);
  }
  return std::string("blend a2c ") + (desc.AlphaToCoverageEnable ? "on" : "off") + " dual " + (dual ? "on" : "off") +
         " independent " + (desc.IndependentBlendEnable ? "on" : "off") + targets;
}

inline const char *
TraceStencilOp(D3D11_STENCIL_OP op) {
  switch (op) {
  case D3D11_STENCIL_OP_KEEP: return "keep";
  case D3D11_STENCIL_OP_ZERO: return "zero";
  case D3D11_STENCIL_OP_REPLACE: return "replace";
  case D3D11_STENCIL_OP_INCR_SAT: return "incr_sat";
  case D3D11_STENCIL_OP_DECR_SAT: return "decr_sat";
  case D3D11_STENCIL_OP_INVERT: return "invert";
  case D3D11_STENCIL_OP_INCR: return "incr";
  case D3D11_STENCIL_OP_DECR: return "decr";
  default: return "?";
  }
}

/* "keep/keep/replace always": fail, depth-fail, pass, then the function */
inline std::string
TraceStencilFace(const D3D11_DEPTH_STENCILOP_DESC &face) {
  return std::string(TraceStencilOp(face.StencilFailOp)) + "/" + TraceStencilOp(face.StencilDepthFailOp) + "/" +
         TraceStencilOp(face.StencilPassOp) + " " + TraceComparison(face.StencilFunc);
}

inline std::string
TraceDepthStencilState(const D3D11_DEPTH_STENCIL_DESC &desc, unsigned stencil_ref) {
  return std::string("ds depth ") + (desc.DepthEnable ? "on" : "off") + " write " +
         (desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL ? "all" : "zero") + " " + TraceComparison(desc.DepthFunc) +
         "; stencil " + (desc.StencilEnable ? "on" : "off") + " read " + TraceStateHex(desc.StencilReadMask) +
         " write " + TraceStateHex(desc.StencilWriteMask) + " front " + TraceStencilFace(desc.FrontFace) + " back " +
         TraceStencilFace(desc.BackFace) + " ref " + std::to_string(stencil_ref);
}

/* Spec 05 clarification 44: which of SV_RenderTargetArrayIndex (bit 1) and SV_ViewportArrayIndex
   (bit 2) a DXBC shader's output signature (OSGN, OSG5 or OSG1 chunk) declares; 0 for neither or
   no output signature; -1 when the container can't be read. */
inline int
DxbcOutputWrites(const uint8_t *data, size_t size) {
  auto u32 = [&](size_t offset) -> uint32_t {
    return uint32_t(data[offset]) | uint32_t(data[offset + 1]) << 8 | uint32_t(data[offset + 2]) << 16 |
           uint32_t(data[offset + 3]) << 24;
  };
  if (!data || size < 32 || std::memcmp(data, "DXBC", 4) != 0)
    return -1;
  uint32_t chunks = u32(28);
  if (size < 32 + size_t(chunks) * 4)
    return -1;
  int bits = 0;
  for (uint32_t c = 0; c < chunks; c++) {
    size_t chunk = u32(32 + c * 4);
    if (chunk + 8 > size)
      return -1;
    size_t element_size = 0, sv_offset = 0;
    if (std::memcmp(data + chunk, "OSGN", 4) == 0) {
      element_size = 24; // name, index, system value, component type, register, masks
      sv_offset = 8;
    } else if (std::memcmp(data + chunk, "OSG5", 4) == 0) {
      element_size = 28; // stream first
      sv_offset = 12;
    } else if (std::memcmp(data + chunk, "OSG1", 4) == 0) {
      element_size = 32; // stream first, minimum precision last
      sv_offset = 12;
    } else {
      continue;
    }
    size_t body = chunk + 8, body_size = u32(chunk + 4);
    if (body + body_size > size || body_size < 8)
      return -1;
    uint32_t count = u32(body);
    if (8 + size_t(count) * element_size > body_size)
      return -1;
    for (uint32_t e = 0; e < count; e++) {
      uint32_t system_value = u32(body + 8 + e * element_size + sv_offset);
      if (system_value == D3D_NAME_RENDER_TARGET_ARRAY_INDEX)
        bits |= 1;
      else if (system_value == D3D_NAME_VIEWPORT_ARRAY_INDEX)
        bits |= 2;
    }
  }
  return bits;
}

/* Spec 05 clarification 49: a DXGI format's name without its DXGI_FORMAT_ prefix (values from
   Microsoft's DXGI_FORMAT documentation, 0-115); "format <n>" past those. */
inline std::string
TraceDxgiFormat(DXGI_FORMAT format) {
  static const char *const names[] = {
      "UNKNOWN", "R32G32B32A32_TYPELESS", "R32G32B32A32_FLOAT", "R32G32B32A32_UINT", "R32G32B32A32_SINT",
      "R32G32B32_TYPELESS", "R32G32B32_FLOAT", "R32G32B32_UINT", "R32G32B32_SINT", "R16G16B16A16_TYPELESS",
      "R16G16B16A16_FLOAT", "R16G16B16A16_UNORM", "R16G16B16A16_UINT", "R16G16B16A16_SNORM", "R16G16B16A16_SINT",
      "R32G32_TYPELESS", "R32G32_FLOAT", "R32G32_UINT", "R32G32_SINT", "R32G8X24_TYPELESS",
      "D32_FLOAT_S8X24_UINT", "R32_FLOAT_X8X24_TYPELESS", "X32_TYPELESS_G8X24_UINT", "R10G10B10A2_TYPELESS",
      "R10G10B10A2_UNORM", "R10G10B10A2_UINT", "R11G11B10_FLOAT", "R8G8B8A8_TYPELESS", "R8G8B8A8_UNORM",
      "R8G8B8A8_UNORM_SRGB", "R8G8B8A8_UINT", "R8G8B8A8_SNORM", "R8G8B8A8_SINT", "R16G16_TYPELESS",
      "R16G16_FLOAT", "R16G16_UNORM", "R16G16_UINT", "R16G16_SNORM", "R16G16_SINT", "R32_TYPELESS", "D32_FLOAT",
      "R32_FLOAT", "R32_UINT", "R32_SINT", "R24G8_TYPELESS", "D24_UNORM_S8_UINT", "R24_UNORM_X8_TYPELESS",
      "X24_TYPELESS_G8_UINT", "R8G8_TYPELESS", "R8G8_UNORM", "R8G8_UINT", "R8G8_SNORM", "R8G8_SINT",
      "R16_TYPELESS", "R16_FLOAT", "D16_UNORM", "R16_UNORM", "R16_UINT", "R16_SNORM", "R16_SINT", "R8_TYPELESS",
      "R8_UNORM", "R8_UINT", "R8_SNORM", "R8_SINT", "A8_UNORM", "R1_UNORM", "R9G9B9E5_SHAREDEXP",
      "R8G8_B8G8_UNORM", "G8R8_G8B8_UNORM", "BC1_TYPELESS", "BC1_UNORM", "BC1_UNORM_SRGB", "BC2_TYPELESS",
      "BC2_UNORM", "BC2_UNORM_SRGB", "BC3_TYPELESS", "BC3_UNORM", "BC3_UNORM_SRGB", "BC4_TYPELESS", "BC4_UNORM",
      "BC4_SNORM", "BC5_TYPELESS", "BC5_UNORM", "BC5_SNORM", "B5G6R5_UNORM", "B5G5R5A1_UNORM", "B8G8R8A8_UNORM",
      "B8G8R8X8_UNORM", "R10G10B10_XR_BIAS_A2_UNORM", "B8G8R8A8_TYPELESS", "B8G8R8A8_UNORM_SRGB",
      "B8G8R8X8_TYPELESS", "B8G8R8X8_UNORM_SRGB", "BC6H_TYPELESS", "BC6H_UF16", "BC6H_SF16", "BC7_TYPELESS",
      "BC7_UNORM", "BC7_UNORM_SRGB", "AYUV", "Y410", "Y416", "NV12", "P010", "P016", "420_OPAQUE", "YUY2", "Y210",
      "Y216", "NV11", "AI44", "IA44", "P8", "A8P8", "B4G4R4A4_UNORM",
  };
  static_assert(sizeof(names) / sizeof(names[0]) == 116, "DXGI formats 0-115");
  unsigned value = static_cast<unsigned>(format);
  if (value < sizeof(names) / sizeof(names[0]))
    return names[value];
  return "format " + std::to_string(value);
}

/* Spec 05 clarification 49: the kind of a shader resource view. The spec's seven, plus tex1d,
   tex1d-array, tex2d-ms and tex2d-ms-array for the dimensions it doesn't name. */
inline const char *
TraceSrvKind(const D3D11_SHADER_RESOURCE_VIEW_DESC &view) {
  switch (view.ViewDimension) {
  case D3D11_SRV_DIMENSION_BUFFER:
    return view.Format == DXGI_FORMAT_UNKNOWN ? "buffer-structured" : "buffer-typed";
  case D3D11_SRV_DIMENSION_BUFFEREX:
    if (view.BufferEx.Flags & D3D11_BUFFEREX_SRV_FLAG_RAW)
      return "buffer-raw";
    return view.Format == DXGI_FORMAT_UNKNOWN ? "buffer-structured" : "buffer-typed";
  case D3D11_SRV_DIMENSION_TEXTURE1D:
    return "tex1d";
  case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
    return "tex1d-array";
  case D3D11_SRV_DIMENSION_TEXTURE2D:
    return "tex2d";
  case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
    return "tex2d-array";
  case D3D11_SRV_DIMENSION_TEXTURE2DMS:
    return "tex2d-ms";
  case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
    return "tex2d-ms-array";
  case D3D11_SRV_DIMENSION_TEXTURE3D:
    return "tex3d";
  case D3D11_SRV_DIMENSION_TEXTURECUBE:
  case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
    return "cube";
  default:
    return "unknown";
  }
}

/* What a view's line needs from its resource: a texture's top-level size and mip count, or a
   buffer view's bytes per element (its byte length over its element count). */
struct TraceSrvResource {
  unsigned width = 0;
  unsigned height = 0;
  unsigned depth = 0;
  unsigned mips = 0;
  unsigned stride = 0;
};

/* "srv vs t3 tex2d-array R16G16B16A16_FLOAT 512x256x4 mips 1",
   "srv vs t0 buffer-structured UNKNOWN first 0 count 4096 stride 32". A texture's size is at the
   view's most detailed mip; its third number is an array view's slices, a cube array's cubes or
   a 3D view's depth. */
inline std::string
TraceSrvLine(const char *stage, unsigned slot, const D3D11_SHADER_RESOURCE_VIEW_DESC &view, const TraceSrvResource &res) {
  std::string line = std::string("srv ") + stage + " t" + std::to_string(slot) + " " + TraceSrvKind(view) + " " +
                     TraceDxgiFormat(view.Format) + " ";
  auto size = [&](unsigned most_detailed, unsigned levels, unsigned third) {
    unsigned shift = most_detailed < 32 ? most_detailed : 31;
    unsigned w = std::max(res.width >> shift, 1u), h = std::max(res.height >> shift, 1u);
    unsigned mips = levels == ~0u ? (res.mips > most_detailed ? res.mips - most_detailed : 0) : levels;
    std::string text = std::to_string(w) + "x" + std::to_string(h);
    if (third)
      text += "x" + std::to_string(third);
    return text + " mips " + std::to_string(mips);
  };
  switch (view.ViewDimension) {
  case D3D11_SRV_DIMENSION_BUFFER:
    return line + "first " + std::to_string(view.Buffer.FirstElement) + " count " +
           std::to_string(view.Buffer.NumElements) + " stride " + std::to_string(res.stride);
  case D3D11_SRV_DIMENSION_BUFFEREX:
    return line + "first " + std::to_string(view.BufferEx.FirstElement) + " count " +
           std::to_string(view.BufferEx.NumElements) + " stride " + std::to_string(res.stride);
  case D3D11_SRV_DIMENSION_TEXTURE1D:
    return line + size(view.Texture1D.MostDetailedMip, view.Texture1D.MipLevels, 0);
  case D3D11_SRV_DIMENSION_TEXTURE1DARRAY:
    return line + size(view.Texture1DArray.MostDetailedMip, view.Texture1DArray.MipLevels,
                       view.Texture1DArray.ArraySize);
  case D3D11_SRV_DIMENSION_TEXTURE2D:
    return line + size(view.Texture2D.MostDetailedMip, view.Texture2D.MipLevels, 0);
  case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
    return line + size(view.Texture2DArray.MostDetailedMip, view.Texture2DArray.MipLevels,
                       view.Texture2DArray.ArraySize);
  case D3D11_SRV_DIMENSION_TEXTURE2DMS:
    return line + size(0, 1, 0);
  case D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY:
    return line + size(0, 1, view.Texture2DMSArray.ArraySize);
  case D3D11_SRV_DIMENSION_TEXTURE3D: {
    unsigned shift = view.Texture3D.MostDetailedMip < 32 ? view.Texture3D.MostDetailedMip : 31;
    return line + size(view.Texture3D.MostDetailedMip, view.Texture3D.MipLevels, std::max(res.depth >> shift, 1u));
  }
  case D3D11_SRV_DIMENSION_TEXTURECUBE:
    return line + size(view.TextureCube.MostDetailedMip, view.TextureCube.MipLevels, 0);
  case D3D11_SRV_DIMENSION_TEXTURECUBEARRAY:
    return line + size(view.TextureCubeArray.MostDetailedMip, view.TextureCubeArray.MipLevels,
                       view.TextureCubeArray.NumCubes);
  default:
    return line + "?";
  }
}

inline std::string
TraceSrvNull(const char *stage, unsigned slot) {
  return std::string("srv ") + stage + " t" + std::to_string(slot) + " null";
}

/* "cb ps b2 4096 bytes first 16 count 64" (first and count in 16-byte constants, as bound) */
inline std::string
TraceConstantBufferLine(const char *stage, unsigned slot, uint64_t bytes, unsigned first, unsigned count) {
  return std::string("cb ") + stage + " b" + std::to_string(slot) + " " + std::to_string(bytes) + " bytes first " +
         std::to_string(first) + " count " + std::to_string(count);
}

inline std::string
TraceConstantBufferNull(const char *stage, unsigned slot) {
  return std::string("cb ") + stage + " b" + std::to_string(slot) + " null";
}

inline std::string
TraceRasterizerState(const D3D11_RASTERIZER_DESC &desc) {
  const char *cull = desc.CullMode == D3D11_CULL_NONE ? "none" : desc.CullMode == D3D11_CULL_FRONT ? "front" : "back";
  return std::string("rs cull ") + cull + " ccw " + (desc.FrontCounterClockwise ? "on" : "off") + " fill " +
         (desc.FillMode == D3D11_FILL_WIREFRAME ? "wire" : "solid") + " bias " + std::to_string(desc.DepthBias) +
         " slope " + TraceStateNumber(desc.SlopeScaledDepthBias) + " clamp " + TraceStateNumber(desc.DepthBiasClamp) +
         " clip " + (desc.DepthClipEnable ? "on" : "off") + " scissor " + (desc.ScissorEnable ? "on" : "off") +
         " msaa " + (desc.MultisampleEnable ? "on" : "off");
}

} // namespace dxmt
