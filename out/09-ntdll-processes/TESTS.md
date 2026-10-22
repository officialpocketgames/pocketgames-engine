# Spec 09: tests

## Host syntax check (before each commit that touches ntdll's files)

`WINE_BUILD=~/wine-build tests/host-syntax-check.sh <file>...` (a host configure of
refs/wine-11.4 in `WINE_BUILD`) compiles the merged sources with `WINE_IOS` as build-ios.sh does,
with `-Werror=implicit-function-declaration` like Apple clang, and with `tests/darwin-shim/`
standing in for the Darwin headers, so a call before its declaration (the dcf321b CI failure)
fails here too. All of `virtual.c loader.c process.c env.c ios_dylib.c ios_process.c ios_exit.c
server.c thread.c debug.c system.c file.c sync.c ios_spawn.c ios_unixlib.c ios_hwident.c
cdrom.c` pass. What it can't check: `__aarch64__`-only code on an x86-64 host, and the real SDK's
declarations (the shim gives names and types only); the iOS CI build checks those.

## Host tests (pure functions)

`tests/run-host-tests.sh` builds `ntdll-unix/ios_pure.c` with `tests/test_ios_pure.c` on any host
with a C11 compiler and pthreads, runs it and exits non-zero on a failure (it prints
`N checks, M failed`). It covers:

- boot failures (Parts A and C2, `tests/test_noexit.c`, built with `-DWINE_IOS` and
  `-include ios_noexit.h` like ntdll's files): a boot stage that calls `exit()`, `_exit()`,
  `abort()` or fails an `assert()` comes back by longjmp with its exit code and a log line, to
  the app's buffer when it is set for the thread, else to `__wine_main`'s own; only a thread that
  isn't the session's leaves by `pthread_exit`; a once-per-app set-up left by such a jump is
  reported as failed on the next run instead of blocking it. (On the device the same routing
  runs in `ios_exit.c`; the test's stand-ins call the same `ios_exit_route`.)
- the farm of an image (Part B, `ios_pe_farm_dir`): main machine 8664 → ntdll (looked up as
  ARM64) and x64 images from `/arm64ec-windows`; aa64 and i386 programs → `/aarch64-windows`
  for 64-bit images, `/i386-windows` for i386 ones; before the main image is known, by the
  image's machine. On the device: an x64 program's log shows `nojit: ntdll.dll (machine 8664)`
  and no `error 4000000e`;
- a child's name (Part C4, `ios_child_image_name`): a builtin started with no NT name (NULL
  buffer, garbage length, as wineboot.exe at the first boot) is named from its image path and
  its NULL buffer is never read; a resolved NT name wins; neither gives NULL and 0;
- the helper-name rule (Part C5): `crash`, `crs-handler`, `handler`, `report`, `helper` in the
  file name, any case; folders don't count;
- the refused-spawn match (Part C6) for each of the six names, any case and anywhere in the path,
  and the `conhost.exe` file-name test;
- the per-PEB registry (Part B2): publish, lookup, duplicate refusal, removal, reuse of a slot by
  the same key (a PEB at the same address starts clean), a full table, slot reuse, and eight
  threads publishing, looking up and removing at once (no lookup may return another key's value);
- the unix library table names (Part A), including `winemetal` → `dxmt_winemetal_…`, the wow64
  suffix, paths and extensions, and names that don't fit;
- the live-children count (Part C5): helpers, system processes, `max_age`, truncated name
  lists, a full table;
- the clock adjustment of Part F2 (`CNTFRQ_EL0` 24 MHz → 3072 MHz).

## On-device checks

Build with `build-ios.sh`, link with the other libraries, and run on a device. Every line this
spec's code writes starts with `ntdll-ios:` in the log named by `wine_log_set_file` (routine lines
are off with `MADEIRA_QUIET=1`; turn it off for these checks). The app's own lines come from
spec 02.

0. **Boot steps (always logged).** Every boot writes `ntdll-ios: boot: ...` lines, even with
   `MADEIRA_QUIET=1`, each with its own `write`: `__wine_main: argc ..., program ..., main|secondary
   thread, stack N bytes, WINESERVERSOCKET n`, `TEB key`, `app init: paths`, `paths: ...`,
   `app init: virtual memory`, `virtual memory: host page 16384, user space ...`,
   `virtual memory ready`, `app init: environment and NLS tables`, `app init done`, then per
   process `TEB and PEB`, `TEB ..., PEB ...`, `connecting to the server`, `connected`,
   `shared data, CPU info and drives done`, `main image mapped, machine ...`,
   `thread stack ready; mapping ntdll`, `ntdll mapped`, `ntdll loaded`,
   `signal handlers and exception port ready`, `running, entry ...`. When the app dies during a
   boot, the last line names the step that didn't finish; a `fatal:` or
   `server protocol error:` line before it gives the reason. Inside virtual memory init:
   `address space: task_info kr 0, min ..., max ...; user limit ...`, `view tables: N bytes at
   ...`, `exe window ...` or `no exe window (...)` (with `exe window check: kr, region,
   protection`), and for a 32-bit main program `guest window: highest mapped ..., searching
   ...` then `guest window: ...-... reserved` or `none reserved after N tries (first kr, last
   kr)`. In `build_initial_params`: `environment built ...; starting wineboot`,
   `wineboot: NtCreateUserProcess status 0`, `wineboot done`, `hardware registry written`,
   `load_main_exe status ..., module ..., machine ...`. A fault before the signal handlers exist
   logs `boot: FAULT signal N, address Y, after step: ...; pc img+... lr img+...; image ...
   slide ...; frames: <- img+... ...` (offsets from the main image's load address: look them
   up in the binary at __TEXT start + offset) and is then reported as a crash as before.
   First boot of a fresh prefix: `process 2 (wineboot.exe)` boots; its boot lines follow.

1. **A 64-bit program boots and exits with its exit code.** Start a 64-bit console program that
   returns 42 from `main`. Look for the app's phases `ntdll start`, `connected`,
   `main image mapped`, `ntdll loaded`, `entry point` (spec 02 logs `winios_phase`), then
   `ntdll-ios: process 1 (<name>) ended, exit code 42 (0x2a)`. The app logs that `__wine_main`
   exited with code 42 (`wine_ios_exit_code`) and `wine_launched_process_did_exit(42)` was
   called. The app keeps running.
2. **A 32-bit program boots.** Start a 32-bit (i386) program with `ios_main_image_i386 = 1`.
   Look for `process 1 (...): guest window at 0x...` (an address 4 GB aligned, above 4 GB), the
   same phases, and the program's window or output. (Needs spec 10's 32-bit memory to run past
   the loader; with only this spec, check that the window is reserved before
   `TEB and PEB allocated`.)
