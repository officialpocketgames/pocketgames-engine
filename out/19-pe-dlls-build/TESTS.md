# Spec 19 tests

## Host tests (WSL)

From Git Bash:

```bash
wsl.exe -d Ubuntu -- bash -lc "cd /path/to/pocketgames-engine/out/19-pe-dlls-build && ./tests/run-host-tests.sh"
```

`tests/run-host-tests.sh` (POSIX sh) builds every test with gcc and clang, `-Wall -Wextra
-Werror`, ASan and UBSan, and exits non-zero on any failure.

| Test | What it checks |
|---|---|
| `test_part_a.c`: tick | A1: `tick_convert.h` for frequencies 10 MHz, 0, 24 MHz, 1 GHz, the prime 1000003, the prime 2^61 − 1 and 2^64 − 1 (the last two take the long-multiplication path), counters 0, small, around 2^63 and up to 2^64 − 1: the 100 ns value, the milliseconds and their low 32 bits against `unsigned __int128` arithmetic |
| `test_part_a.c`: clamp | A3: with and without the large-address flag, totals below, at and above `0x80000000` |
| `test_part_a.c`: minidump | A6: the 32 bytes for `MiniDumpNormal`, `MiniDumpWithFullMemory \| MiniDumpWithThreadInfo`, and a type with a bit above 32 |
| `test_part_a.c`: nsi | A9: the switch parser (unset, empty, `0`, `00`, `0off`, `1`, `10`, ` 0`, `off`); the status mapping of items 4 and 5 and the probe; when a missing device is remembered and when requests stay pending; the pending-request state machine on a fake `OVERLAPPED` with fake events (request, cancel, cancel twice, cancel of a request never made, NULL, the event handle's low bit, no event) |
| `test_part_a.c`: services | A10: the autostart decision for start types 0–4, PnP and not |
| `test_part_a.c`: d3dcompiler | A12: `compile_failure_log.h`: 32 reports, then one notice, then silence (also for huge counts and 0); message lines with CR dropped and empty lines skipped, a NUL ending the text, a blob without a NUL, at most 12 lines, at most 2048 bytes (the last line cut, also one line longer than the budget) |
| `pe-syntax-check.sh` | Every C file of `pe/` compiles with clang 21 for `arm64ec-windows`, `aarch64-windows` and `i686-windows`, `-Wall -Werror`, as makedep compiles a PE object (`-D__WINE_PE_BUILD`, the module's `EXTRADEFS`, `_UCRT`, Wine's msvcrt headers), with spec 17's shared headers in front of `include/`. The modules' `.idl` headers are made with `$WINE_BUILD/tools/widl/widl`. Skipped without a configured Wine build (`WINE_BUILD`, default `~/wine-build`) |

| `test-pecheck.sh` (with `mkpe.c`) | B5.2, with pecheck built by gcc and by clang (ASan/UBSan): a good ARM64EC, aarch64 and i386 farm (imports inside the farm in any case, `api-ms-win-*` and `ext-ms-win-*` imports, an import of each of the ten "provided by other parts" names, a `.tlb` and a `.msstyles` that aren't images, the size table); no builtin marker; each wrong machine (aarch64, ARM64X `0xa64e`, plain x86-64 and i386 in the ARM64EC farm; x86-64, ARM64EC, ARM64X and a hybrid `0xaa64` in the aarch64 farm; x86-64 and aarch64 in the i386 farm); 4 KB alignment in a 64-bit farm (error) and 16 KB (a note); PE32 in a 64-bit farm; a data section before code; a `.debug_info` section left (a long name); a missing normal import in a 64-bit and the i386 farm (errors); missing delay imports in both descriptor formats (reported, not errors, API sets and farm members not reported); a subfolder, a hidden file, a symlink, a file that isn't a PE image; `--imports`, `--machine`, `--list-provided` |
| `test-unixlibs.sh` | B4 against spec 09's `unixlibs.txt`: every DLL with a unix call table is in `farms/arm64ec-windows.txt` and `farms/aarch64-windows.txt` and not left out by `farms/i386-skip.txt`, or `farms/unixlibs-skip.txt` names it for that farm with a reason; a skip line for a DLL a farm lists, an unknown farm, a missing reason or a DLL not in `unixlibs.txt` (stale) fail. `run-host-tests.sh` also runs it on a fixture with a DLL no farm lists and expects that failure |
| `test-plan.sh` | B7 `plan` against `refs/wine-11.4` and the real overlays with `--allow-missing-overlays` (each overlay and its files, `new` marks, every name of both 64-bit lists mapped to a target, no programs in the ARM64EC farm, no errors); without the option, each missing overlay named as an error; a fixture where spec 15's and spec 18's folders both deliver `dlls/dinput/dinput.c` (plan fails naming both; the `tree` step fails naming its step and copies nothing); a fixture with `dlls/x/unix/b.c` and `server/thread.c` (both named as errors) |
| `test-build-fake.sh` | The whole of `build-pe.sh all` in about half a minute, on a fake Wine tree (one Makefile.in per module of both lists, plus extra and i386-only modules, and a stub `configure` whose Makefile makes each module file with `mkpe`: the right machine per tree, and imports per module) and a fake toolchain whose clang and strip do nothing. Checks: the configure arguments of each tree (the arm64ec tree builds the tools, the others use them; only the aarch64 tree enables the four programs); the type library link; the import closure over two rounds (`closure adds extra1.dll (imported by kernel32.dll)`, then extra2), with API sets, a provided name and a delay import not added; `--extra` (a provided name and another DLL; and a refused extra named like a Wine module); the i386 farm (the rule, the skip list, `.sys` and `.exe16` left out, `.tlb` in); pecheck passing on all three farms; NLS; MANIFEST.txt lines (one per file; `wine`, `wine+spec 19`, `extra:<dir>`; machine words) and BUILDINFO.txt; `<dest>` holding exactly the six entries; a second run copying, configuring and building nothing; a module whose rule fails (retried with `-j1`, then reported with its log and first error line, naming the step); a toolchain archive with the wrong SHA-256 (removed, not unpacked; a planted file, no download); the read-only trees (a symlinked `--wine`: `<work>/src` is a real folder and the overlay never reaches the linked tree; a `--work` inside `refs/`, directly or through a link, inside the `--wine` tree, a `--dest` inside `refs/`, and a `--wine` inside `--work` are refused and create nothing); the ARM64EC tree's configure gets the plain clang and the other two `--with-mingw=llvm-mingw`; the relinking kit (spec 00 E1: its files, the overlaid source, the product's MANIFEST.txt, the README's command) and a rebuild from the unpacked kit with that command giving the same three farms; the App Review 2.5.2 check (a clean appwiz.cpl passes with its line; one built with "winemono" appended in UTF-16 stops `collect` naming the farm, file and string, and no `<dest>` is written). What it can't check is the real configure and compile |
| `check-app-store.sh` | Spec 00 F, run on `out/19-pe-dlls-build` by `run-host-tests.sh`: must print "clean" |
| `test-tree.sh` (`SLOW=1`) | B2 for real with fixture overlays: the copy is overlaid; a second run with nothing changed is skipped; after one overlay file changed and another was removed, the unchanged one keeps its time, the changed one and the one back to upstream's content are newer, an upstream file keeps its time. A few minutes (three copies of the tree); pass `TREE_SCRATCH` on a Linux file system |

What only a real build can check: linking (the new `GetPointerDevice` export, `__wine_init_unix_call`
from winecrt0 in nsi.dll), and everything at run time below.

## The full build check

It needs the pinned llvm-mingw for Linux (the `toolchain` step downloads
`llvm-mingw-20260421-ucrt-ubuntu-22.04-x86_64.tar.xz` from the llvm-mingw release and checks its
SHA-256), and a few hours. In WSL, on a Linux file system:

```bash
wsl.exe -d Ubuntu -- bash -lc "cd /path/to/pocketgames-engine/out/19-pe-dlls-build && ./build-pe.sh --work ~/pe-work --allow-missing-overlays all"
```

Expected: all three farms build, every `pecheck` passes, and the farm lists and every module's
export names equal those of the latest macOS CI run (the PE output doesn't depend on the build
host).

2026-10-21, WSL, every overlay present (no `--allow-missing-overlays`): `all` succeeds.
arm64ec-windows 145 files (133 MB; the closure adds `bluetoothapis.dll`), aarch64-windows 126
files (104 MB), i386-windows 715 files (152 MB); pecheck: 0 errors in each (notes for the three
data-only modules, delay imports listed); `kerberos.dll`, `msv1_0.dll`, `winspool.drv` and
`msvcp_win.dll` in all three farms; d3dcompiler_47/_43 carry A12's log strings; the relinking
kit is written.

Earlier (2026-10-19, WSL, llvm-mingw 20260421 = clang 22.1.4, a fresh `--work`):

- The `arm64ec` step: 140 of the 141 listed modules build, msvcrt, mswsock and msxml3 among
  them. The one failure is `ntdll.dll`, in spec 18's overlay `dlls/ntdll/loader.c`
  (`bind_module_delay_imports` passes `char *base` to `get_rva( HMODULE, ... )`; clang 22 makes
  that an error), reported to the spec 18 side. Before the B3 compiler-mode change
  (QUESTIONS 19-13) every C file failed on winnt.h's `__fastfail`; before the type library link
  fix (QUESTIONS 19-5) eight modules failed with `cannot find stdole2.tlb`.
- pecheck on those 140 files, stripped with `arm64ec-w64-mingw32-strip --strip-debug` and
  re-marked with `winebuild --builtin`: all are ARM64EC images with the builtin marker, 64 KB
  section alignment, code first, no `.debug` sections; three data-only modules get a note
  (QUESTIONS 19-15); the only errors are imports of `ntdll.dll` (not built) and
  `bluetoothapis.dll` (imported by bthprops.cpl, which `collect`'s import closure adds).
- `tests/try-arm64ec-modes.sh` (a diagnostic) compiles one Wine file with a toolchain in the GNU
  triple and in the MSVC target, for any CPU: ARM64EC fails only in the GNU triple; aarch64 and
  i686 compile in both.
- Not yet run: the aarch64 and i386 trees, `collect`, `kit`.

## On the device

Each with the log lines to look for.

1. **Tick count (A1).** A 64-bit game that calls `GetTickCount64` in its main loop: the fault
   handler's low-window count (spec 11) stays flat while it runs. A 32-bit game's tick count
   advances normally.
2. **Version (A2).** `winver`, or a program printing `GetVersionEx`, reports the prefix's version,
   not 6.2.9200.
3. **Steam's web UI (A4, A7, A8.1, A9 with spec 13).** It starts, shows its login page and its QR
   code, and stays up: no delay-load crash, loopback peers accepted, no cryptnet crash. A
   `synchronous retrieval got ERROR_IO_PENDING from ...` line may appear instead of a crash;
   `FIONBIO to ... failed` lines say why.
4. **Crash handler (A6).** On a hung web helper: one `MiniDumpWriteDump`-side line
   `pid <n> type <t>: header only` (channel `dbghelp`), the handler finishes at once, memory
   stays flat.
5. **COM right after boot (A5).** A program's first COM call succeeds; when it had to wait,
   `service manager opened after <n> retries` (channel `ole`).
6. **Adapter polling (A9).** A program calling `NotifyAddrChange` in a loop: `no \\.\Nsi: change
   requests stay pending` once, `\\.\Nsi doesn't exist (error 2); not trying again in this
   process` once, and no tight loop of NSI reads in the log. With `MADEIRA_NSI_NOTIFY_PENDING=0`
   the requests fail as upstream.
7. **Services (A10).** The services log names every service started automatically
   (`autostart <name>: type ..., start type ..., pnp ..., result ...`) and no disabled one.
8. **Memory status (A3).** A 32-bit program without the large-address flag: `GlobalMemoryStatus`
   matches `GlobalMemoryStatusEx`; with `MADEIRA_LAA=0` both show 2 GB.
9. **URL cache (A8.2–A8.4).** A broken `index.dat`: `broken index ... re-initialised in place:
   error 0` and no stack overflow. A full index: `index of ... is full` lines and no loop. Steam's
   start-up purge with files in use: `cache folder ... partly deleted ..., counted as success` once
   per call, and the purge stops calling.
11. **No add-on downloads (A13).** A program that embeds HTML (mshtml) or a .NET program
    (mscoree) on a prefix without Gecko or Mono: `err:appwizcpl:install_addon Gecko add-on: not
    installed (this build installs no add-ons)` (or Mono) once per process, no installer dialog,
    no network traffic to winehq.org, and the program goes on as without the add-on.
10. **Failed shader compiles (A12).** A game that compiles HLSL at run time with a shader that
    fails (or, for checking, any HLSL with a syntax error through `D3DCompile`): `err:d3dcompiler`
    lines `compile failed: 0x80004005 source "..." entry "..." target "ps_5_0" flags 0x...`
    followed by `compile failed:   <file>:<line>:<col>: E....: ...` lines with the compiler's
    message, at most 12; after 32 failures one `further failures are not logged` line and then
    silence. A successful compile logs nothing on that channel.
