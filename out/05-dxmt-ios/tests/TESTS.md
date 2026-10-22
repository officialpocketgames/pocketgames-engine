# Spec 05 tests (Part I)

## 1. Host unit tests (any OS, any C++20 compiler)

```sh
out/05-dxmt-ios/tests/run-host-tests.sh            # builds into tests/build/
CXX=clang++ out/05-dxmt-ios/tests/run-host-tests.sh /tmp/spec05-tests
```

Each program prints `<name>: N checks, M failures`, prints a `FAIL file:line` line for
each failed check, and exits non-zero on any failure. The script exits non-zero if
either program fails.

| Program | Covers |
|---|---|
| `test_bc_decode.cpp` + `dxmt/src/dxmt/dxmt_bc_decode.cpp` | Part C decoder: BC1 four-colour, 565 expansion and rounding, three-colour (including `color0 == color1` and transparent black); BC2 explicit alpha with the colour block always four-colour; BC3 alpha eight-value and six-value; BC4 unsigned (eight, six, equal endpoints) and signed (eight, six, negative, −128 → −127, full range); BC5 unsigned and signed; BC7 modes 0–7 (partitions 0/17/13/18/1 with their anchors, unique and shared P-bits, rotation 0–3, index selection 0/1, 2/3/4-bit weights) and the reserved mode; whole subresources with sizes that aren't multiples of 4, source and destination pitches, several rows of blocks, a 2×2 level, invalid arguments; 3D volumes decoded slice by slice (depth pitches, a too-small depth pitch); BC6H grey fill. |
| `test_mip_clamp.cpp` (header-only `dxmt_mip_clamp.hpp`) | D5 policy: bias choice (multiple-of-4 and ≥ 4×4 top level, at least one level), auto candidates, requery rule (first, every 16th, every one below 2× threshold), pressure rule, `RequestedBias` with a scripted headroom query (explicit wins, disabled, not a candidate, plenty/low/near-threshold headroom, stale reading, query unavailable), view-range translation, min LOD over lost levels, 2×2 box filter (rounded mean, signed truncation, odd and 1-texel sizes, 1/2/4 channels, pitches). |

Expected values in the BC tests are worked out by hand in the comments next to each block
(from Microsoft's block-compression and BC7 pages), not produced by the decoder.

Spec 06 (32-bit programs) adds:

| Program | Covers |
|---|---|
| `test_wow64.cpp` (header-only `winemetal/unix/ios_wow64.h`, `ios_wow64_objects.h`) | C1/C2 pointer conversion with a fake base (NULL, a normal value, the window's last byte, host pointers outside the window, a high half set); the C4 chain walker on synthetic chains (empty, one record, many, compute payloads, a cycle, a self-cycle, longer than the maximum, bad links) with refused chains checked byte for byte; the Part E record table on fake objects (create/retain/release, teardown with references left, teardown after the program released everything, a release the count didn't see, an address reused, a full table). The cycle cases need the test arena above 4 GB (as every window is on iOS); on a host where it isn't they print a note and are skipped. |
| `test_atomic_wait.cpp` (`util/util_atomic_wait.hpp`, generic backend) | a waiter wakes on notify, notify_all wakes every waiter, a wait whose value already changed returns at once, the backend is logged once. |

Spec 07 (Direct3D 9) adds:

| Program | Covers |
|---|---|
| `test_dxso_args32.cpp` (header-only `winemetal/unix/dxso_args32.h` with `airconv/airconv_public.h`) | A3: the i386 argument structs' sizes; guest-pointer conversion (NULL, a normal value, the window's last byte); the chain conversion on synthetic chains in a fake window: empty, an input layout (its element pointer converted, a NULL one kept), every argument type in one chain with all fields checked, reserved and unknown types skipped, a self-cycle stopped at the limit, a cycle of skipped types; the 32-bit chain is checked byte for byte to be left unwritten. |
| `bench_dynamic_discard.cpp` (+ `dxmt_buffer.cpp`, `dxmt_dynamic.cpp`, `dxmt_allocation.cpp`, `util_bloom.cpp`, fake Metal) | Map(`WRITE_DISCARD`) on a 64 KB dynamic constant buffer, 33,000 cycles a frame, through DXMT's own buffer code and the discard policy, with a GPU model running two chunks behind and a frame latency of 3. `--check` (in the runner, 4 frames): every draw still reads the bytes it was recorded with when its chunk finishes, peak Metal-buffer memory stays under the in-flight cap + one commit + the finished pool, and no Metal buffers are created after the first frame. Without `--check` it is the benchmark: `bench_dynamic_discard [frames] [cycles] [new|old]` prints the time per cycle (with and without the memcpy), Metal buffers created and peak memory; `old` is the behaviour before this change. Only the device can measure the real Metal and D3D11-entry costs and the render-pass breaks the implicit commits add. |
| `test_swap_effect.cpp` (header-only `d3d11/d3d11_swap_effect.hpp`) | Which swap-chain descriptions are presented as asked: `FLIP_SEQUENTIAL` with 2–16 buffers (not 0, 1 or 17), `DISCARD` / `FLIP_DISCARD` with any count, blit-model `SEQUENTIAL` and unknown effects with one buffer only. |
| `test_d3d9_policy.cpp` (header-only `d3d9/d3d9_ios_policy.hpp`) | The switch rule (unset / empty / `0` / other) and number parsing; C3 threshold (default 64 MB, `0` never, junk), the pressure count, reset by any commit, the remembered pressure commit; C4 16 MiB budget on and off, the 64 KiB reserve cap (off with the budget), the tight-copy rule and copy; C5 scene rule; C6 batch split (16 MiB, an oversized entry alone); C7 poll runs (start-to-start gap of 250 µs, the third not-ready poll and later park, a new run after a longer gap, a clock going backwards, non-adaptive counting since Issue, the flush once per Issue); C9 mode lists for the three formats and none for others; C13 limit parsing and cap; C15 budget (256 KB per resource, 32 MB in all, release). |

