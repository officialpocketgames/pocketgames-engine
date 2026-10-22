# Spec 16: tests

## Host tests (WSL, Linux)

```sh
./tests/run-host-tests.sh                        # gcc and clang
WINE_BUILD=~/wine-build ./tests/run-host-tests.sh   # also compiles the modified Wine files
TSAN=1 CC=clang ./tests/run-host-tests.sh           # under ThreadSanitizer (no race reported)
```

Linux stands in for Darwin: the Mach waiter semaphore is a POSIX semaphore and the address wait
is the futex system call. What they check:

- `test_config_pace` (Part A, G): the settings reader (BOM, comments, lines without `=`, trimming,
  values with `=`, the last occurrence wins, booleans), defaults and clamping of every key, the
  engine choice for every combination of `inproc-sync` and `MADEIRA_MADSYNC_SESSION`, the real
  file through `MADEIRA_DOCS_DIR` (read once), the fastsync switches and their bounds, the yield
  streak (2 ms gap, limit, sleep 0 = never), eco classes and generations.
- `test_usync` (Part E): semaphores (create, release, overflow, read), mutexes (recursion, unlock
  by a non-owner, kill, `EOWNERDEAD` reported once, owner 0), manual and auto events, a pulse
  releasing only the current waiters (one for auto), wait-any order, wait-all all-or-nothing and
  duplicates, the alert index (wait-any and wait-all), monotonic and realtime timeouts, a close
  during a wait, a thread cancelled while queued (the destructor), the hand-off (replace, other
  process / token, missing, 256 posts), libc pass-through, table growth and slot reuse, and an
  8-thread stress run that checks tokens are conserved and a mutex keeps a counter exact.
- `test_fastsync_model` (Parts B-C): a fake one-at-a-time server running the real hooks against
  client threads: an auto event (fast and server-only waiters, three setters) where releases +
  final state == RESET->SET transitions; a semaphore where the count stays in [0, max] (sampled)
  and consumed + left == released; a failed wait-all leaves every object as it was; a stale
  (index, generation) takes nothing; disable folds SET / CLAIMED / RESET and the count; a pulse;
  the timer re-check; a full table.
- `test_fastsync_client` (Part D): the seqlock cache (publish, evict by handle, process-id flush,
  -1 entries, the access check, no torn reads under a concurrent writer), the reply check, the
  spin credit, the auto window, the peek periods, the remaining-timeout arithmetic, the watchdog
  slices, the cell operations and the fast wait (park, wake by a set, recycled cell, short budget).

- `check-wine-files.sh` (with `WINE_BUILD`): the changed server and ntdll files compile against
  Wine's headers (x86-64 host: the iOS paths of the server's `thread.c` / `process.c` need Mach
  headers and are only compiled by the iOS build).

What only the device can check: the Mach semaphores and `os_sync_wait_on_address` (Linux uses
POSIX semaphores and futex), the QoS classes, the polling park below iOS 17.4, the real server thread
and its request rate, timing on the phone's cores.

## On the device

1. A heavily threaded game with `MADEIRA_FASTSYNC=auto`: the log shows
   `sync: fastsync: mode auto, ...` once and later `sync: fastsync: armed after N operations in
   T s`; the server's request rate (spec 08's counters) falls.
2. The same game with `MADEIRA_FASTSYNC=0` still runs (nothing logged by fastsync).
3. `inproc-sync = 1` in `madeira.cfg`: `sync: in-process ntsync: inproc-sync = 1 (settings file
   found), MADEIRA_MADSYNC_SESSION unset -> on`; programs boot and run. With
   `MADEIRA_MADSYNC_SESSION=0` the line ends `-> off` and the session uses fastsync.
4. A program that uses `PulseEvent` (waiters released, the event reset).
5. The watchdog's line `sync: fastsync: watchdog: ...` never appears in a normal run.
6. A .NET (Mono) game whose threads meet on SRW locks and condition variables doesn't stall
   (Part F: one queue table for the ARM64 and the ARM64EC ntdll).
7. A thread polling `WaitForSingleObject(h, 0)` with `SwitchToThread` doesn't keep a core at
   100 % (`sync: yield: streak 256, sleep 100 us` logged once).
8. Switching eco mode in the app logs `sync: eco: on at HH:MM:SS.mmm (generation N)` and the
   threads' QoS class changes (Instruments, or `thread_policy_get`).
9. Two programs one after the other (cache entries of the first process don't leak into the
   second; spec 08's ids never reused).
10. Force quit while threads are parked in fast waits: the app ends them within the park cap.