3. **A launcher that starts the real game and exits keeps the session.** Start a launcher
   program that runs `Game.exe` with `CreateProcess` and exits, with `MADEIRA_WAIT_CHILDREN=1`.
   Look for `process 2 (Game.exe): child started` and its boot stages, then
   `process 1 (<launcher>) ended`. The app's step 16 sees `madeira_live_game_children` > 0 and
   keeps waiting while the game runs; when the game exits (`process 2 (Game.exe) ended`) the
   count drops to 0 and the session ends. A crash reporter the game starts is not counted
   (its file name contains `crash`); `services.exe` and other Wine system processes are not
   counted once they mark themselves as system processes.
4. **Two programs one after the other.** Run program A to its end, then start program B in the
   same app run. B gets `process <n+1>` and boots through the same stages; no
   `registry full or PEB ... already registered`; the shared data page and signal handlers are
   not set up a second time (no new `failed to map the shared user data` error); A's exit doesn't
   affect B. If B's PEB lands at A's old address, B still starts clean (its handles, fd cache and
   load-order keys are its own).
5. **A program that spawns a refused helper continues.** From a test program, `CreateProcess`
   `SteamErrorReporter.exe` (any of the six names). The call fails with `ERROR_ACCESS_DENIED`;
   the log has `refused to start SteamErrorReporter.exe: an optional helper (steamerrorreporter)
   that can only fault on iOS` once (a second attempt logs nothing); the program goes on.
   `conhost.exe` from an ARM64EC (x64) program is refused the same way unless
   `MADEIRA_EC_CONHOST=1`.
6. **Force quit ends every program.** With a game and its launcher both running (or a program
   that hangs in `Sleep(INFINITE)`, one in `WaitForSingleObject` on an event nobody sets and one
   in `EnterCriticalSection` on a held section), use the app's force quit (spec 08
   `wineserver_kill_programs`). Each pseudo-process logs `ended, exit code <the force-quit code>`
   within about a second; none logs `didn't leave within 2000 ms` unless a thread spins in user
   code without system calls; `__wine_main` returns to the app; the live-children count is 0.
7. **Working folder.** Start a program with `MADEIRA_INITIAL_CWD=C:\Games\Foo\` that prints
   `GetCurrentDirectory`: it prints `C:\Games\Foo\`. The log has `working folder C:\Games\Foo\`.
   The app's own working directory (`getcwd` in the app) is the same before and after, also
   after the program has listed directories (`dir` in `cmd.exe`, `FindFirstFile` loops).
8. **A child that dies during boot.** Start a child whose main image is missing a DLL that
   ntdll itself needs, or remove the server socket (debug build): the log has
   `process <n> (<name>) died during boot (stage: ...)` or `can't boot: ... (stage: ...)` once,
   and the parent's `CreateProcess` fails.
