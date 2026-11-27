// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarification 9 (Part D):
//   - The vertex element conversion at TEXCOORD usage: a vs_3_0 with dcl_texcoord6 and
//     dcl_texcoord7 (the real DXSO decoder walks it) gets a UBYTE4N element at TEXCOORD6 and a
//     D3DCOLOR element at TEXCOORD7 in its declared registers (VsInputRegister, what the device
//     calls), with the fetch format from the type alone, and the modelled shader fetch gives
//     D3D9's values: D3DCOLOR 0xAARRGGBB as (R, G, B, A), UBYTE4N bytes as x, y, z, w. The same
//     element at COLOR0 or TEXCOORD0 converts the same.
//   - The trace line's fields (dxmt/src/d3d9/d3d9_frame_trace.hpp): the draw (both kinds, UP,
//     instances, the shaders' hashes and versions, ffp, the declaration's number), the textures
//     (bound, unbound but sampled, bound but unused, cube and volume sizes, vertex samplers),
//     the output state (COLORWRITEENABLE 0-3, blend with and without the separate alpha, alpha
//     test, z, stencil with both faces, sRGB write, fog), the bool and int constants; the render
//     state numbers against d3d9.h.
//   - The declaration numbers the trace prints: the same for equal elements, the first 64 logged.

#include "test_common.hpp"

#include "d3d9_frame_trace.hpp"
#include "d3d9_vertex_input.hpp"
// The DXSO decoder (airconv, the code the device's shader scan runs) compares a signed register
// number with an unsigned count, which gcc's -Wsign-compare flags; it isn't the code under test.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#include "dxso_decoder.hpp"
#pragma GCC diagnostic pop

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace dxmt;
using namespace dxmt::d9trace;

#define CHECK_STR(a, b)                                                                                                \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    const std::string sa_ = (a), sb_ = (b);                                                                            \
    if (sa_ != sb_) {                                                                                                  \
      g_failures++;                                                                                                    \
      std::printf("FAIL %s:%d:\n  got  %s\n  want %s\n", __FILE__, __LINE__, sa_.c_str(), sb_.c_str());               \
    }                                                                                                                  \
  } while (0)

/* the render-state numbers the lines read are d3d9types.h's */
static_assert(kRsZEnable == D3DRS_ZENABLE && kRsZWriteEnable == D3DRS_ZWRITEENABLE && kRsZFunc == D3DRS_ZFUNC);
static_assert(kRsAlphaTestEnable == D3DRS_ALPHATESTENABLE && kRsAlphaRef == D3DRS_ALPHAREF &&
              kRsAlphaFunc == D3DRS_ALPHAFUNC);
static_assert(kRsSrcBlend == D3DRS_SRCBLEND && kRsDestBlend == D3DRS_DESTBLEND && kRsBlendOp == D3DRS_BLENDOP &&
              kRsAlphaBlendEnable == D3DRS_ALPHABLENDENABLE && kRsBlendFactor == D3DRS_BLENDFACTOR);
static_assert(kRsSeparateAlphaBlendEnable == D3DRS_SEPARATEALPHABLENDENABLE &&
              kRsSrcBlendAlpha == D3DRS_SRCBLENDALPHA && kRsDestBlendAlpha == D3DRS_DESTBLENDALPHA &&
              kRsBlendOpAlpha == D3DRS_BLENDOPALPHA);
static_assert(kRsFogEnable == D3DRS_FOGENABLE && kRsFogColor == D3DRS_FOGCOLOR &&
              kRsFogTableMode == D3DRS_FOGTABLEMODE && kRsFogVertexMode == D3DRS_FOGVERTEXMODE);
static_assert(kRsStencilEnable == D3DRS_STENCILENABLE && kRsStencilFail == D3DRS_STENCILFAIL &&
              kRsStencilZFail == D3DRS_STENCILZFAIL && kRsStencilPass == D3DRS_STENCILPASS &&
              kRsStencilFunc == D3DRS_STENCILFUNC && kRsStencilRef == D3DRS_STENCILREF &&
              kRsStencilMask == D3DRS_STENCILMASK && kRsStencilWriteMask == D3DRS_STENCILWRITEMASK);
