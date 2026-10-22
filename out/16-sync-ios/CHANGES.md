# Spec 16: synchronization on iOS, what was done where

This folder holds the new code (MIT) and the one PE file of this spec. The changes to spec 08's
server and spec 09's ntdll unix side are listed in those trees' `CHANGES.md`, tagged "spec 16".
Questions and the answers chosen are in `QUESTIONS.md` under "Spec 16".

## The library (`include/`, `src/`), compiled into `libntdll_unix.a`

- B: `include/ios_fastsync.h`: the 32-byte cell (`word` = generation:state, `srv_waiters`,
  `cli_waiters`, `manual`, `kind`, `max`, `reserved`) with `_Static_assert`s, the states, the
  learn-reply flags and the three `event_op` codes, 64-bit seq-cst accessors, a compile error on
  big-endian. `src/ios_sync_cells.c`: the one definition of the 8192-cell table; park / wake on
  the state half (`os_sync_wait_on_address_with_timeout` on the Mach absolute clock and
  `os_sync_wake_by_address_any/all` from iOS 17.4; below that, spec 00 B1 rules out upstream's
  undeclared `__ulock_wait` / `__ulock_wake`, so a park re-reads the word every 50 us with
  `nanosleep` until it changes or the time is up, and a wake does nothing; futex on Linux for
  the host tests); the server's allocator (C1: last freed
  first, then a bump index, full table logged once; generation bumped on every allocation and
  free, skipping 0, written with the state in one store; free wakes parked clients).
- A: `src/ios_sync_config.c`: the settings file (`$MADEIRA_DOCS_DIR/madeira.cfg`, spec 02 Part G's
  rules: BOM, `#`, trimming, last occurrence wins) read once: `inproc-sync`, `madsync-lock-spin`,
  `madsync-spin-us`, `yield-streak`, `yield-sleep-us`, `eco`, `eco-qos`, `alert-spin-us`
  (out-of-range numbers are clamped, non-numbers keep the default). The fastsync switches
  (`MADEIRA_FASTSYNC` 1/on/yes, auto, cells; `MADEIRA_FASTSYNC_SEM`, `MADEIRA_FS_POLLPEEK`,
  `MADEIRA_FASTSYNC_SPIN_US`, `MADEIRA_FASTSYNC_CAP_US`, `MADEIRA_FS_AUTO_REQS`) read once per app
  run; a later session asking for something else is logged once. A2's decision (`inproc-sync`
  true and `MADEIRA_MADSYNC_SESSION` not exactly `0`), made once and logged with the value read,
  whether the file was found, and the result; a later session whose environment asks for the
  other engine is logged once at its process initialisation and ignored.
- C: `src/ios_fastsync_srv.c`: the server's side of a cell as functions the server's hooks call
  (allocation rules of C2, `signaled` with the CLAIMED claim, `satisfied`, set / reset with the
  client wake, disable with the fold, the wait-all give-back, semaphore release / take / give
  back / disable / count). `-1` = DISABLED, the server's own field rules.
