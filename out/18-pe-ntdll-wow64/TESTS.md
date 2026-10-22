# Spec 18 tests

## Host tests (run here, in WSL)

```
wsl.exe -d Ubuntu -- bash -lc "cd /path/to/pocketgames-engine/out/18-pe-ntdll-wow64 && ./tests/run-host-tests.sh"
```

`tests/run-host-tests.sh` builds each test with gcc and clang, `-std=gnu11 -O2 -Wall -Wextra
-Werror`, once plain and once with ASan/UBSan, runs it and exits non-zero on any failure. The tests
include the same pure files the PE sources include (no Windows headers); a Wine build is not needed
(`WINE_BUILD` is accepted and ignored).

| test | covers |
|---|---|
| `test_arm64ec_loader.c` | D3 `arm64ec_iat_slot_is_x64_export` on synthetic images: a match, stub outside every x64 range, in an ARM64EC or ARM64 range, a second x64 range, forwarder string entry, misaligned and out-of-image slots, export directory too small / past the end / huge size, address table past the end, absurd function count, empty or out-of-image code map, the stub crossing the range end, a stub at the image's end. D1 `ec_redirect_resolve` on mapped synthetic modules and a fake module list: NULL, table hit, out-of-image destinations, fast-forward (both signs), broken sequence, bound / unbound `FF 25` slots (small RVA, 0, self, just below 0x10000000), slot into an x64 module or an unlisted one, a chain of two forwarders, slot outside the image, unknown pattern, pointer outside the given module, the code-map restriction and idempotence, a slot cycle |
| `test_unwind.c` | F1 window (budget bounds, several regions, foreign allocation ends it, uncommitted `Rsp`), F2 frame checks, F4 guard (moving up, same frame other pc, backwards, four repeats and reset, guest↔emulator switches, budget, no window), F3 rules (end of stack, livelock counter to 16 and reset by another pair, > 65536 steps, terminal frame once only and exact, first-step bad frame), F5 coverage (several regions, foreign region ends the walk, too small → 2^47, too small for the caller's code, NULL / unqueryable bitmap) and the bit test, G3 stall guard |
| `test_misc.c` (clarification 1) | `test_farm`: the `sysaa64` name rule (full NT path, `/` separators, no extension, other extension, no base name, empty, exact `MAX_PATH` fit and one too long with the longer prefix); the order on a model of `find_dll_file` (found in system32: the farm is never tried; `STATUS_INVALID_IMAGE_FORMAT` and `STATUS_DLL_NOT_FOUND`: tried; other errors: not tried; not in the farm either: the original status); logging once per DLL name across case and path, and a full table |
| `test_misc.c` | H3 always-logged names, H5 XInput names (case, paths, near misses), H6 `sysx64` path (directory, `/`, no extension, other extension, exact `MAX_PATH` fit, one too long, small output buffer), H7 conversion at 24 MHz and 1 GHz up to 2^64, measured offset, offset rule (first, smaller, +1 ms, > 1 ms), measurement due, monotonic over a 30 s fake sleep, I1 parser (`1`, `0`, `11`, empty, NULL), I2 grow/retry sizes in both modes and termination, I6 census (32 tracked sites, untracked sum, top 6 order, fewer than 6) |
| `test_wow64.c` | J2/K2 conversions with bases 0, 0x100000000, 0x7200000000 (NULL, normal, last byte, below 0x10000, identity at B = 0, low 32 bits), J5 gate check, J3 cache (B = 0 no query, own process, NULL handle, miss then hit, failing query, 9 handles round robin, reused handle value with another pid), J7 every code round-trips 32 → 64 → 32, ordinals stay, > 15 parameters clamped, `Wow64RaiseException`'s sentinel, J9/J10/J12 small rules, K5/K7 tables (each listed code and message converts, a sample of others doesn't) |

## Device checks for the owner

Each with the log line or behaviour that shows it.

1. **A 64-bit x64 game starts and runs.** No `could not load`, `no usable dispatchers`,
   `failed to initialize` or `CPU start failed` line for `xtajit64.dll`.
2. **An x64 program whose executable uses thread-local variables (slot 0) starts** and its
   thread-locals start zeroed/initialised (e.g. a MSVC program with `thread_local` state read before
   any write).
3. **x64 exceptions**: a NULL dereference caught in `__try`, a C++ exception caught by `catch`, and
   `setjmp`/`longjmp` all work; no `invalid frame`, `walk makes no progress` or `unwind stopped`
   lines.
4. **RPC through `sechost`** (e.g. `sc query` or a program calling `OpenSCManager` /
   `QueryServiceStatus`) returns correct results (Part D6).
5. **An x64 program started by an ARM64 program finds its DLLs**: with `WINEDEBUG=warn+module`
   the log shows `warn:module:find_farm_dll <name>: using \??\C:\windows\sysx64\<name>` once per
   DLL name (clarification 2: at WARN, so the default log has no such line).
5a. **Clarification 1: in an x64 session, `wineboot.exe` and other ARM64 programs start**: no
   `failed to load L"kernel32.dll", status c000007b`; with `WINEDEBUG=warn+module` the log shows
   `L"kernel32.dll": using \??\C:\windows\sysaa64\kernel32.dll` once per DLL name, and the
   default log shows no `err:module:find_farm_dll` line.
6. **A game shipping `xinput1_3.dll` sees the controller** (the built-in XInput is loaded from
   `system32`; `MADEIRA_NATIVE_XINPUT=1` makes it use the game's copy again).
7. **`__fastfail`**: a program calling it ends alone with `fast fail: code ... terminating the
   process`; other programs keep running.
8. **`QueryPerformanceCounter`** agrees with `GetTickCount64` over 60 s including a device sleep
   and never goes backwards (in every thread).
9. **A 32-bit program** starts, shows a dialog (class `#32770`) and a themed control with window
   properties.
10. **A 32-bit program raises and catches an access violation and names a thread**; no
    `guest address ... arrived unconverted` line.
11. **A 32-bit launcher starts a 32-bit game.**
12. **A 32-bit debugger-style tool reads another 32-bit process's memory** (`ReadProcessMemory`,
    `VirtualQueryEx`) and gets that process's data.
13. **A 32-bit program with DEP off runs code it wrote** (the CPU module receives
    `BTCpuNotifyProcessExecuteFlagsChange`).
14. **With `MADEIRA_HEAP_COMPACT=1 MADEIRA_HEAP_RECLAIM=1 MADEIRA_HEAP_STATS=1`** a memory-hungry
    32-bit game runs; the log has `32-bit heap policies: compact 1 combined 0 reclaim 1 stats 1` and
    `heap ... census` lines.

Only the device can check: everything that needs the real loader, the CPU modules, the timer
registers, exception dispatch and the guest window; the host tests cover the pure decisions.
