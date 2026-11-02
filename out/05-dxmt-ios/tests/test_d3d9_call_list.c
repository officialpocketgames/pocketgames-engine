/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 07 clarification 3 (Part D), coverage, first half: the call-statistics
 * lists in dxmt/src/d3d9/d3d9_call_list.h hold exactly the vtables d3d9.h
 * declares for the interfaces the frontend hands out. Compiled as C, so d3d9.h
 * declares each interface's vtable as a struct (<interface>Vtbl) with one
 * member per slot: every listed method must sit at its list position, each
 * list must end where its vtable ends, and no vtable may be listed twice. The
 * second half, check-d3d9-call-sites.sh, checks that each listed pair is
 * counted exactly once, first thing in the method that implements it.
 *
 *   cc -std=c11 -I../dxmt/include/native/windows -I../dxmt/include/native/directx \
 *      -I../dxmt/src/d3d9 test_d3d9_call_list.c -o test_d3d9_call_list
 */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "d3d9.h"
#include "d3d9_call_list.h"

struct slot {
  const char *label;
  const char *vtable;
  const char *method;
  size_t offset;      /* offsetof(<vtable>Vtbl, method) */
  size_t vtable_size; /* sizeof(<vtable>Vtbl) */
};

#define SLOT(label, vtable, method) {#label, #vtable, #method, offsetof(vtable##Vtbl, method), sizeof(vtable##Vtbl)},
static const struct slot slots[] = {D9_CALLS_COM(SLOT)};
#undef SLOT

/* The three Ex objects: the methods of the base interface carry the base name
   as their label, the Ex ones the Ex name. The base vtable is the Ex vtable's
   first slots. */
struct ex_base {
  const char *vtable;
  const char *base;
  size_t base_size;
};
static const struct ex_base ex_bases[] = {
    {"IDirect3D9Ex", "IDirect3D9", sizeof(IDirect3D9Vtbl)},
    {"IDirect3DDevice9Ex", "IDirect3DDevice9", sizeof(IDirect3DDevice9Vtbl)},
    {"IDirect3DSwapChain9Ex", "IDirect3DSwapChain9", sizeof(IDirect3DSwapChain9Vtbl)},
};

static int failures = 0;

static void
fail(const struct slot *s, const char *what) {
  printf("FAIL %s::%s (vtable %s): %s\n", s->label, s->method, s->vtable, what);
  failures++;
}

int
main(void) {
  const size_t n = sizeof(slots) / sizeof(slots[0]);
  size_t first = 0; /* first slot of the current vtable's list */
  size_t vtables = 0;
  for (size_t i = 0; i < n; i++) {
    const struct slot *s = &slots[i];
    if (i > first && strcmp(s->vtable, slots[first].vtable) != 0) {
      fail(s, "the previous vtable's list is not finished when this one starts");
      first = i;
    }
    if (i == first) {
      vtables++;
      for (size_t j = 0; j < first; j++)
        if (strcmp(slots[j].vtable, s->vtable) == 0) {
          fail(s, "vtable listed twice");
          break;
        }
    }
    const size_t index = i - first;
    if (s->offset != index * sizeof(void *))
      fail(s, "not at its vtable slot (the list is out of order or misses a slot)");

    /* Labels: the object's own interface, or for the Ex objects the base
       interface for the base slots. */
    const char *want = s->vtable;
    for (size_t k = 0; k < sizeof(ex_bases) / sizeof(ex_bases[0]); k++)
      if (strcmp(ex_bases[k].vtable, s->vtable) == 0 && s->offset < ex_bases[k].base_size)
        want = ex_bases[k].base;
    if (strcmp(s->label, want) != 0)
      fail(s, "wrong label");

    const int last = i + 1 == n || strcmp(slots[i + 1].vtable, s->vtable) != 0;
    if (last) {
      if ((index + 1) * sizeof(void *) != s->vtable_size)
        fail(s, "the list ends before its vtable does (a slot is missing)");
      first = i + 1;
    }
  }
  if (vtables != 15) {
    printf("FAIL %zu vtables listed, want the 15 the frontend hands out\n", vtables);
    failures++;
  }
  printf("test_d3d9_call_list: %zu slots of %zu vtables, %d failures\n", n, vtables, failures);
  return failures ? 1 : 0;
}