9. **Drives.** In a fresh prefix, `GetLogicalDrives()` returns bits C and Z (`0x02000004`);
   `QueryDosDevice("C:")` returns `\Device\HarddiskVolume1`; `<prefix>/dosdevices/c:` and `z:`
   exist. A second program in the same session sees the same.
10. **Hardware identity.** In `regedit` (or a program reading the registry):
    `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0` has `ProcessorNameString`
    `AMD Ryzen 7 5800X 8-Core Processor`, `VendorIdentifier` `AuthenticAMD`, `~MHz` 3072 on a
    device whose counter runs at 24 MHz, one key per core; `...\System\BIOS` has the BIOS and
    board strings. `GetSystemFirmwareTable('RSMB', 0, ...)` returns a table whose type 0, 1, 2, 3
    and 4 entries carry the same strings (check with a small SMBIOS dumper or `wmic`-style
    queries through WMI). The log has `hardware identity: ...`.
11. **Unix libraries by name.** A DLL whose table isn't linked into the app (rename one in a
    test build) fails to load its unix side with `STATUS_DLL_NOT_FOUND` and the log has
    `unixlib: <dll> has no table <name>_unix_call_funcs linked into the app` once. DXMT's
    `winemetal.dll` binds `dxmt_winemetal_unix_call_funcs` (64-bit program) and
    `dxmt_winemetal_unix_call_wow64_funcs` (32-bit program). A library that defines
    `<name>_unix_lib_init` (win32u) has it called when a program binds it: win32u's system calls
    work in the first program and in a child.
12. **Threads.** A program's threads report `QOS_CLASS_USER_INTERACTIVE`
    (`qos_class_self()` from a debugger, or Instruments' thread list); a game's frame pacing with
    `Sleep(1)` loops shows ~1 ms sleeps, not ~10–30 ms.
13. **Contexts (ARM64EC).** An x64 program calls `SuspendThread`/`GetThreadContext` on a thread
    blocked in `WaitForSingleObject`: `Rip` points into the program's code after the wait call
    and `Rsp` inside that thread's stack (spec 08 Part E reads the CPU area this spec keeps up to
    date).
14. **No exits.** A unix-side `exit()` (for example `WINEDEBUG=help`, which prints the usage and
    calls `exit(1)`) logs `exit(1) in process N (...): ending its pseudo-process` and ends only
    that program; the app stays.
15. **Exported tables (build check, clarification 6).** After linking the app (Release, stripped
    as shipped), every name in `<out>/exports.txt` from `build-ios.sh` is a global symbol of
    the app binary; this prints nothing:
    `nm -gU <App>.app/<App> | awk '{print $NF}' | sort -u > have.txt; sort -u exports.txt | comm -23 - have.txt`.
    A name it prints is a table the loader can't find: the program using that DLL then logs
    `unixlib: <dll> has no table ... STATUS_DLL_NOT_FOUND`. The host tests check that
    `exports.txt` (written by `write-exports.sh` from `unixlibs.txt`) holds exactly the names
    the loader's rule produces.

# Spec 10: tests

## Host tests (pure functions)

`tests/run-host-tests.sh` also builds `ntdll-unix/ios_vm_pure.c` with
`tests/test_ios_vm_pure.c` (fake bases). It covers:

- the highest user address (A2): rounding down to 64 KB, the Windows cap, an unknown maximum;
- the large-address-aware rules (E5): the ceiling with and without the flag and `MADEIRA_LAA`,
  the low-2-GB preference and `MADEIRA_LAA_LOWFIRST`;
- limit translation (A1, E4): guest limits into the window, the 64 KB floor, the ceiling cap,
  impossible ranges, host limits untouched, a 64-bit process asking for memory below 4 GB;
- the window slot state machine (E2, E7, E8): reserve, bind, release (stops resolving), teardown
  only with no threads left, alert-then-teardown, abandon when a thread stays (never reused),
  abandon by a process that stops being 32-bit, a FREE pre-reservation taken first, no address
  space, every slot in use;
- the exe window (D): parsing, fit, claim with host-page rounding (16 KB), confirm, one occupant
  at a time, retire, an unaligned base, a rounded size that no longer fits, no window;
