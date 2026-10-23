// SPDX-License-Identifier: LGPL-2.1-or-later
// From David Acevedo's DXMT fork (github.com/dacevedo12/dxmt), tag v0.4-d3d9,
// commit e8dd4c656dcb74a6d970a30a397d1558b0e3fb2b. LGPL-2.1-or-later: see COPYING.LIB
// and LICENSE.LGPL at the top of this tree. Imported for MYIOSDECK (spec 07).
#include "d3d9_shader_scan.hpp"

#include "dxso_decoder.hpp"
#include "dxso_header.hpp"

namespace dxmt {

namespace {
// CreateVertexShader / CreatePixelShader carry no length, so the End
// token is the only delimiter. A blob with no reachable End would walk
// forever; cap at a size no SM1-3 shader approaches. Fully bounding a
// truncated blob would need a length-carrying decode context.
constexpr uint32_t kMaxScanDwords = 65536;
} // namespace

size_t
shader_bytecode_dword_count(const DWORD *byte_code) {
  if (!byte_code)
    return 0;
  const auto *words = reinterpret_cast<const uint32_t *>(byte_code);
  // Length comes from the canonical instruction walker, so it shares one
  // source of truth with the rest of the create path. A bespoke scan that
  // examined every DWORD is what let an operand aliasing the Comment
  // opcode (low 16 bits == 0xFFFE) be misread as a comment header and skip
  // past End into unmapped memory.
  auto header = parse_dxso_header(words, kMaxScanDwords);
  if (!header)
    return 0;
  DxsoBytecodeIter it(words, kMaxScanDwords, *header);
  DxsoInstruction ins{};
  while (it.next(ins)) {
    if (ins.opcode == DxsoOpcode::End)
      return ins.offset_dwords + 1;
  }
  return 0;
}

} // namespace dxmt