- D: `src/ios_fastsync_cli.c`: the app-wide handle -> cell cache (2048 entries, direct-mapped on
  `handle >> 2`, keyed by handle and process id, seqlock readers, one writer mutex with signals
  blocked, eviction by handle whatever the process and by process id); the learn-reply check;
  the gates (armed, the `auto` counting in 10 s windows on `CLOCK_UPTIME_RAW`, logged once);
  set / reset / release / query / try / peek on a cell with the generation learnt; the fast
  wait (96 tries, the timed spin under the app-wide credit with probes at 0, the park of at most
  64 rounds on the smaller of the cap and the caller's time); the remaining-timeout arithmetic;
  the poll-peek periods (16 and 64); the watchdog slices (2, 8, 32, 60 s); the process
  initialisation (eviction by process id, the A6 log line, or "not started" when the client was
  handed the ntsync device).
- E: `include/linux/ntsync.h` (the documentation's argument structures, this library's request
  codes, `IOS_USYNC_DEVICE` 0x6fffffff) and `src/ios_usync.c`: the userspace ntsync engine.
  Pseudo descriptors `0x70000000 | index` (index 0 unused), a table of 4096 slots that doubles and
  is read without the lock, references (creation, hand-off, waits; a slot is reused after the last
  one), the hand-off table (256 posts, a second post for the same pair replaces the first), one
  mutex with every signal blocked, a spin of `madsync-lock-spin` tries before blocking, heap
  waiter records with a FIFO Mach semaphore in thread-specific data (the destructor unlinks a
  thread that ends while queued), lock-free polls, the optional `madsync-spin-us` spin. A wait
  that a longjmp or a handler left behind is cleaned up by the thread's next wait.
- G: `src/ios_sync_pace.c`: the yield streak (gaps under 2 ms; past `yield-streak`, sleep
  `yield-sleep-us`; the values logged once); eco mode (`madeira_set_eco` logs a timestamped
  line and bumps a generation that starts at 1; `madeira_get_eco`; `ios_eco_apply` applies the
  current class to the calling thread when its generation is older: on = `eco-qos`'s class,
  off = `QOS_CLASS_USER_INTERACTIVE`; a thread that never saw eco on makes no call).

## Part F: `pe/dlls/ntdll/sync.c` (modified copy of Wine 11.4's `dlls/ntdll/sync.c`, LGPL)

Spec 19 builds it in place of upstream's file in every PE ntdll; nothing else differs from
upstream (`diff` against `refs/wine-11.4/dlls/ntdll/sync.c` shows only these, each marked
"MYIOSDECK").

- F3: `get_futex_queue` takes its bucket array from `get_futex_table`: the first time, a 64-bit
  ntdll asks `NtQuerySystemInformation` for the private class `0xf00d` with a pointer-sized
  buffer (spec 17 D4's shared table, upstream's layout, `C_ASSERT`ed as three pointers); on
  success with a non-NULL address it uses that table, otherwise (and always in a 32-bit ntdll,
  whose layout differs and whose class WoW64 doesn't translate) the file's own array. The choice
  is published once with a compare-and-swap. Same hash as upstream.
- F4: `RtlWaitOnAddress` passes the address to `NtWaitForAlertByThreadId` instead of NULL.

## Spec 00 (App Store rules)

- B1: public API only. Spec 16 B5 asks for upstream's fallback below iOS 17.4 (`__ulock_*`,
  not declared by the SDK); the allowed alternative is the `nanosleep` polling park above. Mach
  semaphores (`semaphore_create` / `_wait` / `_timedwait` / `_signal` / `_destroy` on
  `mach_task_self()`), `os_sync_wait_on_address*`, `pthread_set_qos_class_self_np` and
  `clock_gettime_nsec_np` are public SDK API on the app's own task and threads.
- C, A: no process creation, no executable memory, nothing loaded at run time.
- D4: no privacy-sensitive API. D5 (required-reason API, system boot time):
  `clock_gettime_nsec_np( CLOCK_UPTIME_RAW )` (the fastsync timing, the yield streak) measures
  time between events inside the app: reason `35F9.1`. The settings file is read with
  `fopen` / `fread` (no timestamps, no user defaults).
- F: `tests/run-host-tests.sh` runs `check-app-store.sh` on this folder and the server / ntdll
  files this spec added to, and fails on `__ulock_*` / `__psynch_*` in this part.

## Tests

- J: `tests/run-host-tests.sh` (gcc and clang, `-Wall -Wextra -Werror`): `test_config_pace`
  (A, G), `test_usync` (E), `test_fastsync_model` (B-C as a model with client threads),
  `test_fastsync_client` (D's pure parts and the cell operations). `TESTS.md`: what only the
  device can check.
- J: `tests/check-wine-files.sh` (run by the runner when `WINE_BUILD` is set): the modified
  server files (`event.c`, `semaphore.c`, `inproc_sync.c`, `async.c` with and without `WINE_IOS`;
  `thread.c`, `process.c` without it, their iOS paths need Mach) and ntdll's `sync.c` and
  `server.c` (both ways) compile, syntax only, against a host Wine build; implicit declarations
  and incompatible pointers are errors, warnings are shown.

## The hooks in the Wine trees

- Spec 08's server (Parts C, E, H): `out/08-wineserver-ios/CHANGES.md`, "Spec 16".
- Spec 09's ntdll unix side (Parts D, E, G): `out/09-ntdll-processes/CHANGES.md`,
  "Clarification 5".
