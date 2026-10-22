# Spec 08: changes to upstream Wine 11.4's server

Base: `refs/wine-11.4/server/` (Wine 11.4 release, LGPL-2.1-or-later). Modified copies are in
`server/` with their licence headers kept and a "Changed for MYIOSDECK" line. New files
(`server/ios_*`) are MIT. Every change is compiled only with `WINE_IOS` defined, so the
modified files still build the desktop server unchanged.

One line per change, by Part.

## Part A: build

- A: `build-ios.sh` compiles every `.c` in upstream `server/Makefile.in` `SOURCES` (modified copy from `server/` where one exists) plus `server/ios_*.c`, for arm64, iOS 17.0, `-O2`, `-DWINE_IOS=1`, and writes `libwineserver.a`; it checks that the library defines `wineserver_main` and no `main`.
- A: `server/ios_config.h` (new, passed with `-include`): includes the host `config.h` first, then undefines kqueue, epoll, event ports, inotify, ptrace/procfs, Linux-only and SIGIO-with-fd features.
- A: `main.c`: `main` is named `wineserver_main`.
- A: `process.h`: on iOS the tracing backend is `USE_IOS_THREADS` instead of `USE_MACH`.
- A: `server/ios_thread.c` (new): the tracing interface for pseudo-processes of one task (no-op process tracing, no thread signals, process memory read/written in the app's own task with `vm_read_overwrite` / `vm_write`).
- A (iOS CI): `ios_thread.c`, `ios_context.c`: own-task memory goes through `vm_read_overwrite` / `vm_write` (`<mach/vm_map.h>`, through `<mach/mach.h>`), because `<mach/mach_vm.h>` and its `mach_vm_*` calls are marked unsupported in the iPhoneOS SDK; same behaviour (a bad address is a Mach error, not a fault).
- A: `build-ios.sh` picks up the Part E files (`ios_context.c`, `ios_suspend.c`) with the other `server/ios_*.c`; no script change.

## Part B: a server that is a thread

- B1: `main.c`: options never exit (`-h`/`-v` print, `-k`/`-w` are logged and ignored, a parse error prints usage and goes on); `foreground` is forced and `master_socket_timeout` is infinite.
- B1: `main.c`: the first `wineserver_main` does upstream's start-up; a later call in the same app run reuses that state (QUESTIONS 08-1); after the main loop it clears `wineserver_ready`, saves the registry and returns.
- B1: `request.c`: `fatal_error` is not defined (the app provides it); no fork/daemon path; `close_socket_timeout` asks the loop to stop instead of `exit(0)`; no `atexit` handler.
- B1 (spec 00 C1): `request.c`: the server never forks; upstream's daemon `fork()` stays only in the desktop (`#else` of `WINE_IOS`) branch and carries an `app-store-ok` marker for `check-app-store.sh`, which `tests/run-host-tests.sh` now runs.
- B2: `main.c`, `signal.c`, `ios_server.c`: every signal blocked on the server thread (at entry and again at the start of the main loop), `SIGPIPE` ignored, no handlers for `SIGHUP`/`SIGINT`/`SIGQUIT`/`SIGTERM`/`SIGABRT`; `init_signals` creates no handler pipes.
- B2: `signal.c`: the watchdog is a deadline on the monotonic clock instead of `alarm()`.
- B3: `ios_server.c` `ios_kill()`: a signal to the app's own pid or to a process group does nothing and reports success; `request.c` and `process.c` route every `kill()` through it.
- B4: `request.c`: the server directory is `$WINEPREFIX/.wineserver/server-<dev>-<ino>` (upstream's Android layout); "lock held" and "socket in use" are `fatal_error`s instead of `exit(2)`.
- B5: `unicode.c`, `ios_server.c`: `wineserver_set_nls_dir()`; NLS tables come from that directory, else from `nls/` next to the app's executable.
- B6: `main.c`, `ios_server.c`: `wineserver_ready = 1` (release store) after `init_registry` and the rest of start-up, right before the main loop.
- B (clarification 6): `main.c`: on the first start, right after `init_registry` (the object namespace exists since `init_directories`), calls spec 15's weak `ios_hidpad_server_init()` once; skipped when it isn't linked.
- B7: `ios_log.c` (new): `wineserver_log_set_file`, `ws_log` (timestamped line), `wine_ui_log` (QUESTIONS 08-2); `object.h` maps `stderr` and `perror` to the log, so every server message to stderr becomes a timestamped log line.

## Part C: connecting clients

- C1: `ios_server.c`: `wineserver_inject_client_fd()` (any thread) appends the fd to a mutex-protected FIFO (grows as needed, nothing dropped) and wakes the loop.
- C1: `request.c`: `ios_new_client_connection()` does what upstream does for an accepted connection (non-blocking, process with no parent, its first thread); the master socket's accept path and `ios_accept_injected_clients()` (main loop step 2) both use it.
- C2: `request.c`: the master socket is still created, and is non-blocking (the loop tries `accept()` every iteration, Part D).
- C3: no change: fds still arrive with `SCM_RIGHTS` on the process socket and the server keeps its own copies.
- C4: no change: `init_first_thread` / `init_thread` already store the pid and tid the client sends (the app's pid; the thread's Mach port name, spec 09).

## Part D: the main loop

- D: `fd.c`: on iOS `main_loop()` is a new loop: (1) `get_next_timeout`, (2) injected connections, (3) sleep until the earlier of the next timer and 1 ms on a Mach semaphore, (4) `set_current_time`, (5) network sockets, (6) everything else; it runs until `g_wineserver_should_stop`, a former exit path (`ios_request_loop_exit`) or the last user is gone. `server_start_time` is kept across restarts.
- D3: `ios_server.c`: `wineserver_request_wake()` signals the semaphore; the loop drains the count right after waking (bounded), so a backlog only costs one extra iteration and never stops it from sleeping.
- D5: `fd.c`: per poll slot the class of its descriptor (network if `getsockname` says `AF_INET`/`AF_INET6`) is cached with the fd it was decided for and reset when the slot is reused; network users are `poll()`ed with timeout 0 in batches of 128; before each dispatch the slot must still hold the same `struct fd` and unix fd.
- D6: `fd.c`: other users get `POLLIN` when `ioctl(FIONREAD)` reports bytes, or every iteration when the fd is marked always-readable; `POLLOUT` every iteration. Cost per iteration is one pass over the slots plus one syscall per waiting descriptor.
- D6: `fd.c` `set_fd_ios_always_read()`; `thread.c` marks request pipes (and makes the server-created ones non-blocking), `process.c` client connections, `request.c` the master socket.

## Part F: shared data page, timers, windows, sections

- F1: `fd.c`: `KUSER_SHARED_DATA` times (`SystemTime`, `InterruptTime`, `TickCount`, `TickCountLowDeprecated`) are published by `set_current_time` on every loop iteration in upstream's high/low/high order; `TimeZoneBias` is not published; `MADEIRA_USD_TIME=0` turns publishing off.
- F2: `file.h` `timeout_to_abstime`, `timer.c` `set_timer`: a relative timeout whose conversion would overflow (or reach INT64_MIN) becomes `TIMEOUT_INFINITE`.
- F3: `window.c`: the forced `get_desktop_window` path is `create_desktop_windows()`; `create_window` calls it when a top-level window is made on a desktop without a desktop window.
- F4: `winstation.c`: every new window station is `WSF_VISIBLE`; `set_process_winstation` marks the process's station visible and gives it an input desktop if it has desktops but none is the input one (`create_desktop` already sets the first one).
- F5: `ios_server.c`, `window.c`, `winstation.c`, `fd.c`: the screen size comes from `wineserver_set_screen_size()` or `MADEIRA_SCREEN_W/H` (QUESTIONS 08-3); `get_virtual_screen_rect` uses it while the monitor union is empty; a new desktop's cursor clip starts as that rectangle; a size change re-clips every desktop (loop step 2).
- F5 (clarification 3): `winstation.c` `set_winstation_monitors`: once monitors are reported the monitor list is used as upstream does, and every such request resets each desktop's cursor clip to the new virtual screen.
- F6: `mapping.c`: on iOS `create_temp_file` creates `tmpmap-*` with `openat(server_dir_fd, ...)` and `unlinkat`, never via the working directory; `grow_file` checks `ftruncate`/`fstat` and fails with `STATUS_DISK_FULL` when the file is not exactly the requested size.
- F7: `mapping.c`: `get_mapping_info` gives a 64-bit image with a preferred base below 4 GB, queried by a 64-bit process, an address above 4 GB from the 64-bit range (kept per mapping, freed on destroy) and `IMAGE_FLAGS_ImageDynamicallyRelocated`; `map_image_view` expects it there. 32-bit (WoW64) processes are unchanged.

## Part G: ending programs

- G1: `ios_server.c`: `wineserver_kill_programs()` (any thread) records the exit code, increments `g_wineserver_force_quits` and wakes the loop; `process.c` `ios_terminate_programs()` (loop step 2) terminates every non-system process with running threads via upstream's `terminate_process` and then alerts each of its threads through `wineserver_thread_alert_hook` (QUESTIONS 08-4).
- G2: `thread.c`, `thread.h`: a thread told to terminate itself is marked `ios_exiting`; with `MYIOSDECK_SESSION_REUSE=1`, if the server still has it 250 ms later, its request pipe is polled with timeout 0, the result logged, and the thread killed as a closed pipe would.
- G3: `thread.c` `queue_apc`: a thread that is terminated or `ios_exiting` gets no APC; an async I/O completion goes to a waiting thread of the process or is dropped (its async completes as upstream does when no APC can be queued); a process-wide system APC goes to a live thread, which is alerted; the alert replaces the `SIGUSR1` upstream sends for a thread-specific system APC.

## Part E: thread suspension and contexts

- E: `server/ios_thread_layout.h` (new): the TEB64 offsets (syscall frame 0x378, kernel stack 0x3a8, TlsSlots, ChpeV2CpuAreaInfo), `CHPE_V2_CPU_AREA_INFO.ContextAmd64`, the `WOW64_CPURESERVED` header and upstream's `struct syscall_frame` layout, with `_Static_assert`s against `winternl.h` / `winnt.h` and on the frame's own offsets; marked as having to match spec 11 Part A2's shared header.
- E: `server/ios_suspend.c` / `.h` (new, pure, host-tested): the snapshot window (`ios_freeze` / `ios_thaw`), the hold (`ios_hold_take` / `ios_hold_release`), "inside a system call" (`ios_in_syscall`), the frame fingerprint (`ios_frame_mark_*`) and native writes into a frame (`ios_frame_apply_native`); Mach calls come in as an ops table.
- E1: `server/ios_context.c` (new): `ios_capture_context` stops the thread (`thread_suspend`), copies `ARM_THREAD_STATE64` / `ARM_NEON_STATE64` and, with `vm_read_overwrite` on the own task, its syscall frame (if sp is on its kernel stack below the frame) and guest context (ARM64EC `ContextAmd64`, WoW64 `I386_CONTEXT`), resumes it, then converts outside the window (no allocation, log or lock while stopped). Native half from the frame inside a system call, else from the thread state; x18 reported as the TEB; guest half filled when the process machine isn't native.
- E1: `thread.c`: `stop_thread` takes that snapshot instead of sending `SIGUSR1` and completes the context at once (threads before `init_thread` keep upstream's cooperative path); a snapshot of a running thread is dropped after the request; the snapshot is never given to the thread as its own context (`send_thread_wakeup`, `select` reply); `struct context` has `ios_captured`, the thread `ios_mark` (frame address and pc/sp/fp/lr at capture).
- E2: `thread.c`, `process.c`: `ios_suspend_begin` / `ios_suspend_end` at the points where a thread becomes suspended or stops being suspended (thread or process count). Default: count plus snapshot, the thread runs. `MADEIRA_REAL_SUSPEND=1`: one `thread_suspend` hold taken before the snapshot, nested suspensions only count, the end releases exactly that hold; `thread->ios_hold` is 0/1 from `init_thread_structure`; `cleanup_thread` releases a dying thread's hold.
- E3: `thread.c`, `ios_context.c`: `set_thread_context` on another thread records the changed parts (`ios_dirty`, per half); `ios_apply_context` runs when the thread resumes (at once for a running thread): with the thread stopped, it checks the same frame address and return pc/sp/fp/lr, writes the native registers into the frame (never x18 or the dispatcher's bookkeeping; `restore_flags` CONTROL / FLOATING_POINT) and the guest registers into the CPU area (WoW64 also gets `WOW64_CPURESERVED_FLAG_RESET_STATE`); otherwise the write is dropped and logged (after any hold is released). A thread that sends its own context with `select` while a snapshot exists takes over the changed registers in upstream's pending form.
- E (clarification 5): `tests/test_ios_suspend.c`, `tests/run-host-tests.sh`: host tests for the hold counter and the fingerprint (fake Mach calls).
- E (spec 11 clarification 4): `ios_thread_layout.h` takes the syscall-frame and kernel-stack TEB offsets from spec 11 Part A2's shared `ios_syscall_frame.h` in the server build and asserts its own frame copy against it; `build-ios.sh` copies that header from `out/09-ntdll-processes/ntdll-unix/` into the staging folder (and fails without it); `ios_context.c` refuses to build without it. The host tests keep the written-out numbers, proved equal by those asserts.
- E1 (clarification 9): `ios_context.c`: the i386 half of a WoW64 capture is read only when the thread is inside a system call (and the CPU block's machine is i386); otherwise it has the machine and no flags. The ARM64EC x64 half is unchanged.
- E1 (clarification 10): `thread.c` `ios_refresh_snapshot`: every GetThreadContext on another, suspended thread takes a fresh snapshot (new frame and fingerprint) into the kept context, unless a SetThreadContext is pending, in which case the cached context with the changes is returned; SetThreadContext never re-captures.
- E3 (clarification 11): `thread.c`: SetThreadContext on another thread that isn't suspended fails with `STATUS_UNSUCCESSFUL` and changes nothing (replaces "applied at once"); a thread setting its own context is unchanged.

## Part H: tests

- H: `TESTS.md`: on-device checks (boot, network, GetTickCount, two programs in a row, force quit, sections, images, log, signals, wake-up) with the log lines to look for.

## Spec 17 Part H: sockets and desktops (spec 08 clarification 7)

- spec 17 A1, `build-ios.sh`: `out/09-ntdll-processes/include/` (the shared `wine/unixlib.h` and
  `winternl.h`) is on the include path before the Wine tree's `include/`.
- spec 17 H1, `server/sock.c` (modified copy of upstream's, LGPL): on Apple targets without
  `netinet/tcp_fsm.h` the `TCPS_*` values are the BSD ones (`server/ios_s17_pure.h`,
  `IOS_S17_TCP_STATES`: CLOSED 0, LISTEN 1, SYN_SENT 2, SYN_RECEIVED 3, ESTABLISHED 4,
  CLOSE_WAIT 5, FIN_WAIT_1 6, CLOSING 7, LAST_ACK 8, FIN_WAIT_2 9, TIME_WAIT 10) that Darwin's
  `TCP_CONNECTION_INFO` reports; upstream's `tcp_state_to_mib_state` is unchanged. Before, the
  Linux numbers made every listener `ESTAB` and every live connection `FIN_WAIT_1`. Nothing else
  in `sock.c` changes (spec 17 Part I).
- spec 17 H1, `server/ios_config.h`: `HAVE_NETINET_TCP_FSM_H` is undefined. The macOS host
  configure finds the header, the iPhoneOS SDK doesn't ship it (the first iOS CI build stopped at
  `sock.c`'s `#include <netinet/tcp_fsm.h>`); with the define gone, `sock.c` takes the BSD
  numbers above. No other file of the server, ntdll or nsiproxy builds includes that header.
- spec 17 H2, `server/winstation.c` `remove_desktop_user`: the desktop's close timeout is never
  armed; the first time it would have been, one line is logged (`wineserver: desktop close
  timeout suppressed ...`). `close_desktop_timeout` stays (marked unused).
- spec 17 H3, `server/winstation.c` `create_desktop`: a new desktop whose request carries
  `DF_WINE_VIRTUAL_DESKTOP` (not one that only inherits it from the creating thread's desktop)
  makes its window station visible (`WSF_VISIBLE`), gets the cursor clip `(0, 0)`–screen size
  (`ios_get_screen_size`, else 1024 x 768: `ios_s17_virtual_desktop_size`), and becomes the
  station's input desktop even if another one already is; one line is logged (`wineserver:
  virtual desktop ...: visible station, clip 0,0-W,H, input desktop`). Other desktops keep
  upstream's rule.
- spec 17 Part J, `tests/run-host-tests.sh` + `tests/test_s17_server.c`: `sock.c`'s own
  `tcp_state_to_mib_state`, extracted with `sed`, with the BSD numbers: each of 0-10 maps to its
  `MIB_TCP_STATE`, others to `CLOSED`; the virtual desktop's clip size.

## Spec 16: synchronization (clarification 8)

The library these hooks call (the shared cells, the server's cell hooks, the userspace ntsync
engine) is `out/16-sync-ios/` (MIT), linked from `libntdll_unix.a`; its own `CHANGES.md` has
the details.

- spec 16, `build-ios.sh`: `-DHAVE_LINUX_NTSYNC_H=1 -DIOS_USYNC=1` and `out/16-sync-ios/include`
  on the include path; `server/ios_config.h` keeps `HAVE_LINUX_NTSYNC_H` when `IOS_USYNC` is
  defined (it undefined it for every iOS build before).
- spec 16 A2 / E5, `inproc_sync.c` (new modified copy): `get_inproc_device_fd` is
  `IOS_USYNC_DEVICE` when the settings file selects in-process ntsync (decided once, logged),
  else -1; `ioctl` / `close` go to `ios_usync_ioctl` / `ios_usync_close`.
- spec 16 E4, `inproc_sync.c`, `thread.c`: `get_inproc_sync_fd` and `get_inproc_alert_fd` post
  {process id, token, descriptor} with a reference (`ios_post_inproc_fd`) instead of
  `send_client_fd`; a post that can't be stored is logged and fails with
  `STATUS_INSUFFICIENT_RESOURCES`. `init_first_thread` still sets `inproc_device` but sends
  nothing.
- spec 16 C7, `inproc_sync.c`: without a descriptor, `get_inproc_sync_fd` answers for a
  cell-backed event (`0x40000000 | manual 0x20000000 | index`) or semaphore (`0x40000000 |
  index`) with success; anything else `STATUS_NOT_IMPLEMENTED` as upstream.
- spec 16 C2 / C3, `event.c` (new modified copy): `struct event_sync` has `ios_cell`; events made
  by `create_event` get a cell (internal syncs never); `signaled` claims an auto event's SET
  (CLAIMED), `satisfied` resets it, set / reset go through the cell and wake parked clients, then
  the server queue as upstream; `add_queue` / `remove_queue` count `srv_waiters`; destroy frees
  the cell; query, dump and replies report SET or CLAIMED as signalled. The field is kept as the
  fallback.
- spec 16 C3 / C7, `event.c` `event_op`: `PULSE_EVENT` disables the cell first (state folded into
  the field, parked clients woken); `0x10001` WAKE wakes the server queue (one for auto, all for
  manual) and changes nothing; `0x10002` DISABLE_EVENT and `0x10003` DISABLE_SEMAPHORE need only
  `SYNCHRONIZE` and are answered before the other checks (other objects: success, nothing done).
- spec 16 C4, `semaphore.c` (new modified copy): with `MADEIRA_FASTSYNC_SEM`, a semaphore with
  `1 <= max <= 0x7fffffff` has a cell; release goes through the cell (limit as upstream) and then
  wakes the server queue with no limit, always (also for a release of 0, the client's "re-run
  your queue"); `signaled` takes a token and counts a claim, `satisfied` consumes it; query and
  dump read the cell.
- spec 16 C5, `thread.c` `check_wait`: when a wait-all pass fails, every entry gives back what the
  pass claimed (`ios_event_sync_unclaim`, `ios_semaphore_sync_unclaim`; other objects nothing).
- spec 16 C6, `thread.c` `thread_timeout`: once any cell exists, the objects are checked again
  before `STATUS_TIMEOUT` is reported.
- spec 16 H1, `process.c` `alloc_ptid`: freed process and thread ids are never reused.
- spec 16 H2, `async.c` (new modified copy): `free_async_queue` holds a reference across
  `async_terminate`, takes the queue's head each time, and only while the async still belongs to
  the queue unlinks it, points its node at itself, clears its queue and drops the queue's
  reference.
- spec 16, `object.h`: the declarations of the hooks above.
- spec 16 Part J: `out/16-sync-ios/tests/check-wine-files.sh` compiles `event.c`, `semaphore.c`,
  `inproc_sync.c` and `async.c` with `WINE_IOS` and these four plus `thread.c` and `process.c`
  without it, against a host Wine build.

## Spec 22 Part D (session reuse) and spec 08 clarification 12

- D1, checked: `main.c` sets the master socket timeout to infinite after the options (Part B1),
  so `process_died` never arms the shutdown timer and the loop never stops for lack of clients;
  no other path closes the master socket without a shutdown request. An ended program's process
  leaves `process_list` when its last thread is removed (upstream `remove_process_thread`); its
  threads go when its socket closes (spec 09's `finish_process` closes it, upstream's
  `kill_process` then ends every thread still listed, also those spec 22 C5 suspended for good);
  `process_sigkill` finds the pid "gone" through `ios_kill` and calls `process_died` within about
  a second. New, `process.c` `process_died`: logs `process %04x was the last program: none runs
  now, N system process(es) stay; the server waits for the next connection` when the last user
  process is gone.
- D2, checked: `ios_new_client_connection` (`request.c`), used for every injected connection,
  creates the process with no parent (`parent_id` 0, so `InheritedFromUniqueProcessId` is 0);
  new log line `client connection N (fd F): process P, no parent`.

## Device bug, build 9: a sent message never woke a thread in MsgWaitForMultipleObjectsEx

- `queue.c` (new modified copy) `set_queue_mask`: upstream drops the whole request when `poll_events` is set and the queue has no fd. The iOS driver's queues have none, and spec 12's win32u (E2) sends `poll_events = 1` whenever `ProcessEvents` delivered something before a wait, so the waiting thread kept its old masks (0 on a new queue) and a `SendMessage` to it set `QS_SENDMESSAGE` without signalling the queue: dinput's hook thread and the launcher's main thread waited for each other forever. On iOS the masks are always stored and only the fd part needs an fd; the first such request of an app run is logged (`set_queue_mask polls driver events on a queue without fd; masks ... stored`).
- `server/ios_queue_pure.h` (new, LGPL-2.1-or-later): the queue's wake rules as plain functions (`ios_queue_status`, `ios_queue_mask_stored`, `ios_queue_set_mask`, `ios_queue_set_bits`, `ios_queue_clear_bits`); `queue.c` uses `ios_queue_mask_stored`.
- `tests/test_queue_mask.c` (new, in `run-host-tests.sh`): the upstream rules reproduce the hang; the iOS rules wake the receiver in 100000 runs of the pattern (either order of mask request and send, with and without a delivering ProcessEvents), and the bits clear after the message is taken.
