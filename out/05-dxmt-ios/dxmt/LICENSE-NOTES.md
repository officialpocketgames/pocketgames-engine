# Licences in this tree

This tree combines code under three licences. Each file's header says which one applies.

| Part | Licence | Files |
|---|---|---|
| Upstream DXMT a3a6e8f (github.com/3Shain/dxmt) | MIT, `LICENSE` | Every file without another notice; files changed for MYIOSDECK keep this licence and say they were changed. |
| MYIOSDECK's own code (specs 05–07) | MIT | Files that start with `SPDX-License-Identifier: MIT` and "Copyright (c) 2026 the MYIOSDECK author". |
| Direct3D 9 frontend and DXSO / fixed-function translator from David Acevedo's DXMT fork (github.com/dacevedo12/dxmt, tag `v0.4-d3d9`, commit e8dd4c6), and the blocks of its newer DXMT core taken for them (copyright Feifan He for CodeWeavers and that fork's authors) | LGPL-2.1-or-later, `LICENSE.LGPL` and `COPYING.LIB` | `src/d3d9/` (all of it); `src/airconv/dxso_*`, `src/airconv/ffp_compile.*`; and the blocks marked "LGPL-2.1-or-later, from ... v0.4-d3d9" inside other files (listed under "spec 07" in `out/05-dxmt-ios/CHANGES.md` of the MYIOSDECK repository). |
| `src/util` files derived from DXVK | zlib, `src/util/dxvk.LICENSE` | As marked in those files. |

A file that contains an LGPL block is distributed under LGPL-2.1-or-later as a whole. The MIT
code in it stays available under MIT on its own.
