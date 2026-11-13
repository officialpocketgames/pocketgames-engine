// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 68 (DXMT_PASS_MERGE_COMPUTE, default off): after the encoder optimiser,
// a compute pass B joins an earlier compute pass A (B's commands run after A's, in A's
// encoder), and a blit pass joins an earlier blit pass the same way, when:
//   - every encoder between them, after earlier merges, has no data dependency with B in either
//     direction;
//   - none of those encoders is a barrier (a present, an event signal or wait, an upscale);
//   - for compute, A and B use the same argument-buffer allocation.
// The reach counts the live encoders left between A and the candidate: at most `threshold`.
// Sweeps repeat until one merges nothing, so a long run of dispatch and copy pairs ends as one
// compute pass and one blit pass. Nothing else moves, and the order inside a merged pass is the
// recorded order.
//
// Pure: the encoding context and the host test drive it through `Ops`:
//   MergeKind kind(Encoder *)                    what an encoder is;
//   bool sameArguments(Encoder *a, Encoder *b)   compute only;
//   bool dependent(Encoder *b, Encoder *x)       a data dependency either way;
//   void merge(Encoder *a, Encoder *b)           b's commands and sets after a's, b emptied.
// Each candidate is checked against each encoder between, one by one, so no union of sets grows
// with the reach.

#pragma once

#include <vector>

namespace dxmt {

enum class MergeKind { Null, Compute, Blit, Barrier, Other };

struct MergeCounts {
  unsigned compute = 0;
  unsigned blit = 0;
};

template <typename Encoder, typename Ops>
MergeCounts
MergeIndependentPasses(Encoder **encoders, unsigned count, unsigned threshold, Ops &ops) {
  MergeCounts merged;
  std::vector<Encoder *> between;
  for (bool again = true; again;) {
    again = false;
    for (unsigned j = 0; j < count; j++) {
      const MergeKind kind = ops.kind(encoders[j]);
      if (kind != MergeKind::Compute && kind != MergeKind::Blit)
        continue;
      between.clear();
      for (unsigned i = j + 1; i < count; i++) {
        Encoder *candidate = encoders[i];
        const MergeKind other = ops.kind(candidate);
        if (other == MergeKind::Null)
          continue;
        if (other == MergeKind::Barrier)
          break;
        bool joins = other == kind && (kind != MergeKind::Compute || ops.sameArguments(encoders[j], candidate));
        for (unsigned k = 0; joins && k < between.size(); k++)
          joins = !ops.dependent(candidate, between[k]);
        if (joins) {
          ops.merge(encoders[j], candidate);
          (kind == MergeKind::Compute ? merged.compute : merged.blit)++;
          again = true;
          continue;
        }
        if (between.size() >= threshold)
          break;
        between.push_back(candidate);
      }
    }
  }
  return merged;
}

} // namespace dxmt
