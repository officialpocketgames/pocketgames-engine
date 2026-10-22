# Spec 08: on-device checks

There is no host test harness for the server as a whole. The pure rules of Part E have host
tests (below); everything else runs in the app on a device, with
the server log (`wineserver_log_set_file`, spec 02 Part A) open. Every server line starts with a
UTC timestamp and `[wineserver]`. Lines quoted below are what the code writes; `N` is a number.

## 0. Build

`build-ios.sh <wine-11.4> <host build dir> <out>` prints one `CC <file> (upstream|ios)` line per
file and ends with `AR libwineserver.a`. It fails if any file fails, if the library still
defines `main`, or if it lacks `wineserver_main`.

## 1. The app boots a program

1. Start any program (e.g. `winecfg` or a small game).
2. Look for, in this order:
   - `starting (pid=N)`, with the app's pid
   - `client connection fd N queued` (the socket pair from spec 02 Part C)
   - `ready (pid N)`. It must come before the first line from ntdll for that program; the app
     starts no Windows program before it.
3. The program's window appears. A first top-level window works without explorer.exe (F3):
   no `ACCESS_DENIED` from window creation in the ntdll log.
4. Touch, mouse and keys reach the program (F4: the station is visible and has an input
   desktop). Moving the pointer to the far corners gives cursor positions near the screen
   size, not (0,0) (F5).
5. No `FATAL:` line (the app's `fatal_error`) and the app stays alive.

## 2. A program opens a network connection

1. Run a program that connects out (a launcher that checks for updates, or `curl.exe` from a
   Windows build).
2. The connection completes and data arrives: network sockets are polled every loop
   iteration (D5).
3. Nothing about the socket in the server log. A stall here (the program waits forever on
   connect or recv) means the socket was not classified as network.

## 3. GetTickCount advances

1. Run a program that prints `GetTickCount()` (or `GetTickCount64`, `QueryInterruptTime`)
   in a loop with `Sleep(100)`, or a game that waits "N ms after X".
2. The values grow by about 100 each line (F1). A program that waits on them continues.
3. With `MADEIRA_USD_TIME=0` the values stay frozen: that confirms the switch.
4. A program that arms a thread-pool timer with a huge relative due time does not spin: the
   server's CPU use stays low (F2).

## 4. Two programs, one after the other

1. Start a program, quit it normally, start another (spec 02 session reuse).
2. For the second start look for `continuing with the state of the previous run` and a new
   `ready (pid N)` (QUESTIONS 08-1), then `client connection fd N queued` again.
3. Between the two: `main loop will stop` is **not** expected; `stopped` appears when spec 02
   stops the server thread.
4. With `MYIOSDECK_SESSION_REUSE=1`, a thread that ends itself and is still known 250 ms
   later shows
   `wineserver: XXXX: still here 250 ms after terminating itself; request pipe fd N poll=N revents=0xN; ending it`
   (G2). It may never appear; it must not appear for every thread.
5. The registry keeps changes made by the first program (`flush_registry` when the loop
   stops).

## 5. Force quit ends a hung program

1. Run a program that hangs (an infinite loop, or blocked in `WaitForSingleObject(INFINITE)`).
2. The app calls `wineserver_kill_programs(code)`.
3. Look for `force quit (exit code N): K program(s)` with K >= 1; `g_wineserver_force_quits`
   went up by one.
4. The program's process ends with that exit code (spec 02 logs the program's exit). A
   thread blocked in a server wait wakes at once; a thread asleep elsewhere depends on the
   alert hook (QUESTIONS 08-4).
5. System processes (services, winedevice) are still there: a following program starts
   without a new `starting` line.

## 6. Smaller checks

- **Sections (F6):** a program that creates a large anonymous file mapping
  (`CreateFileMapping(INVALID_HANDLE_VALUE, ..., 512 MB)`) gets either a working mapping or
  an error, never a crash on access. A short backing file logs
  `wineserver: backing file is N bytes, wanted N`. No `tmpmap-*` files appear in the app's
  working directory.
