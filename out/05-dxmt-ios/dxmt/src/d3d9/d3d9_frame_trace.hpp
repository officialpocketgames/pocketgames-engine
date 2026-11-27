// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 clarification 9: a Direct3D 9 draw's lines in the frame trace (spec 05 clarifications 21
// and 44: trace.txt, one entry per draw in its render pass), so a pass that draws black colour
// shows what each draw had. The encoder puts "pass <n> draw <k>" in front of each line:
//   draw indexed trianglelist indices 300 start 0 base 0 instances 1; vs 0123456789abcdef vs_3_0;
//     ps fedcba9876543210 ps_3_0; decl 7
//   tex s0 DXT5 1024x1024 2d mips 11, s1 unbound, s4 A8R8G8B8 64x64 2d mips 1 unused, vs0 R32F 64x64 2d mips 1
//   output colorwrite rgba,rgba,-,-; blend SRCALPHA INVSRCALPHA ADD factor ffffffff;
//     alphatest GREATER 128; z on func LESSEQUAL write on; stencil off; srgbwrite off; fog off
//   consts vs b 0003 i0 4,0,1,0; ps b 0000 i -
// (each is one line in the trace; wrapped here).
//   - the shaders: bytecode_hash (d3d9_shader.cpp), the 16 hex digits in the compile dumps' names
//     (DXMT_DUMP_PATH: <vs|ps>_<major>_<minor>_<hash>.bin), and the version token; "ffp" for none;
//   - decl: the declaration's number in the `[d3d9] vertex declaration <n>` log (clarification 6);
//   - tex: every bound sampler slot, and every slot the shaders declare a sampler for (`unbound`
//     when nothing is bound there: it samples as zero); `unused` for a bound slot the shaders
//     don't declare (shader model 2 and up; ps_1_x declares none, so nothing is marked);
//   - output: COLORWRITEENABLE 0-3 as the channels written, blend (with the separate alpha blend
//     when on), alpha test, ZENABLE / ZFUNC / ZWRITEENABLE, stencil (both faces when two-sided),
//     and, as they also decide what colour reaches the target, SRGBWRITEENABLE and fog;
//   - consts: b0-b15 as a mask (bit k for b<k>), and every i<k> that isn't 0,0,0,0.
//
// Pure: no Windows or Metal types, so the host test (tests/test_d3d9_frame_trace.cpp) builds it as
// it is. The D3D enums are plain numbers (Microsoft's d3d9types.h); the test checks them against
// d3d9.h.

#pragma once

#include "d3d9_fail_log.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dxmt::d9trace {

/* D3DRENDERSTATETYPE values the lines read (d3d9types.h) */
constexpr unsigned kRsZEnable = 7;
constexpr unsigned kRsZWriteEnable = 14;
constexpr unsigned kRsAlphaTestEnable = 15;
constexpr unsigned kRsSrcBlend = 19;
constexpr unsigned kRsDestBlend = 20;
constexpr unsigned kRsZFunc = 23;
constexpr unsigned kRsAlphaRef = 24;
constexpr unsigned kRsAlphaFunc = 25;
constexpr unsigned kRsAlphaBlendEnable = 27;
constexpr unsigned kRsFogEnable = 28;
constexpr unsigned kRsFogColor = 34;
constexpr unsigned kRsFogTableMode = 35;
constexpr unsigned kRsStencilEnable = 52;
constexpr unsigned kRsStencilFail = 53;
constexpr unsigned kRsStencilZFail = 54;
constexpr unsigned kRsStencilPass = 55;
constexpr unsigned kRsStencilFunc = 56;
constexpr unsigned kRsStencilRef = 57;
constexpr unsigned kRsStencilMask = 58;
constexpr unsigned kRsStencilWriteMask = 59;
constexpr unsigned kRsFogVertexMode = 140;
constexpr unsigned kRsColorWriteEnable = 168;
constexpr unsigned kRsBlendOp = 171;
constexpr unsigned kRsTwoSidedStencilMode = 185;
constexpr unsigned kRsCcwStencilFail = 186;
constexpr unsigned kRsCcwStencilZFail = 187;
constexpr unsigned kRsCcwStencilPass = 188;
constexpr unsigned kRsCcwStencilFunc = 189;
constexpr unsigned kRsColorWriteEnable1 = 190;
constexpr unsigned kRsColorWriteEnable2 = 191;
constexpr unsigned kRsColorWriteEnable3 = 192;
constexpr unsigned kRsBlendFactor = 193;
constexpr unsigned kRsSrgbWriteEnable = 194;
constexpr unsigned kRsSeparateAlphaBlendEnable = 206;
constexpr unsigned kRsSrcBlendAlpha = 207;
constexpr unsigned kRsDestBlendAlpha = 208;
constexpr unsigned kRsBlendOpAlpha = 209;

