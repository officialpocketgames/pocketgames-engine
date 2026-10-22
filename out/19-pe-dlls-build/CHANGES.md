# Spec 19: changes to upstream Wine 11.4

Every file under `pe/` is a full file at its path in the Wine tree. Modified upstream files keep
their licence header and say near the top that they were changed for MYIOSDECK; each change is
marked in the code with `MYIOSDECK (spec 19 Part ...)`. New files (MIT) hold the pure decisions
that the host tests check.

## Part A

| Item | File | Function | Change |
|---|---|---|---|
| A1 | `dlls/kernelbase/sync.c` | `GetTickCount`, `GetTickCount64`, `QueryInterruptTime` | `#ifdef _WIN64` (the ARM64EC and aarch64 farms): one `RtlQueryPerformanceCounter` + `RtlQueryPerformanceFrequency`, converted to 100 ns (`tick_counter_to_100ns`), `/ 10000` for milliseconds, low 32 bits for `GetTickCount`. The i386 build keeps upstream's shared-data reads (and the `user_shared_data` pointer moved inside the `#else`) |
| A1 | `dlls/kernelbase/tick_convert.h` (new) | `tick_counter_to_100ns`, `tick_scale_remainder`, `tick_100ns_to_ms`, `tick_100ns_to_ms32` | The counter as is at 10 MHz or frequency 0; otherwise whole seconds and remainder separately. For a frequency above 2^64 / 10^7 (no real counter) the remainder is scaled by binary long multiplication, so no product overflows for any input; the result is `floor(counter * 10^7 / frequency)` modulo 2^64 |
| A2 | `dlls/kernelbase/version.c` | `init_current_version` | Reports exactly what `RtlGetVersion` reports. Upstream's Windows 8 / 8.1 tables and the manifest compatibility lookup are removed (they had no other user) |
| A3 | `dlls/kernel32/heap.c` | `GlobalMemoryStatus` (32-bit) | The 2 GB clamp applies only when the image lacks `IMAGE_FILE_LARGE_ADDRESS_AWARE` **and** `GlobalMemoryStatusEx` reported a total virtual size of at most `0x80000000` (`memstatus_clamp.h`, new) |
| A4 | `dlls/user32/misc.c`, `user32.spec` | `GetPointerDevice` | New export `@ stdcall GetPointerDevice(ptr ptr)`, in place of the commented `# @ stub GetPointerDevice` line of the spec file (the alphabetical position, six lines above `GetPointerDevices`). NULL info → `ERROR_INVALID_PARAMETER`, otherwise `ERROR_NOT_SUPPORTED`; always `FALSE`. A `FIXME` on the first call only |
| A5 | `dlls/combase/rpc.c` | `start_rpcss` | `OpenSCManagerW` is retried every 250 ms, at most 20 times, before upstream's failure; `ERR("service manager opened after %u retries")` once when it came up after retries |
| A6 | `dlls/dbghelp/minidump.c` | `MiniDumpWriteDump` | Writes the 32-byte header (`minidump_header.h`, new: signature `MDMP`, `MINIDUMP_VERSION`, 0 streams, directory RVA 32, checksum 0, `time(NULL)`, the dump type as flags) at offset 0 of the file and returns `TRUE`, without looking at the process. `ERR("pid %lu type %#x: header only")` per call. Upstream's dump code stays under `#ifdef MYIOSDECK_FULL_MINIDUMP` (never defined) |
| A7 | `dlls/cryptnet/cryptnet_main.c` | `CRYPT_DownloadObject` (query data available, read), `HTTP_RetrieveEncodedObjectW` (send request, end request) | With no context (a synchronous retrieval), `ERROR_IO_PENDING` → `ERROR_TIMEOUT` and one `ERR` line naming the wininet call (`CRYPT_SyncRetrievalPending`) |
| A8.1 | `dlls/wininet/netconnection.c` | `set_socket_blocking` | `is_blocking` is updated only when `ioctlsocket(FIONBIO)` succeeded; on failure an `ERR` with the requested mode, `WSAGetLastError()` and the mode kept |
| A8.2 | `dlls/wininet/urlcache.c` | `cache_container_open_index` | A broken index: close the mapping, reopen `index.dat` and `cache_container_set_size(container, file, MIN_BLOCK_NO)` (a new empty index of the minimum size, new cache directories), `ERR` with the result. No `FreeUrlCacheSpaceW` |
| A8.3 | `dlls/wininet/urlcache.c` | `cache_container_clean_index` | When cleaning leaked entries frees nothing: `ERR` and `ERROR_NOT_ENOUGH_MEMORY`. The close/double/re-map growth is removed |
| A8.4 | `dlls/wininet/urlcache.c` | `cache_container_delete_dir` | `SHFileOperationW` returning `ERROR_SHARING_VIOLATION` or `ERROR_CANCELLED`, or success with `fAnyOperationsAborted`, counts as success (with an `ERR` line); other failures stay failures |
| A9 | `dlls/nsi/nsi.c` | `get_nsi_device`, `NsiEnumerateObjectsAllParametersEx`, `NsiGetAllParametersEx`, `NsiGetParameterEx`, `NsiRequestChangeNotificationEx`, `NsiCancelChangeNotification` | The fallback to nsi.dll's own unix-call table (spec 13 Part I1), see below |
| A9 | `dlls/nsi/nsi_fallback.h` (new) | `nsi_switch_on`, `nsi_map_enumerate_status`, `nsi_map_parameter_status`, `nsi_probe_allows_parameters`, `nsi_remember_missing_device`, `nsi_notify_stays_pending`, `nsi_pending_request`, `nsi_pending_cancel` | The decisions, shared with the tests |
| A10 | `programs/services/services.c` | `scmdatabase_autostart_services`, `delayed_autostart_callback` | A `SERVICE_DISABLED` service is never started automatically, also when it is a root PnP device service (`services_autostart`, `autostart.h`, new). One `ERR` per service started automatically: name, service type, start type, PnP or not, result. For a delayed auto-start service (started later from a timer, after the device list is gone) the PnP column reads `n/a` |
| A12 (2026-10-21, device-log follow-up) | `dlls/d3dcompiler_43/compiler.c` (built into every `d3dcompiler_*` through their `PARENTSRC`, so d3dcompiler_47 and _43 in all farms) | `D3DCompile2` (reached by `D3DCompile` and `D3DCompileFromFile`), `D3DPreprocess`, `D3DCompileFromFile` | A failure HRESULT is logged at `ERR` on the `d3dcompiler` channel: `compile failed: <hr> source "<name or (memory)>" entry "<entry or ->" target "<target or ->" flags <flags1>`, then up to 12 lines, at most 2 KB, of the compiler's messages (the error blob's text) as `compile failed:   <line>` (`compile_failure_log.h`, new: empty lines skipped, the last line cut to fit). At most 32 reports per process, then `compile failed: further failures are not logged (32 reported)` once. Successes log nothing. D3DCompileFromFile logs its own failures before compiling (file not opened or read, no messages); a compile failure is logged once, by D3DCompile2. When the caller passes no messages blob, D3DCompile2 and D3DPreprocess ask the compiler for one of their own and release it, so the log still has the messages; the caller sees no difference. D3DPreprocess's line has entry and target `-` and flags 0 |
| A13 (2026-10-22, mock App Review must-fix, guideline 2.5.2: no downloaded code) | `dlls/appwiz.cpl/addons.c`, `dlls/appwiz.cpl/appwiz.rc` (every farm that ships appwiz.cpl; today i386-windows) | `install_addon` (behind `install_gecko` / `install_mono`, which mshtml and mscoree start as `control.exe appwiz.cpl install_gecko` / `install_mono`) | `install_addon` returns `FALSE` at once: no registry, directory, cache or download lookup, no dialog; one `ERR` per process (`Gecko add-on: not installed (this build installs no add-ons)`). Upstream's installer (the `http://source.winehq.org/winegecko.php` and `winemono.php` URLs, the urlmon download, the SHA check, the dialog procedure) stays in the file under `#ifdef MYIOSDECK_ADDON_DOWNLOAD`, which no build defines, so it is not compiled. appwiz.rc loses `ID_DWL_GECKO_DIALOG` and `ID_DWL_MONO_DIALOG` (their text linked to `https://gitlab.winehq.org/...`; the translations of those dialogs no longer apply). `start_params` is unchanged: it still reports the parameter as handled, so control.exe opens no applet window either |