- **Images below 4 GB (F7):** a 64-bit program or DLL whose preferred base is below 4 GB
  loads, at an address above 4 GB (the ntdll loader log shows the relocation). 32-bit
  programs load their images at the preferred base inside their guest window.
- **Log (B7):** `wineserver -v`-style messages and any `fprintf(stderr, ...)` from the server
  show up in the log file with timestamps, one per line.
- **Signals (B2):** the server installs no handler for `SIGHUP`/`SIGINT`/`SIGQUIT`/`SIGTERM`
  (check with a debugger: their dispositions are whatever the app set) and its thread has
  every signal blocked (`pthread_sigmask` read on the server thread).
- **Wake-up (D3):** with a busy program, the server thread's CPU use is a few percent, and
  request round trips stay well below 1 ms (Instruments, time profiler on the server thread).

## 7. Thread suspension and contexts (Part E)

### Host tests (clarification 5)

`tests/run-host-tests.sh [build-dir]` (any C11 compiler, `CC` to choose; macOS or Linux)
builds `tests/test_ios_suspend.c` with `server/ios_suspend.c` and fake Mach calls
(`-Wall -Wextra -Werror`; passes with gcc 15 and clang 21 in WSL), then runs
`check-app-store.sh` on this part (spec 00 F). It prints `test_ios_suspend: all passed` and
`check-app-store: clean`, or one `CHECK failed` line per failure and exits 1. Covered:

- default mode: suspensions only count; each snapshot is one suspend/resume pair;
- `MADEIRA_REAL_SUSPEND=1`: the first suspension takes the hold (Mach count 1), nested ones
  only count, the last resume releases it (count 0), a resume too many makes no Mach call;
  thread and process suspension share one hold in either order;
- a snapshot never releases a hold, also when its own `thread_suspend` fails; a failed hold
  is never released; the hold flag stays 0 or 1;
- "inside a system call" (sp on the kernel stack strictly below the frame);
- the write fingerprint: same frame address and return pc/sp/fp/lr matches (volatile
  registers may differ); any of them different, or no frame at capture, doesn't;
- native registers into a frame: never x18, `restore_flags` gets CONTROL / FLOATING_POINT
  only, the dispatcher's bookkeeping untouched; the written byte ranges skip x18.

### On the device

A small test program (64-bit, then the same as a 32-bit build for WoW64): a worker thread
that loops (`for (;;) counter++;`) and one that sleeps in `Sleep(INFINITE)` or
`WaitForSingleObject`.

1. **GetThreadContext on a running thread** (both workers, `CONTEXT_FULL`): returns at once
   with a plausible `Rip`/`Eip` and `Rsp`/`Esp` (inside the program's code and stack). No
   request hangs; there is no `can't stop thread port` line in the server log. For the 32-bit
   build only the sleeping worker (inside a system call) has its x86 registers; the looping
   worker's x86 half comes back empty (clarification 9: its CPU block is stale while x86
   code runs).