/* the render-state array the device keeps (D3DRS_* index -> value) */
constexpr unsigned kRenderStateCount = 256;
/* sampler slots: 0-15 pixel, 16-19 vertex (D3DVERTEXTEXTURESAMPLER0-3) */
constexpr unsigned kSamplerSlots = 20;
constexpr unsigned kPixelSamplerSlots = 16;
/* b0-b15 and i0-i15, in each stage */
constexpr unsigned kBoolConsts = 16;
constexpr unsigned kIntConsts = 16;

struct TraceShader {
  bool bound = false;
  uint64_t hash = 0;    // bytecode_hash of its bytecode
  uint32_t version = 0; // the version token (its first DWORD)
};

struct TraceTexture {
  unsigned slot = 0;
  bool bound = false;
  uint32_t format = 0; // D3DFORMAT
  uint32_t type = 0;   // D3DRESOURCETYPE: 3 texture, 4 volume texture, 5 cube texture
  uint32_t width = 0, height = 0, depth = 0, levels = 0;
};

struct TraceDraw {
  bool indexed = false;
  bool up = false;        // DrawPrimitiveUP / DrawIndexedPrimitiveUP
  uint32_t primitive = 0; // D3DPRIMITIVETYPE, as the draw runs (a fan is drawn as a list)
  uint32_t count = 0;     // vertices, or indices
  uint32_t start = 0;     // the first vertex, or index
  int32_t base = 0;       // BaseVertexIndex (indexed)
  uint32_t instances = 1;
  TraceShader vs, ps;
  unsigned decl = 0; // 0: no number
  std::vector<TraceTexture> textures; // the bound slots, in slot order
  uint32_t sampled = 0;               // bit per slot the shaders declare a sampler for
  bool sampled_known = false;         // false: a shader that declares none (ps_1_x), or none bound
  const uint32_t *rs = nullptr;       // kRenderStateCount values
  const int32_t *vs_b = nullptr, *ps_b = nullptr;             // kBoolConsts each
  const int32_t (*vs_i)[4] = nullptr, (*ps_i)[4] = nullptr;   // kIntConsts each
};

/* ---- names ---- */

inline std::string
Number(uint32_t value) {
  return std::to_string(value);
}

inline std::string
HexDigits(uint64_t value, int digits) {
  char text[24];
  std::snprintf(text, sizeof(text), "%0*llx", digits, (unsigned long long)value);
  return text;
}

inline std::string
PrimitiveName(uint32_t primitive) {
  switch (primitive) {
  case 1: return "pointlist";
  case 2: return "linelist";
  case 3: return "linestrip";
  case 4: return "trianglelist";
  case 5: return "trianglestrip";
  case 6: return "trianglefan";
  default: return "primitive " + Number(primitive);
  }
}

inline std::string
BlendName(uint32_t blend) {
  static const char *const names[] = {
      nullptr,         "ZERO",          "ONE",          "SRCCOLOR",       "INVSRCCOLOR",   "SRCALPHA",
      "INVSRCALPHA",   "DESTALPHA",     "INVDESTALPHA", "DESTCOLOR",      "INVDESTCOLOR",  "SRCALPHASAT",
      "BOTHSRCALPHA",  "BOTHINVSRCALPHA", "BLENDFACTOR", "INVBLENDFACTOR", "SRCCOLOR2",    "INVSRCCOLOR2",
  };
  if (blend < sizeof(names) / sizeof(names[0]) && names[blend])
    return names[blend];
  return Number(blend);
}

inline std::string
BlendOpName(uint32_t op) {
  static const char *const names[] = {nullptr, "ADD", "SUBTRACT", "REVSUBTRACT", "MIN", "MAX"};
  if (op < sizeof(names) / sizeof(names[0]) && names[op])
    return names[op];
  return Number(op);
}

inline std::string
CmpName(uint32_t func) {
  static const char *const names[] = {nullptr,   "NEVER",    "LESS",         "EQUAL", "LESSEQUAL",
                                      "GREATER", "NOTEQUAL", "GREATEREQUAL", "ALWAYS"};
  if (func < sizeof(names) / sizeof(names[0]) && names[func])
    return names[func];
  return Number(func);
}