- the dylib folder per machine and the path (C1);
- only our builtins take their dylib (clarification 9, `ios_image_takes_dylib`): a
  builtin-marked image with a dylib takes the dylib; an unmarked same-named file outside the
  system directories is mapped from its file; a marked file in the game folder (a copied
  builtin) takes the dylib; an unmarked file in system32 (a user replaced a builtin) is mapped
  from its file; the unix loader's own builtins take the dylib (these cases with plain x64
  images). Clarification 9a: an unmarked ARM64EC image (x64 machine with ARM64EC metadata) in
  system32, an unmarked ARM64 image and an ARM64X image take the dylib; an unmarked plain-x64
  same-named file in the game folder and an unmarked i386 file are mapped from their files. On
  the device: no `a native copy` line for `xtajit.dll` or `xtajit64.dll`; and: a 64-bit game
  with Microsoft's D3DCompiler_47.dll next to its exe logs `[nojit] D3DCOMPILER_47.dll: a native
  copy (...), mapped from its file` once and loads it (no `doesn't fit its dylib`, no c000007b);
- no second instance in the same process (clarification 10, `test_loaded_again`): the base
  name, upper case, the NT and DOS system32 paths, the `sysx64` farm path and forward slashes
  all give the same dylib path; another DLL name or the other machine's directory doesn't;
  `ios_dylib_view_reusable` hands back the calling process's own view at the image start, not
  another process's view, a view starting elsewhere, a view that isn't a dylib image, or one
  without an owner. On the device: the Godot/.NET 9 game starts with no `[pe-instance] … new
  instance` line for a DLL its process already has; at most one `[nojit] <dll>: second mapping
  in process <n> (…)` line per DLL, naming what the loader asked for; `GetOSVersion` no longer
  faults. Only the device checks the refcounted unmap (`FreeLibrary` of the second load leaves
  the module mapped);
- a DLL's machine against its process (clarification 8, `ios_image_fits_process`): 0x8664 is
  refused for an ARM64 process and accepted for an x64 one; 0xAA64 is accepted for an ARM64
  process, and for an x64 one only when hybrid; WoW64 processes, an unknown main image and
  non-64-bit images are left to upstream. On the device: an ARM64 program started after an x64
  one in the same session logs `[nojit] kernel32.dll: machine 8664 doesn't fit this arm64
  process; refused` once and runs, with its DLLs from `sysaa64`;
- the window holds only guest requests (E4, `ios_win_request_in_window`): a translated guest
  request may use the window, a request with no guest limit may not; with a guest window the
  shared data page is placed outside `[B, B + 4 GB)`, a guest request inside; and with real
  memory, the page placed by that rule is still readable after the window's low 64 KB are made
  `PROT_NONE`. On the device: a 32-bit program's boot line `shared data page at ...` shows an
  address outside its guest window, no `exception c0000005 in a system call ... address B + 0x320`
  appears, and the GameMaker game draws;
- builtins named by guest address (E4, `ios_module_match`): a builtin registered for a WoW64
  image is found by its guest address and by its host address, in the calling process's own
  window when two 32-bit processes have a DLL at the same guest base; a 64-bit process matches
  host addresses only; NULL and other addresses match nothing. On the device: the 32-bit
  `winemetal.dll` initialises (no `failed to initialize`, no `is not a registered builtin` line);
- shared image sections (clarification 7): the shareable rule (whole host pages only; a 4 KB
  section on 16 KB pages, or one sharing its host page, is not; 4 KB host pages behave as
  upstream), and a mock i386 image with 4 KB alignment and a writable shared `.mydata` between
  `.text` and `.data`, loaded by two processes: it loads, nothing is shared, `.mydata` holds the
  file's data and zeros past its raw size, a write lands only in that process's copy, and the
  neighbours keep their contents and protections. On the device: the GameMaker game of the
  fourth run loads and logs `[nojit] <game>.exe: shared section .mydata mapped privately (16 KB
  pages)` once;
- the executable-request rules (B);
- the low windows (clarification 1): the shared page for everybody, two programs at
  `0x400000`, lookups with and without a PEB, removal by owner, a full table, and four readers
  while a writer keeps reusing one slot for two different windows (no torn read).

## On-device checks

Turn off `MADEIRA_QUIET`. Lines from this spec start with `ntdll-ios:`, dylib lines with
`nojit:` (also in `$MYIOSDECK_NOJIT_TRACE`).

1. **A 64-bit game linked at `0x140000000` starts twice in one session.** Start it; the log has
   `process <n>: exe window 0x140000000-... claimed` and `GetModuleHandle(NULL)` is
   `0x140000000`. Quit it; the log has `exe window 0x140000000 given back` before
   `process <n> ... ended`. Start it again in the same app run: the same claim line, the same
   base. While the first copy still runs, a second copy (if it allows two instances) is relocated
   or, with stripped relocations, fails to start with `STATUS_CONFLICTING_ADDRESSES`; the first
   copy is unaffected.
