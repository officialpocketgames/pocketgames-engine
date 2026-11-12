// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 30: the frame trace's state lines (dxmt/src/d3d11/d3d11_trace_state.hpp)
// from Direct3D 11 descriptions, and their deduplication within a pass (TraceAddDistinct, as
// ArgumentEncodingContext::traceState uses it).

#include "test_common.hpp"

// d3d11.h (mingw-directx-headers, upstream's copy) has an empty extern "C" struct
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextern-c-compat"
#endif
#include "d3d11_trace_state.hpp"
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include "dxmt_frame_trace.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace dxmt;

D3D11_SAMPLER_DESC
DefaultSampler() {
  D3D11_SAMPLER_DESC desc;
  std::memset(&desc, 0, sizeof(desc));
  desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  desc.MaxAnisotropy = 1;
  desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  desc.MinLOD = -3.402823466e+38f;
  desc.MaxLOD = 3.402823466e+38f;
  return desc;
}

void
TestSampler() {
  CHECK(TraceSamplerState("ps", 0, DefaultSampler()) ==
        "sampler ps s0 addr clamp/clamp/clamp filter lin/lin/lin cmp never border 0,0,0,0 lod -max..max bias 0 "
        "aniso 1");
  // a shadow sampler: border address, comparison filter, a border colour, a LOD clamp and bias
  D3D11_SAMPLER_DESC shadow = DefaultSampler();
  shadow.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
  shadow.AddressU = shadow.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
  shadow.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  shadow.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
  shadow.BorderColor[0] = shadow.BorderColor[1] = shadow.BorderColor[2] = shadow.BorderColor[3] = 1.0f;
  shadow.MinLOD = 0;
  shadow.MaxLOD = 4;
  shadow.MipLODBias = -0.5f;
  CHECK(TraceSamplerState("ps", 3, shadow) ==
        "sampler ps s3 addr border/border/wrap filter lin/lin/pt cmp cmp less_equal border 1,1,1,1 lod 0..4 bias "
        "-0.5 aniso 1");
  D3D11_SAMPLER_DESC aniso = DefaultSampler();
  aniso.Filter = D3D11_FILTER_ANISOTROPIC;
  aniso.MaxAnisotropy = 16;
  aniso.AddressU = D3D11_TEXTURE_ADDRESS_MIRROR;
  aniso.AddressV = D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
  CHECK(TraceSamplerState("cs", 1, aniso) ==
        "sampler cs s1 addr mirror/mirror_once/clamp filter aniso cmp never border 0,0,0,0 lod -max..max bias 0 "
        "aniso 16");
  CHECK(TraceFilter(D3D11_FILTER_MAXIMUM_MIN_MAG_MIP_POINT) == "pt/pt/pt max");
  CHECK(TraceFilter(D3D11_FILTER_MINIMUM_ANISOTROPIC) == "aniso min");
  CHECK(TraceSamplerNull("vs", 2) == "sampler vs s2 null");
}

D3D11_BLEND_DESC1
DefaultBlend() {
  D3D11_BLEND_DESC1 desc;
  std::memset(&desc, 0, sizeof(desc));
  for (auto &rt : desc.RenderTarget) {
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.LogicOp = D3D11_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
  }
  return desc;
}

void
TestBlend() {
  CHECK(TraceBlendState(DefaultBlend(), 1) == "blend a2c off dual off independent off rt0 off mask rgba");
  D3D11_BLEND_DESC1 alpha = DefaultBlend();
  alpha.RenderTarget[0].BlendEnable = TRUE;
  alpha.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
  alpha.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
  alpha.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN;
  // not independent: one target described, whatever the bound count
  CHECK(TraceBlendState(alpha, 4) ==
        "blend a2c off dual off independent off rt0 on src_alpha/inv_src_alpha add, one/zero add mask rg--");
  // independent, dual source, alpha-to-coverage, per bound target
  D3D11_BLEND_DESC1 dual = DefaultBlend();
  dual.AlphaToCoverageEnable = TRUE;
  dual.IndependentBlendEnable = TRUE;
  dual.RenderTarget[0].BlendEnable = TRUE;
  dual.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC1_COLOR;
  dual.RenderTarget[1].RenderTargetWriteMask = 0;
  CHECK(TraceBlendState(dual, 2) ==
        "blend a2c on dual on independent on rt0 on one/inv_src1_color add, one/zero add mask rgba; rt1 off mask ----");
}