inline std::string
StencilOpName(uint32_t op) {
  static const char *const names[] = {nullptr,   "KEEP",   "ZERO", "REPLACE", "INCRSAT",
                                      "DECRSAT", "INVERT", "INCR", "DECR"};
  if (op < sizeof(names) / sizeof(names[0]) && names[op])
    return names[op];
  return Number(op);
}

inline std::string
FogModeName(uint32_t mode) {
  static const char *const names[] = {"NONE", "EXP", "EXP2", "LINEAR"};
  if (mode < sizeof(names) / sizeof(names[0]))
    return names[mode];
  return Number(mode);
}

/* COLORWRITEENABLE's channels: "rgba", "rgb", "a", "-" for none (bit 0 red ... bit 3 alpha) */
inline std::string
WriteMaskText(uint32_t mask) {
  std::string text;
  if (mask & 1)
    text += "r";
  if (mask & 2)
    text += "g";
  if (mask & 4)
    text += "b";
  if (mask & 8)
    text += "a";
  return text.empty() ? std::string("-") : text;
}

inline std::string
OnOff(uint32_t value) {
  return value ? "on" : "off";
}

/* "0123456789abcdef vs_3_0", or "ffp" */
inline std::string
ShaderText(const TraceShader &shader) {
  if (!shader.bound)
    return "ffp";
  return HexDigits(shader.hash, 16) + " " + d9fail::ShaderVersionToken(true, shader.version);
}

/* "s3", or "vs0" for a vertex sampler */
inline std::string
SlotName(unsigned slot) {
  return slot < kPixelSamplerSlots ? "s" + Number(slot) : "vs" + Number(slot - kPixelSamplerSlots);
}

/* "DXT5 1024x1024 2d mips 11", "A8R8G8B8 64x64x32 3d mips 1", "L8 128x128 cube mips 8" */
inline std::string
TextureText(const TraceTexture &t) {
  std::string size = Number(t.width) + "x" + Number(t.height);
  const char *type = "2d";
  if (t.type == 4) {
    size += "x" + Number(t.depth);
    type = "3d";
  } else if (t.type == 5) {
    type = "cube";
  }
  return d9fail::FormatName(t.format) + " " + size + " " + type + " mips " + Number(t.levels);
}

/* ---- the lines ---- */

/* "draw indexed trianglelist indices 300 start 0 base 0 instances 1; vs ... vs_3_0; ps ffp; decl 7" */
inline std::string
DrawLine(const TraceDraw &d) {
  std::string line = std::string("draw") + (d.indexed ? " indexed" : "") + (d.up ? " up" : "") + " " +
                     PrimitiveName(d.primitive) + (d.indexed ? " indices " : " vertices ") + Number(d.count) +
                     " start " + Number(d.start);
  if (d.indexed)
    line += " base " + std::to_string(d.base);
  line += " instances " + Number(d.instances);
  line += "; vs " + ShaderText(d.vs) + "; ps " + ShaderText(d.ps) + "; decl " + (d.decl ? Number(d.decl) : "-");
  return line;
}

/* "tex s0 DXT5 1024x1024 2d mips 11, s1 unbound, ..."; "tex -" with nothing to list */
inline std::string
TexLine(const TraceDraw &d) {
  std::vector<std::string> entries;
  size_t next = 0; // d.textures is in slot order
  for (unsigned slot = 0; slot < kSamplerSlots; slot++) {
    const TraceTexture *bound = nullptr;
    while (next < d.textures.size() && d.textures[next].slot < slot)
      next++;
    if (next < d.textures.size() && d.textures[next].slot == slot && d.textures[next].bound)
      bound = &d.textures[next];
    const bool sampled = d.sampled_known && (d.sampled & (1u << slot));
    if (!bound && !sampled)
      continue;
    if (!bound)
      entries.push_back(SlotName(slot) + " unbound");
    else
      entries.push_back(SlotName(slot) + " " + TextureText(*bound) + (d.sampled_known && !sampled ? " unused" : ""));
  }
  if (entries.empty())
    return "tex -";
  std::string line = "tex";
  for (size_t i = 0; i < entries.size(); i++)
    line += (i ? ", " : " ") + entries[i];
  return line;
}

/* "func ALWAYS ref 1 mask ff write ff fail KEEP zfail KEEP pass REPLACE" for one face */
inline std::string
StencilFaceText(uint32_t func, uint32_t fail, uint32_t zfail, uint32_t pass, const uint32_t *rs) {
  return "func " + CmpName(func) + " ref " + Number(rs[kRsStencilRef]) + " mask " +
         HexDigits(rs[kRsStencilMask], 0) + " write " + HexDigits(rs[kRsStencilWriteMask], 0) + " fail " +
         StencilOpName(fail) + " zfail " + StencilOpName(zfail) + " pass " + StencilOpName(pass);
}