## 2. `build-ios.sh` in CI (macOS runner, Xcode 26)

```sh
out/05-dxmt-ios/build-ios.sh out/05-dxmt-ios/dxmt <llvm-15.0.7-build> <llvm-project>/llvm <out>
```

Pass: every file prints `OK`, the script exits 0, and `<out>/libdxmt_unix.a` exists.
Also check:

- `nm <out>/libdxmt_unix.a | grep dxmt_winemetal_unix_call_funcs` shows the table, and the
  exported `madeira_*`/`dxmt_has_40_cap`/`gpu_meter_*` functions are defined (`T`).
- No `__wine_unix_call_funcs` symbol is defined (the iOS tables are renamed).
- The Windows half still builds with meson for both cross files
  (`build-win-aarch64.txt`, `build-win-arm64ec.txt`, toolchain path filled in), with
  `-Dmetal_std=metal3.1`.

## 3. Device checks (owner)

On an iPhone, with the app linking `libdxmt_unix.a`:

1. **Test cube.** A D3D11 sample that draws a spinning cube presents at a steady 60 fps
   with the default pacing (vsync mode 1); `madeira_get_present_count` keeps increasing.
2. **BC textures on an A15 iPhone** (no BC support): a game with BC1/BC3/BC5/BC7 textures
   shows correct colours and alpha; the log has the B5 capability line with BC = no; no
   B10 "row pitch too small" lines for BC textures.
3. **Memory.** Loading a large game stays below the app's memory limit: watch the D1 budget
   log lines (moves of 64 MB), set `vram-mb` / `ram-reserve-mb` in `madeira.cfg` and check
   `DedicatedVideoMemory` follows; with `d3d11.mipClampAuto = True` large BC textures are
   clamped once headroom drops below `d3d11.mipClampAutoMB`.
4. **Shader cache.** The second launch of a game compiles fewer shaders; the cache lives
   under the app's Caches directory.

Spec 06 (32-bit programs; build the Windows half with `build-win-i686.txt` and
`-Dwine_builtin_dll=true`, which must build cleanly):

5. **32-bit test program.** A 32-bit D3D11 test program renders (the cube of check 1).
   The log shows `atomic wait: ntdll ...` once per module and no "call refused" or
   "outside the program's window" lines.
6. **32-bit game.** A 32-bit game runs and its textures look right.
7. **Two 32-bit games in one session.** Play one, quit it, start another in the same Wine
   session: the log shows `released N Metal objects of the 32-bit program at window ...`
   for the first, and the app's memory goes back down instead of keeping the first game's.

Spec 07 (Direct3D 9; `d3d9.dll` from the same i386 / ARM64EC builds, installed as the
programs' `d3d9.dll`):

8. **A 32-bit Direct3D 9 game renders.** For example a visual novel: its launcher (often
   windowed, GDI text through `GetDC`) and then the game itself, windowed and fullscreen.
   The log has one `d3d9 device: bc_decode=... upload_commit_mb=64 ...` line per device and
   no per-frame lines. Going fullscreen with `MADEIRA_SCREEN_W` set changes the virtual
   monitor's mode to the back buffer size; leaving fullscreen restores it (no "virtual
   display mode ... refused" line). A game that offers 16-bit modes lists them.
9. **DXT textures on an iPhone without BC** (an A15 device): a Direct3D 9 game with DXT1/DXT3/
   DXT5 textures looks right (colours and alpha), including after alt-tab / device reset and
   after the game re-locks its managed textures; the log line says `bc_decode=1` and has no
   "BC decode failed" lines.
10. **Query polling.** A game that waits on `EVENT` or `OCCLUSION` queries in a loop (many
   older engines do at the end of a frame) doesn't keep a whole core busy: watch the CPU
   use of the game's thread while it waits (Instruments or the Xcode gauge); compare with
   `DXMT_D9_QUERY_ADAPTIVE=0`, which parks from the third not-ready poll after each Issue
   however far apart the polls are.
11. **Loading.** A game that uploads many textures while loading (no Present for a while)
   finishes loading without the app being killed for memory; set `DXMT_D9_UPLOAD_COMMIT_MB=0`
   and `MADEIRA_D3D9_BATCH_BUDGET=0` to compare.