void
TestDepthStencilAndRasterizer() {
  D3D11_DEPTH_STENCIL_DESC ds;
  std::memset(&ds, 0, sizeof(ds));
  ds.DepthEnable = TRUE;
  ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  ds.DepthFunc = D3D11_COMPARISON_LESS;
  ds.StencilReadMask = ds.StencilWriteMask = 0xff;
  ds.FrontFace = {D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_COMPARISON_ALWAYS};
  ds.BackFace = ds.FrontFace;
  CHECK(TraceDepthStencilState(ds, 0) == "ds depth on write all less; stencil off read ff write ff front "
                                         "keep/keep/keep always back keep/keep/keep always ref 0");
  ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
  ds.StencilEnable = TRUE;
  ds.StencilWriteMask = 0x80;
  ds.FrontFace = {D3D11_STENCIL_OP_ZERO, D3D11_STENCIL_OP_INCR, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_EQUAL};
  CHECK(TraceDepthStencilState(ds, 128) == "ds depth on write zero less; stencil on read ff write 80 front "
                                           "zero/incr/replace equal back keep/keep/keep always ref 128");

  D3D11_RASTERIZER_DESC rs;
  std::memset(&rs, 0, sizeof(rs));
  rs.FillMode = D3D11_FILL_SOLID;
  rs.CullMode = D3D11_CULL_BACK;
  rs.DepthClipEnable = TRUE;
  CHECK(TraceRasterizerState(rs) == "rs cull back ccw off fill solid bias 0 slope 0 clamp 0 clip on scissor off msaa off");
  rs.CullMode = D3D11_CULL_NONE;
  rs.FrontCounterClockwise = TRUE;
  rs.DepthBias = -100;
  rs.SlopeScaledDepthBias = 1.5f;
  rs.DepthBiasClamp = 0.01f;
  rs.DepthClipEnable = FALSE;
  rs.ScissorEnable = TRUE;
  CHECK(TraceRasterizerState(rs) ==
        "rs cull none ccw on fill solid bias -100 slope 1.5 clamp 0.01 clip off scissor on msaa off");
}

void
TestDistinctPerPass() {
  // two draws with the same state and one with another: three state lines become two
  std::vector<std::string> pass;
  D3D11_RASTERIZER_DESC rs;
  std::memset(&rs, 0, sizeof(rs));
  rs.FillMode = D3D11_FILL_SOLID;
  rs.CullMode = D3D11_CULL_BACK;
  TraceAddDistinct(pass, TraceRasterizerState(rs));
  TraceAddDistinct(pass, TraceRasterizerState(rs));
  rs.CullMode = D3D11_CULL_NONE;
  TraceAddDistinct(pass, TraceRasterizerState(rs));
  TraceAddDistinct(pass, TraceSamplerState("ps", 0, DefaultSampler()));
  TraceAddDistinct(pass, TraceSamplerState("ps", 0, DefaultSampler()));
  CHECK_EQ(pass.size(), 3);
  CHECK(pass[0].rfind("rs cull back", 0) == 0); // first-use order
  CHECK(pass[1].rfind("rs cull none", 0) == 0);
  CHECK(pass[2].rfind("sampler ps s0", 0) == 0);
  // a pass's line with its prefix stays within the trace's line limit
  for (auto &line : pass)
    CHECK(("dxmt-frame 1800: pass 14 " + line).size() <= kFrameTraceMaxLineBytes);
}

/* A DXBC container with one signature chunk (OSGN, OSG5 or OSG1) listing `values` as system
   values, as the compiler lays it out. */
std::vector<uint8_t>
Dxbc(const char *fourcc, std::vector<uint32_t> values) {
  size_t element = std::strcmp(fourcc, "OSGN") == 0 ? 24 : std::strcmp(fourcc, "OSG5") == 0 ? 28 : 32;
  size_t sv_offset = std::strcmp(fourcc, "OSGN") == 0 ? 8 : 12;
  std::vector<uint8_t> blob(32 + 4 + 8 + 8 + values.size() * element, 0);
  auto put = [&](size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++)
      blob[at + i] = uint8_t(v >> (8 * i));
  };
  std::memcpy(blob.data(), "DXBC", 4);
  put(24, 1);              // version
  put(28, 1);              // one chunk
  put(32, 36);             // at offset 36
  std::memcpy(blob.data() + 36, fourcc, 4);
  put(40, uint32_t(8 + values.size() * element));
  put(44, uint32_t(values.size()));
  put(48, 8);
  for (size_t e = 0; e < values.size(); e++)
    put(52 + e * element + sv_offset, values[e]);
  return blob;
}

void
TestOutputSignature() {
  // clarification 44: SV_RenderTargetArrayIndex (4) and SV_ViewportArrayIndex (5) in the outputs
  auto position_only = Dxbc("OSGN", {1 /* SV_Position */, 0});
  CHECK_EQ(DxbcOutputWrites(position_only.data(), position_only.size()), 0);
  auto rt = Dxbc("OSGN", {1, 0, 4});
  CHECK_EQ(DxbcOutputWrites(rt.data(), rt.size()), 1);
  auto vp = Dxbc("OSG5", {1, 5}); // a geometry shader's, with streams
  CHECK_EQ(DxbcOutputWrites(vp.data(), vp.size()), 2);
  auto both = Dxbc("OSG1", {4, 5, 1});
  CHECK_EQ(DxbcOutputWrites(both.data(), both.size()), 3);
  // an input signature isn't looked at
  auto input = Dxbc("ISGN", {4});
  CHECK_EQ(DxbcOutputWrites(input.data(), input.size()), 0);
  // not a container, cut short, a chunk past the end: unknown
  uint8_t junk[40] = {'D', 'X', 'B', 'X'};
  CHECK_EQ(DxbcOutputWrites(junk, sizeof(junk)), -1);
  CHECK_EQ(DxbcOutputWrites(rt.data(), 20), -1);
  auto cut = rt;
  cut.resize(50);
  CHECK_EQ(DxbcOutputWrites(cut.data(), cut.size()), -1);
  CHECK_EQ(DxbcOutputWrites(nullptr, 0), -1);
}

} // namespace

int
main() {
  TestOutputSignature();
  TestSampler();
  TestBlend();
  TestDepthStencilAndRasterizer();
  TestDistinctPerPass();
  return TestSummary("test_trace_state");
}
