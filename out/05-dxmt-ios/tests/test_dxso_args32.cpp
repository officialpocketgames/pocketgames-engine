// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 07 Part D: host tests for the conversion of a 32-bit program's DXSO
// compilation-argument chain (dxmt/src/winemetal/unix/dxso_args32.h, used by
// the i386 unix-call entries of spec 07 A3). A plain buffer stands in for the
// program's 4 GB window: guest address g is host address base + g. No Metal,
// no Wine.
//
//   c++ -std=c++20 -I../dxmt/src/airconv -I../dxmt/src/winemetal/unix test_dxso_args32.cpp -o test_dxso_args32

#include "test_common.hpp"

#include "airconv_public.h"
#include "dxso_args32.h"

#include <cstdlib>
#include <vector>

namespace {

/* The fake window: 64 KB of host memory; guest addresses are offsets into it
   (0 is NULL, so nothing is placed at offset 0). */
struct Window {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(64 * 1024, 0);

  uint64_t
  base() const {
    return (uint64_t)(uintptr_t)bytes.data();
  }

  template <typename T>
  T *
  at(uint32_t guest) {
    return reinterpret_cast<T *>(bytes.data() + guest);
  }
};

/* Number of 64-bit arguments linked after first. */
unsigned
ChainLength(const DXSO_SHADER_COMPILATION_ARGUMENT_DATA &first) {
  unsigned n = 0;
  for (auto *a = (const DXSO_SHADER_COMPILATION_ARGUMENT_DATA *)first.next; a;
       a = (const DXSO_SHADER_COMPILATION_ARGUMENT_DATA *)a->next)
    n++;
  return n;
}

const DXSO_SHADER_COMPILATION_ARGUMENT_DATA *
Nth(const DXSO_SHADER_COMPILATION_ARGUMENT_DATA &first, unsigned index) {
  auto *a = (const DXSO_SHADER_COMPILATION_ARGUMENT_DATA *)first.next;
  while (a && index--)
    a = (const DXSO_SHADER_COMPILATION_ARGUMENT_DATA *)a->next;
  return a;
}

void
TestLayouts() {
  // The i386 layouts: 4-byte links and pointers.
  CHECK_EQ(sizeof(DXSO_SHADER_COMPILATION_ARGUMENT_DATA32), 8);
  CHECK_EQ(sizeof(DXSO_SHADER_IA_INPUT_LAYOUT_DATA32), 32);
  CHECK_EQ(sizeof(DXSO_SHADER_PSO_PIXEL_SHADER_DATA32), 28);
  CHECK_EQ(sizeof(DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA32), 24);
  CHECK_EQ(sizeof(DXSO_SHADER_PS_FOG_DATA32), 16);
  CHECK_EQ(sizeof(DXSO_SHADER_PS_POINT_SPRITE_DATA32), 8);
  CHECK_EQ(sizeof(DXSO_SHADER_VS_POINT_SIZE_DATA32), 8);
}

void
TestPointer() {
  CHECK(dxso_args32_ptr(0, 0x700000000ull) == nullptr); // NULL stays NULL
  CHECK((uint64_t)(uintptr_t)dxso_args32_ptr(0x1000, 0x700000000ull) == 0x700001000ull);
  CHECK((uint64_t)(uintptr_t)dxso_args32_ptr(0xFFFFFFFFu, 0x700000000ull) == 0x7FFFFFFFFull);
}

void
TestEmpty() {
  Window w;
  DXSO_SHADER_COMPILATION_ARGUMENT_DATA first{};
  first.next = (void *)&w; // must be reset
  dxso_args32_convert(&first, 0, w.base());
  CHECK(first.next == nullptr);
  CHECK(first.type == DXSO_SHADER_ARGUMENT_TYPE_MAX);
  dxso_args32_free(&first); // nothing to free
  CHECK(first.next == nullptr);
}

void
TestInputLayout() {
  Window w;
  auto *ia = w.at<DXSO_SHADER_IA_INPUT_LAYOUT_DATA32>(0x100);
  ia->next = 0;
  ia->type = DXSO_SHADER_IA_INPUT_LAYOUT;
  ia->index_buffer_format = DXSO_INDEX_BUFFER_FORMAT_UINT16;
  ia->slot_mask = 0x5;
  ia->num_elements = 3;
  ia->elements = 0x2000;
  ia->position_transformed = 1;
  ia->vs_float_const_count = 8192;

  DXSO_SHADER_COMPILATION_ARGUMENT_DATA first{};
  dxso_args32_convert(&first, 0x100, w.base());
  CHECK_EQ(ChainLength(first), 1);
  auto *out = (const DXSO_SHADER_IA_INPUT_LAYOUT_DATA *)Nth(first, 0);
  CHECK(out != nullptr);
  if (out) {
    CHECK(out->type == DXSO_SHADER_IA_INPUT_LAYOUT);
    CHECK(out->index_buffer_format == DXSO_INDEX_BUFFER_FORMAT_UINT16);
    CHECK_EQ(out->slot_mask, 0x5);
    CHECK_EQ(out->num_elements, 3);
    CHECK((const void *)out->elements == (const void *)w.at<uint8_t>(0x2000)); // embedded pointer converted
    CHECK_EQ(out->position_transformed, 1);
    CHECK_EQ(out->vs_float_const_count, 8192);
    CHECK(out->next == nullptr);
  }
  dxso_args32_free(&first);

  // A NULL element pointer stays NULL.
  ia->elements = 0;
  dxso_args32_convert(&first, 0x100, w.base());
  out = (const DXSO_SHADER_IA_INPUT_LAYOUT_DATA *)Nth(first, 0);
  CHECK(out != nullptr && out->elements == nullptr);
  dxso_args32_free(&first);
}

void
TestFullChain() {
  Window w;
  // pixel shader -> sampler layout -> point sprite -> VS point size -> fog -> FFP key
  auto *ps = w.at<DXSO_SHADER_PSO_PIXEL_SHADER_DATA32>(0x100);
  ps->next = 0x200;
  ps->type = DXSO_SHADER_PSO_PIXEL_SHADER;
  ps->alpha_test_func = 5;
  ps->dual_source_blending = 1;
  ps->flat_shading = 1;
  ps->emit_sample_mask = 1;
  ps->unorm_output_reg_mask = 0x3;

  auto *sl = w.at<DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA32>(0x200);
  sl->next = 0x300;
  sl->type = DXSO_SHADER_PS_SAMPLER_LAYOUT;
  for (unsigned i = 0; i < 16; i++)
    sl->kinds[i] = uint8_t(i * 3);

  auto *spr = w.at<DXSO_SHADER_PS_POINT_SPRITE_DATA32>(0x300);
  spr->next = 0x340;
  spr->type = DXSO_SHADER_PS_POINT_SPRITE;

  auto *vps = w.at<DXSO_SHADER_VS_POINT_SIZE_DATA32>(0x340);
  vps->next = 0x380;
  vps->type = DXSO_SHADER_VS_POINT_SIZE;

  auto *fog = w.at<DXSO_SHADER_PS_FOG_DATA32>(0x380);
  fog->next = 0x400;
  fog->type = DXSO_SHADER_PS_FOG;
  fog->mode = 2;
  fog->coord_is_w = 1;

  auto *ffp = w.at<DXSO_SHADER_FFP_KEY_DATA32>(0x400);
  ffp->next = 0;
  ffp->type = DXSO_SHADER_FFP_KEY;
  ffp->kind = 1;
  ffp->has_diffuse = 1;
  for (unsigned s = 0; s < 8; s++)
    for (unsigned k = 0; k < 3; k++)
      ffp->stages[s][k] = s * 10 + k;
  ffp->lighting_key = 0xABCD;
  ffp->sampler_kind_key = 0x12345678;
  ffp->emit_sample_mask = 1;

  const std::vector<uint8_t> before = w.bytes;
  DXSO_SHADER_COMPILATION_ARGUMENT_DATA first{};
  dxso_args32_convert(&first, 0x100, w.base());
  CHECK(w.bytes == before); // the 32-bit chain is only read
  CHECK_EQ(ChainLength(first), 6);

  auto *ops = (const DXSO_SHADER_PSO_PIXEL_SHADER_DATA *)Nth(first, 0);
  CHECK(ops && ops->type == DXSO_SHADER_PSO_PIXEL_SHADER);
  if (ops) {
    CHECK_EQ(ops->alpha_test_func, 5);
    CHECK_EQ(ops->dual_source_blending, 1);
    CHECK_EQ(ops->flat_shading, 1);
    CHECK_EQ(ops->emit_sample_mask, 1);
    CHECK_EQ(ops->unorm_output_reg_mask, 0x3);
  }
  auto *osl = (const DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA *)Nth(first, 1);
  CHECK(osl && osl->type == DXSO_SHADER_PS_SAMPLER_LAYOUT);
  if (osl)
    for (unsigned i = 0; i < 16; i++)
      CHECK_EQ(osl->kinds[i], i * 3);
  CHECK(Nth(first, 2) && Nth(first, 2)->type == DXSO_SHADER_PS_POINT_SPRITE);
  CHECK(Nth(first, 3) && Nth(first, 3)->type == DXSO_SHADER_VS_POINT_SIZE);
  auto *ofog = (const DXSO_SHADER_PS_FOG_DATA *)Nth(first, 4);
  CHECK(ofog && ofog->type == DXSO_SHADER_PS_FOG);
  if (ofog) {
    CHECK_EQ(ofog->mode, 2);
    CHECK_EQ(ofog->coord_is_w, 1);
  }
  auto *offp = (const DXSO_SHADER_FFP_KEY_DATA *)Nth(first, 5);
  CHECK(offp && offp->type == DXSO_SHADER_FFP_KEY);
  if (offp) {
    CHECK_EQ(offp->kind, 1);
    CHECK_EQ(offp->has_diffuse, 1);
    CHECK_EQ(offp->stages[7][2], 72);
    CHECK_EQ(offp->stages[3][1], 31);
    CHECK_EQ(offp->lighting_key, 0xABCD);
    CHECK_EQ(offp->sampler_kind_key, 0x12345678);
    CHECK_EQ(offp->emit_sample_mask, 1);
    CHECK(offp->next == nullptr);
  }
  dxso_args32_free(&first);
  CHECK(first.next == nullptr);
}

void
TestSkippedTypes() {
  Window w;
  // reserved bump-env (6), an unknown type (99), then a point sprite
  auto *bump = w.at<DXSO_SHADER_COMPILATION_ARGUMENT_DATA32>(0x100);
  bump->next = 0x180;
  bump->type = DXSO_SHADER_PS_BUMP_ENV;
  auto *unknown = w.at<DXSO_SHADER_COMPILATION_ARGUMENT_DATA32>(0x180);
  unknown->next = 0x200;
  unknown->type = (DXSO_SHADER_COMPILATION_ARGUMENT_TYPE)99;
  auto *spr = w.at<DXSO_SHADER_PS_POINT_SPRITE_DATA32>(0x200);
  spr->next = 0;
  spr->type = DXSO_SHADER_PS_POINT_SPRITE;

  DXSO_SHADER_COMPILATION_ARGUMENT_DATA first{};
  dxso_args32_convert(&first, 0x100, w.base());
  CHECK_EQ(ChainLength(first), 1);
  CHECK(Nth(first, 0) && Nth(first, 0)->type == DXSO_SHADER_PS_POINT_SPRITE);
  dxso_args32_free(&first);
}

void
TestCycle() {
  Window w;
  // An entry that links to itself: the walk stops after the limit instead of
  // looping forever.
  auto *spr = w.at<DXSO_SHADER_PS_POINT_SPRITE_DATA32>(0x100);
  spr->next = 0x100;
  spr->type = DXSO_SHADER_PS_POINT_SPRITE;
  DXSO_SHADER_COMPILATION_ARGUMENT_DATA first{};
  dxso_args32_convert(&first, 0x100, w.base());
  CHECK_EQ(ChainLength(first), DXSO_ARGS32_MAX_ARGUMENTS);
  dxso_args32_free(&first);

  // A two-entry cycle of skipped types: nothing copied, the walk still ends.
  auto *a = w.at<DXSO_SHADER_COMPILATION_ARGUMENT_DATA32>(0x200);
  auto *b = w.at<DXSO_SHADER_COMPILATION_ARGUMENT_DATA32>(0x210);
  a->next = 0x210;
  a->type = DXSO_SHADER_PS_BUMP_ENV;
  b->next = 0x200;
  b->type = DXSO_SHADER_PS_BUMP_ENV;
  dxso_args32_convert(&first, 0x200, w.base());
  CHECK_EQ(ChainLength(first), 0);
  dxso_args32_free(&first);
}

} // namespace

int
main() {
  TestLayouts();
  TestPointer();
  TestEmpty();
  TestInputLayout();
  TestFullChain();
  TestSkippedTypes();
  TestCycle();
  std::printf("test_dxso_args32: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