### A9 in detail

- **Item 1.** `nsi_fallback_available()`: `__wine_init_unix_call()` (winecrt0) once per process
  (`InitOnceExecuteOnce`); success sets `__wine_unixlib_handle`, and the entries are called with
  `WINE_UNIX_CALL(entry, params)`, the caller's parameter block as is (the unix side's wow64
  table converts a 32-bit caller's).
- **Item 2.** `nsi_parameters_available()`: once per process, entry 0 with a zeroed
  `struct nsi_enumerate_all_ex` except `module = &nsi_ndis_module` (the NDIS module id defined in
  `nsi.c`, from `netiodef.h`'s value) and `table = NSI_NDIS_IFINFO_TABLE`; anything but
  `STATUS_NOT_SUPPORTED` allows entries 1 and 2. Logged once (`ERR`) with the status and count.
- **Item 3.** `get_nsi_device()` remembers `ERROR_FILE_NOT_FOUND` per handle kind
  (`nsi_missing_device_error[0]` synchronous, `[1]` overlapped) when the fallback exists and
  `MADEIRA_NSI_DEVICE_CACHE` is on; later calls set that last error and fail at once. Logged once.
- **Item 4.** No device: no fallback → the device error; otherwise entry 0, first call logged
  once (device error, status); `STATUS_BUFFER_OVERFLOW` → `ERROR_MORE_DATA`,
  `STATUS_NOT_SUPPORTED` → the device error, else `RtlNtStatusToDosError`.
  `NsiAllocateAndGetTable` and `NsiEnumerateObjectsAllParameters` reach it through
  `NsiEnumerateObjectsAllParametersEx` unchanged.
- **Item 5.** No device: no fallback or no item 2 answer → the device error; otherwise entry 1 or
  2; `STATUS_NOT_SUPPORTED` → the device error, else `RtlNtStatusToDosError`.
- **Item 6.** No device with `ERROR_FILE_NOT_FOUND`, the fallback, and `MADEIRA_NSI_NOTIFY_PENDING`
  on: with an `OVERLAPPED`, `Internal = STATUS_PENDING`, `InternalHigh = 0`, its event (low bit
  masked) reset, `*handle` (if asked for) the process-wide manual-reset event nothing sets,
  `ERROR_IO_PENDING`; without one, `WaitForSingleObject(that event, INFINITE)`. Logged once.
  `NsiCancelChangeNotification`, when that event exists and the overlapped device is missing:
  `Internal != STATUS_PENDING` → `ERROR_NOT_FOUND`; otherwise `Internal = STATUS_CANCELLED`, the
  event (low bit masked) set, `ERROR_SUCCESS`.
- **Item 7.** Both switches read once per process with `GetEnvironmentVariableA` (Wine copies
  the app's environment into the Windows one); off only when set and starting with `0`.

## A11: modules that stay as upstream

`dlls/d3d9` (not in any farm: Part B4's lists leave it out and the i386 skip list names it),
`dlls/dwrite/font.c`, `dlls/rpcrt4`, and every module not named above or in specs 14–18 are
compiled from upstream's sources unchanged, in the one overlaid tree of Part B2.

## Part B: `build-pe.sh`, `farms/`, `tools/pecheck.c`

`build-pe.sh [options] <step>` (bash, `set -euo pipefail`, bash 3.2 compatible). The script's
header lists the steps and options; this is what each part of the spec became.

| Item | Where | What |
|---|---|---|
| B1 | `LLVM_MINGW_*`, `step_toolchain`, `setup_path` | The two pinned assets and their SHA-256; the one for the host (`uname -s`/`-m`) is downloaded with `curl -fL` into `<work>/downloads/`, checked (a mismatch removes the download and stops before unpacking), unpacked into `<work>`. `--toolchain <dir>` skips that. Either way `bin/<cpu>-w64-mingw32-clang` and `-strip` must exist for aarch64, arm64ec, i686 and x86_64, and `bin/clang` and `bin/lld-link` for the ARM64EC tree (B3). The toolchain's `bin/` goes first on `PATH`; Homebrew's bison and flex (`/opt/homebrew/opt/...`, `/usr/local/opt/...`) are added when present; bison older than 3 stops the build |
| B2.1 | `abspath`, `physpath`, `inside`, `step_tree` | Paths are resolved physically (symlinks followed). `--work` or `--dest` resolving into `refs/` (next to `--out-root`) or into the `--wine` tree, or a `--wine` tree inside `--work`, is refused before anything is created (both trees are read-only). `<work>/src` is a new real folder holding a copy (`cp -pR <tree>/.`) of `--wine`, never the reference tree itself or a link to it (QUESTIONS 19-14). It is re-made when the stamp's hash (the script, every overlay file's path and SHA-256, the tree's `VERSION`) changed or a file of the reference tree is newer than the stamp. On a re-copy, every overlaid path whose content is the same as before keeps its old time, and every other one (changed, new, or back to upstream's file) gets the current time, so make rebuilds what changed and only that |
| B2.2 | `overlay_table` | The table, in one place: name, source under `--out-root`, path in the tree, spec (`\|`-separated). A file source is a single-file row (only that file). Missing overlays are errors, or with `--allow-missing-overlays` a logged skip. Files whose names start with `.` are not overlay files |
| B2.3 | `overlay_errors` | A path delivered by two overlays: `ERROR: <path> is delivered by two overlays: <a> and <b>`. A path under `dlls/*/unix/` or `server/`: an error naming the path and its overlay. The `tree` step stops before copying anything |
| B2.4 | `configure_tree` | Overlays are in place before configure. Configure runs again when its arguments, `configure`, any Makefile.in an overlay delivers, or the toolchain changed |
| B3 | `mingw_option`, `configure_args`, `configure_tree`, `ensure_tools` | Three build directories `<work>/build-{arm64ec,aarch64,i386}`, each `--enable-archs=<one arch>`. The compiler: `--with-mingw=llvm-mingw` (the GNU triple `<cpu>-w64-mingw32-clang`) for aarch64 and i386; for ARM64EC the toolchain's plain clang, `--with-mingw=<toolchain>/bin/clang`, which configure targets as `arm64ec-windows` (MSVC environment), because under the GNU triple clang defines `__x86_64__` without `_MSC_VER` and every file then fails on winnt.h's x86 `__fastfail` assembly (QUESTIONS 19-13). Then `--disable-tests --without-x --without-freetype --without-gnutls --without-vulkan --without-wayland --enable-winegstreamer CROSSCFLAGS=-O2`. The arm64ec tree builds Wine's host tools (`make __tooldeps__`); the other two get `--with-wine-tools=<work>/build-arm64ec`. The aarch64 tree gets `enable_conhost=aarch64 enable_rpcss=aarch64 enable_services=aarch64 enable_wineboot=aarch64`. No alignment or register flags |
| B3 | `link_typelibs` | ARM64EC tree only: widl looks for an imported type library under `aarch64-windows/` for an ARM64EC target (`tools/tools.h`, `get_arch_dir`), on every host, while the tree builds it under `arm64ec-windows/`. Before building, each `.tlb` module rule `dlls/<m>/arm64ec-windows/<f>.tlb` gets the link `dlls/<m>/aarch64-windows/<f>.tlb -> ../arm64ec-windows/<f>.tlb` (it may dangle until make builds the target, which the importing `.res` rules depend on). The other trees' widl looks under their own folder (QUESTIONS 19-5) |
| B3 | `make_targets` | Only file targets (`dlls/<m>/<arch>-windows/<file>`), `make -k -j<jobs>`; each missing file is retried once with `-j1` into `<work>/logs/<arch>-<module>.log`; anything still missing stops the step listing each module, its log and its first error lines |
| B4.1, B4.2 | `farms/arm64ec-windows.txt`, `farms/aarch64-windows.txt`, `list_targets`, `farm_targets` | The lists, one name per line. A name maps to its folder through the `MODULE =` line of the tree's Makefile.in files; a name with no module, or one the configured tree has no rule for, stops the step |
| B4.1, B4.2 (2026-10-21) | `farms/arm64ec-windows.txt`, `farms/aarch64-windows.txt` | Added beyond the spec's lists, each marked in the list: `kerberos.dll` and `msv1_0.dll` to both 64-bit farms (secur32, in both, loads them as its LSA packages from `HKLM\System\CurrentControlSet\Control\Lsa`; a 64-bit game failed with `load_dll failed to load L"Kerberos.dll", status c0000135` and the same for MSV1_0.dll, while spec 12 clarification 9 had linked their unix tables); `winspool.drv` to aarch64-windows (comdlg32's print dialogs delay-load it for Wine's ARM64 programs; it was already in arm64ec-windows); later the same day `msvcp_win.dll` to both 64-bit farms (Microsoft's x64 `D3DCompiler_47.dll` in a game's folder failed with `import_dll Library msvcp_win.dll ... not found`; Wine's msvcp_win is a spec-only module forwarding 3374 exports to msvcp140, which both farms have, with 386 stubs; the i386 farm already had it) |
| B4 / spec 09 clarification 6 | `farms/unixlibs-skip.txt`, `tests/test-unixlibs.sh` | No unix call table without its DLL: every DLL of `out/09-ntdll-processes/unixlibs.txt` is in both 64-bit lists and not skipped from i386-windows, unless `unixlibs-skip.txt` names it for that farm (or `*`) with a reason. Today's exceptions: `winemetal.dll` (DXMT's), `winecoreaudio.drv` (reached by name only, spec 14 Part A item 3), `winegstreamer.dll` for aarch64 (the 64-bit media path is ARM64EC's) |
| B4.3 | `farms/i386-skip.txt`, `tree_rules`, `i386_targets` | Every `dlls/*/i386-windows/*` and `programs/*/i386-windows/*` rule of the configured i386 tree with one of the ten extensions, minus the skip list (names or shell patterns, compared in lower case) |
| B4.4 | `closure` | In `collect`, for both 64-bit farms: `pecheck --imports` on every built (unstripped) module and every `--extra` file; a normal import that is neither in the farm, provided (`pecheck --list-provided`) nor `api-ms-win-*`/`ext-ms-win-*` is added if the tree has a rule for it, built, and printed (`closure adds X (imported by ...)`); otherwise the step stops. Repeated until nothing is added. Delay imports are only listed. The additions are kept in `<work>/stamps/closure-<farm>` so the arch step builds them next time |
| B4.5 | `tools/pecheck.c` (`provided_names`), `prepare_extras`, `check_extras` | The ten names, in one place (the script asks `pecheck --list-provided`). `--extra <farm>=<dir>` (repeatable) copies the regular files of `<dir>` (lower-case names) into the farm before the checks; a name that is also a module built for that farm is an error unless it is one of the ten |
| B4.6 | `step_collect` | Files keep Wine's lower-case names |
| B5.1 | `step_collect` | `<cpu>-w64-mingw32-strip --strip-debug` on the copy in the farm folder, then `winebuild --builtin` on it (from the arm64ec tree's tools), as Wine's own `make install` does, so the builtin marker survives whatever strip does to the DOS stub. `.tlb` and `.msstyles` are copied as they are. No padding |
| B5.2 | `tools/pecheck.c`, `build_pecheck` | `pecheck <farm> <folder>`: see below. Built with the host C compiler (`CC`, else `cc`, `clang`, `gcc`), resolved to a full path before the toolchain's `bin/` goes first on `PATH`: a `CC=clang` would otherwise pick the toolchain's Windows-targeting clang |
| B5.4 | `step_collect` | `src/nls/*.nls` into `<dest>/nls/` |
| B5.5 | `step_collect` | `MANIFEST.txt`: tab-separated farm, name, size, SHA-256, machine (`pecheck --machine`; `-` for NLS tables), origin (`wine`; `wine+spec NN[,MM]` when an overlay delivers a file inside the module's folder or its Makefile.in's `PARENTSRC` folder (d3dcompiler_47 compiles d3dcompiler_43's sources), from the overlay table's spec column; `extra:<dir>`). Spec 17's `include/` overlay doesn't count as a module's own source change (it reaches every module). `BUILDINFO.txt`: the tree's `VERSION` line, the toolchain asset and hash (or the `--toolchain` folder), `uname -srm`, and per overlay the SHA-256 of its "path SHA-256" lines (or `missing`) |
| B6 | `step_collect` | `<dest>` is built as `<dest>.new` and replaces `<dest>` at the end: exactly `arm64ec-windows/`, `aarch64-windows/`, `i386-windows/`, `nls/`, `MANIFEST.txt`, `BUILDINFO.txt`. pecheck refuses a farm folder that isn't flat. An existing non-empty `<dest>` without a `MANIFEST.txt` is never replaced |
| B5 (2026-10-22, App Review 2.5.2) | `NO_DOWNLOAD_FILES`, `NO_DOWNLOAD_STRINGS`, `check_no_download` | After each farm's pecheck, `appwiz.cpl` (in every farm that has it) may contain none of `winegecko`, `winemono`, `http`, without regard to case, in ASCII or UTF-16 (the file is searched with every NUL removed, which also turns UTF-16 resource text into ASCII); a hit stops `collect` naming farm, file and string, and no `<dest>` is written |
| B7 | `build-pe.sh` | Options and steps as the spec lists them; every step runs the earlier steps it needs (their stamps make that cheap). `plan` prints the overlays (each file, `new` when upstream lacks it), B2's errors (exit status 1 if any), and each farm's targets (i386: the rule until the tree is configured). Failures print `build-pe: step '<step>' failed: <what>` and the log. Logs: `<work>/logs/<step>.log`, `configure-<arch>.log`, `tools.log`, `<arch>-<module>.log`. Default `--work`: `build/pe-work` next to `out/` (git-ignored) |

### pecheck

- Every file but `.tlb`/`.msstyles`: a PE image (MZ, `e_lfanew`, `PE\0\0`) with the 32 bytes
  `Wine builtin DLL` + 16 NULs at `0x40`.
- Machine: `arm64ec-windows` needs `0x8664` with a non-zero `CHPEMetadataPointer` (offset `0xc8`
  of the 64-bit load configuration, data directory 10); `0x8664` without it is "plain x86-64",
  unless no section holds code or executes (Wine's `--data-only` modules: apisetschema, normaliz,
  usp10), which gets a `note` and the machine word `x86_64-data` (QUESTIONS 19-15).
  `aarch64-windows` needs `0xaa64` without it; `0xaa64` with it is an ARM64X image, as is
  `0xa64e` in any farm. `i386-windows` needs `0x014c`.
- 64-bit farms: PE32+; section alignment a multiple of `0x4000` (a note when it isn't
  `0x10000`); by address, no section that is executable or holds code after one that is
  neither; no section whose name (long names resolved through the COFF string table) starts
  with `.debug`.
- Imports (all three farms): a normal import missing from the folder, not provided and not an
  API set is an error; delay imports (both descriptor formats) missing that way are listed per
  DLL with their importers. Names compare without regard to case.
- Folder: only regular files, no symlinks, subfolders or hidden files (B6).
- Table: file count, total size, the ten largest files. Exit status 1 on any error.
- Also `--imports <file>`, `--machine <file>`, `--list-provided` for the script.

## Spec 00: the App Store rules

`check-app-store.sh out/19-pe-dlls-build` prints "clean" (`tests/run-host-tests.sh` runs it).
What the rules mean for this part:

- **A (no code generation).** Nothing here generates or downloads code at run time. Every file
  of `arm64ec-windows` and `aarch64-windows` becomes a signed framework of the bundle in the
  owner's packaging (spec 10 Part C; B6 above); the `i386-windows` files are data for the 32-bit
  interpreter and never executable. The DLL changes of Part A make no memory executable.
- **B (public API).** Part A's code is Windows-side code calling Win32/NT API inside Wine; it calls
  no Apple API. nsi.dll reaches the host only through its unix-call table, which spec 13 Part I
  links into the app (`__wine_init_unix_call` asks ntdll for the table by name, spec 09 Part A);
  nothing is `dlopen`ed.
- **C (one process).** `services.exe`, `rpcss.exe` and the other programs of the aarch64 farm run
  as pseudo-processes (spec 09). A5's retry only waits for the service manager; it starts nothing.
  The host-only programs that would start host processes (`winemenubuilder`, `wineconsole`,
  `winedbg`) are not in any farm.
- **D3 (no downloaded code).** `build-pe.sh`'s `toolchain` step downloads llvm-mingw on the build
  machine (CI), checked by SHA-256; the app downloads nothing.
- **D4, D5.** Part A uses no privacy-sensitive or "required reason" API (it calls none on the
  Apple side; the clock behind A1's performance counter is spec 18's, which names its reason).
- **E (licences).** Wine and the libraries it bundles in `libs/`, which the build links
  statically into its DLLs, are listed with their licences in `PROVENANCE.md`; all are App Store
  compatible (MIT/BSD/zlib style, OpenLDAP, public domain, LGPL-2.1-or-later). For the LGPL code
  (Wine itself, fluidsynth, mpg123, vkd3d) the new `kit` step (also run by `all`) writes the
  **relinking kit** `<work>/pe-relink-kit.tar.xz`: the exact overlaid source tree the farms were
  built from (`src/`, with every licence file), `build-pe.sh`, `farms/`, `tools/pecheck.c`,
  `BUILDINFO.txt`, `MANIFEST.txt` and a `README.txt` saying how to build the farms again
  (`./build-pe.sh --wine src --out-root out --allow-missing-overlays all`). It is a source kit,
  more than object files: a user can change any part and rebuild. It lives outside `<dest>`,
  whose contents B6 fixes; the owner ships it with the app's other relinking material
  (QUESTIONS 19-12).

## Clarifications this spec added to other specs

- Spec 13 #2: `nsi.dll` is built here with A9, the nsi.dll side of Part I1.
- Spec 14 #1: the overlay table takes `out/14-media-audio-ios/pe/metahost.c` as
  `dlls/mscoree/metahost.c` (a single-file row).
- Spec 15 #1: `out/15-controllers/pe/` is applied as it is, before configure, so its Makefile.in
  copies put `joystick_ios.c` into dinput's and dinput8's rules in all three trees. `plan` shows
  no path that spec 15 shares with another overlay.
- Spec 17 #1: the one copy of ntdll's `unixlib.h`, `out/09-ntdll-processes/ntdll-unix/unixlib.h`,
  is a single-file row to `dlls/ntdll/unixlib.h`; `out/09-ntdll-processes/include/` goes over
  `include/`. Nothing else in `ntdll-unix/` is taken.
