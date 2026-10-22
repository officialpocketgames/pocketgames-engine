# Spec 12 tests

## Build

    ./build-ios.sh <wine-11.4 tree> <wine build dir> <out dir>

Prints `patch <name> OK` for every patch (a hunk that doesn't apply stops the build and prints
`patch`'s message), then `<file> OK` for every FreeType and win32u file, then
`built <out>/libwin32u_unix.a`. A failed file prints the first 60 lines of its compiler output.
CI links the library with ntdll's unix side (spec 09), the wineserver (spec 08) and the app; that
link is the check for name clashes (Part A2) and for the symbols spec 09's loader looks up
(`win32u_unix_call_funcs`, `win32u_unix_call_wow64_funcs`, `win32u_unix_lib_init`; they must be
exported from the app binary, see QUESTIONS.md Spec 09 question 12).

## Host tests (Part G)

    ./tests/run-host-tests.sh          # CC=clang ./tests/run-host-tests.sh

Compiles `tests/test_w32ios.c` with `w32ios_pure.c` and `w32ios_kmt_pure.c` (plain C11, no Wine
headers) and prints `<n> checks, 0 failures`; exit status 1 on any failure, with the failing
`CHECK` and its values. Covered:

| Area | Test |
|---|---|
| C4 mode list: order, cap, current first, no duplicates, `MADEIRA_EXTENDED_MODES` | `test_modes` |
| C5 mode-change decision: restore, accept, BADMODE, the four-times rule, one missing field | `test_mode_decision` |
| C3 device-name matching (adapter, monitor, case, NULL, others) | `test_device_names` |
| C6 driver version strings and their QWORD | `test_driver_version` |
| C2 session default from the environment | `test_session_default` |
| Clarification 10: a new program's size (first, same, change, stays), two programs 1280x720 then 960x720 and the modes list the second sees, the refusal; which processes count as other user programs | `test_launch_size`, `test_user_program` |
| E3 cursor conversions: colour with and without alpha, monochrome | `test_cursor` |
| E1 extended-key rule, event log rule | `test_extended_key`, `test_log_event` |
| E2 slicing deadline arithmetic, pure-poll request rule | `test_slicing` |
| E4/E5 whole-screen and dialog rectangles | `test_rects` |
| B per-PEB table: lookup, publish, reuse, full table, recycling | `test_pp_table` |
| B2 client procedures: per-process table, global fallback, matching, handle index | `test_client_procs` |
| B3 built-in class registration decisions | `test_builtin_action` |
| B1 allocation ceiling | `test_ceiling` |
| B7 window procedure handles from 32-bit code | `test_winproc_handle` |
| F XInput packing: State, Caps, SetVibration, bad index, no buffer, disconnected, sizes | `test_xinput` |
| D4 QueryAdapterInfo answers, short buffers, non-zero indexes | `test_kmt` |
| D1/D2 LUID from registryID, `vram-mb` rule | `test_memory_and_luid` |
| settings file reader (spec 02 Part G format), boolean switches | `test_settings` |

The script compiles with `-Wall -Wextra -Werror` and then runs `check-app-store.sh` on this
part (spec 00 Part F). Run in WSL (Ubuntu) from Git Bash:

    wsl.exe -d Ubuntu -- bash -lc "cd /path/to/pocketgames-engine/out/12-win32u-ios && ./tests/run-host-tests.sh"

Last run (2026-10-22): `1025 checks, 0 failures`, `check-app-store: clean`, with gcc 15 and with
`CC=clang` (clang 21). Everything that needs Wine, Mach or Metal (the `w32ios_*.c` files that
include Wine's headers, `w32ios_mach.c`, `w32ios_metal.c`) is only built by `build-ios.sh` and
checked on the device below.

`tests/syntax-check-patched.sh <host wine build dir> [file.c ...]` is a closer host check of
those files: it applies `patches/*.patch` to a scratch copy of `refs/wine-11.4/dlls/win32u` and
runs `-fsyntax-only -DWINE_IOS` with `build-ios.sh`'s defines, include order and forced includes
on the patched `sysparams.c` and the Wine-dependent `w32ios_*.c` files. The build dir supplies
`config.h` and the headers generated from IDL (a host build of Wine 11.4). It allows patch fuzz
5: many hunks have less context after the change than before, which GNU patch only accepts at a
file's end without fuzz (macOS's patch, used by CI, accepts them). Last run (2026-10-22, gcc 15):
all nine files OK.

## On the device

Every iOS log line of this part starts with `[win32u] ` (in ntdll's log, spec 09). Lines marked
"verbose" are left out with `MADEIRA_QUIET=1`.

1. **A game lists modes and switches to 1280 x 720, and the picture follows.** Start a game
   that offers a resolution menu, with `MADEIRA_SCREEN_W=1024`, `MADEIRA_SCREEN_H=768`.
   - At start: `[win32u] virtual monitor 1024x768, 32 bpp, 60 Hz (MADEIRA_SCREEN_SRC=...)`, then
     `[win32u] reporting the virtual monitor to the server` once the game asks for the screen
     size, and `[win32u] registry GPU 106B:0001 "Apple GPU" {...}, LUID ...:..., 4096 MB, driver
     35.0.10.1000` (NVIDIA `10DE:2544` ... `32.0.15.8157` with `DXMT_ENABLE_NVEXT=1`).
   - The menu lists 1024x768 first, then the standard modes up to 1024x768's pixel count
     (1152x648 and 1176x664 included, 1280x720 not); with `MADEIRA_EXTENDED_MODES=1` up to
     2048x1536.
   - Choosing 1280x720: `[win32u] ChangeDisplaySettings 1280x720 (fields ..., flags ...): accept`;
     the app's `winios_display_mode_changed(1280, 720)` runs; the picture is scaled from
     1280x720; `GetSystemMetrics(SM_CXSCREEN)` is 1280 afterwards; the cursor reaches the whole
     new screen (spec 08 resets the clip on `set_winstation_monitors`).
   - A size such as 1000x700 is refused (`: bad mode`, `DISP_CHANGE_BADMODE`).
   - With `MADEIRA_VIRTUAL_MODE_SET=0` the change is accepted and the size stays 1024x768.
   - A second game started afterwards in the same app run gets its own launch's size
     (clarification 10, below); verbose: `process XXXX (PEB ...) uses slot N, new session`, for
     the booted program only.
   - A game started by its launcher keeps the size the launcher set: its slot line has no
     `, new session`, and neither do wineboot's or the system processes'.
2. **A game's launcher window and a message box over a game are drawn and take taps.**
   - The launcher's window appears in the app (`winios_window_frame` then
     `winios_surface_present` for its HWND); buttons react to taps.
   - A message box over a running game: `[win32u] thread XXXX: window ... (WxH) waits in slices`
     (at most 8 such lines); the box is drawn and takes taps although its thread waits in
     `GetMessage`. With `MADEIRA_GAME_INPUT_WAKE=0` the line doesn't appear and the box only
     reacts when another message wakes its thread.
   - The game's own fullscreen window gets no GDI surface (no `winios_surface_present` for it).
   - With `MADEIRA_GAME_WINDOWS=0` no window image reaches the app, frames still do.
3. **A key press reaches a game with the right scan code.** With an input monitor in the game
   (or a key-logging test program), press A and Left Arrow on a hardware keyboard.
   Verbose: `[win32u] key N: vk 0x41 scan 0x1e flags 0x0` and
   `[win32u] key N: vk 0x25 scan 0xe04b flags 0x1` (extended). Page Up gives `flags 0x1` and with
   `MADEIRA_NAV_KEYS_E0=0` `flags 0x0` (unless the layout's scan code has the prefix).
   `WM_KEYDOWN`'s lParam bits 16-23 hold the scan code, bit 24 the extended flag.
4. **An XInput game sees the controller and rumbles.** Connect a controller, start an XInput
   game. `XInputGetState(0)` succeeds and its packet number moves with the sticks; the game's
   rumble reaches `winios_gamepad_set_vibration` (the controller vibrates when
   `winios_gamepad_rumble_caps()` is 1). Unplugged: `ERROR_DEVICE_NOT_CONNECTED`. If the game's
   window isn't the foreground window, verbose: `process XXXX reads a controller: window ...
   made foreground (was ...)` (once a second at most); not with `MADEIRA_FOREGROUND_FIX=0`.
5. **Two GUI programs one after the other each get working built-in controls.** Run a program
   with buttons, edit boxes and list boxes (a dialog), quit it, run another. Both show working
   controls. Verbose: `process XXXX: user32 at ...`, `process XXXX: built-in classes
   registered` once per program. No `class list ... longer than 8192 entries` line.
6. **A launcher that starts the game without a window of its own.** Use a console launcher (or
   one that never opens a window) that starts a GUI game. The game's window appears.
   `[win32u] process XXXX: no desktop window for thread YYYY, registering the desktop classes`,
   possibly `... has no desktop, connecting it`, then `[win32u] process XXXX: desktop window
   found`. Not with `MADEIRA_CHILD_DESKTOP=0` (the game then logs `no desktop window for thread
   ... yet` and shows nothing).
7. **A 32-bit GUI program.** Run a 32-bit program with a window and text. Verbose:
   `GDI handle table for 32-bit process XXXX at 0x...` (an address inside its guest window). The
   window draws text and controls; no fault in gdi32. Built-in controls work (B7: procedure
   handles converted by WoW64 thunks are recognised; `MADEIRA_WINPROC_HANDLE=0` turns that off).
8. **The D3DKMT adapter (opt-in).** With `MADEIRA_KMT_ADAPTER=1`:
   `[win32u] D3DKMT adapter: Metal registryID ..., LUID ...:..., N MB` and the registry GPU line
   shows the same LUID; a tool that calls `D3DKMTEnumAdapters2` sees one adapter whose LUID equals
   the one DXGI reports, `D3DKMTQueryAdapterInfo(KMTQAITYPE_DRIVERVERSION)` gives 3100 and
   `KMTQAITYPE_GETSEGMENTSIZE` the dedicated size (`vram-mb` of the settings file when 256 or
   more, else 4096 MB). Without the variable `D3DKMTEnumAdapters2` lists nothing.

## Spec 17 (Parts F and G)

Host tests: `tests/test_w32ios_s17.c` (run by `tests/run-host-tests.sh`, `-Werror`): the 32-bit
session decision (switch set, `0`, empty, unset with the session armed, not armed, or the
function missing); the publish rules (the per-thread marker, again after the PEB changes,
master for 64-bit, a view for 32-bit only in a 32-bit session, the 8 remembered views by
(process id, PEB), a failed view remembered, the oldest overwritten); the owner rule of F5/F6
(64-bit with 64-bit shares; any 32-bit side only the same process id and PEB; a later process
at a dead one's PEB doesn't match); the `GA_ROOT` walk over a fake tree (a button in a group box
in a dialog, a top-level window, an unreadable parent, a cycle, a non-child parent).

The patches were checked in WSL: spec 12's patches applied with GNU patch 2.8 to a copy of
`refs/wine-11.4/dlls/win32u`, then `S17-*.patch` with `-F0` (no fuzz), giving the edited tree
exactly; `gdiobj.c`, `dc.c`, `dce.c`, `dib.c`, `window.c`, `message.c`, `input.c`,
`dibdrv/bitblt.c`, `syscall.c`, `w32ios_mem.c` and `w32ios_s17_pure.c` compile with
`-fsyntax-only -DWINE_IOS` against a host Wine build's headers.

On the device:

1. **A 32-bit and a 64-bit GUI program at once.** Both draw text and controls (DCs, stock
   objects) and neither crashes on a GDI handle; `[win32u] GDI table shared with 32-bit
   programs: yes (32-bit session)` (verbose) and `[win32u] GDI handle table for 32-bit process
   ... at 0x...` (verbose, an address inside the 32-bit program's window). Then the 32-bit one
   exits and the 64-bit one goes on drawing. With `MADEIRA_GDI_SHARED_SECTION=0` the line says
   `no` and 32-bit programs get the master address (logged).
2. **A launcher dialog's buttons work with taps, Quit included.** If the parent list can't be
   read, `[win32u] GetAncestor(..., GA_ROOT): no parent list, root ...` appears once; if
   activation fails, `[win32u] click on ...: top window ... not activated, click delivered`
   appears once, and the button still acts.
3. **A program that calls `ActivateKeyboardLayout` with flags in a loop** logs `flags ... not
   supported (logged once)` once (with `WINEDEBUG=fixme+keyboard`).
4. **A 32-bit program's DIB section from its own section** (`CreateDIBSection` with a file
   mapping) gets bits it can write (an address inside its window).
5. **Source-bits hook.** With the app's `winios_dump_srcbits` linked, it's called only for
   `PutImage`s onto 640 x 400 or larger; without it, nothing is called and nothing fails to link.

## Spec 22 Part E

Host: `test_w32ios_s22` (in `run-host-tests.sh`): the rule's table (own process, another
process, no owner, an older window's object, non-window objects by owner only), then a fake shared
user handle table: game 1's windows found by full and short handle; after game 1 ends, its slot
reused for game 2's desktop window (no client object) gives NULL, not game 1's freed window; game
2's own window in game 1's old slot is found; an own stale object under a newer generation is
dropped; enumeration returns game 2's window only; a menu slot holding an ended process's object
is skipped.

On the device (with `MYIOSDECK_SESSION_REUSE=1`): a second game after a first that had windows
(a launcher, a game with dialogs) starts, shows its window and takes input; no crash in a window
procedure of the first game (the case this part is about: a message dispatched through an ended
game's freed window object). Each game logs `virtual monitor WxH` with its own launch's size.

## Clarifications 8 and 9 (stand-in unix tables)

Host test `tests/test_unix_tables.c`, built and run by `tests/run-host-tests.sh` once per line of
`unix-tables/tables.txt` (`-DTEST_<NAME>`, `-Werror`). For each library, `gen-entries.sh` runs on
its header in `refs/wine-11.4` (output in `$TMPDIR`), and `<name>_ios.c` is compiled against that
real header and `refs/wine-11.4/include` with a fake `w32ios_unix_unsupported`. Checks per
library:
- both tables have the enum's count of entries and no NULL;
- the setup entries, listed in the test separately from `tables.txt`, return 0 (with a guard-page
  pointer and with NULL) and don't report;
- every other entry, called with its parameter pointer on a `PROT_NONE` page (a touch ends the
  test with SIGSEGV), returns `STATUS_NOT_SUPPORTED` and reports its own index with its library's
  log head; three entries are also checked by name;
- the names agree with spec 09's `ios_unixlib_symbol` (linked from
  `out/09-ntdll-processes/ntdll-unix/ios_pure.c`) for the DLL file name and for an upper-case full
  path;
- the log-once set: same process, same PEB with another id, same id with another PEB, the oldest
  forgotten after 32.

The script also fails when a library isn't listed in spec 09's `unixlibs.txt`.
`unix_tables_log.c` is checked with `gcc` and `clang -fsyntax-only -Wall -Wextra -Werror` against
`refs/wine-11.4/include`.

Last run (2026-10-21), gcc 15 and clang 21: `opengl32.dll: 61 checks`, `winspool.drv: 53`,
`netapi32.dll: 53`, `kerberos.dll: 53`, `msv1_0.dll: 49`, 0 failures. Spec 09's host tests:
`33 names, write-exports.sh matches the loader's rule`.

On the device:

1. The 32-bit Direct3D 9 game that ended with `0xC0000142` starts. Its log has
   `[opengl32] no OpenGL on iOS: <n> returns STATUS_NOT_SUPPORTED` at most once per program.
2. A Unity game starts past winspool: no `unixlib: winspool.drv has no table ...` line and no
   fault in `__wine_unix_call_dispatcher`. If it asks for printers, it logs
   `[winspool] no printing on iOS: 1 returns STATUS_NOT_SUPPORTED` (or 2, 3) once.
3. No `unixlib: ... has no table` line appears for netapi32.dll, kerberos.dll or msv1_0.dll. A
   program that asks for a domain, a Kerberos ticket or NTLM gets an error and logs its library's
   line once.
4. `nm -gU` of the app lists the ten `_<name>_unix_call_funcs` / `_<name>_unix_call_wow64_funcs`
   names (spec 09's exports check).

## Clarification 10 (each program's own screen shape)

Host tests in `tests/test_w32ios.c`: `test_launch_size` (the decision; two top-level programs,
1280x720 then 960x720 with nothing else running, and the modes list the second one's
`EnumDisplaySettings` builds: 960x720 first, 1280x720 not offered; the refusal while another
program runs; the same size again; a size changed by `ChangeDisplaySettings` going back to the
launch's) and `test_user_program` (system images inside the Windows directory, games outside it,
a look-alike directory, an unreadable path, helpers by name). The display state itself and the
server update are Wine code: `tests/syntax-check-patched.sh` compiles them, the device runs them.

On the device, with session reuse on (`MYIOSDECK_SESSION_REUSE=1`):

1. **The second program gets its own size.** Start game A with `MADEIRA_SCREEN_W=1280`,
   `MADEIRA_SCREEN_H=720`, quit it, start game B with 960x720 in the same session. B's log:
   `[win32u] virtual monitor 960x720 (was 1280x720) for a new program`. From B's first call,
   `GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN)` give 960/720, `EnumDisplaySettings(ENUM_CURRENT_
   SETTINGS)` and `ENUM_REGISTRY_SETTINGS` 960x720, the modes list starts with 960x720 and has
   nothing above its pixel count, `SPI_GETWORKAREA` is 0,0-960,720, `MonitorFromPoint` /
   `GetMonitorInfo` give 0,0-960,720, and the cursor reaches the whole 960x720 screen (the server
   got the new monitor). No `WM_DISPLAYCHANGE` is sent.
2. **Same size:** B with 1280x720 after A at 1280x720 logs (verbose)
   `virtual monitor 1280x720 for a new program (unchanged)`.
3. **Refused:** if a user program of the session is still running when B starts (for example A's
   game started by its launcher, still running), B logs
   `[win32u] virtual monitor stays 1280x720: another program is running` and, verbose,
   `process XXXX (Game.exe) is running`; B's metrics are 1280x720. Wine's own processes
   (`services.exe`, `explorer.exe`, `winedevice.exe`, ...) and crash handlers never cause this.