2. **A 32-bit game, its end, then another 32-bit game.** With `ios_main_image_i386 = 1`, start a
   32-bit game: `guest window <B>: reserved for the 32-bit program`, `process 1: guest window <B>`
   (4 GB aligned). It runs; `NtQueryInformationProcess(1010)` from a test DLL returns `B`.
   Quit it, start another 32-bit game in the same session: `guest window <B>: torn down`, then the
   same `B` (or, if a thread was left, `abandoned: ...` and a new `B`). The second game runs; its
   `KUSER_SHARED_DATA` reads (`GetTickCount`) advance.
3. **A 32-bit launcher that starts a 32-bit game.** The launcher's window and the game's window
   differ (two `guest window` lines); both run at once; when the launcher exits, its window is
   only released (no teardown line until another 32-bit process needs a slot).
4. **Signed images.** With `MYIOSDECK_PE_DIR`/`_A64` set, every DLL line `nojit: <dll> (machine
   8664|aa64) at ...` appears once per process that loads it; a second process gets a different
   address for the same DLL (a fresh instance). With `MYIOSDECK_PE_DIR` unset, DLLs map the
   upstream way (no `nojit:` lines) and the first `no executable memory on iOS` lines explain the
   crash.
5. **Address space.** A 64-bit program's `GetSystemInfo().lpMaximumApplicationAddress` is at most
   the device's `max_address`; `VirtualAlloc2` with a `MEM_ADDRESS_REQUIREMENTS` high limit
   below 4 GB fails in a 64-bit program and returns an address below that limit in a 32-bit one.
   A 32-bit program without `LARGE_ADDRESS_AWARE` gets addresses below 2 GB until that half is
   full.
6. **Low windows.** A 64-bit program reading `*(ULONG *)0x7ffe0320` (tick count) gets a moving
   value (needs spec 11's fault servicing). A packed 64-bit program based at `0x400000` runs
   (`image based at 400000 without relocations runs at ...` if its relocations are stripped).

# Spec 11: tests

## Host tests (pure functions)

`tests/run-host-tests.sh` also builds `ntdll-unix/ios_sig_pure.c` with each of five programs
(`-Wall -Wextra -Werror`, shared macros in `tests/test_sig_check.h`). Each prints
`N checks, M failed`; the script exits non-zero on any failure. They pass in WSL with gcc 15 and
clang 21, also under ASan/UBSan, and the registry test under TSan.

- `test_sig_ctx.c` (Part A5): `ios_ctx_from_darwin`/`ios_ctx_to_darwin` field by field (x0-x28,
  fp, lr, sp, pc, cpsr, v0-v31, fpcr and fpsr not swapped); X18 reads as the TEB whatever Darwin
  had, x18 is never written back; without a NEON state the floating-point part is left alone;
  the debug registers and `ContextFlags` are never touched; both round trips give back the
  input. The mirrors' sizes and offsets against Apple's `ARM_THREAD_STATE64_COUNT` (68) and
  `ARM_NEON_STATE64_COUNT` (132) and Microsoft's `ARM64_NT_CONTEXT` (0x390 bytes, Pc at 0x108,
  V at 0x110, ...).
- `test_sig_exc.c` (Part B2): `ios_sig_exception` per signal: SIGSEGV/SIGBUS read, write (data
  abort with WnR) and execute (address = pc, or instruction abort) with the address in `[1]`;
  `mrs xN, CurrentEL` emulated (register, pc + 4), other SIGILLs illegal instruction; `brk`
  `#0xf000`, `#0xf001`, `#0xf003` (fast fail, code from x0, non-continuable), `#0xf004` and
  other immediates, an unreadable or non-brk instruction, single steps (also in front of a
  `brk`); every `FPE_*` code. No case moves the pc except the emulated `mrs`.
- `test_sig_storm.c` (Part B4): the counter restarts on a different pc, address or signal and
  after a reset; alternating faults never build up; the 1000th identical fault in a row is the
  first that reaches `IOS_STORM_LIMIT` (the comparison `check_storm` makes), the 999th isn't.
- `test_sig_threadreg.c` (Part D): register, exact lookups by port and by id (no neighbour, no
  default, id 0 and ports 0/0xffffffff never found), removal (count unchanged), reuse of an
  empty slot, re-registering a port in place, a full table of 1024 (update still allowed, a
  freed middle slot taken), `ios_thread_registry_range_busy` bounds, and four writers
  registering and removing while two readers check that no lookup returns another thread's TEB.