/* "output colorwrite rgba,rgba,-,-; blend ...; alphatest ...; z ...; stencil ...; srgbwrite ...; fog ..." */
inline std::string
OutputLine(const uint32_t *rs) {
  std::string line = "output colorwrite " + WriteMaskText(rs[kRsColorWriteEnable]) + "," +
                     WriteMaskText(rs[kRsColorWriteEnable1]) + "," + WriteMaskText(rs[kRsColorWriteEnable2]) +
                     "," + WriteMaskText(rs[kRsColorWriteEnable3]);
  if (!rs[kRsAlphaBlendEnable]) {
    line += "; blend off";
  } else {
    line += "; blend " + BlendName(rs[kRsSrcBlend]) + " " + BlendName(rs[kRsDestBlend]) + " " +
            BlendOpName(rs[kRsBlendOp]);
    if (rs[kRsSeparateAlphaBlendEnable])
      line += ", alpha " + BlendName(rs[kRsSrcBlendAlpha]) + " " + BlendName(rs[kRsDestBlendAlpha]) + " " +
              BlendOpName(rs[kRsBlendOpAlpha]);
    line += " factor " + HexDigits(rs[kRsBlendFactor], 8);
  }
  if (!rs[kRsAlphaTestEnable])
    line += "; alphatest off";
  else
    line += "; alphatest " + CmpName(rs[kRsAlphaFunc]) + " " + Number(rs[kRsAlphaRef] & 0xff);
  const uint32_t z = rs[kRsZEnable];
  line += std::string("; z ") + (z == 0 ? "off" : z == 2 ? "usew" : "on") + " func " + CmpName(rs[kRsZFunc]) +
          " write " + OnOff(rs[kRsZWriteEnable]);
  if (!rs[kRsStencilEnable]) {
    line += "; stencil off";
  } else {
    line += "; stencil " + StencilFaceText(rs[kRsStencilFunc], rs[kRsStencilFail], rs[kRsStencilZFail],
                                           rs[kRsStencilPass], rs);
    if (rs[kRsTwoSidedStencilMode])
      line += ", ccw " + StencilFaceText(rs[kRsCcwStencilFunc], rs[kRsCcwStencilFail], rs[kRsCcwStencilZFail],
                                         rs[kRsCcwStencilPass], rs);
  }
  line += "; srgbwrite " + OnOff(rs[kRsSrgbWriteEnable]);
  if (!rs[kRsFogEnable])
    line += "; fog off";
  else
    line += "; fog on table " + FogModeName(rs[kRsFogTableMode]) + " vertex " + FogModeName(rs[kRsFogVertexMode]) +
            " color " + HexDigits(rs[kRsFogColor], 8);
  return line;
}

/* "b 0003 i0 4,0,1,0 i3 1,1,1,1", "b 0000 i -"; "-" for a stage without a shader */
inline std::string
StageConstsText(bool bound, const int32_t *b, const int32_t (*i)[4]) {
  if (!bound || !b || !i)
    return "-";
  uint32_t mask = 0;
  for (unsigned k = 0; k < kBoolConsts; k++)
    if (b[k])
      mask |= 1u << k;
  std::string text = "b " + HexDigits(mask, 4);
  bool any = false;
  for (unsigned k = 0; k < kIntConsts; k++) {
    if (!i[k][0] && !i[k][1] && !i[k][2] && !i[k][3])
      continue;
    text += " i" + Number(k) + " " + std::to_string(i[k][0]) + "," + std::to_string(i[k][1]) + "," +
            std::to_string(i[k][2]) + "," + std::to_string(i[k][3]);
    any = true;
  }
  if (!any)
    text += " i -";
  return text;
}

inline std::string
ConstsLine(const TraceDraw &d) {
  return "consts vs " + StageConstsText(d.vs.bound, d.vs_b, d.vs_i) + "; ps " +
         StageConstsText(d.ps.bound, d.ps_b, d.ps_i);
}

/* the draw's entry: its four lines, '\n'-separated (the encoder's traceDraw takes them so) */
inline std::string
DrawLines(const TraceDraw &d) {
  std::string lines = DrawLine(d) + "\n" + TexLine(d);
  if (d.rs)
    lines += "\n" + OutputLine(d.rs);
  lines += "\n" + ConstsLine(d);
  return lines;
}

} // namespace dxmt::d9trace
