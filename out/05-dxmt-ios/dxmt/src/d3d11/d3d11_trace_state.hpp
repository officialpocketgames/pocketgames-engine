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