- `test_sig_a64.c` (clarification 1): `ios_a64_emulate` on a fake window that records each
  request. For every supported form (unsigned-immediate, unscaled, `LDTR`/`STTR`, pre/post-index,
  register offset with UXTW/LSL/SXTW/SXTX, sign- and zero-extending sizes, xzr and sp as
  operands, SIMD B/S/D/Q, `LDP`/`STP`/`LDNP`/`LDPSW`/SIMD pairs, `LDAR`/`LDLAR`/`STLR`/`LDAPR`/
  `STLUR`/`LDAPURSH`, `LDADD`/`STADD`/`SWPAL`/`LDSMAXH`/`CAS`/`CASALB`) it checks the address,
  size and direction asked for and the whole register file afterwards (destinations, base
  write-back, pc + 4, nothing else). Refused with no memory request and no register change:
  x18 as base, index, source or destination, write-back bases that are also transferred, a pair
  into one register twice, exclusives, literals, `PRFM`, unallocated size/opc and extend
  combinations, SIMD structure loads, non-memory instructions. `NO_ACCESS` for addresses
  outside the window, spans leaving it, refused protection, misaligned atomics and wrapping
  addresses, again with no register change. `ios_a64_base_reg` and `ios_a64_is_simd`. The test
  encodings were checked once against clang 21's assembler (`--target=aarch64-linux-gnu
  -march=armv8.4-a+lse+rcpc`).

Not covered here yet: `ios_lowwin_find`/`ios_lowwin_note_service` in `ios_vm_pure.c`, the
`ios_lowwin_service` glue in `ios_lowwin_fault.c` (Mach calls), `ios_sfmt` and `ios_once_pair`.

## On-device checks

Build with `build-ios.sh`, link with the app, run on a device with routine log lines on
(`MADEIRA_QUIET` unset). Lines from this spec start with `ntdll-ios:`. Write each test program
twice where it says so: ARM64 (MSVC or clang for `aarch64-windows`) and x64 (runs in the app's
interpreter, ARM64EC process).

1. **NULL dereference in `__try`.** `__try { *(volatile int *)0 = 1; } __except (
   GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER :
   EXCEPTION_CONTINUE_SEARCH) { print ExceptionInformation[0..1] }`. Expect write (1), address 0;
   a read version reports 0; calling a NULL function pointer reports 8. The program keeps
   running. Both builds.
2. **Divide by zero.** An ARM64 MSVC build of `int f(int a, int b) { return a / b; }` with
   `b = 0` inside `__try`: `EXCEPTION_INT_DIVIDE_BY_ZERO` (the compiler's `brk #0xf004`), the
   context's pc on the `brk`. The x64 build: the same code from the interpreter.
3. **`__debugbreak`.** Inside `__try`, `__except (EXCEPTION_EXECUTE_HANDLER)`: code
   `EXCEPTION_BREAKPOINT`, `ExceptionAddress` = the `brk #0xf000`. A vectored handler that adds 4
   to `ContextRecord->Pc` and returns `EXCEPTION_CONTINUE_EXECUTION` lets the program go on after
   the break (this returns through `NtContinue` and the SIGTRAP full-context path: check that
   x19-x28 and v8-v15 kept their values across it).
4. **Interpreted x64 access violation.** The x64 build of check 1 reaches its `__except`. With
   the app's interpreter logging `mid_fxi_mach_fault`, the fault is seen there first. If the
   interpreter declines it, the signal path delivers it.
5. **App crash stays an app crash.** Make the app crash on its UI thread (a debug menu entry that
   dereferences NULL) while a Windows program runs: the app's own crash report appears, as without
   Wine, and no `ntdll-ios:` exception line is logged for it.
6. **Unix-side fault returns a status** (clarification 3). A system call given a bad caller
   pointer that the unix side reads outside `__TRY` (or a test unix library call that
   dereferences one) returns `STATUS_ACCESS_VIOLATION` to the program, which keeps running; the
   log has `exception c0000005 in a system call at pc …, address …: the call returns it as its
   status (fault N of this run)`, the app image base and up to 8 return addresses. Repeat it 600
   times: lines for faults 1-16, 256 and 512 only. Inside `__TRY` the call returns its
   `__EXCEPT` status with no log line.
7. **Signal storm.** A vectored handler that returns `EXCEPTION_CONTINUE_EXECUTION` for an access
   violation without fixing anything: after 1000 faults, one `identical faults in a row` line and
   the program ends with `0xc0000005`.
