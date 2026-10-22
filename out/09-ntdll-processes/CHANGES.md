# Spec 09: changes to Wine 11.4's `dlls/ntdll/unix/`

The base is `refs/wine-11.4/dlls/ntdll/unix/` (LGPL-2.1-or-later). `ntdll-unix/` holds the
modified upstream files (they keep their licence header and say that they were changed) and the
new `ios_*` files (MIT). Every change sits under `#ifdef WINE_IOS` or carries a `MYIOSDECK`
comment, so a non-iOS build of these files is upstream's. `build-ios.sh` copies the upstream
directory, puts these files on top and compiles the result.

One line per change, by Part.

## Part A: build

- `build-ios.sh`: compiles every `.c` of the merged directory for arm64, iOS 17.0, `-O2`, with
  `WINE_IOS=1` and `WINE_UNIX_LIB` (plus upstream's ntdll defines), into `libntdll_unix.a`.
  Prints `<file> OK/FAILED` and the errors of failed files. `unix/version.c` isn't built: the
  app provides `wine_build`.
- `build-ios.sh` generates a `config.h` that includes the host configure's and then
  `ios_config.h`, which turns off the preloader, process renaming (`setprogname` would rename
  the app) and a few non-Darwin features.
- No IOKit (not public API on iOS, spec 00 B1) and no headers the iOS SDK lacks (first iOS CI
  build): `ios_config.h` turns off `HAVE_SYS_VNODE_H`, `HAVE_SYS_USER_H`, `HAVE_SYS_RANDOM_H`,
  `HAVE_GETRANDOM` (random bytes come from `/dev/urandom`, upstream's fallback),
  `HAVE_SYS_DISK_H` and `HAVE_SYS_CONF_H`. `file.c` defines what it used from those two on
  iOS: Darwin's `FIODTYPE` device types `D_TAPE` 1, `D_DISK` 2, `D_TTY` 3 (so `get_device_info`
  answers as upstream on macOS) and the symlink vnode type as `IOS_VLNK` 5 (its `getattrlistat`
  directory lookup). `<mach/mach_vm.h>` is an unsupported stub in the
  iOS SDK: `virtual.c` uses `vm_map`/`vm_region_64`/`vm_deallocate` of `<mach/vm_map.h>` on iOS
  (`anon_mmap_tryfixed`, the guard-window reservation), with a maximum protection of read and
  write so those mappings can never become executable (spec 00 A1); `reserve_area`'s macOS path
  isn't compiled on iOS (`mmap_init` reserves nothing there). `cdrom.c` (copied from upstream) takes its generic branches on iOS: every
  `__APPLE__` branch also needs `!WINE_IOS` (no `<sys/disk.h>`, no IOKit, no optical drive;
  CD-ROM requests get upstream's `STATUS_NOT_SUPPORTED`). `system.c` doesn't include IOKit on
  iOS, and its battery state takes upstream's generic branch (`STATUS_NOT_IMPLEMENTED`; the
  SMBIOS path was already iOS's own, Part F2). `build-ios.sh` no longer adds IOKit headers.
- Name clashes, `ios_rename.h` (forced include): `init_startup_info` (also in win32u),
  `get_thread_context`, `set_thread_context`, `native_machine`, `supported_machines`,
  `supported_machines_count`, `server_start_time`, `user_shared_data` (also in the wineserver)
  get an `ntdll_` prefix on ntdll's side. Found by comparing the external definitions of the
  three upstream trees; CI's link of all libraries is the check.
- No exits, `ios_noexit.h` (forced include): `exit()`, `_exit()` and `abort()` become
  `ios_unix_exit()`, which ends only the calling pseudo-process (Part D), or only the calling
  thread when it has no TEB (Part E3). `abort()` exits with code 3.
- Unix libraries by name, `ios_unixlib.c` + `virtual.c`: `MemoryWineLoadUnixLib[Wow64]` takes
  the DLL name from the builtin module's export directory and `MemoryWineLoadUnixLibByName[Wow64]`
  from the given name; both look up `<name>_unix_call_funcs` or `<name>_unix_call_wow64_funcs`
  (`winemetal` → `dxmt_winemetal_…`) with `dlsym(RTLD_DEFAULT, …)`. The wow64 info classes (a
  32-bit caller) bind the 32-bit table, the others the 64-bit one. No table:
  `STATUS_DLL_NOT_FOUND`, logged once per name. `MemoryWineUnloadUnixLib` succeeds and does
  nothing. As upstream calls a library's `__wine_unix_lib_init` each time it binds it, the
  library's optional `<name>_unix_lib_init` (found with `dlsym` the same way) is called after
  binding, on the loading thread, and its status returned (win32u registers its system calls
  there, spec 12).
- Clarification 6, exported tables: `unixlibs.txt` lists the unix libraries linked into the app
  (win32u with its init, winemetal, ws2_32, dnsapi, bcrypt, secur32, crypt32, nsi,
  winegstreamer, dwrite, winecoreaudio); `write-exports.sh` turns it into the symbol names the
  loader looks up (with Mach-O's `_`), and `build-ios.sh` writes them to `<out>/exports.txt`
  for the app's `nm -gU` check (TESTS.md item 15). The host tests check the script against
  `ios_unixlib_symbol`. ntdll's own unix side defines no such table (it binds its own directly),
  so the `visibility("default"), used` declarations belong to each library's part (specs 05,
  12, 13, 14); the app's link settings belong to the app project.
- Clarification 7, futex waits with public API only (spec 00 B1): `sync.c` has an iOS branch of
  `futex_wait` / `futex_wake_one` with no `__ulock_*`. From iOS 17.4 (`__builtin_available`)
  `os_sync_wait_on_address[_with_timeout]` and `os_sync_wake_by_address_any`, as upstream's
  macOS path; below 17.4 a polling wait that re-checks the word and the (relative) timeout
  between `nanosleep`s of 50 us doubling to 1 ms, and a wake that does nothing (the poller sees
  the value). The macOS branch is upstream's again.
- Contract Section B (interfaces/wine_unix_contract.h, now on `build-ios.sh`'s include path):
  `ios_dylib.c` and `virtual.c` take `mid_pe_dylib_instance` and `mid_fxi_win_host_table` /
  `mid_fxi_wow_host_table` from it (hard dependencies, no more weak local declarations).
- `debug.c`: `wine_log_set_file(path)` (contract Section A) sends ntdll's log lines (debug
  channels, `__wine_dbg_write` from the PE side, `ios_log`) to that file, appending.

## Part B: the pseudo-process

- `ios_process.h`/`ios_process.c`: `struct ios_proc` holds every per-process global (class (c)
  below). The upstream name of each global becomes a macro for the calling thread's field
  (`#define wow_peb (ios_proc_current()->pp_wow_peb)`), so upstream code reads the right
  pseudo-process without being rewritten. The exception is `peb`, a common member and
  parameter name (server requests, `ios_vm_pure.h`, spec 11's fault code): upstream's uses read
  `ios_cur_peb()` and the one write uses `ios_set_cur_peb()` (static inline in
  `unix_private.h`; outside `WINE_IOS` they are the global `peb`).
- `ios_proc_current()`: the TEB's thread data names the thread's `ios_proc` (set by `init_teb`);
  otherwise the registry keyed by the TEB's PEB; otherwise a thread-local override (a booting
  thread before its TEB exists, the reaper); otherwise a static "no process" with neutral values.
- `ios_current_peb()` (Part B1): the calling thread's PEB, or NULL with no TEB.
- The registry (`ios_pure.c`): 64 slots, each FREE → BUSY (claimed by compare-and-swap) →
  LIVE (key and value written first, then a release store); lookups only trust LIVE slots
  (acquire) and are lock-free, so fault handlers can use them. Removal recycles the slot.
- `unix_private.h`: includes `ios_process.h` in place of the per-process `extern`s; adds
  `ios_proc`, `ios_no_interrupt` and `ios_killed` to `struct ntdll_thread_data`.
- Members with the same name as a per-process macro: the members of
  `SYSTEM_DLL_INIT_BLOCK` are read and written at the end of `loader.c`
  (`ios_fill_wow64_init_block`, `ios_wow64_thread_start`) where those macros are undefined;
  `keyed_event` (also `select_op.keyed_event`) is spelled `ios_keyed_event` at its three uses.
- Locals renamed because their names are now macros: `startup_info_size` in
  `NtCreateUserProcess` (`startup_size`), `app_key` in `open_app_key` (`key`), the parameters
  of `get_load_order_value` (`std_hkey`, `app_hkey`).
- `virtual.c`: `init_teb` records the thread's `ios_proc`; `virtual_alloc_first_teb` maps the
  shared user data page only once for the app, no longer creates `teb_key` (Part E2), and
  registers the pseudo-process under its new PEB.
- `server.c`, `sync.c`: the fd cache and the inproc-sync cache allocate their first block like
  the others (no static first block shared by everyone).
- `debug.c`: debug options are copied into each pseudo-process's PEB area and the app-wide list
  stays in malloc'd memory; a thread with no TEB logs through its own buffer and without
  process/thread ids.
- `signal_arm64.c`: `signal_init_process` sets up the calling thread for every pseudo-process
  but installs the signal handlers once for the app. (Since spec 11 this is in
  `signal_arm64_ios.c`; `signal_arm64.c` isn't compiled on iOS.)

## Part C: starting programs

- Fifth device run, x64 session (`failed to load .../aarch64-windows/ntdll.dll error 4000000e`):
  upstream looks ntdll and builtins up in `get_pe_dir(<host machine>)` and relies on ARM64X
  images in `aarch64-windows`; the app's farms are separate builds (spec 19 B4). On iOS
  `get_pe_dir` asks `ios_pe_farm_dir` (ios_pure.c) with the calling pseudo-process's main image
  machine: an x64 (ARM64EC) program's ntdll and 64-bit builtins come from `arm64ec-windows`,
  a plain ARM64 or 32-bit program's 64-bit images from `aarch64-windows`, i386 images from
  `i386-windows`. Wine's programs (wineboot.exe, services.exe) are plain ARM64 in every session
  (the ARM64EC farm has none, spec 19 B4), so they keep the aarch64 farm.
- Third device run (`FAULT signal 11` in `ntdll_wcstoumbs` during `init_startup_info`): at the
  first boot `run_wineboot` starts `C:\windows\system32\wineboot.exe`, which has no file in the
  prefix yet. `get_nt_and_unix_names` fails, leaving `nt_name` with a NULL buffer and the
  uninitialised length of NtCreateUserProcess's local; upstream starts the builtin anyway and
  never reads `nt_name` again, but `ios_spawn_child` converted it to name the child. The child
  is now named from `nt_name` only when it has a buffer, else from the image path the caller
  gave (`ios_child_image_name`, ios_pure.c); `nt_name` starts zeroed on iOS; `ios_spawn_child`
  and `ios_dylib_open` convert no NULL buffer.
- Boot lines in `build_initial_params`: environment built, wineboot's `NtCreateUserProcess`
  status, wineboot done, hardware registry written, `load_main_exe` status / module / machine.
  The boot fault line also gives lr and up to 12 return addresses from the x29 chain (followed
  only inside the faulting thread's stack, upward), each as an offset from the main image
  (`img+offset` = the binary's address minus its `__TEXT` start), with the load address and
  slide taken when the logger is installed.
- Second device run (the boot stopped after "app init: virtual memory"): `debug.c`'s
  `init_options` read `main_argv[1]` (WINEDEBUG's program filter) on the first channel lookup,
  `virtual_init`'s first `TRACE`, while no pseudo-process had arguments yet: a NULL pointer read
  with no handler. It now reads no program name then, and `__wine_main` creates the first
  program's `ios_proc` (with its argv) before the app-wide set-up, so upstream code there sees
  its per-process values as in a real process.
- Faults during the boot before spec 11's handlers exist: `ios_boot_fault_logger_install` (start
  of `__wine_main`) catches SIGSEGV, SIGBUS, SIGILL and SIGTRAP and writes
  `ntdll-ios: boot: FAULT signal N at pc X, address Y, after step: <last boot step>` (no lock,
  no allocation), then puts back the action installed before and returns, so the fault repeats
  and is handled or reported as a crash exactly as without it (spec 11 Part B1). It is removed
  before `signal_init_process` and when `__wine_main` returns.
- More boot lines in virtual memory init: the task's `TASK_VM_INFO` min and max addresses and
  the user limit; the view tables' address and size; the exe window, or why there is none; each
  guest window search (highest mapped address, range, the first and last `kern_return_t` of
  refused tries, the reserved range); the shared data page, its low window and each guest view.
- `WINE_IOS_EXE_WINDOW` is used only when `vm_region_64` shows the range is one inaccessible
  region (the app's reservation): views are later mapped there with `MAP_FIXED`, which must
  never replace what dyld or a framework mapped. Otherwise it is ignored with a log line (x64
  programs linked there take the relocation path).
- Every way out of a boot comes back to the app (after the device runs ended with no crash
  report: a quiet exit). `__wine_main` sets its own exit buffer first (`setjmp`); the thread
  that called it (`ios_is_session_thread`: the app's buffer is set for it, or it is inside
  `__wine_main`) always leaves by longjmp, to the app's `wine_ios_exit_jmpbuf` when that is set
  for the thread, else to `__wine_main`'s own buffer, which then returns. It is never ended by
  `pthread_exit` (on the app's main thread that ends the app without a trace);
  `ios_exit_route` (ios_pure.c) decides. The app-wide set-up (`__wine_main`) and the
  connected set-up (`start_main_thread`) use a non-blocking state (`ios_init_begin/end`) instead
  of `pthread_once`, which a longjmp would leave "running" for ever: a set-up that failed is
  reported on the next run instead of hanging it.
- `assert()` (spec 09 question 5): this folder's `assert.h`, found first through `-I$SRC`,
  includes the system header and redefines `assert` under `WINE_IOS` to `ios_assert_fail`,
  which logs `assertion failed: ...` and ends the pseudo-process like `abort()`; `NDEBUG` is
  honoured. Spec 16's sources (built beside these) call none of exit, `_exit`, abort or assert.
- Boot diagnostics (after the first device run died silently in `__wine_main`):
  `ios_boot_mark()` writes one `ntdll-ios: boot: ...` line per step to the log fd, at once (one
  `write`, not silenced by `MADEIRA_QUIET`). `__wine_main` starts with the program, main or
  secondary thread, its stack size and `WINESERVERSOCKET`; then the TEB key and TSD offset, the
  paths (ntdll, data, config dirs), virtual memory (host page, user space range, view tables,
  guest windows), the environment and NLS tables, and in `start_main_thread` the TEB/PEB, the
  server connection, shared data / CPU info / drives, the main image, ntdll, WoW64 ntdll and
  apisets, the signal handlers and the entry point. Upstream's `fatal_error` / `fatal_perror`
  (loader.c, server.c) and the server protocol errors also go to the log (they print to stderr).
  Two asserts that would abort the whole app now end only the boot: the view tables' mmap in
  `virtual_init` (logged, `exit(1)`: the pseudo-process exit) and `init_process_done`'s status
  (`ios_boot_failed`). `__wine_main` returns at once if the TEB key couldn't be created.
- `loader.c` `__wine_main` (iOS version): once per app, `init_paths`, the file limit,
  `virtual_init`, `init_environment` (no command-line check, no re-exec, no preloader,
  `WINELOADERNOEXEC` stays as the app set it); then a new `ios_proc` for the session's first
  program (a private copy of argv, which `rebuild_argv` rewrites), its guest window first when
  `ios_main_image_i386` is 1, and `start_main_thread()` on the calling thread. It returns only if
  the program can't start at all.
- `loader.c` `start_main_thread`: shared by the first program and every child; the app-wide
  parts (`virtual_map_user_shared_data`, `init_cpu_info`, `init_files`) run once, after the
  first connection; boot stages are recorded (`ios_proc_set_stage`) and, for the first program,
  reported through `winios_phase`: `ntdll start`, `connected`, `main image mapped`,
  `ntdll loaded`, `entry point`.
- `loader.c`: `exec_wineloader` returns `STATUS_NOT_SUPPORTED`, `start_server` does nothing;
  `posix_spawn`/`exec` paths, the re-exec and command-line code and the macOS run-loop thread
  are not compiled.
- `server.c` `server_init_process`: the first program takes its socket from `WINESERVERSOCKET`,
  a child from its parent (`boot_socket`); no `server_connect`, no lookup of a server
  directory. A missing or bad descriptor ends the boot (`ios_boot_failed`, Part D2). The
  signal set `server_block_set` is built once.
- `server.c` `server_init_process_done`: no `chdir("/")`, no `send_server_task_port` (the
  server is in this task).
- Working folder (C3), `env.c` `build_initial_params`: the current directory of the first
  program's process parameters is `MADEIRA_INITIAL_CWD` (`ios_initial_directory`), else
  upstream's (from the unix cwd). Nothing changes the unix working directory: the
  `chdir`/`fchdir` calls of `server.c` and `process.c` are gone on iOS, and `file.c`'s
  directory listing no longer `fchdir`s (Part C3, below).
- Main images (C7), `env.c`: `STATUS_IMAGE_NOT_AT_BASE` from `load_main_exe` is success; no
  "alternate loader" check; a 32-bit main image that fails to map ends its pseudo-process
  with that status instead of falling back to the 64-bit `start.exe`.
- Children (C4), `process.c` `NtCreateUserProcess`: refused spawns are checked first; the socket
  pair is made and announced to the server exactly as upstream (`new_process` with the
  server's end, `new_thread` with `request_fd = -1`); then `ios_spawn_child` copies the
  arguments (`build_argv` of the command line, a 32-bit parent's strings converted from guest
  addresses), records the main image's machine (an IL-only image without `32BITREQUIRED`,
  `IMAGE_FLAGS_ComPlusNativeReady`, is 64-bit), tracks the child (C5) and starts its boot thread
  (`ios_spawn.c`, `QOS_CLASS_USER_INTERACTIVE`, 8 MB stack, joinable), which reserves a 32-bit
  child's guest window before anything else and then runs `start_main_thread`. The parent keeps
  nothing of the child's socket end and waits for `process_info` as upstream.
- `process.c`: the image path, the handle list and the environment of a 32-bit parent are
  converted from guest addresses (`ios_host_ptr`, a no-op above 4 GB); `env.c`
  `get_params_string` does the same for the strings copied into the startup info.
- `process.c`: `__wine_unix_spawnvp` (and its wow64 thunk) and `fork_and_exec` return
  `STATUS_NOT_SUPPORTED`, logged once; no `fork`.
- Live children (C5), `ios_pure.c` + `ios_spawn.c`: every child is tracked with its file name,
  start time and whether it is a helper (`crash`, `crs-handler`, `handler`, `report`, `helper`
  in the file name, any case). `madeira_live_game_children` counts the live non-helpers no older
  than `max_age` and writes their names. A child that makes itself a Wine system process
  (`ProcessWineMakeProcessSystem`, as services.exe does) stops counting (see QUESTIONS.md).
- Refused spawns (C6), `ios_spawn.c`: `STATUS_ACCESS_DENIED` for paths containing
  `steamerrorreporter`, `gldriverquery`, `vulkandriverquery`, `steamsysinfo`,
  `hardwareupdater` or `unitycrashhandler64`, and for `conhost.exe` in an ARM64EC process
  unless `MADEIRA_EC_CONHOST=1`; the reason is logged once per name.
- `ios_mem.c`: the memory hooks spec 10 replaces. Its minimal guest window reservation: 4 GB
  aligned to 4 GB plus a guard page, `PROT_NONE`.

## Part D: ending pseudo-processes

- `ios_exit.c` `ios_thread_leave`: the one way a Windows thread leaves (`thread.c`'s
  `abort_thread`, `exit_thread` and `pthread_exit_wrapper` all end there). It releases the
  unix-side locks the thread holds, decrements the pseudo-process's thread count, closes the
  thread's request/reply/wait/alert fds, tells the memory layer (`ios_mem_thread_gone`) and
  clears the TEB key. The last thread finishes the process.
- Finishing a process (D1, `finish_process`): logs the boot stage if it died during boot (C4),
  calls `ios_mem_process_ending` (spec 10 frees the exe window before the server sees the end),
  closes the server socket (the server sees EOF), drops the fd cache closing every fd in it
  (`server.c` `ios_fd_cache_drop`), the inproc-sync cache (`sync.c`) and the directory cache
  (`file.c`), stops counting the child (C5) and removes the registry slot. After the last
  thread has cleared its TEB key, `ios_mem_release_process` hands the memory back (the guest
  window in `ios_mem.c`'s minimal version) and the struct is freed.
- `NtTerminateProcess` on the own process (`exit_process`/`abort_process`, `process.c`):
  `ios_end_pseudo_process` alerts the other threads, waits up to 2 s for them to leave
  (alerting again every 100 ms), then leaves itself, last. Threads that don't leave in time
  (busy in code that makes no system call) are logged; the process is finished without them
  and its memory is handed back when the last of them leaves.
- The session's first program (C2): the thread that called `__wine_main` never `pthread_exit`s.
  If it leaves while other threads of its program run, it waits (parked) until the program has
  ended; then it calls `wine_launched_process_did_exit(code)` (weak) and
  `longjmp(wine_ios_exit_jmpbuf, 1)` with `wine_ios_exit_code` = the Windows exit code. Exactly
  one of the parked thread and the last thread frees the struct.
- Boot failures (D2, `ios_boot_failed`): no server socket, no guest window: logged with the boot
  stage, the child's socket closed, the boot thread `pthread_exit`s (the session's first program
  returns to the app instead).
- `ios_unix_exit` (Part A's `exit`/`_exit`/`abort`): ends the calling pseudo-process; on a thread
  with no TEB it ends only the thread (E3), or reports a boot failure.
- Being killed (D3): `ios_thread_alert(unix_tid)` maps the server's unix tid (Mach port name) to
  the Windows tid through a lock-free table filled at thread start (`ios_thread_started`, in
  `server_init_process`/`server_init_thread`) and emptied at exit, then pokes the thread
  (`sync.c` `ios_alert_wake`): a `poke` word next to the thread's alert futex is set and both are
  woken. `NtWaitForAlertByThreadId` and `NtDelayExecution` (whose non-alertable sleep now waits
  on the poke word instead of `select`) handle the poke by asking the server
  (`server_select` with timeout 0): a killed thread's pipes are closed, so it leaves through
  `abort_thread`; a pending system APC runs. Nothing is allocated or locked on the server's
  thread; an unknown or gone tid does nothing. Set as spec 08's
  `wineserver_thread_alert_hook` during initialisation.
- Uninterruptible sections (D3): `server_enter/leave_uninterrupted_section` count their depth;
  a poke inside waits until the outermost section is left. `mutex_lock`/`mutex_unlock` always
  lock (upstream skips them while its one process exits, but here the locks are shared by every
  pseudo-process) and record the held locks; a thread that dies inside releases them first.
- `NtTerminateProcess` on the own process alerts the process's other threads right after the
  server killed them.
- A dead server pipe already makes upstream's request fail and the thread leave
  (`read_reply_data`, `send_request`, `wine_server_receive_fd` → `abort_thread`); that now
  goes through `ios_thread_leave`, which also covers a second failure on the way out.

## Part E: threads

- E1: `NtCreateThreadEx` (`thread.c`) and the child boot threads (`ios_spawn.c`) get
  `QOS_CLASS_USER_INTERACTIVE` through their attributes (`ios_set_thread_qos`,
  `pthread_attr_set_qos_class_np`).
- E2: `teb_key` is created once for the app (`ios_process_init_once`), with no destructor, and
  every TEB lookup goes through it (`ios_current_teb`, upstream's `NtCurrentTeb`); no raw TSD
  slot is written. `virtual_alloc_first_teb` no longer creates a key per process. A leaving
  thread clears its key (`ios_thread_leave`).
- E3: a thread with no TEB that reaches a Windows exit path (`ios_unix_exit`,
  `ios_thread_leave`) just `pthread_exit`s (the session's thread returns to the app).
- E4: `send_request` and `wine_server_send_fd` call `wineserver_request_wake()` (spec 08) after
  every request or descriptor they write.
- E5: a leaving thread closes its request, reply, wait and alert fds
  (`ios_close_thread_fds`), calls `ios_mem_thread_gone` and clears its TEB key. Upstream's
  deferred TEB release (a leaving thread joins the one before it and frees its TEB) is kept, per
  pseudo-process (`pp_prev_teb`), without the session's thread and without the last thread.
- E6: `signal_arm64.c` `ios_cpu_area_sync`, called by `__wine_syscall_dispatcher` after it has
  saved the frame (with the incoming x0–x8) and on its return path, writes an ARM64EC thread's
  registers into the x64 context of its CPU area (`ChpeV2CpuAreaInfo->ContextAmd64`) with the
  ARM64EC register mapping, unless the emulator is running (`InSimulation`). A WoW64 thread's
  i386 context is written by its CPU module when it leaves guest code; nothing is added for it.
  The asm additions are string macros (`IOS_SYSCALL_ENTRY_SYNC`, `IOS_SYSCALL_RETURN_SYNC`),
  empty without `WINE_IOS`; spec 11 ports these dispatchers. (Since spec 11 all of this is in
  `signal_arm64_ios.c`.)
- A thread's unix tid toward the server stays upstream's `get_unix_tid()`, its Mach thread port
  name (spec 08 Part C4).

## Part F: devices and machine identity

- F1, `ios_hwident.c` `ios_create_dos_devices` (called by `start_main_thread` for the session's
  first program, once connected): creates `<prefix>/dosdevices` and `<prefix>/drive_c` if
  missing, the links `dosdevices/c:` → `../drive_c` and `dosdevices/z:` → `/` if missing (by
  absolute path, no `chdir`), and the permanent NT symbolic links `\DosDevices\C:` →
  `\Device\HarddiskVolume1` and `\DosDevices\Z:` → `\Device\HarddiskVolume2` (upper-case
  letters: `GetLogicalDrives` derives the drive bit from them). An existing link is kept.
  Then (clarification 1) spec 15's weak `ios_hidpad_publish()` is called if it is linked, still
  before the program's code runs.
- F2, the identity: one table, `ios_hw_identity` in `ios_hwident.c` (an AMD Ryzen 7 5800X
  desktop on an AMI BIOS and an ASRock B550 board, a fixed UUID).
- F2, registry (`ios_write_hardware_registry`, from `env.c` `build_initial_params` after
  wineboot has run): `HARDWARE\DESCRIPTION\System` (Identifier, SystemBiosVersion,
  SystemBiosDate), `...\System\BIOS` (BIOS, system and baseboard strings) and
  `...\System\CentralProcessor\N` for each core (Identifier, VendorIdentifier,
  ProcessorNameString, `~MHz`). Each missing level is created on its own, volatile like
  Windows' HARDWARE tree.
- F2, clock: `~MHz` comes from `CNTFRQ_EL0`: its MHz doubled until it is at least 2000, at
  most 5000 (24 MHz → 3072 MHz) (`ios_plausible_mhz`).
- F2, SMBIOS (`system.c`): an iOS `create_smbios_data` builds the `RSMB` table from the identity
  (BIOS, system, chassis, baseboard, processor, boot info) with upstream's SMBIOS helpers;
  `create_smbios_processors` uses the identity's processor name, vendor and family (the arm64
  core-id records upstream adds stay).
- `system.c` `init_cpu_info`: the processor count in the PEB and the process cookie are set for
  every pseudo-process; the machine model is read once.

### Audit of the globals and statics of `dlls/ntdll/unix/`

(a) truly app-wide, (b) per thread already, (c) per process → now per pseudo-process.
"spec 10" or "spec 11" marks items whose behaviour those specs own; this spec classifies them.

| File | Global or static | Class | Notes |
|---|---|---|---|
| cdrom.c | `cdrom_cache`, `cache_mutex` | a | keyed by device |
| cdrom.c | `buffer` in a debug helper | a | debug string |
| debug.c | `init_done`, `initial_info`, `default_flags`, `nb_debug_options`, `options_size`, `debug_options` | a | one WINEDEBUG, one log; per-process copy in each PEB |
| debug.c | `log_fd` (new) | a | the log file |
| debug.c | `thread_info` (new) | b | thread-local buffer for threads with no TEB |
| env.c | `peb`, `wow_peb`, `startup_info_size`, `is_prefix_bootstrap`, `main_argc`, `main_argv`, `main_wargv` | c | |
| env.c | `uctable`, `lctable`, `unix_cp`, `nfc_table`, `user_lcid`, `system_lcid`, `user_ui_language`, `nt_build_dir`, `nt_data_dir`, `system_locale`, `user_locale` | a | one config, one NLS table |
| env.c | `system_dir`, `bootstrapW` | a | constants |
| file.c | `dir_data_cache`, `dir_data_cache_size` | c | indexed by the server's per-process cache entry |
| file.c | `ignored_files`, `ignored_files_count`, `show_dot_files`, `start_umask`, `dir_mutex`, `mnt_mutex`, `fs_cache_mutex`, drive `cache`/`last_update`/`nb_drives` (in `get_dos_device`), `fileio_freelist`, `warned`/`once` flags | a | one file system and config |
| loader.c | `main_image_info` | c | |
| loader.c | `pDbgUiRemoteBreakin`, `pKiRaiseUserExceptionDispatcher`, `pKiUserExceptionDispatcher`, `pKiUserApcDispatcher`, `pKiUserCallbackDispatcher`, `pKiUserEmulationDispatcher`, `pLdrInitializeThunk`, `pRtlUserThreadStart`, `p__wine_ctrl_routine`, `pLdrSystemDllInitBlock` | c | each pseudo-process maps its own PE ntdll |
| loader.c | `bin_dir`, `dll_dir`, `ntdll_dir`, `alt_build_dir`, `dll_path_maxlen`, `home_dir`, `data_dir`, `build_dir`, `config_dir`, `wineloader`, `dll_paths`, `system_dll_paths`, `user_name` | a | one prefix, one bundle |
| loader.c | `started` (start_server) | a | never used on iOS |
| loadorder.c | `std_key`, `app_key`, `init_done`, `main_exe_loaded` | c | handles, app name |
| loadorder.c | `env_list` | a | WINEDLLOVERRIDES |
| process.c | `execute_flags`, `process_error_mode`, `process_cookie` | c | |
| server.c | `fd_socket`, `process_exiting`, `initial_cwd`, `fd_cache` | c | |
| server.c | `supported_machines[_count]`, `native_machine`, `server_start_time`, `server_dir`, `server_pid` | a | one server |
| server.c | `server_block_set`, `fd_cache_mutex` | a | the mutex guards each process's cache; one lock is enough |
| server.c | `have_pipe2` | a | |
| signal_arm64.c (since spec 11: signal_arm64_ios.c) | the signal handlers (installed once) | a | spec 11 |
| sync.c | `keyed_event`, `inproc_device_fd`, `inproc_sync_cache` | c | handle, per-process device |
| sync.c | `tid_alert_blocks` | a | indexed by thread id, unique app-wide |
| sync.c | `timebase` (monotonic_counter) | a | |
| syscall.c | `syscall_names`, `usercall_names` | a | |
| system.c | `cpu_name`, `cpu_vendor`, `cpu_level`, `cpu_revision`, `cpu_id`, `cpu_features_bitmap`, `performance_cores[_capacity]`, `logical_proc_info*`, `system_cpu_mask`, `tsc_from_jiffies`, `logical_proc_init_once`, `smbios_data`, `timezone_mutex`, `cached_tzi`, `current_year`, `current_bias`, `idle`, `current` (execution state), once flags | a | one machine |
| thread.c | `nb_threads` | c | the last thread ends the pseudo-process |
| thread.c | `prev_teb` (exit_thread) | c | a TEB is freed by a later thread of the same process (Part D) |
| thread.c | `teb_key` | a | created once (Part E2) |
| virtual.c | `teb_block`, `next_free_teb`, `teb_block_pos`, `teb_list` | c | a pseudo-process's TEBs and PEB |
| virtual.c | `user_space_wow_limit`, `arm64ec_view` | c | depend on the main image; spec 10 |
| virtual.c | `force_exec_prot`, `enable_write_exceptions` | c | process settings; spec 10 owns which views they touch |
| virtual.c | `reserved_areas`, `builtin_modules`, `views_tree`, `virtual_mutex`, `host_page_size/mask`, `address_space_start/limit`, `working_set_limit`, `host_addr_space_limit`, `pages_vprot*`, `use_kernel_writewatch`, `preload_reserve_start/end`, `free_ranges*`, `pagemap_fd`, `user_shared_data`, once flags | a | one address space; spec 10 |
| signal_i386.c, signal_x86_64.c, signal_arm.c | all | n/a | not compiled for arm64 (the files are empty there) |
| registry.c, security.c, serial.c, socket.c, tape.c | `once` flags only | a | |
| ios_pure.c (new) | `children`, `children_mutex`, `next_child_id` | a | the session's children (Part C5) |
| ios_process.c (new) | the registry, `no_proc`, `next_serial`, `ios_teb_key_ready`, `ios_quiet` | a | |
| ios_process.c (new) | `thread_proc` | b | thread-local override |
| ios_unixlib.c (new) | `logged_names` | a | log-once list |

# Spec 10: memory on iOS

Same tree, same rules (`WINE_IOS`, `MYIOSDECK` comments). Each entry is tagged "spec 10" and
the Part. The iOS code is mostly one block at the end of `virtual.c` (it needs the file's views,
reserved areas and `virtual_mutex`); the pure rules are in `ios_vm_pure.c` (host-tested), the
dylib images in `ios_dylib.c`. Spec 09's placeholder `ios_mem.c` is gone: its four hooks are
now in `virtual.c`.

## Part A: the address space (spec 10)

- spec 10 A1, `virtual.c`: `address_space_start` is 4 GB + 64 KB. `mmap_init`'s reservations
  of Windows' fixed ranges do nothing on iOS: the low 4 GB is `__PAGEZERO` and the high ranges
  are beyond what the device maps.
- spec 10 A1, `map_view` → `ios_vm_translate`: a 64-bit process's request with a high limit
  below 4 GB, or for a fixed address below 4 GB, fails (`STATUS_NO_MEMORY`,
  `STATUS_CONFLICTING_ADDRESSES`), as upstream fails an impossible request.
- spec 10 A2, `virtual_init` → `ios_vm_init_limits`: `address_space_limit`, `user_space_limit`,
  `working_set_limit` and `host_addr_space_limit` are `task_info(TASK_VM_INFO).max_address`
  rounded down to 64 KB (`MACH_VM_MAX_ADDRESS` if unknown), never above `0x7fffffff0000`
  (`ios_vm_user_limit`). `virtual_set_large_address_space` keeps them for 64-bit programs.
- spec 10 A3: upstream's host-page handling is kept as it is (`host_page_size` from
  `sysconf`, protection changes in host pages with the most permissive 4 KB page, per-4 KB
  `pages_vprot`).

## Part B: no executable memory (spec 10)

- spec 10 B, `mprotect_exec` → `ios_vm_mprotect`: the range is split by place
  (`ios_dylib_find`, the next image start, the guest windows) and each piece gets
  `ios_vm_exec_prot`: signed code (below an image's data) is a successful no-op, signed data
  and other memory lose `PROT_EXEC`, a guest window's executable request becomes the same
  without `PROT_EXEC` (execute alone → read). The first 64 requests stripped outside images and
  windows are logged (`no executable memory on iOS: <range> prot <x> given without PROT_EXEC`).
  `force_exec_prot` no longer makes anything executable.
- spec 10 B: `map_view` already maps without `PROT_EXEC` (upstream); image views get their
  section protections through `set_vprot` → `mprotect_range` → the same rule.
- spec 10 clarification 10, a process never gets a second instance of a DLL it has loaded (a
  Godot/.NET 9 game: `[pe-instance] …/libadvapi32.dll.dylib: new instance`, the same for kernel32
  and ntdll in process 1, then `0xC0000005 at System.Environment.GetOSVersion()`). The dylib
  views carry `VPROT_SYSTEM`, so `NtAreMappedFilesTheSame` never called two of them the same
  file and the PE loader's `find_existing_module` couldn't catch a second mapping that its name
  matching had missed. Now `virtual_map_image`, before Part C's dylib mapping of a DLL at offset
  0, looks for the calling process's view of the same dylib (`ios_vm_own_dylib_view`: the
  dylib's instances with a view from `ios_dylib_instances` in ios_dylib.c, then the pure rule
  `ios_dylib_view_reusable`: the view starts at the instance's image, is a dylib image view and
  is owned by the calling pseudo-process). If found, `ios_vm_reuse_view` returns its base and
  size with `STATUS_SUCCESS` and one more reference on its builtin module entry (created if
  the first mapping had none), as upstream's `get_builtin_so_handle` does for a loaded .so
  builtin. Nothing is mapped, no instance is made, no server request is sent, no relocation
  happens. `unmap_view_of_section` already drops only a reference while more than one is left.
  An ended process's image views go with all their references (`ios_vm_release_image`). One line
  per DLL name per process: `[nojit] <dll>: second mapping in process <n> (<name asked for>);
  the loaded copy at <base> is used` (`proc->reused_images`). `ios_dylib_exists` and the new
  lookup share `dylib_path_of`.
- spec 10 clarification 9a (public-20: every launch crashed; the CPU modules xtajit.dll and
  xtajit64.dll, copied into system32 without a Wine marker, were mapped from their files):
  `ios_image_takes_dylib` first gives native ARM code its dylib whatever its marker or path:
  machine 0xAA64, or `is_hybrid` (the server sets it for any image whose load config has CHPE
  metadata: ARM64X, and x64-machine ARM64EC builds). The dylib lookup then decides whether
  there is one. Only images without native ARM code (plain x64, i386) follow clarification 9's
  marker rule.
- iOS build fix for dcf321b (CI: `call to undeclared function 'ios_dylib_exists'` in
  virtual.c): `virtual.c` includes `ios_dylib.h` with its other iOS includes at the top, not only
  in the iOS block at its end; the ARM64EC range update in the dylib mapping is under
  `__aarch64__` like upstream's own call (always true on iOS). `tests/host-syntax-check.sh` now
  compiles like the iOS build: `-Werror=implicit-function-declaration`, `_GNU_SOURCE` for every
  file, spec 16's include folder, and `tests/darwin-shim/` (stand-ins for the Mach, QoS and
  `F_GETPATH` declarations, from Apple's public headers' names) so `virtual.c`, `ios_spawn.c`
  and the other iOS files are checked through to their end.
- spec 10 clarification 9, a native DLL with a builtin's name is mapped from its own file
  (device run public-18: Microsoft's D3DCompiler_47.dll in a game's folder was swapped for our
  dylib, `image of 47f000 bytes doesn't fit its dylib`, c000007b): in `virtual_map_image` the
  signed dylib replaces an image only when it is ours (`ios_image_takes_dylib`, ios_vm_pure.c):
  the server found Wine's builtin marker in the file, or the unix loader itself loads it as a
  builtin from the bundle's farms (`is_builtin`). Any other file, wherever it lies (a game's own
  copy, or an unmarked file a user put in system32), is mapped from its file under Part B's
  rules; sizes never decide. When such a file shadows one of our dylibs (`ios_dylib_exists`)
  it logs once per DLL name per process: `[nojit] <name>: a native copy (<path>), mapped from
  its file`. The spec's "or its path is inside the system directories" is read as the unix
  loader's own builtins: an unmarked file in system32 is the clarification's fourth test case,
  mapped from its file, and every DLL of our farms carries the marker.
- spec 10 clarification 8, a DLL image of the wrong 64-bit machine is refused (device run: an
  ARM64 program after an x64 one in the same session loaded system32's ARM64EC kernel32 and
  died with 0xC000001D): `virtual_map_image`, which every image mapping goes through (the PE
  loader's sections, the unix side's builtins and the signed-dylib path with its instances),
  checks a DLL's machine against the process before anything is mapped (`ios_image_fits_process`,
  ios_vm_pure.c): a plain ARM64 process takes only 0xAA64 images, an x64 process 0x8664 images
  and hybrid (ARM64X) 0xAA64 ones; WoW64 processes and a not-yet-known main image keep
  upstream's rules. A mismatch returns `STATUS_INVALID_IMAGE_FORMAT`, so spec 18's `sysaa64`
  fallback applies, and logs once per DLL name per process: `[nojit] <dll>: machine <m> doesn't
  fit this <arm64|x64> process; refused`.
- spec 10 E4, a 32-bit process's window holds only guest requests (public-4 device run: a black
  screen; `NtGetTickCount` faulted at B + 0x320 from every `NtUserPeekMessage`): the shared
  data page, allocated with no limit in `virtual_alloc_first_teb` after the boot thread took its
  window, was placed at the window base B, the program's NULL page. `ios_vm_area_for_others` now
  gives the owner's window only to a request whose whole range lies inside it
  (`ios_win_request_in_window`, ios_vm_pure.c), i.e. a guest request translated by
  `ios_vm_translate`. Unix-side requests with no guest limit (the shared data page, kernel
  stacks, the unix side's own buffers) go outside the window, where 32-bit code can neither
  see nor overwrite them. TEB blocks (a guest 2 GB limit) and win32u's guest-visible tables (a
  `zero_bits` limit) still go inside.
- spec 10 E4, builtins named by guest address (fifth device run: the 32-bit winemetal.dll's
  unix-call init got `STATUS_DLL_NOT_FOUND`): builtins are registered by host address
  (`add_builtin_module(view->base)`), but WoW64 passes a 32-bit DLL's base as its guest
  address. `get_builtin_module` now matches either (`ios_module_match`, ios_vm_pure.c: the same
  address, or the caller's window base + a guest address below 4 GB), so the unix-library
  lookup, `get_builtin_so_handle` and `set_builtin_unixlib_name` accept a 32-bit process's guest
  module address; `load_builtin_unixlib` reads the export name at the registered host address.
  A module that isn't a registered builtin (for example a DLL without Wine's builtin mark) logs
  `unixlib: module ... is not a registered builtin` before `STATUS_DLL_NOT_FOUND`.
- spec 10 clarification 7, shared image sections (fourth device run: a 32-bit GameMaker game's
  writable shared `.mydata`, 4 KB aligned, refused the image with
  `Could not map ... shared section .mydata`): in `map_image_into_view` a writable shared section
  keeps upstream's mapping from the server's shared file only when it starts and ends on host
  pages (`ios_shared_section_shareable`, ios_vm_pure.c) and that mapping succeeds. Otherwise it
  goes through the ordinary section path (the file's data copied, zero-filled past the raw size,
  the section's own protections), the shared file's offset still advances, the image loads, and
  one line per image is logged: `[nojit] <image>: shared section <name> mapped privately (16 KB
  pages)`. The section is then private to that pseudo-process.
- spec 10 clarification 6, `NtFlushInstructionCache`: on iOS Apple's public
  `sys_icache_invalidate` (`<libkern/OSCacheControl.h>`) for the own process, in place of
  `__clear_cache` (the iOS link has none). It only touches memory that is already executable
  (signed images) and never makes anything executable.

## Part C: images from signed dylibs (spec 10)

- spec 10 C1/C2, `ios_dylib.c` `ios_dylib_open`: folder by machine (`ios_dylib_dir`: AMD64 from
  `MYIOSDECK_PE_DIR`, ARM64 from `MYIOSDECK_PE_DIR_A64`, i386 never), path
  `<dir>/lib<lower-case file name>.dylib` (`ios_dylib_path`); no variable or no file: the
  upstream mapping. First use: `dlopen(RTLD_NOW | RTLD_LOCAL)`, the four symbols, the TEB offset
  stored in `myiosdeck_teb_offset`. A dylib missing a symbol (or whose symbols aren't in the
  order image ≤ data ≤ end) isn't used.
- spec 10 C3, `virtual_map_image` → `ios_vm_map_dylib_image` (under `virtual_mutex`): a view at
  the dylib's image with `SEC_IMAGE | SEC_FILE | VPROT_SYSTEM`, committed, read, write-copy,
  exec; headers read-only; each section from its characteristics. A `map_size` beyond
  `myiosdeck_pe_image_end` fails with `STATUS_INVALID_IMAGE_FORMAT`. ARM64EC images: code ranges
  and the redirected entry point from the load config (`update_arm64ec_ranges`), on the
  unrelocated image. No relocation: the server is told the view's base, the loader relocates
  once. `VPROT_SYSTEM` keeps `delete_view` from unmapping signed pages.
- spec 10 C4: a DLL whose first instance already has a view gets `mid_pe_dylib_instance`
  (contract Section B, a hard dependency); failure logs the app's error and fails the mapping
  with `STATUS_NO_MEMORY`. The instance is registered like a first image.
- spec 10 C5, `ios_vm_image_mapped`: after `xtajit64.dll` (ARM64EC) / `xtajit.dll` (ARM64) is
  mapped from a dylib, `mid_fxi_win_host_table` / `mid_fxi_wow_host_table(ios_teb_tls_slot_offset)`
  is stored into the slot named by the export `MyiosdeckFxiHost` / `MyiosdeckWowHost`, only if
  the slot lies in `[data, end)` (else a trace line, nothing stored).
- spec 10 C6, `ios_process.c`: `ios_teb_tls_slot_offset = teb_key * 8`, set when the TEB key is
  made, before any image is mapped (Darwin's `pthread_getspecific` reads TSD slot `key`).
- spec 10 C7, `ios_dylib.c`: 4096 entries, append-only, published with a release store of the
  count, read lock-free (`ios_dylib_find`, `ios_dylib_next_start`); never freed.
- spec 10 C8, `ios_dylib_trace`: `nojit: ` lines to stderr and appended to
  `$MYIOSDECK_NOJIT_TRACE` (opened on first use).

## Part D: the exe window (spec 10)

- spec 10 D, `ios_vm_init_windows`: `WINE_IOS_EXE_WINDOW=<hex>:<hex>` (`ios_exewin_parse`) is
  added as a reserved area; not host-page aligned → ignored with a log line.
- spec 10 D1/D2, `map_image_view` → `ios_vm_exe_window_claim`: a 64-bit main image (not a DLL,
  not i386, not in a guest window) whose base and host-page-rounded size fit claims the window
  before `map_view` maps it at its base (over the reservation); a failed map un-claims.
  `ios_vm_image_mapped` confirms the claim with the view's size (`ios_exewin_confirm`) or
  retires it.
- spec 10 D, `ios_vm_translate` (`fixed_forbidden`) and `ios_vm_area_for_others`: nobody maps
  at a fixed address in the exe window but its occupant inside its claim, and no general
  allocation is placed in it.
- spec 10 D3, `ios_mem_process_ending` (spec 09 D1, before the socket closes): the occupant's
  views in the claim are deleted (back to `PROT_NONE` in the reserved area) and the claim
  retired.
- spec 10 D4: an occupied window makes the second program's fixed map fail with
  `STATUS_CONFLICTING_ADDRESSES`, so `map_image_view` takes upstream's relocation path.

## Part E: guest windows (spec 10)

- spec 10 E1, `ios_vm_init_windows`: with `ios_main_image_i386`, a window is reserved during
  `virtual_init` as a FREE slot.
- spec 10 E2, `win_reserve`: `B` is 4 GB aligned above the highest region the app has mapped
  (`mach_vm_region`, ignoring the top 256 MB) and above earlier windows; 4 GB + one host page
  (guard) are reserved with `mach_vm_map(VM_FLAGS_FIXED, VM_PROT_NONE)`; the 4 GB become a
  reserved area. If nothing fits above, the highest free 4 GB aligned range below is used. Slot
  table: `ios_win_*` in `ios_vm_pure.c`, 8 slots, states empty/free/reserved/bound/released/
  abandoned, all changes under `virtual_mutex`. No slot: the boot fails with
  `STATUS_NO_MEMORY` and `no guest window for a 32-bit process`.
- spec 10 E3: `ios_mem_reserve_wow_window` (spec 09 C4, boot thread) takes a slot and sets
  `wow_base`, `wow_size`, `wow_slot`; `virtual_alloc_first_teb` binds it to the PEB
  (`ios_vm_bind_window`) right after the PEB exists. `ios_wow_base()` is the calling thread's
  process's `wow_base`; `ios_wow_base_for_peb(peb)` the bound window of a PEB;
  `NtQueryInformationProcess(1010)` returns `B` (own process) or `ios_wow_base_for_peb` of the
  other process's PEB, 0 for a 64-bit one.
- spec 10 E4: guest addresses passed to the Nt* memory calls (allocate, free, protect, query,
  map and unmap views, lock, unlock, flush, read and write own memory) become `B + a`
  (`ios_host_ptr`). In `map_view`, `ios_vm_translate` turns a guest high limit into
  `[B + max(low, 64 KB), B + min(high, ceiling − 1)]` (`ios_vm_translate_limits`); a 32-bit
  image is mapped at `B + ImageBase` or elsewhere in the window, never outside; the TEB block
  and PEB of a 32-bit process go below `B + 2 GB`. Generic allocations never land in another
  process's window or a released one (`ios_vm_area_for_others` in `map_reserved_area`).
- spec 10 E4: the 32-bit view of the shared data page: when the window is taken a
  `VPROT_SYSTEM` view holds `B + 0x7ffe0000`; once the real page exists
  (`virtual_map_user_shared_data` → `ios_vm_usd_mapped`, which keeps a dup of the section fd)
  the same page is mapped read-only there, for this and every later window.
- spec 10 E5, `virtual_set_large_address_space`: `user_space_wow_limit` is
  `ios_wow_ceiling − 1` (4 GB unless no `IMAGE_FILE_LARGE_ADDRESS_AWARE` and `MADEIRA_LAA=0`);
  `wow_lowfirst` (no flag, 4 GB, `MADEIRA_LAA_LOWFIRST` not `0`) makes `map_view` try
  `[low, B + 2 GB − 1]` first for requests without a fixed address.
- spec 10 E6 / clarification 2: `ios_wow_fixup_peb64_ptrs()` adds `B` to non-zero values below
  4 GB in the PEB64's `AnsiCodePageData`, `OemCodePageData` and `UnicodeCaseTableData`
  (idempotent; nothing for a 64-bit process). ntdll's unix side itself doesn't read those
  fields, so nothing here calls it; win32u does (spec 12 B6).
- spec 10 E7: `ios_mem_release_process` releases the slot (stops resolving at once).
  `ios_win_take` tears a released window down only when the spec 11 thread registry
  (`ios_thread_registry_range_busy`, weak) shows no thread in it: before taking
  `virtual_mutex`, threads left in released windows are alerted (`ios_thread_alert`) and waited
  for up to 0.5 s; one still there abandons the window. Teardown deletes every view inside
  (releasing builtin modules) and maps the whole window `PROT_NONE` again. Each teardown and
  abandon is logged.
- spec 10 E8, `virtual_set_large_address_space`: a process with a window that isn't WoW64
  (`ios_vm_window_abandon`) abandons it; it keeps living there.
- spec 10 E9: `ios_wow_in_window(addr)`, `ios_wow_guest_addr(host)`.

## Part F: threads and the memory layer (spec 10)

- spec 10 F, `struct file_view` has an `owner` (the creating pseudo-process's serial,
  `ios_vm_set_owner` in `create_view`; the app's views and the shared data page: NULL).
- spec 10 F, `ios_mem_release_process` (spec 09 D1): low windows of the PEB removed, the
  window slot released, then every view the process owns outside its window deleted (images
  release their builtin module; dylib views keep their signed pages, `VPROT_SYSTEM`), except a
  view the calling thread's TEB or stack lies in, which is deferred. With threads left nothing
  is deleted.
- spec 10 F, `ios_mem_thread_gone` (spec 09 E5) and every later reservation: deferred views
  whose thread has had a second to leave are deleted. A thread's own TEB and stacks are freed by
  upstream's path (`virtual_free_teb` by the next leaving thread) or with its process.

## Clarification 1: low windows (spec 10)

- spec 10 cl. 1, `user_shared_data` starts NULL: the real page is allocated anywhere above
  4 GB; `ios_get_real_usd()` returns it (0 before). `ios_vm_usd_mapped` registers the low
  window `0x7ffe0000 → real page`, owner NULL.
- spec 10 cl. 1, `ios_vm_image_mapped`: a 64-bit image based below 4 GB, mapped above, gets a
  low window `ImageBase → view` owned by its PEB. If its relocations are stripped and it has no
  dynamic base, its header's `ImageBase` is rewritten to the view's address first (so the loader
  doesn't try to relocate it) and the window alone serves its absolute addresses.
- spec 10 cl. 1, `ios_vm_pure.c`: 64 slots, each `{low, size, real, owner}` changed between two
  increments of a sequence number, so lock-free readers never see a half-written or reused
  slot. `ios_lowwin_translate(addr, peb)`: the PEB's own window, else an owner-NULL one; no PEB:
  any match; 0 if none. A process's windows go in `ios_mem_release_process`.

# Spec 11: exceptions, signals and system calls on iOS

Same tree, same rules. Each entry is tagged "spec 11" and the Part. Upstream's
`signal_arm64.c` is no longer compiled on iOS (`#if defined(__aarch64__) && !defined(WINE_IOS)`,
otherwise upstream's text); `signal_arm64_ios.c` is its Darwin port (LGPL, upstream's header
plus a list of the changes) and also holds spec 09's iOS additions that were in
`signal_arm64.c` (`signal_init_process` once per app, `ios_cpu_area_sync`). The pure rules are in
`ios_sig_pure.c` (host-tested), the frame layout in `ios_syscall_frame.h`.

## Part A: the TEB and the system-call boundary (spec 11)

- spec 11 A1: the unix side's `NtCurrentTeb()` already reads spec 09's key (`thread.c`,
  `pthread_getspecific`). The assembly reads the same slot: `IOS_ASM_LOAD_TEB` = `TPIDRRO_EL0`
  (low three bits cleared) + `ios_teb_tls_slot_offset` (an `adrp`/`@PAGE` load). Nothing reads,
  saves or restores x18 anywhere in the file.
- spec 11 A2, `ios_syscall_frame.h`: upstream's `struct syscall_frame` (0x330 bytes), the
  offsets the assembly uses, and the TEB offsets (`syscall_table` 0x370, `syscall_frame` 0x378,
  `syscall_trace` 0x380, `kernel_stack` 0x3a8, `Peb` 0x60), with `_Static_assert`s; the TEB
  offsets are checked against `struct ntdll_thread_data` in `signal_arm64_ios.c`. Spec 08 Part E
  reads the same numbers.
- spec 11 A2, `__wine_syscall_dispatcher`: upstream's, in Darwin syntax (`_` prefix, `L` local
  labels, through `wine/asm.h`); the TEB comes from TSD into x11 and then lives in x24 (saved in
  the frame at entry, restored on return) where upstream uses x18; the frame's x18 slot is
  neither written nor read. Spec 09's `IOS_SYSCALL_ENTRY_SYNC`/`IOS_SYSCALL_RETURN_SYNC` are
  kept at the same places.
- spec 11 A2, `__wine_syscall_dispatcher_return` (entered by `handle_syscall_fault` with sp = the
  frame) and `__wine_unix_call_dispatcher`: the same changes.
- spec 11 A2/B5, the full-context return (`syscall_dispatcher_return_slowpath`, taken when
  `CONTEXT_INTEGER` is restored and x16/x17 don't match pc/sp): upstream raises `SIGUSR2`, which
  has no handler on iOS. The thread marks itself (`full_restore`) and sends itself `SIGTRAP`; the
  SIGTRAP handler loads the frame into the signal context (Part B). The floating-point registers
  are loaded only when `CONTEXT_FLOATING_POINT` is restored (upstream's SIGUSR2 handler loads them
  always, also after a unix call whose frame holds only v8-v15).
- spec 11 A3, `init_syscall_frame` (on the new thread, before any Windows code):
  `ios_signal_thread_start` gives the thread its alternate signal stack and registers it (Parts B
  and D). `frame->x[18]` is still set to the TEB, but only to report it. The WoW64 start address
  is spec 09's `ios_wow64_thread_start()`.
- spec 11 A4, `call_user_mode_callback` / `user_mode_callback_return`: the TEB stays in x4
  (upstream copies it into x18 before entering user mode). Fix: upstream saves FPSR into the low
  half of the FPCR/FPSR pair (`bfi x1, x2, #0, #32`) but restores the low half into FPCR; here
  FPCR is saved low and FPSR high (`bfi x1, x2, #32, #32`), and only the 32-bit halves are
  written back.
- spec 11 A4, `KeUserModeCallback`, `NtCallbackReturn`, `NtContinue` (through
  `signal_set_full_context`), `call_user_apc_dispatcher`, `call_user_exception_dispatcher`:
  upstream's.
- spec 11 A5, `save_context` / `restore_context`: `ios_ctx_from_darwin` / `ios_ctx_to_darwin`
  (`ios_sig_pure.c`): x0-x28, fp, lr, sp, pc, cpsr from `__ss`, v0-v31, fpsr, fpcr from `__ns`;
  `X18` reads as the TEB; x18 is never written back. The mirrors of `arm_thread_state64_t`,
  `arm_neon_state64_t` and the ARM64 `CONTEXT` are checked field by field with `C_ASSERT`.
- spec 11 A5, `NtSetContextThread` on the own thread ignores `CONTEXT_ARM64_X18`;
  `NtGetContextThread` reports the TEB as X18. Another thread's context goes to the server
  (`get_thread_context`/`set_thread_context`, spec 08 Part E), as upstream. ARM64EC and WoW64
  contexts (`get_native_context`, `get_wow_context`, `get/set_thread_wow64_context`, the CPU
  area) are upstream's.
- spec 11: the thread data's `cpu_data` (unused by upstream's arm64 code) holds
  `struct arm64_thread_data`: the alternate stack, the storm counter (Part B4), the registered
  Mach port (Part D) and the `full_restore` flag.

## Part B: signals deliver Windows exceptions (spec 11)

- spec 11 B, `signal_init_process`: handlers for `SIGSEGV`, `SIGBUS`, `SIGILL`, `SIGTRAP`,
  `SIGFPE` and `SIGABRT` with `SA_SIGINFO | SA_RESTART | SA_ONSTACK` and upstream's mask
  (`server_block_set`), installed once for the app; the app's previous actions are kept
  (`prev_actions`). The per-thread part (syscall frame, alternate stack) runs for every
  pseudo-process's first thread.
- spec 11 B, alternate stacks: `signal_alloc_thread` maps 128 KB plus a guard page
  (`anon_mmap_alloc`) and keeps it in the thread data; the thread turns it on with `sigaltstack`
  in `signal_init_process` and at thread start (`ios_signal_thread_start`, from
  `init_syscall_frame`); `ios_thread_leave` (spec 09, `ios_exit.c`) turns it off through
  `ios_signal_thread_leave`; `signal_free_thread` unmaps it with the TEB.
- spec 11 B1, `leave_to_app`: on a thread with no TEB (`ios_current_teb()`), the handler puts
  the app's previous action back and returns, so the fault repeats into the app's own handling;
  a signal that was sent (SIGABRT, `SI_USER`, `SI_QUEUE`, code 0) is sent again.
- spec 11 B2, Windows code (`is_unix_fault` false): `setup_exception`/`setup_raise_exception` as
  upstream (debugger event, `virtual_setup_exception` below the interrupted sp, 16-byte aligned,
  pc = `KiUserExceptionDispatcher`), without x18 and without upstream's `Pc -= 4` for
  breakpoints (the pc is never moved forward in the first place).
- spec 11 B2, `segv_handler` for `SIGSEGV` and `SIGBUS`: `EXCEPTION_ACCESS_VIOLATION` with
  `[0]` = 8 when the address is the pc or the syndrome is an instruction abort, 1 for a data
  abort with WnR, else 0, and `[1]` = the address; `virtual_handle_fault` first, as upstream
  (upstream maps SIGBUS to a misalignment).
- spec 11 B2, `ill_handler`: upstream's `mrs xN, CurrentEL` emulation (never into x18), else
  `EXCEPTION_ILLEGAL_INSTRUCTION`; the instruction is read with `mach_vm_read_overwrite`.
- spec 11 B2, `trap_handler`: the `brk` immediate from the syndrome (EC 0x3c) or the
  instruction; `#0xf000` and unknown immediates → `EXCEPTION_BREAKPOINT`, `#0xf001` →
  `STATUS_ASSERTION_FAILURE`, `#0xf004` → `EXCEPTION_INT_DIVIDE_BY_ZERO`, all with the context's
  pc on the `brk`; `#0xf003` (fast fail) ends the pseudo-process with
  `STATUS_STACK_BUFFER_OVERRUN`, logging the code from x0; `TRAP_TRACE` → single step. The same
  handler does the dispatcher's full-context return (Part A2).
- spec 11 B2, fix (found by the Part E host tests): a single-step trap (`TRAP_TRACE`, syndrome
  not a BRK) is `EXCEPTION_SINGLE_STEP` whatever the next instruction is. `ios_sig_exception`
  used to look at the instruction at pc first, so a step that stopped in front of a `brk` was
  reported as that brk (a breakpoint, or for `brk #0xf003` the end of the pseudo-process) before
  the brk had run, and the brk then trapped again.
- spec 11 E, `ios_sig_pure.c`: the `LDPSW` case of the pair decoder written on separate lines
  inside braces (clang 21's `-Wmisleading-indentation` stopped the `-Werror` host build); no
  behaviour change. The host tests needed no other refactoring: everything they test was already
  in `ios_sig_pure.c`.
- spec 11 B2, `fpe_handler`: upstream's mapping of Darwin's `FPE_*` codes.
- spec 11 B3, `handle_syscall_fault`, for every fault kind (upstream: access violations only):
  a unix-side fault (sp on the kernel stack and pc not in a PE image) inside `__TRY` longjmps to
  the `__EXCEPT` as upstream; any other one logs `exception <code> in a system call at pc …,
  address …`, the app image base, pc/lr/sp/fp and the frame-pointer chain of return addresses
  (read only inside the kernel stack), and ends the pseudo-process with the exception code.
- spec 11 B3/B4/B5, ending a pseudo-process from a handler (`redirect_to_end`): the signal
  context is pointed at `ios_end_pseudo_process(status)` on the thread's kernel stack below its
  current syscall frame (`uc_onstack` cleared), so the handler returns first and the thread then
  leaves through spec 09 Part D, releasing its recorded locks.
- spec 11 B4, `check_storm`: per thread (`ios_storm_note`), 1000 identical faults in a row
  (signal, pc, address) log one line and end the pseudo-process with the fault's code; emulated
  faults reset the count.
- spec 11 B, a fault while on the alternate stack (`fault_in_handler`) is a fault of the handler
  itself: logged, and the pseudo-process ends.
- spec 11 B5: no `SIGUSR1`/`SIGUSR2` handlers (upstream's suspend and set-context signals; spec
  08 Part E does both through Mach) and none for `SIGINT`/`SIGQUIT` (upstream's
  `int_handler`/`quit_handler`); `SIGTERM` was never handled. `abrt_handler`: `SIGABRT` on a
  Windows thread logs and ends the pseudo-process with abort's exit code (3, spec 09).

## Part C: the Mach exception port (spec 11)

- spec 11 C1, `ios_mach_exc.c` `ios_mach_exc_init` (from `signal_init_process`, once for the
  app, before the first program's first thread runs Windows code): a receive port with a send
  right, a detached server thread with `QOS_CLASS_USER_INTERACTIVE` (all asynchronous signals
  blocked on it), then `task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS, port,
  EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64)`. Any failure is logged and
  leaves the signal path alone in charge.
- spec 11 C2, the server: `mach_exception_raise` (2405) messages; for `EXC_BAD_ACCESS` with two
  codes, the thread is looked up in the registry (Part D) by the port name in the message and,
  failing that, by its unique thread id (`THREAD_IDENTIFIER_INFO`), both exact; a registered
  thread's `ARM_THREAD_STATE64` is read and `mid_fxi_mach_fault(thread, &state, code[1])` (weak)
  is called; non-zero: `thread_set_state` and `KERN_SUCCESS`.
- spec 11 C3: every other case is answered `KERN_FAILURE` (the kernel then raises the BSD
  signal, Part B); the thread and task rights of the message are deallocated on every path; a
  message that isn't an exception is destroyed with its rights.
- spec 11 C4: the server allocates nothing, takes no lock and logs only through
  `ios_log_write` (write(2)) with `ios_sfmt`; it never calls Windows code.
- spec 11 C/D, `ios_sig_pure.c`: registry slots also keep the thread's unique 64-bit id
  (`ios_threadreg_add(port, id, teb)`, `ios_threadreg_teb_of_id`).

## Part D: the thread registry (spec 11)

- spec 11 D1, `ios_sig_pure.c`: 1024 slots of {Mach port name, unique thread id, TEB}. A slot is
  claimed by compare-and-swap of its port (0 → busy), its fields change between two increments
  of a sequence number and the port is stored last; readers retry on an odd or changed
  sequence (bounded: a reader that can't get a stable read reports an empty slot) and never
  lock. Removal empties the slot; empty slots are reused; a port registered again gets the new
  values.
- spec 11 D1, `signal_arm64_ios.c` `thread_register` (from `ios_signal_thread_start`, i.e.
  `init_syscall_frame`, on the thread before its first Windows code, every thread including a
  program's first): the port is `mach_thread_self()` (its extra reference dropped, as
  `get_unix_tid` does, so it's the server's unix tid), the id `pthread_threadid_np`.
  `thread_unregister` runs in `ios_signal_thread_leave` (spec 09's `ios_thread_leave`).
- spec 11 D2: lookups by port (`ios_threadreg_teb_of`) and by id (`ios_threadreg_teb_of_id`)
  are exact; there is no default entry.
- spec 11 D3, exports for the app: `ios_thread_registry_count()` (slots in use or ever used),
  `ios_thread_registry_teb(i)`, `ios_thread_registry_mach(i)` (0 for an empty slot).
- spec 11 D4: `ios_thread_registry_range_busy(base, size)`: whether a registered TEB lies in
  `[base, base + size)`; spec 10 E7 (`virtual.c`, weak) uses it.

## Clarification 1: faults at low windows (spec 11)

- spec 11 cl. 1, `ios_vm_pure.c`: `ios_lowwin_find(addr, peb, &real)` is `ios_lowwin_lookup`
  that also returns the window's slot (lookup now calls it); each slot counts its serviced
  accesses (`ios_lowwin_note_service`, reset when the slot is reused).
- spec 11 cl. 1, `ios_lowwin_fault.c` `ios_lowwin_service(ss, ns, addr, peb)`: for a data fault
  below 4 GB in a window of the PEB's process (or an owner-NULL one), the instruction at pc is
  read with `mach_vm_read_overwrite` and decoded (`ios_sig_pure.c`). Fast path: a base register
  (never sp, never x18) holding an address in `[0x7ffe0000, 0x7ffe1000)` of the same window is
  moved to the same offset in the real page and the instruction is retried. Otherwise the
  access is emulated: the span must lie in that one window and the real memory must allow it
  (`mach_vm_region` protection, every region of the span), then `memcpy`-style copies (atomics
  with `__atomic` builtins on naturally aligned addresses), destination registers written back
  (x18 never, xzr discards), indexed forms' base written back, pc + 4. An instruction fetch, an
  encoding outside the list, an unreadable pc, a span outside the window or refused memory is
  declined and logged once per (pc, address page) (`ios_once_pair`); the fault then goes on as
  an access violation. Every 4096th access of a window is logged.
- spec 11 cl. 1, supported encodings (`ios_a64_emulate`): `LDR`/`STR` (integer B/H/W/X and
  `LDRS*`, SIMD B/H/S/D/Q) unsigned-immediate, unscaled (`LDUR`/`STUR`, `LDTR`/`STTR` as at
  EL0), register offset (UXTW, LSL, SXTW, SXTX, scaled or not), pre- and post-index; `LDP`/
  `STP`/`LDNP`/`STNP` (W, X, S, D, Q), `LDPSW`; `LDAR`/`STLR` (and `LDLAR`/`STLLR`), `LDAPR`,
  `LDAPUR`/`LDAPURS*`/`STLUR`; LSE `LDADD`/`LDCLR`/`LDEOR`/`LDSET`/`LDSMAX`/`LDSMIN`/`LDUMAX`/
  `LDUMIN` (and their `ST*` aliases), `SWP`, `CAS` (B/H/W/X, any ordering). Encodings naming x18
  as a general register, pair loads into one register twice and written-back bases that are
  also transferred are refused.
- spec 11 cl. 1, Mach server (`ios_mach_exc.c` `service_low_window`): before
  `mid_fxi_mach_fault`, with the PEB read from the registry entry's TEB (`TEB+0x60`, through
  `mach_vm_read_overwrite`), never the server thread's; the NEON state is fetched only for a
  SIMD access; the new state is written back with `thread_set_state` and the reply is
  `KERN_SUCCESS`.
- spec 11 cl. 1, signal path (`segv_handler`): before the storm check and before any exception
  is built, with the thread's own PEB, on the signal context's `__ss`/`__ns`; a serviced fault
  resets the storm count. It applies to unix code too (a unix function reading an absolute low
  address a program passed).

## Clarification 2: the wineserver's alert hook (spec 11)

- spec 11 cl. 2: already done by spec 09 Part D3 and kept: `ios_process.c` `init_process_once`
  (through `ios_process_init_once()`, the first call of `__wine_main`, before any server request
  or Windows thread) sets spec 08's `wineserver_thread_alert_hook` to `ios_thread_alert`, once
  for the app. `ios_thread_alert` does nothing for a unix tid that has gone (`ios_exit.c`), as
  spec 08 clarification 4 requires. No code change for spec 11.

# Spec 17: smaller unix-side changes

Same tree, same rules (`WINE_IOS`, `MYIOSDECK` comments). Each entry is tagged "spec 17" and
the Part. Pure rules are in `ntdll-unix/ios_s17_pure.c` (no Wine headers) and
`ntdll-unix/ios_s17.c` (Wine types, no `unix_private.h`), both host-tested.

## Part A: build (spec 17)

- spec 17 A1, `build-ios.sh`: `out/09-ntdll-processes/include/` (this tree's copies of
  `wine/unixlib.h` and `winternl.h`) is on the include path before the Wine tree's `include/`.
- spec 09 clarification 2, `build-ios.sh`: spec 13's `ntdll-unix/socket.c` and `ios_afd.h` are
  copied over upstream's after this tree's files, so spec 13's `socket.c` is the one compiled.

## Part E: the shared headers (spec 17)

- spec 17 E1, `include/wine/unixlib.h` (modified copy of upstream's, LGPL): for unix libraries
  (`WINE_UNIX_LIB`), under its own guard `__WINE_IOS_WOW_PTR_HELPERS`: with `WINE_IOS`,
  `ios_wow_base` declared weak and `ios_wow_host_base()` (0 if not linked),
  `ios_wow_host_ptr(addr)` (NULL for 0, else base + addr) and `ios_wow_guest_ptr32(host)` (0 for
  NULL, else host − base truncated to 32 bits); without `WINE_IOS`, `ULongToPtr` /
  `PtrToUlong` (and a base of 0).
- spec 17 E2, `include/winternl.h` (modified copy, LGPL): `ProcessWineIosWowGuestBase = 1010`
  after Wine's private process classes (inside `__WINESRC__`); `RtlWow64SuspendThread(HANDLE,
  ULONG *)` declared with the 64-bit branch's `RtlWow64*` functions (also inside
  `__WINESRC__`).

## Part B: which pseudo-process is asking (spec 17)

- spec 17 B, `ios_s17.c`: the switches `MADEIRA_WOW64_BY_TEB`, `MADEIRA_IO_STATUS_OWNER` and
  `MADEIRA_SECTION_PROCESS_LIMIT` are read once per app run (on unless exactly `0`); the steady
  state is one relaxed load (`ios_s17_switch`, `ios_s17.h`).
- spec 17 B1, `unix_private.h` `is_wow64()`: TRUE for a thread whose TEB has `WowTebOffset != 0`,
  else the pseudo-process's `wow_peb` (`ios_s17_is_wow64`).
- spec 17 B2, `unix_private.h` `in_wow64_call()` (64-bit): `ios_wow_base() != 0`, the calling
  pseudo-process has a guest window (`ios_s17_in_wow64_call`); spec 13's `socket.c` uses the
  same test.
- spec 17 B3, `unix_private.h` `is_ec_code()`: the `EcCodeBitMap` of the PEB of the calling
  thread's TEB (spec 09's key), the pseudo-process's PEB only without a TEB; a NULL bitmap (or no
  PEB) is FALSE (`ios_s17_ec_bit`).
- spec 17 B4, `unix_private.h` `map_section()`: the ceiling is `user_space_wow_limit` only for a
  pseudo-process with a guest window, else 0 (`ios_s17_section_limit`).

## Part C: files and volumes (spec 17)

- spec 17 C1, `file.c`: when the mount manager query fails and the calling thread is WoW64
  (`WowTebOffset != 0`), `FileIdInformation`'s `VolumeSerialNumber` and
  `FileFsVolumeInformation` come from the prefix's stored value (`ios_s17_wow_volume_serial`,
  `ios_s17_volume_info`): serial = FNV-1a over the value's four bytes (LSB first) and the drive
  letter the query left (`'a' + n`, or 0); 0 or `0xffffffff` becomes the stored value. The volume
  answer: creation time 0, empty label, `SupportsObjects` TRUE, `Information` = offset of
  `VolumeLabel` (18); a zero serial keeps `STATUS_NOT_IMPLEMENTED`. 64-bit callers are unchanged.
- spec 17 C1, `ios_s17_pure.c` `ios_s17_load_serial`: `<config_dir>/.madeira-volume-serial`
  (8 lower-case hex digits and a newline; up to 15 bytes parsed); missing, 0 or garbage: a value
  from `arc4random` (never 0 or `0xffffffff`), the file created 0600; a creator that loses the
  race takes the file's value (QUESTIONS.md, spec 17 #1). The value is cached for the app run
  with relaxed atomics.
- spec 17 C1, fix (CI host tests failed intermittently: eight threads creating the file at once
  didn't all end with its value): a reader could open the file between another thread's
  `O_CREAT | O_EXCL` and its `write`, find it empty and replace it. Now the racers of the app
  (threads of this one process) take turns under a mutex, and the file appears whole: the value
  is written to `<file>.<pid>.tmp` (`O_EXCL`, 0600) and `link`ed to the final name (`EEXIST`:
  another program won, its value is used, the same meaning as `O_EXCL`), or `rename`d over a
  file that holds no value; the temporary file is removed. The host test runs 200 rounds of
  eight racing threads (from a missing file and from a file holding 0).
- spec 17 E2, fix (iOS CI: `server/user.c` failed on `va_list` in `RtlFormatMessage`):
  `include/winternl.h` includes `<stdarg.h>` itself; outside a unix library `__ms_va_list` is
  `va_list`, and a file that includes `windef.h` and `winternl.h` first had no declaration of it
  with the iPhoneOS SDK. `tests/test_s17_winternl_alone.c` compiles the header alone, as the
  server does (the failure itself only shows with the iPhoneOS SDK's headers).
- spec 17 C2, `file.c` `NtCreateFile`: every failed open is logged (`ios_s17_log_failed_open`):
  status, disposition, access, options, unix name and NT name (printable ASCII, others `?`); an
  open that writes or creates (`FILE_WRITE_DATA`, `FILE_APPEND_DATA`, `GENERIC_WRITE`, or a
  disposition other than `FILE_OPEN`) also logs the share mode and has its own budget of 192
  lines, the others 64 (QUESTIONS.md, spec 17 #2).
- spec 17 C2 addendum, failed opens beyond NtCreateFile (a game's `NtOpenFile` failures with
  0xc0000034 never showed: the 64-line budget was gone by then). `NtCreateFile`'s body is
  `create_file` with the call's name, and `NtOpenFile` calls it as `"NtOpenFile"`, so its lines
  say `NtOpenFile failed: …` in the same format. `NtQueryAttributesFile` and
  `NtQueryFullAttributesFile` log their failures (`ios_s17_log_failed_query`: `<call> failed:
  status … unix … nt …`). A third budget: read-only failures whose NT name is outside
  `C:\windows\` (`ios_s17_is_windows_path`, `ios_s17_budget_kind`) take 256 lines of their
  own; those under `C:\windows\` (or without a name) keep the 64-line budget, and writes and
  creates keep the 192-line budget.

## Part D: logging, CPU count, system information, the unix-call table (spec 17)

- spec 17 D1, `debug.c`: a thread with no TEB (or any thread before initialisation) logs through
  upstream's shared `initial_info` (spec 09's thread-local `calloc`'d buffer is gone,
  QUESTIONS.md spec 17 #3); its header prints `no-teb:` in place of the process and thread ids.
- spec 17 D2, `debug.c` `wow64_wine_dbg_write`: the string is converted with
  `ios_wow_host_ptr` (Part E1).
- spec 17 D3, `system.c` `init_cpu_info` → `ios_s17_cpu_count`: `MADEIRA_CPU_COUNT` if a positive
  integer, else `cpu-count` from `$MADEIRA_DOCS_DIR/madeira.cfg` (spec 02 Part G's format: BOM,
  `#` lines, trimming, last occurrence wins); applied only if 1 to 63; decided once per app run
  and logged once (`processor count: reporting N (host has M)`).
- spec 17 D4, `ios_s17.c`: `ios_get_shared_futex_queues()`, a static zeroed table of 256
  `{ void *next; void *prev; LONG lock; }` (24 bytes, asserted); `system.c`
  `NtQuerySystemInformation` class `0xf00d` with a pointer-sized buffer writes its address
  (NULL buffer: `STATUS_ACCESS_VIOLATION`; the returned length if asked); other sizes go on to
  upstream's handling.
- spec 17 clarification 2 (D4 amended, a device crash: a second program of the session walked
  into a stale wait entry of an ended program's parked thread in the shared table): one table per
  pseudo-process. `struct ios_proc` has `futex_queues` (NULL until asked);
  `ios_get_process_futex_queues()` (`ios_process.c`, replaces `ios_get_shared_futex_queues`)
  returns the calling thread's process's table, created zero-filled with `calloc` at its first
  query (`ios_s17_futex_table_get`: published by compare-and-swap, so the ARM64EC and ARM64 images
  asking at once get the same one). Fallback: a thread with no pseudo-process gets NULL and class
  `0xf00d` answers `STATUS_UNSUCCESSFUL` (after the size and NULL-buffer checks, which are
  unchanged), so spec 16's PE side keeps its image's own array. `ios_proc_free` frees the table
  (`ios_s17_futex_table_free`): the struct goes only after the last thread left, and threads
  suspended for good never run, so nothing walks it again; a table is never given to another
  process. The unix side has no other use of the table.
- spec 17 D5, `syscall.c` (modified copy, new in this tree): `ntdll_syscall_name(id)`: table
  `(id >> 12) & 3`, number `id & 0xfff`, NULL without names or at/above the table's
  `ServiceLimit`; bounded reads only.
- spec 17 D6, `ntdll-unix/unixlib.h` (modified copy of `dlls/ntdll/unixlib.h`, the one copy both
  halves of ntdll use, spec 17 clarification 1): `unix_ios_push_jit_aliases` (8),
  `unix_ios_register_hold_release` (9), `unix_ios_jit_alias_probe` (10),
  `unix_ios_mono_bridge_ptr` (11), `unix_ios_get_fex_arena` (12) appended after
  `unix_system_time_precise`, with their argument blocks; sizes asserted (24, 88, 16) on 64-bit.
  `loader.c`: the 64-bit table gets `ios_unixcall_*` (`ios_s17.c`; no JIT here: 8 checks its
  callback and succeeds without calling it, 9 and 10 check size and version 1, 10 zeroes every
  field after `addr`, 11 writes 0 and returns `STATUS_NOT_SUPPORTED`, 12 returns an empty arena);
  the WoW64 table has the same length and answers all five `STATUS_NOT_SUPPORTED`.
- Audit: `debug.c`'s `thread_info` (class b) is gone; new in `ios_s17.c`: `ios_s17_switch_bits`,
  `stored_serial`, `open_budgets`, `cpu_override`, `shared_futex_queues` (all class a).

## Spec 10 clarification 3 (for spec 17 Part F2)

- spec 10 cl. 3, `virtual.c`: `int ios_wow_session_armed(void)` (acquire load) is 1 once the
  session is armed (release store, never undone): in `ios_vm_init_windows` when
  `ios_main_image_i386` announces a 32-bit main image, and at the start of
  `ios_mem_reserve_wow_window`, which every 32-bit process (the first program and every child)
  calls before its first thread exists. win32u reads it (spec 17 F2).

## Clarification 3: unix-side faults return to the caller (spec 11)

- spec 11 cl. 3, `signal_arm64_ios.c` `handle_syscall_fault` (replaces B3's "end the
  pseudo-process"): a unix-side fault inside `__TRY` unwinds to its `__EXCEPT` as before; any
  other one ends the system call with the exception code as its status, as upstream: x0 = the
  code, sp = the current syscall frame, pc = `__wine_syscall_dispatcher_return` (x18 untouched).
  The first 16 such faults of the app run, then every 256th (`unix_fault_count`), are logged with
  the pc, the address, the app image base and up to 8 return addresses. With no syscall frame
  the fault stays a Windows exception (B2); the storm rule (B4) still runs first. Fast fail,
  SIGABRT, storms and faults inside the handlers still end the pseudo-process.

## Clarification 4: `kernel_stack` in the shared header (spec 11)

- spec 11 cl. 4: already in Part A: `ios_syscall_frame.h` `IOS_TEB_KERNEL_STACK` (0x3a8), checked
  in `signal_arm64_ios.c` against `struct ntdll_thread_data`.

## Clarification 5: synchronization (spec 16)

The cell logic, the userspace ntsync engine, the settings reader and the pacing are
`out/16-sync-ios/src/` (MIT; its `CHANGES.md` has the details); these are the hooks in this tree.

- spec 16, `build-ios.sh`: compiles `out/16-sync-ios/src/*.c` into `libntdll_unix.a` (one copy
  of the cell table and of the ntsync state per app; the server links against them), defines
  `HAVE_LINUX_NTSYNC_H=1` and `IOS_USYNC=1`, and puts `out/16-sync-ios/include` on the include
  path; `ntdll-unix/ios_config.h` keeps `HAVE_LINUX_NTSYNC_H` when `IOS_USYNC` is defined.
- spec 16 E5, `sync.c`: `ioctl` and `close` go to `ios_usync_ioctl` / `ios_usync_close` (other
  descriptors pass to libc), so upstream's `linux_*` ntsync calls run the userspace engine.
- spec 16 E4, `sync.c`: `get_server_inproc_sync` and `get_inproc_alert_fd` take the descriptor
  the server posted (`ios_usync_take`, by the TEB's process id and the handle / token) instead of
  receiving one; nothing posted: `STATUS_INVALID_HANDLE` (alert: -1), never blocking.
  `server.c` `init_first_thread`: a non-zero `inproc_device` sets `inproc_device_fd` to
  `IOS_USYNC_DEVICE`; nothing is received.
- spec 16 D1, `server.c`: after `init_first_thread`, `ios_fs_process_init` evicts the fastsync
  cache entries of the new process id and logs the A6 line (or that the other engine runs).
- spec 16 D1, `sync.c` `close_inproc_sync`: evicts the fastsync entry with the handle value,
  whatever its process, before upstream's early return when no ntsync device is in use.
- spec 16 D1-D8, `sync.c` (block after `inproc_signal_and_wait`): `ios_fs_entry` (cache lookup,
  learn request on a miss under `fd_cache_mutex`; no caching for handle 0, pseudo handles or a
  thread with no TEB); `NtSetEvent` / `NtResetEvent` (D3, `event_op` WAKE when server waiters
  exist), `NtReleaseSemaphore` (D4, `release_semaphore` with count 0 for server waiters),
  `NtQuerySemaphore` (D5) try the cell after upstream's in-process attempt declines.
  `NtWaitForSingleObject` and a one-handle `WaitAny` `NtWaitForMultipleObjects` take the fast wait
  (try, spin, park; the remaining relative timeout to the server, absolute unchanged), the poll
  peek (D7) and the watchdog for infinite waits on cell objects (D8: server slices 2, 8, 32, 60 s;
  a disagreement is logged once, the object disabled with `event_op` DISABLE_EVENT /
  DISABLE_SEMAPHORE and its entry evicted). Alertable waits, wait-all, several handles,
  `NtSignalAndWaitForSingleObject`, keyed events and delays stay on the server.
- spec 16 D9, `sync.c` `ios_inproc_cache_drop`: the blocks of an ended process's in-process sync
  cache are no longer unmapped (a late thread may be inside a lookup).
- spec 16 G1, `sync.c` `NtYieldExecution`: past `yield-streak` yields each under 2 ms after the
  previous one, sleeps `yield-sleep-us`; upstream's body is `yield_execution`, which
  `NtDelayExecution` uses (a `Sleep` is not part of the streak).
- spec 16 G2, `sync.c`: `NtWaitForSingleObject`, `NtWaitForMultipleObjects`,
  `NtDelayExecution`, `NtYieldExecution` and `NtWaitForAlertByThreadId` apply the current eco
  choice (`ios_eco_apply`). Spec 09 E1's QoS class is only the default.
- spec 16 G3, `sync.c` `NtWaitForAlertByThreadId`: spins on the alert word for `alert-spin-us`
  (default 0) before parking.
- spec 16 B5: the iOS 17.4 availability test of the Apple futex helpers was already there (spec
  09); no change.
- spec 16 F5, `sync.c` `NtAlertThreadByThreadId`: works on a thread with no TEB already (nothing
  reads the caller's TEB or pseudo-process); a comment says so.
- spec 16: `unix_private.h` needed no change (the helpers are static in `sync.c`, the library's
  declarations are in `out/16-sync-ios/include/ios_sync.h`).

## Spec 10 clarifications 4 and 5 (for spec 18; written in the spec 18 session)

- Clarification 4, `ios_dylib.c` `set_ntdll_tsd_offset` (new), `ios_dylib_open`: when the first
  image or a fresh instance of `ntdll.dll` is registered (both 64-bit dylib folders), the PE export
  `ios_teb_tsd_offset` of that image gets `ios_teb_tls_slot_offset`, next to the Part C2 TEB
  offset. Nothing is written while the offset is unknown (the PE side reads 0 as "not known");
  a missing export is traced. `ios_vm_pure.c` `ios_pe_find_export` (new, declared in
  `ios_vm_pure.h`): a bounds-checked lookup of a named export in a mapped PE32/PE32+ image
  (forwarders give NULL); host test `test_pe_export` in `tests/test_ios_vm_pure.c`.
- Clarification 5, `virtual.c` `alloc_arm64ec_map`: no change needed. The bitmap is already one
  view, allocated on first need and sized from `address_space_limit`, which Part A2
  (`ios_vm_init_limits`, run in `virtual_init` before any image is mapped) sets to the reported
  address space; `NtQueryVirtualMemory` reports the view's base as the `AllocationBase` of every
  region of it, which is what the PE side's `RtlIsEcCode` measures (spec 18 Part F5).

## Spec 22 Part C: later programs in a kept session

From `specs/22-session-reuse.md` Part C and spec 09 clarification 8 (round 3). New files:
`ios_session_pure.c/.h` (pure rules, host-tested), `ios_mach_thread.c` (two Mach calls on the
app's own threads, spec 00 B2).

- **C1** `loader.c` `ios_session_start_program` (contract Section A, exported): needs the app-wide
  set-up done by the first program (`app_init_state`, now file-scope), else logs and returns
  non-zero. A new `ios_proc` (`first` FALSE, new field `top_level` TRUE) with a copy of argv, its
  `boot_socket` = the given fd (server.c takes it as a child's), the machine from `i386`; a 32-bit
  program reserves its guest window before its TEB and PEB; then `start_main_thread()`. The server
  gives no startup info for a process with no parent, so `init_startup_info` builds the parameters
  from argv, the environment and `MADEIRA_INITIAL_CWD`, as for the first program; wineboot is not
  run again (its event lives in services.exe). Its own exit buffer, as `__wine_main`'s: a boot
  failure comes back to the app's buffer (its status was reported), or here, and returns non-zero.
  The function owns the fd from the call: closed on the failures it returns for.
  `ios_boot_phase` now reports milestones of every top-level program (`top_level`, not `first`).
- **C2** No change needed: `session_return` (ios_exit.c) already reports
  `wine_launched_process_did_exit` and longjmps for whichever top-level program the calling thread
  booted, and Part D1's clean-up has no first-program exception.
- **C3** `server.c`: the five places where the server's closing of a thread's pipes or of the
  process socket ends the thread call `ios_thread_killed` (marks the thread, then `abort_thread`);
  `server_call_unlocked` fails a marked thread's later requests with
  `STATUS_THREAD_IS_TERMINATING`. `ios_exit.c` `ios_thread_leave`: a killed thread that booted its
  pseudo-process (`ios_proc_set_main_thread`, called by `start_main_thread`) with other threads
  still there ends the process through `ios_end_pseudo_process` instead of parking.
  Rule in `ios_killed_thread_ends_process`. (A `TerminateThread` aimed at the main thread alone
  also ends the process here: QUESTIONS.md, Spec 22.)
- **C4** Checked: the exiting flag (`pp_process_exiting`), socket, fd and sync caches were already
  per pseudo-process (Part B audit) and go in `finish_process`; a 32-bit window is RELEASED (spec
  10 E7), so it no longer resolves for new threads.
- **C5** `ios_exit.c` `forget_leftover_threads`, after the 2 s wait of `ios_end_pseudo_process`:
  alerts the threads still there, waits 0.5 s, then for each: its Mach run state is read first
  (`ios_mach_thread_waiting`), a thread blocked in the kernel outside Wine's locked sections
  (`ios_no_interrupt`, `ios_leaving` in its TEB) is suspended for good (`thread_suspend`), dropped
  from the alert table and the thread registry and from the thread count; a running thread (guest
  code) or one inside Wine is kept, logged once, and keeps the memory. One line with the counts
  woken / left / suspended / kept. With every thread gone the normal last-thread path runs: hooks
  (C6), then memory. `virtual.c` `ios_mem_release_process` calls DXMT's
  `winemetal_release_window(base)` (weak import, spec 06 E) for a 32-bit window once the threads
  are gone.
- **C6** `ios_process_end_hook_add` (cross-library name, at most 16, each once); the hooks run in
  `ios_thread_leave`'s last-thread path with the PEB, before `ios_mem_release_process`.
- **C7** Tables: the PEB registry, the children table, the alert table and the thread registry
  already reused freed entries; leftover threads now free theirs too (C5). `ios_dylib.c`: the
  image table appended an entry per fresh instance and never reused it; now `delete_view` of a
  dylib image view calls `ios_dylib_view_gone`, which frees a fresh instance's entry for reuse
  (the instance's memory itself is not reused: its data was written by the ended process; see
  QUESTIONS.md). `ios_dylib_find` reads each entry's end with acquire order (NULL: free).
- **C8** `thread.c` `free_previous_teb` goes through `ios_prev_teb_exchange` on the calling
  process's own `pp_prev_teb` (it already was per process). Checked for other caches of "the
  process's" image list, PEB or machine in ntdll's unix side: the per-process globals are in
  `struct ios_proc` (Part B audit); `signal_arm64_ios.c`'s `app_image_base` is the app's own image.
- Host tests: `tests/test_session_pure.c` (in `run-host-tests.sh`).
