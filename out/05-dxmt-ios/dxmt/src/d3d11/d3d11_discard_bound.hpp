// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 66 (DXMT_D11_DISCARD_BOUND, default on): a Map(WRITE_DISCARD) marks dirty
// only the slots that hold the discarded resource, found in one pass over the bound slots. A
// slot that holds it must never be missed: its table carries the address of the slice being
// replaced. With the switch off every slot of the kind is marked, as before.
//
// `Set` is anything iterable as (slot, entry) pairs over its bound slots with set_dirty(slot)
// and set_dirty(): DXMT's BindingSet, or the host test's fake.

#pragma once

#include <cstddef>

namespace dxmt {

template <typename Set, typename Holds>
void
DirtyDiscardedSlots(Set &set, bool only_holding, Holds &&holds) {
  if (!only_holding) {
    set.set_dirty();
    return;
  }
  for (const auto &[slot, entry] : set)
    if (holds(entry))
      set.set_dirty(slot);
}

} // namespace dxmt