8. **Fast fail.** `__fastfail(7)`: `__fastfail(7) at pc …: ending the process`, exit code
   `0xc0000409`, no `__except` runs.
9. **SIGABRT.** A unix library call that ends in a system library's `abort()` on a Windows
   thread: `SIGABRT at pc …: ending the process`, exit code 3, the app keeps running.
10. **Low windows.** A 64-bit program reading `*(volatile ULONG *)0x7ffe0320` in a loop sees a
    moving tick count (fast path: no further faults after the first per base register). A
    program based at `0x400000` with stripped relocations runs (emulated loads and stores, one
    `window …: 4096 accesses serviced` line per 4096). An access the emulator doesn't handle
    (an exclusive load, `ldxr`, at a low address) logs `declined … at pc …` once and is
    delivered as an access violation.
11. **Threads and the registry.** A program that starts and ends 2000 threads (no more than 100
    at once) runs to the end without `thread registry full`; the app's view of the registry
    (`ios_thread_registry_count/teb/mach`) shows only the live threads. A 32-bit game's guest
    window is torn down after it quits (spec 10 E7 uses `ios_thread_registry_range_busy`).
12. **Context of another thread.** `GetThreadContext` on a suspended thread of the same program
    returns its registers (spec 08 Part E); on the own thread, the system call's frame, with X18
    = the TEB.
13. **Debugger.** Under lldb, programs that continue from exceptions stop on SIGTRAP (check 3);
    `process handle SIGTRAP -s false -p true` lets them run.

# Spec 17: tests

## Host tests

`tests/run-host-tests.sh` also builds, against Wine's headers (this tree's `include/` first,
then `refs/wine-11.4/include`, or `WINE_SRC`):

- `test_s17_headers.c` three times (Part E): `ios_wow_host_base`, `ios_wow_host_ptr` and
  `ios_wow_guest_ptr32` with base 0 and base `B` (NULL both ways, 32-bit truncation), with
  `ios_wow_base` not linked (the weak symbol), and without `WINE_IOS`; the value 1010 of
  `ProcessWineIosWowGuestBase` and the `RtlWow64SuspendThread` prototype.
