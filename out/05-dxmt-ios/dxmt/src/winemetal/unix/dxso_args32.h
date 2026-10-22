// SPDX-License-Identifier: LGPL-2.1-or-later
// The structs and the conversion below are from David Acevedo's DXMT fork
// (github.com/dacevedo12/dxmt), tag v0.4-d3d9, commit e8dd4c6,
// src/winemetal/unix/winemetal_unix.c. LGPL-2.1-or-later: see COPYING.LIB and
// LICENSE.LGPL at the top of this tree.
// Changed for MYIOSDECK (spec 07 A3): moved into a header that compiles as C
// and C++ and takes the 32-bit program's window base as an argument (spec 06
// C1: guest address a is host address base + a, NULL stays NULL), so the
// conversion can be unit-tested on any host. Include after airconv_public.h.
//
// DXSO compilation argument chain as an i386 caller lays it out: same shape
// as SM50's 32-bit chain (4-byte pointers), so it is copied into 64-bit
// argument structs before the call and the copies are freed after it.

#ifndef DXMT_DXSO_ARGS32_H
#define DXMT_DXSO_ARGS32_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA32 {
  uint32_t next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

struct DXSO_SHADER_IA_INPUT_LAYOUT_DATA32 {
  uint32_t next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  enum DXSO_INDEX_BUFFER_FORMAT index_buffer_format;
  uint32_t slot_mask;
  uint32_t num_elements;
  uint32_t elements;
  uint32_t position_transformed;
  uint32_t vs_float_const_count;
};

struct DXSO_SHADER_PSO_PIXEL_SHADER_DATA32 {
  uint32_t next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t alpha_test_func;
  uint32_t dual_source_blending;
  uint32_t flat_shading;
  uint32_t emit_sample_mask;
  uint32_t unorm_output_reg_mask;
};

struct DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA32 {
  uint32_t next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint8_t kinds[16];
};

