# MoltenVK patches (spec 34 clarification 1 and Part F)

`build-moltenvk.sh` applies every `*.patch` here to its copy of UTM's MoltenVK fork
(`moltenvk-sources.txt`), and every `../spirv-cross-patches/*.patch` to its copy of UTM's SPIRV-Cross
fork, in name order (C locale), with `patch -p1 -N -F 0`: no fuzz, and the build stops on the
first that fails. Then it writes `MVKPatchRevision.h` (steamac's 0018) from the SHA-256 of both
series.

Numbering:

- `0001`–`0047`: adopted from steamac (github.com/StonkGuy/steamac, commit `56aa855`,
  `host/moltenvk/patches/moltenvk/`, Apache-2.0), with steamac's file names and author lines. 40 of
  its 47; which were skipped and why, and the one line of reason for each adopted one, are in
  `../CHANGES.md` ("Clarification 1"). Game names in their messages are replaced by "a game".
- `0100`–`0199`: ours (Apache-2.0; each changed MoltenVK file keeps its licence header and says it
  was changed for MYIOSDECK, Apache-2.0 section 4(b)). `0100` reports geometry shaders and transform
  feedback only on GPUs with Metal mesh shaders.
- `0200` on: Part F (phase 2), the BC-texture emulation, under the same rules; `bc_decode.h`
  compiles both as Metal Shading Language and as C++20.

`NOTICE-steamac.txt` goes into the product as `LICENSES/steamac/NOTICE` (Apache-2.0 section 4(d)).
Changing a patch here or in `../spirv-cross-patches/` changes the CI's MoltenVK cache stamp
(`ci/ios-part.sh`), so MoltenVK is rebuilt. `tests/test-build-scripts.sh` applies both series to
`refs/`'s copies of the forks, when present, and fails on any fuzz.