- `test_s17.c` with `ios_s17_pure.c` and `ios_s17.c` (a fake `ios_log`):
  - Part B: `is_wow64`, `in_wow64_call` and the section ceiling for a 64-bit thread, a 32-bit
    thread and a 64-bit thread while a 32-bit process exists, each with its switch on and off;
    the switches read once from the environment; `is_ec_code` with a NULL bitmap and a set bit;
  - Part C1: the serials of `0x12345678` and `0xdeadbeef` with letter `c` (`0x1d5b1f12`,
    `0x2d805578`) and 0 (`0xbc5a865f`, `0x50808c91`); the 0 / `0xffffffff` substitution (a value
    and letter found by inverting FNV-1a's last step); parsing (newline, none, upper case, 0,
    garbage, more than 32 bits, the 15-byte limit); the file (read, created with `%08x\n` and
    mode 0600, read back, 0 and garbage replaced, no directory); the `O_EXCL` race (a creator
    that loses takes the winner's value; 200 rounds of eight threads at once, from a missing
    file and from a file holding 0, each round ending with one value and no temporary file);
    `test_s17_winternl_alone.c`: the `winternl.h` override compiles alone without
    `WINE_UNIX_LIB` (the iOS `va_list` failure itself needs the iPhoneOS SDK's headers); the
    `FileFsVolumeInformation` answer (fields, `Information` = 18; a zero serial is
    `STATUS_NOT_IMPLEMENTED`); the cached serial;
  - Part C2: the write/create classification, the two budgets (64 and 192, independent), the
    printable names, the share mode only on write/create lines; the addendum
    (`test_failed_lookups`): `C:\windows` paths (with `\??\`, `\\?\`, any case, `/`; not
    `C:\windowsx\`, `D:\windows\`, or a cut-short name), the budget kinds, the third budget of
    256 independent of the others, `NtOpenFile` and attribute-query lines by name, and, with the
    shared budget used up, `C:\windows\` misses quiet while a game folder's failures still show;
  - Part D3: the settings reader (BOM, comments, no `=`, `=` in values, last wins), the count
    (variable over key, 0, negative, garbage, 64, 63, 1), decided and logged once;
  - Part D4 with clarification 2 (fake pseudo-processes and a model of the PE side's queues):
    program 1 leaves a waiter linked in a queue and its entry's `next` becomes garbage; program 2
    then gets a different, all-zero table, and a wake over 512 addresses covering all 256 queue
    indices finds nothing (walking program 1's queue would fault); two images of one process get
    the same table, also eight asking at once; a thread with no pseudo-process gets
    `STATUS_UNSUCCESSFUL`; the argument checks (no length pointer, NULL buffer, wrong sizes)
    answer as before; freeing is idempotent;
  - Part D5: `ntdll_syscall_name`'s bounds (no names, at the limit, limit 0);
  - Part D6: the five calls' checks and answers, the WoW64 answer, the enum values 7-12 and the
    struct sizes 24, 88, 16.

`tests/host-syntax-check.sh <file.c>...` (not run by `run-host-tests.sh`) compiles named files of
the merged tree with `-fsyntax-only`, `WINE_IOS` and a host Wine build's generated headers
(`WINE_BUILD`, default `~/wine-build`). `syscall.c`, `debug.c`, `system.c`, `loader.c` and
`ios_s17.c` pass on Linux; `file.c` stops only at spec 09's Darwin-only `F_GETPATH`.

## On-device checks (spec 17)

Lines start with `ntdll-ios:`.

1. **Volume serial.** A 32-bit installer that calls `GetVolumeInformation("C:\\")` gets a
   non-zero serial; `$WINEPREFIX/.madeira-volume-serial` exists (mode 0600, 8 hex digits); after
   the app restarts the same serial comes back. A 64-bit program gets upstream's answer (the
   call fails: `STATUS_NOT_IMPLEMENTED` without a mount manager).
2. **Failed opens.** A program that fails to write its log file: `NtCreateFile failed: status …
   disposition … access … options … share … unix … nt …` appears even after the loader's misses
   (lines without `share`), which stop after 64. A game whose data file is missing:
   `NtOpenFile failed: status 0xc0000034 disposition 1 … nt \??\C:\<game>\…` (or
   `NtQueryAttributesFile failed: …`) appears even after many misses under `C:\windows\`.
3. **CPU count.** `cpu-count = 4` in `madeira.cfg`: `processor count: reporting 4 (host has N)`
   and a program's `GetSystemInfo` shows 4; `MADEIRA_CPU_COUNT=2` for one launch wins
   (`reporting 2`).
4. **No-TEB logging.** A crash on a thread Wine didn't create is logged with `no-teb:` in place of
   the ids, once, and the app keeps running.
5. **32-bit debug output.** `OutputDebugStringA` from a 32-bit program prints its text.
6. **Futex table.** In an ARM64EC process (two ntdll images, spec 18), a `WaitOnAddress` waiter
   registered through one image is woken through the other (a game mixing x64 and ARM64EC code
   doesn't hang). Clarification 2: run a program that ends with a worker parked in
   `WaitOnAddress` (its log: `thread … was blocked in a system call: suspended for good`), then a
   second top-level program in the same session (spec 22): the second runs past its first
   seconds without a fault in `RtlWakeAddressAll`.
7. **I/O status layout.** A 64-bit helper started after a 32-bit program reads files and sockets
   normally (64-bit I/O status blocks), and the 32-bit program's overlapped reads still complete.

## Spec 22 Part C (later programs in a kept session)

Host: `test_session_pure` (in `run-host-tests.sh`): 64 then 200 programs one after another through
the PEB registry, the children table and the thread registry (never full; one thread-registry
slot reused; with ten stragglers still alive 200 more fit; a full registry takes a new process once
one ends); the process-end hook list (NULL refused, each once, order, the PEB passed, 16 at most);
the own exiting flag (a fake lock skipped for an exiting process is still taken for a second
process after the first one's exit, the process found by its PEB as `ios_proc_current` does); the
previous-TEB slot per process (a new process's first leaving thread never gets an ended process's
TEB); the C3 rule (killed, main, others, already ending) and the C5 rule (gone / blocked outside
Wine → suspend / blocked inside Wine → kept / running → kept) with its counts.

On the device (with `MYIOSDECK_SESSION_REUSE=1`, see `out/22-session-reuse/TESTS.md` in the app
repository for the whole list):
1. A second program after the first: `ntdll-ios: boot: later program: argc …, server fd N,
   64-bit`, `process M (<name>): a new top-level process in the running session`, the usual boot
   steps, the app's `[phase]` lines again, and at its end `the program ended (exit code …): back
   to the app`.
2. A 32-bit program after a 64-bit one: `guest window at …` before `TEB and PEB`.
3. Force quit of a program whose main thread sleeps in a wait: `its main thread was ended by the
   server: the process ends`, then (if threads sleep where no reply reaches them) `ended with
   threads left: W woken, L left, S suspended, K kept`, and the app gets the program's end.
4. Six starts of one game: `image table full` never appears; the hooks line (`process-end hook(s)
   ran`) once per program when the audio driver registered its hook.