2. **SuspendThread / ResumeThread (default):** counts come back as on Windows (0, 1, 2, then
   2, 1 from the resumes); the looping worker's counter keeps growing while "suspended" (it
   is only counted); the sleeping worker's wait doesn't complete while it is suspended
   (upstream's server-side rule). Nothing in the log.
3. **Real suspension:** with `MADEIRA_REAL_SUSPEND=1`, the looping worker's counter stops
   between SuspendThread and the last ResumeThread and moves again after it; nested
   suspensions need as many resumes. A `can't hold thread port` line means the hold failed
   and the thread ran on.
4. **SetThreadContext on the sleeping worker:** suspend it, GetThreadContext, change a
   callee-saved register (`Rbx` on x64, `Ebx` on x86), SetThreadContext, ResumeThread; the
   worker, after its wait returns, sees the new value. A worker suspended while *running*
   (not in a system call) gets the change dropped, with the line
   `wineserver: XXXX: SetThreadContext dropped: it was not inside a system call when its context was read`.
   A worker that left its wait before the resume logs
   `... dropped: it has left the system call it was in`. Registers from one call never show
   up in another.
5. **A typical user:** a .NET program (its GC suspends threads and reads their contexts) or
   a game with a crash handler that walks other threads' stacks runs without hangs.
6. **Thread killed while held:** with `MADEIRA_REAL_SUSPEND=1`, TerminateThread on a
   suspended thread ends it (spec 09's exit path runs), and force quit (section 5) ends a
   program whose threads are suspended.
7. **Fresh reads while suspended** (clarification 10, default mode): suspend the looping
   worker, call GetThreadContext twice a few ms apart: `Rip`/`Eip` (or the counter register)
   differ between the two reads. Then SetThreadContext and GetThreadContext again: the read
   returns the values just set, not a new capture.
8. **SetThreadContext on a running thread** (clarification 11): on the looping worker,
   without suspending it, SetThreadContext fails (`STATUS_UNSUCCESSFUL`, GetLastError 31) and
   the worker carries on unchanged. A thread setting its own context still works.

## Spec 17 Part H

Host test (`tests/run-host-tests.sh`): `sock.c`'s state mapping with Darwin's numbers, every
BSD state number; the virtual desktop's clip size (known, unknown, not positive).

On the device:

7. **TCP table.** A program that lists its TCP connections (`GetTcpTable2`) while it listens on a
   port and holds a loopback connection to it sees the listener as `LISTEN` and both ends of the
   loopback connection as `ESTAB` (spec 13 Part I2 provides the rows; spec 17 H1 the states).
8. **Desktop mode.** With desktop mode on (spec 12 Part E): the log has `wineserver: virtual
   desktop ...: visible station, clip 0,0-W,H, input desktop` with the screen size; taps reach
   the desktop. Start and close a program on it: `wineserver: desktop close timeout suppressed`
   appears once and the desktop stays (the screen keeps updating, no stale last image).

## Spec 22 Part D (session reuse)

With `MYIOSDECK_SESSION_REUSE=1` (the app's setting on), on the device:
1. End a game and wait a minute before the next: the log has `process … was the last program:
   none runs now, N system process(es) stay; the server waits for the next connection`, then no
   `main loop will stop` line; the next game's start logs `client connection 2 (fd …): process …,
   no parent` and it boots.
2. A game that started a child (launcher) and ended: no process of it is left (the next game's
   `NtQuerySystemInformation` process list in a test program shows only the system processes and
   itself).
3. In the second game, `NtQueryInformationProcess(ProcessBasicInformation)` gives
   `InheritedFromUniqueProcessId` 0 (spec 12's monitor reset runs: `[display]` / win32u's reset
   line for the new size).

## Device bug of build 9: SendMessage to a thread waiting for input

Host: `tests/test_queue_mask.c` checks the queue rules (`server/ios_queue_pure.h`) on a model:
the hang with upstream's rules, the wake-up with the app's, 100000 times in both orders. It does
not run the real server or two real threads; that needs the device.

On the device:
1. Cold app run (fresh prefix or first program after launch, so wineboot and services.exe run
   first): start the 32-bit DirectInput 8 launcher that froze in build 9 and press Play. The
   game starts.
2. The server log shows once per app run
   `set_queue_mask polls driver events on a queue without fd; masks 0x... stored (logged once)`.
   Its presence confirms the path the bug went through; its absence on a cold run with a
   window means win32u no longer sends `poll_events` there.
3. A test program whose thread creates a window and waits in
   `MsgWaitForMultipleObjects(0, NULL, FALSE, INFINITE, QS_ALLINPUT)` while another thread
   `SendMessage`s to that window right after creating it, 1000 times in a row as the first
   activity of a fresh app run: every send returns within a second; no quit-watch dump.
