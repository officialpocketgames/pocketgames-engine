/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 05 clarification 63 (DXMT_D11_CALL_STATS), coverage, first half: the lists in
 * dxmt/src/d3d11/d3d11_call_list.h hold exactly the vtables of ID3D11DeviceContext4 and
 * ID3D11Device5 as the d3d11*.h headers in include/native/directx declare them. Compiled as C,
 * so the headers declare each vtable as a struct with one member per slot: every listed method
 * must sit at its list position, and each list must end where its vtable ends. The second half,
 * check-d3d11-call-sites.sh, checks that each listed method is counted once, first thing in
 * the method that implements it.
 */

#include <stddef.h>
#include <stdio.h>

#include "d3d11_4.h"
#include "d3d11_call_list.h"

struct slot {
  const char *method;
  size_t offset;
};

#define CONTEXT_SLOT(method) {#method, offsetof(ID3D11DeviceContext4Vtbl, method)},
static const struct slot context_slots[] = {D11_CALLS_CONTEXT(CONTEXT_SLOT)};
#undef CONTEXT_SLOT

#define DEVICE_SLOT(method) {#method, offsetof(ID3D11Device5Vtbl, method)},
static const struct slot device_slots[] = {D11_CALLS_DEVICE(DEVICE_SLOT)};
#undef DEVICE_SLOT

static int checks = 0, failures = 0;

static void
check_list(const char *name, const struct slot *slots, size_t n, size_t vtable_size) {
  for (size_t i = 0; i < n; i++) {
    checks++;
    if (slots[i].offset != i * sizeof(void *)) {
      printf("FAIL: %s: %s is listed at %zu, its vtable slot is %zu\n", name, slots[i].method, i,
             slots[i].offset / sizeof(void *));
      failures++;
    }
  }
  checks++;
  if (n * sizeof(void *) != vtable_size) {
    printf("FAIL: %s: %zu methods listed, the vtable has %zu\n", name, n, vtable_size / sizeof(void *));
    failures++;
  }
}

int
main(void) {
  check_list("ID3D11DeviceContext4", context_slots, sizeof(context_slots) / sizeof(context_slots[0]),
             sizeof(ID3D11DeviceContext4Vtbl));
  check_list("ID3D11Device5", device_slots, sizeof(device_slots) / sizeof(device_slots[0]),
             sizeof(ID3D11Device5Vtbl));
  printf("test_d3d11_call_list: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