static_assert(kRsTwoSidedStencilMode == D3DRS_TWOSIDEDSTENCILMODE && kRsCcwStencilFail == D3DRS_CCW_STENCILFAIL &&
              kRsCcwStencilZFail == D3DRS_CCW_STENCILZFAIL && kRsCcwStencilPass == D3DRS_CCW_STENCILPASS &&
              kRsCcwStencilFunc == D3DRS_CCW_STENCILFUNC);
static_assert(kRsColorWriteEnable == D3DRS_COLORWRITEENABLE && kRsColorWriteEnable1 == D3DRS_COLORWRITEENABLE1 &&
              kRsColorWriteEnable2 == D3DRS_COLORWRITEENABLE2 && kRsColorWriteEnable3 == D3DRS_COLORWRITEENABLE3);
static_assert(kRsSrgbWriteEnable == D3DRS_SRGBWRITEENABLE);
static_assert(kPixelSamplerSlots == D3DVERTEXTEXTURESAMPLER0 - D3DDMAPSAMPLER - 1 + 16);

/* ---- the elements at TEXCOORD6 / TEXCOORD7 ---- */

/* SM2+ instruction token for dcl: opcode 31, two parameter tokens */
constexpr uint32_t kDcl = 0x0200001Fu;
/* the dcl's usage token: bit 31, usage in bits 0-3, index in bits 16-19 */
constexpr uint32_t
Usage(uint32_t usage, uint32_t index) {
  return 0x80000000u | usage | (index << 16);
}
/* an input register v<n>, all four components: register type 1 in bits 28-30, mask in 16-19 */
constexpr uint32_t
Input(uint32_t n) {
  return 0x80000000u | (1u << 28) | (0xFu << 16) | n;
}

/* what the device makes of one element for a bound vertex shader: the register it feeds
   (VsInputRegister) and the fetch format (to_mtl_attr_format, from the type only) */
struct Fed {
  int reg;
  uint32_t format;
};
Fed
Feed(const DxsoShaderMetadata &md, const D3DVERTEXELEMENT9 &e) {
  return {VsInputRegister(md.dcls, e.Usage, e.UsageIndex), to_mtl_attr_format(e.Type)};
}

bool
Near(const float got[4], float x, float y, float z, float w) {
  const float want[4] = {x, y, z, w};
  bool ok = true;
  for (int i = 0; i < 4; i++)
    ok &= std::fabs(got[i] - want[i]) <= 1e-6f;
  if (!ok)
    std::printf("  got (%g, %g, %g, %g), want (%g, %g, %g, %g)\n", got[0], got[1], got[2], got[3], x, y, z, w);
  return ok;
}