struct DXSO_SHADER_PS_POINT_SPRITE_DATA32 {
  uint32_t next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

struct DXSO_SHADER_PS_FOG_DATA32 {
  uint32_t next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t mode;
  uint32_t coord_is_w;
};

struct DXSO_SHADER_FFP_KEY_DATA32 {
  uint32_t next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
  uint32_t kind;
  uint32_t has_diffuse;
  uint32_t has_texcoord0;
  uint32_t has_specular;
  uint32_t tex0_mode;
  uint32_t stages[8][3];
  uint32_t point_size;
  uint32_t point_sprite;
  uint32_t point_scale;
  uint32_t texcoord_mask;
  uint32_t texcoord_transform_key;
  uint32_t lighting_key;
  uint32_t fog_vertex_mode;
  uint32_t vertex_blend;
  uint32_t texgen_key;
  uint32_t texcoord_index_key;
  uint32_t sampler_kind_key;
  uint32_t flat_shading;
  uint32_t point_size_per_vertex;
  uint32_t decl_has_diffuse;
  uint32_t range_fog;
  uint32_t emit_sample_mask;
};

struct DXSO_SHADER_VS_POINT_SIZE_DATA32 {
  uint32_t next;
  enum DXSO_SHADER_COMPILATION_ARGUMENT_TYPE type;
};

/* A guest pointer of the 32-bit program, as a host pointer (spec 06 C1). */
static inline void *
dxso_args32_ptr(uint32_t guest, uint64_t base) {
  return guest ? (void *)(uintptr_t)(base + guest) : NULL;
}

/* Links a freshly allocated 64-bit argument after *last. */
static inline void
dxso_args32_link(struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA **last, void *data) {
  (*last)->next = data;
  *last = (struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *)data;
  (*last)->next = NULL;
}

/* At most this many arguments are followed (a cycle or a corrupt chain ends
   there instead of looping). MYIOSDECK spec 07 A3 addition. */
#define DXSO_ARGS32_MAX_ARGUMENTS 64

/* Copies the i386 chain starting at guest address `args32_guest` into 64-bit
   argument structs linked after first_arg (whose own type is set to
   DXSO_SHADER_ARGUMENT_TYPE_MAX). Unknown and reserved argument types are
   skipped. */
static inline void
dxso_args32_convert(
    struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *first_arg, uint32_t args32_guest, uint64_t base
) {
  struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *last_arg = first_arg;
  first_arg->type = DXSO_SHADER_ARGUMENT_TYPE_MAX;
  first_arg->next = NULL;

  const struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA32 *args32 =
      (const struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA32 *)dxso_args32_ptr(args32_guest, base);
  for (unsigned count = 0; args32 && count < DXSO_ARGS32_MAX_ARGUMENTS; count++) {
    switch (args32->type) {
    case DXSO_SHADER_IA_INPUT_LAYOUT: {
      const struct DXSO_SHADER_IA_INPUT_LAYOUT_DATA32 *src = (const struct DXSO_SHADER_IA_INPUT_LAYOUT_DATA32 *)args32;
      struct DXSO_SHADER_IA_INPUT_LAYOUT_DATA *data =
          (struct DXSO_SHADER_IA_INPUT_LAYOUT_DATA *)malloc(sizeof(struct DXSO_SHADER_IA_INPUT_LAYOUT_DATA));
      if (!data)
        return;
      dxso_args32_link(&last_arg, data);
      data->type = src->type;
      data->index_buffer_format = src->index_buffer_format;
      data->slot_mask = src->slot_mask;
      data->num_elements = src->num_elements;
      data->elements = (struct DXSO_IA_INPUT_ELEMENT *)dxso_args32_ptr(src->elements, base);
      data->position_transformed = src->position_transformed;
      data->vs_float_const_count = src->vs_float_const_count;
      break;
    }
    case DXSO_SHADER_PSO_PIXEL_SHADER: {
      const struct DXSO_SHADER_PSO_PIXEL_SHADER_DATA32 *src = (const struct DXSO_SHADER_PSO_PIXEL_SHADER_DATA32 *)args32;
      struct DXSO_SHADER_PSO_PIXEL_SHADER_DATA *data =
          (struct DXSO_SHADER_PSO_PIXEL_SHADER_DATA *)malloc(sizeof(struct DXSO_SHADER_PSO_PIXEL_SHADER_DATA));
      if (!data)
        return;
      dxso_args32_link(&last_arg, data);
      data->type = src->type;
      data->alpha_test_func = src->alpha_test_func;
      data->dual_source_blending = src->dual_source_blending;
      data->flat_shading = src->flat_shading;
      data->emit_sample_mask = src->emit_sample_mask;
      data->unorm_output_reg_mask = src->unorm_output_reg_mask;
      break;
    }
    case DXSO_SHADER_PS_SAMPLER_LAYOUT: {
      const struct DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA32 *src =
          (const struct DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA32 *)args32;
      struct DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA *data =
          (struct DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA *)malloc(sizeof(struct DXSO_SHADER_PS_SAMPLER_LAYOUT_DATA));
      if (!data)
        return;
      dxso_args32_link(&last_arg, data);
      data->type = src->type;
      memcpy(data->kinds, src->kinds, sizeof(data->kinds));
      break;
    }
    case DXSO_SHADER_PS_POINT_SPRITE: {
      struct DXSO_SHADER_PS_POINT_SPRITE_DATA *data =
          (struct DXSO_SHADER_PS_POINT_SPRITE_DATA *)malloc(sizeof(struct DXSO_SHADER_PS_POINT_SPRITE_DATA));
      if (!data)
        return;
      dxso_args32_link(&last_arg, data);
      data->type = DXSO_SHADER_PS_POINT_SPRITE;
      break;
    }
    case DXSO_SHADER_FFP_KEY: {
      const struct DXSO_SHADER_FFP_KEY_DATA32 *src = (const struct DXSO_SHADER_FFP_KEY_DATA32 *)args32;
      struct DXSO_SHADER_FFP_KEY_DATA *data =
          (struct DXSO_SHADER_FFP_KEY_DATA *)malloc(sizeof(struct DXSO_SHADER_FFP_KEY_DATA));
      if (!data)
        return;
      dxso_args32_link(&last_arg, data);
      data->type = DXSO_SHADER_FFP_KEY;
      data->kind = src->kind;
      data->has_diffuse = src->has_diffuse;
      data->has_texcoord0 = src->has_texcoord0;
      data->has_specular = src->has_specular;
      data->tex0_mode = src->tex0_mode;
      memcpy(data->stages, src->stages, sizeof(data->stages));
      data->point_size = src->point_size;
      data->point_sprite = src->point_sprite;
      data->point_scale = src->point_scale;
      data->texcoord_mask = src->texcoord_mask;
      data->texcoord_transform_key = src->texcoord_transform_key;
      data->lighting_key = src->lighting_key;
      data->fog_vertex_mode = src->fog_vertex_mode;
      data->vertex_blend = src->vertex_blend;
      data->texgen_key = src->texgen_key;
      data->texcoord_index_key = src->texcoord_index_key;
      data->sampler_kind_key = src->sampler_kind_key;
      data->flat_shading = src->flat_shading;
      data->point_size_per_vertex = src->point_size_per_vertex;
      data->decl_has_diffuse = src->decl_has_diffuse;
      data->range_fog = src->range_fog;
      data->emit_sample_mask = src->emit_sample_mask;
      break;
    }
    case DXSO_SHADER_VS_POINT_SIZE: {
      struct DXSO_SHADER_VS_POINT_SIZE_DATA *data =
          (struct DXSO_SHADER_VS_POINT_SIZE_DATA *)malloc(sizeof(struct DXSO_SHADER_VS_POINT_SIZE_DATA));
      if (!data)
        return;
      dxso_args32_link(&last_arg, data);
      data->type = DXSO_SHADER_VS_POINT_SIZE;
      break;
    }
    case DXSO_SHADER_PS_FOG: {
      const struct DXSO_SHADER_PS_FOG_DATA32 *src = (const struct DXSO_SHADER_PS_FOG_DATA32 *)args32;
      struct DXSO_SHADER_PS_FOG_DATA *data =
          (struct DXSO_SHADER_PS_FOG_DATA *)malloc(sizeof(struct DXSO_SHADER_PS_FOG_DATA));
      if (!data)
        return;
      dxso_args32_link(&last_arg, data);
      data->type = DXSO_SHADER_PS_FOG;
      data->mode = src->mode;
      data->coord_is_w = src->coord_is_w;
      break;
    }
    case DXSO_SHADER_PS_BUMP_ENV: /* reserved: never emitted by the host */
    case DXSO_SHADER_ARGUMENT_TYPE_MAX:
    default:
      break;
    }
    args32 = (const struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA32 *)dxso_args32_ptr(args32->next, base);
  }
}

/* Frees the 64-bit copies linked after first_arg. */
static inline void
dxso_args32_free(struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *first_arg) {
  struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *arg = (struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *)first_arg->next;
  while (arg) {
    struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *next = (struct DXSO_SHADER_COMPILATION_ARGUMENT_DATA *)arg->next;
    free(arg);
    arg = next;
  }
  first_arg->next = NULL;
}

#endif