void
TestElementsAtTexcoord() {
  // vs_3_0: dcl_position v0, dcl_texcoord6 v3, dcl_texcoord7 v9, dcl_color0 v1
  const uint32_t code[] = {
      0xFFFE0300u,
      kDcl, Usage(D3DDECLUSAGE_POSITION, 0), Input(0),
      kDcl, Usage(D3DDECLUSAGE_TEXCOORD, 6), Input(3),
      kDcl, Usage(D3DDECLUSAGE_TEXCOORD, 7), Input(9),
      kDcl, Usage(D3DDECLUSAGE_COLOR, 0),    Input(1),
      0x0000FFFFu,
  };
  const uint32_t count = sizeof(code) / sizeof(code[0]);
  auto header = parse_dxso_header(code, count);
  CHECK(header.has_value());
  if (!header)
    return;
  auto md = walk_dxso_shader(code, count, *header);
  CHECK(md.has_value());
  if (!md)
    return;
  CHECK_EQ(md->dcls.size(), 4);

  // the level's two extra elements, as clarification 9 describes them
  const D3DVERTEXELEMENT9 ubyte4n_tc6 = {0, 32, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 6};
  const D3DVERTEXELEMENT9 color_tc7 = {0, 36, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 7};
  Fed tc6 = Feed(*md, ubyte4n_tc6), tc7 = Feed(*md, color_tc7);
  CHECK_EQ(tc6.reg, 3); // the register dcl_texcoord6 names, not the usage index
  CHECK_EQ(tc7.reg, 9);
  CHECK_EQ(tc6.format, mtl_vertex_format::UChar4Normalized);
  CHECK_EQ(tc7.format, mtl_vertex_format::UChar4Normalized_BGRA);
  // TEXCOORD indices the shader doesn't declare, and other usages, feed nothing
  CHECK_EQ(VsInputRegister(md->dcls, D3DDECLUSAGE_TEXCOORD, 5), -1);
  CHECK_EQ(VsInputRegister(md->dcls, D3DDECLUSAGE_TEXCOORD, 0), -1);
  CHECK_EQ(VsInputRegister(md->dcls, D3DDECLUSAGE_COLOR, 6), -1);
  CHECK_EQ(VsInputRegister(md->dcls, D3DDECLUSAGE_POSITION, 0), 0);
  CHECK_EQ(VsInputRegister(md->dcls, D3DDECLUSAGE_COLOR, 0), 1);

  // the values the shader reads. Bytes in memory, little-endian.
  // D3DCOLOR 0x80FF4020 (A 0x80, R 0xFF, G 0x40, B 0x20): (R, G, B, A)
  const uint8_t color_bytes[4] = {0x20, 0x40, 0xFF, 0x80};
  // UBYTE4N bytes 05 00 FF 80: (5, 0, 255, 128) / 255
  const uint8_t ubyte_bytes[4] = {0x05, 0x00, 0xFF, 0x80};
  float got[4], want[4];
  ShaderFetchModel(tc7.format, color_bytes, got);
  CHECK(Near(got, 1.0f, 64 / 255.0f, 32 / 255.0f, 128 / 255.0f));
  D3D9Decode(D3DDECLTYPE_D3DCOLOR, color_bytes, want);
  CHECK(Near(got, want[0], want[1], want[2], want[3]));
  ShaderFetchModel(tc6.format, ubyte_bytes, got);
  CHECK(Near(got, 5 / 255.0f, 0, 1, 128 / 255.0f));
  D3D9Decode(D3DDECLTYPE_UBYTE4N, ubyte_bytes, want);
  CHECK(Near(got, want[0], want[1], want[2], want[3]));
  // a zero D3DCOLOR is black with zero alpha; white is (1, 1, 1, 1)
  const uint8_t black[4] = {0, 0, 0, 0}, white[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  ShaderFetchModel(tc7.format, black, got);
  CHECK(Near(got, 0, 0, 0, 0));
  ShaderFetchModel(tc7.format, white, got);
  CHECK(Near(got, 1, 1, 1, 1));

  // the same element at COLOR0, TEXCOORD0, TEXCOORD6 or TEXCOORD7 gets the same format
  for (uint32_t type : {uint32_t(D3DDECLTYPE_D3DCOLOR), uint32_t(D3DDECLTYPE_UBYTE4N)})
    for (uint8_t usage : {uint8_t(D3DDECLUSAGE_COLOR), uint8_t(D3DDECLUSAGE_TEXCOORD)})
      for (uint8_t index : {uint8_t(0), uint8_t(6), uint8_t(7)}) {
        const D3DVERTEXELEMENT9 e = {0, 0, uint8_t(type), D3DDECLMETHOD_DEFAULT, usage, index};
        CHECK_EQ(Feed(*md, e).format, to_mtl_attr_format(type));
      }
}

/* ---- the trace line ---- */

struct States {
  uint32_t rs[kRenderStateCount] = {};
  States() {
    // Direct3D 9's defaults for the states the line reads
    rs[kRsZEnable] = 1;
    rs[kRsZWriteEnable] = 1;
    rs[kRsZFunc] = 4; // LESSEQUAL
    rs[kRsSrcBlend] = 2;
    rs[kRsDestBlend] = 1;
    rs[kRsBlendOp] = 1;
    rs[kRsSrcBlendAlpha] = 2;
    rs[kRsDestBlendAlpha] = 1;
    rs[kRsBlendOpAlpha] = 1;
    rs[kRsAlphaFunc] = 8; // ALWAYS
    rs[kRsStencilFunc] = 8;
    rs[kRsStencilFail] = rs[kRsStencilZFail] = rs[kRsStencilPass] = 1;
    rs[kRsCcwStencilFunc] = 8;
    rs[kRsCcwStencilFail] = rs[kRsCcwStencilZFail] = rs[kRsCcwStencilPass] = 1;
    rs[kRsStencilMask] = rs[kRsStencilWriteMask] = 0xffffffffu;
    rs[kRsColorWriteEnable] = rs[kRsColorWriteEnable1] = rs[kRsColorWriteEnable2] = rs[kRsColorWriteEnable3] = 0xf;
    rs[kRsBlendFactor] = 0xffffffffu;
  }
};

void
TestDrawLine() {
  TraceDraw d;
  d.indexed = true;
  d.primitive = 4;
  d.count = 300;
  d.start = 12;
  d.base = -4;
  d.vs = {true, 0x0123456789abcdefull, 0xFFFE0300u};
  d.ps = {true, 0xfedcba9876543210ull, 0xFFFF0300u};
  d.decl = 7;
  CHECK_STR(DrawLine(d), "draw indexed trianglelist indices 300 start 12 base -4 instances 1; vs 0123456789abcdef "
                         "vs_3_0; ps fedcba9876543210 ps_3_0; decl 7");
  // non-indexed, UP, instanced, fixed function, no number; the hash keeps its leading zeros
  TraceDraw u;
  u.up = true;
  u.primitive = 5;
  u.count = 4;
  u.instances = 3;
  u.ps = {true, 0x42, 0xFFFF0104u};
  CHECK_STR(DrawLine(u), "draw up trianglestrip vertices 4 start 0 instances 3; vs ffp; ps 0000000000000042 ps_1_4; "
                         "decl -");
  u.primitive = 9;
  CHECK(DrawLine(u).find("draw up primitive 9 vertices") == 0);
}

void
TestTexLine() {
  TraceDraw d;
  CHECK_STR(TexLine(d), "tex -");
  TraceTexture t0;
  t0.slot = 0, t0.bound = true, t0.format = 0x35545844u /* DXT5 */, t0.type = 3, t0.width = 1024, t0.height = 512;
  t0.depth = 1, t0.levels = 11;
  TraceTexture t4;
  t4.slot = 4, t4.bound = true, t4.format = 21 /* A8R8G8B8 */, t4.type = 5, t4.width = 256, t4.height = 256;
  t4.depth = 1, t4.levels = 9;
  TraceTexture t6;
  t6.slot = 6, t6.bound = true, t6.format = 50 /* L8 */, t6.type = 4, t6.width = 32, t6.height = 16, t6.depth = 8;
  t6.levels = 1;
  TraceTexture v0;
  v0.slot = 16, v0.bound = true, v0.format = 114 /* R32F */, v0.type = 3, v0.width = 64, v0.height = 64;
  v0.depth = 1, v0.levels = 1;
  d.textures = {t0, t4, t6, v0};
  // the shaders' declared samplers unknown (ps_1_x / fixed function): every bound slot, no marks
  CHECK_STR(TexLine(d), "tex s0 DXT5 1024x512 2d mips 11, s4 A8R8G8B8 256x256 cube mips 9, s6 L8 32x16x8 3d mips 1, "
                        "vs0 R32F 64x64 2d mips 1");
  // declared s0, s1 and s6 and vs0: s1 samples nothing bound, s4 is bound but unused
  d.sampled_known = true;
  d.sampled = (1u << 0) | (1u << 1) | (1u << 6) | (1u << 16);
  CHECK_STR(TexLine(d), "tex s0 DXT5 1024x512 2d mips 11, s1 unbound, s4 A8R8G8B8 256x256 cube mips 9 unused, s6 L8 "
                        "32x16x8 3d mips 1, vs0 R32F 64x64 2d mips 1");
  // nothing bound, two declared
  TraceDraw e;
  e.sampled_known = true;
  e.sampled = (1u << 2) | (1u << 19);
  CHECK_STR(TexLine(e), "tex s2 unbound, vs3 unbound");
}

void
TestOutputLine() {
  States s;
  CHECK_STR(OutputLine(s.rs), "output colorwrite rgba,rgba,rgba,rgba; blend off; alphatest off; z on func LESSEQUAL "
                              "write on; stencil off; srgbwrite off; fog off");
  // the channels each target writes
  s.rs[kRsColorWriteEnable] = 0x7;
  s.rs[kRsColorWriteEnable1] = 0x8;
  s.rs[kRsColorWriteEnable2] = 0x5;
  s.rs[kRsColorWriteEnable3] = 0;
  // blend, then with the separate alpha blend
  s.rs[kRsAlphaBlendEnable] = 1;
  s.rs[kRsSrcBlend] = 5;  // SRCALPHA
  s.rs[kRsDestBlend] = 6; // INVSRCALPHA
  s.rs[kRsBlendFactor] = 0x80ff0000u;
  // alpha test, z off, stencil on one face
  s.rs[kRsAlphaTestEnable] = 1;
  s.rs[kRsAlphaFunc] = 5; // GREATER
  s.rs[kRsAlphaRef] = 0x180; // only the low 8 bits count
  s.rs[kRsZEnable] = 0;
  s.rs[kRsZWriteEnable] = 0;
  s.rs[kRsStencilEnable] = 1;
  s.rs[kRsStencilFunc] = 3; // EQUAL
  s.rs[kRsStencilRef] = 1;
  s.rs[kRsStencilMask] = 0xff;
  s.rs[kRsStencilPass] = 3; // REPLACE
  CHECK_STR(OutputLine(s.rs), "output colorwrite rgb,a,rb,-; blend SRCALPHA INVSRCALPHA ADD factor 80ff0000; "
                              "alphatest GREATER 128; z off func LESSEQUAL write off; stencil func EQUAL ref 1 mask ff "
                              "write ffffffff fail KEEP zfail KEEP pass REPLACE; srgbwrite off; fog off");
  // separate alpha, w-buffer, two-sided stencil, sRGB write, fog
  s.rs[kRsSeparateAlphaBlendEnable] = 1;
  s.rs[kRsSrcBlendAlpha] = 2;  // ONE
  s.rs[kRsDestBlendAlpha] = 1; // ZERO
  s.rs[kRsBlendOpAlpha] = 5;   // MAX
  s.rs[kRsZEnable] = 2;
  s.rs[kRsTwoSidedStencilMode] = 1;
  s.rs[kRsCcwStencilFunc] = 8;
  s.rs[kRsCcwStencilZFail] = 8; // DECR
  s.rs[kRsSrgbWriteEnable] = 1;
  s.rs[kRsFogEnable] = 1;
  s.rs[kRsFogTableMode] = 3;  // LINEAR
  s.rs[kRsFogVertexMode] = 0; // NONE
  s.rs[kRsFogColor] = 0xff000000u;
  const std::string line = OutputLine(s.rs);
  CHECK(line.find("; blend SRCALPHA INVSRCALPHA ADD, alpha ONE ZERO MAX factor 80ff0000;") != std::string::npos);
  CHECK(line.find("; z usew func LESSEQUAL write off;") != std::string::npos);
  CHECK(line.find("pass REPLACE, ccw func ALWAYS ref 1 mask ff write ffffffff fail KEEP zfail DECR pass KEEP;") !=
        std::string::npos);
  CHECK(line.find("; srgbwrite on; fog on table LINEAR vertex NONE color ff000000") != std::string::npos);
  // numbers outside the enums are printed as they are
  s.rs[kRsSrcBlend] = 99;
  s.rs[kRsAlphaFunc] = 0;
  CHECK(OutputLine(s.rs).find("; blend 99 INVSRCALPHA ADD") != std::string::npos);
  CHECK(OutputLine(s.rs).find("; alphatest 0 128;") != std::string::npos);
}

void
TestConstsLine() {
  TraceDraw d;
  int32_t vs_b[kBoolConsts] = {}, ps_b[kBoolConsts] = {};
  int32_t vs_i[kIntConsts][4] = {}, ps_i[kIntConsts][4] = {};
  d.vs_b = vs_b, d.ps_b = ps_b, d.vs_i = vs_i, d.ps_i = ps_i;
  // no shaders: nothing to list
  CHECK_STR(ConstsLine(d), "consts vs -; ps -");
  d.vs.bound = d.ps.bound = true;
  CHECK_STR(ConstsLine(d), "consts vs b 0000 i -; ps b 0000 i -");
  vs_b[0] = 1, vs_b[1] = 1, vs_b[15] = 7; // any non-zero BOOL is true
  vs_i[0][0] = 4, vs_i[0][2] = 1;
  vs_i[3][0] = -1, vs_i[3][1] = 2, vs_i[3][2] = 3, vs_i[3][3] = 4;
  ps_b[4] = 1;
  ps_i[15][3] = 255;
  CHECK_STR(ConstsLine(d), "consts vs b 8003 i0 4,0,1,0 i3 -1,2,3,4; ps b 0010 i15 0,0,0,255");
}

void
TestDrawLines() {
  States s;
  int32_t b[kBoolConsts] = {};
  int32_t i[kIntConsts][4] = {};
  TraceDraw d;
  d.primitive = 4;
  d.count = 3;
  d.vs = {true, 1, 0xFFFE0200u};
  d.ps = {true, 2, 0xFFFF0200u};
  d.decl = 2;
  d.rs = s.rs;
  d.vs_b = d.ps_b = b;
  d.vs_i = d.ps_i = i;
  // four lines, the draw's first, '\n'-separated as the encoder's traceDraw takes them
  CHECK_STR(DrawLines(d), "draw trianglelist vertices 3 start 0 instances 1; vs 0000000000000001 vs_2_0; ps "
                          "0000000000000002 ps_2_0; decl 2\n"
                          "tex -\n"
                          "output colorwrite rgba,rgba,rgba,rgba; blend off; alphatest off; z on func LESSEQUAL write "
                          "on; stencil off; srgbwrite off; fog off\n"
                          "consts vs b 0000 i -; ps b 0000 i -");
}

/* The declaration numbers: equal elements, equal numbers; the first 64 logged, all numbered up
   to kMaxNumbered */
void
TestDeclarationNumbers() {
  D3DVERTEXELEMENT9 decl[] = {
      {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 32, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 6},
      {0, 36, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 7},
      {0xFF, 0, D3DDECLTYPE_UNUSED, 0, 0, 0},
  };
  DeclarationLog log;
  auto first = log.number(decl, 4);
  CHECK_EQ(first.number, 1);
  CHECK(first.log);
  auto again = log.number(decl, 4);
  CHECK_EQ(again.number, 1); // the same elements: the same number, not logged again
  CHECK(!again.log);
  unsigned logged = 1;
  for (uint16_t offset = 100; offset < 200; offset++) {
    decl[0].Offset = offset;
    auto n = log.number(decl, 4);
    CHECK_EQ(n.number, offset - 100 + 2);
    logged += n.log;
  }
  CHECK_EQ(logged, DeclarationLog::kMaxLogged);
  decl[0].Offset = 150;
  CHECK_EQ(log.number(decl, 4).number, 52); // still its number past the logged ones
  CHECK_EQ(log.take(decl, 4), 0u);
}

} // namespace

int
main() {
  TestElementsAtTexcoord();
  TestDrawLine();
  TestTexLine();
  TestOutputLine();
  TestConstsLine();
  TestDrawLines();
  TestDeclarationNumbers();
  return TestSummary("test_d3d9_frame_trace");
}
