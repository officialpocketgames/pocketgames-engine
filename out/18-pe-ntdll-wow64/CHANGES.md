# Spec 18: changes to Wine 11.4's Windows-side ntdll, wow64 and wow64win

Every file under `pe/` is either a modified copy of `refs/wine-11.4/<same path>` (LGPL-2.1-or-later,
licence header kept, a "Changed for MYIOSDECK" note added) or a new MIT file (pure logic shared with
the host tests). Builds: **EC** = ARM64EC ntdll (`__arm64ec__`), **A64** = ARM64 ntdll
(`__aarch64__` without `__arm64ec__`), **x86** = i386 ntdll, **all** = every build.

Shared headers come from spec 17 (`out/09-ntdll-processes/include/`: `winternl.h` with
`ProcessWineIosWowGuestBase = 1010` and `RtlWow64SuspendThread`; ntdll's private `unixlib.h` from
`out/09-ntdll-processes/ntdll-unix/`). `wow64win` compiles against
`out/15-controllers/pe/include/ntuser.h` (`NtUserCallTwoParam_GetGamepadState`). None of them is
delivered here.

## New files (MIT, pure logic, no Windows headers)

| file | contents | used by |
|---|---|---|
| `pe/dlls/ntdll/arm64ec_x64_export_iat.c` | `arm64ec_iat_slot_is_x64_export` (Part D3) | `loader.c` (EC), tests |
| `pe/dlls/ntdll/arm64ec_redirect.h` | thunk decoding of `arm64ec_redirect_ptr` (Part D1) | `signal_arm64ec.c`, tests |
| `pe/dlls/ntdll/arm64ec_unwind_guard.h` | stack window, phase-one and phase-two guards, code-bitmap coverage (Part F1-F5) | `signal_arm64ec.c`, tests |
| `pe/dlls/ntdll/arm64_unwind_stall.h` | the stall guard (Part G3) | `signal_arm64.c`, tests |
| `pe/dlls/ntdll/qpc_arith.h` | counter conversion and offset rule (Part H7) | `time.c`, tests |
| `pe/dlls/ntdll/heap_policy.h` | policy switch parser, grow/retry sizes, large-block census (Part I) | `heap.c`, tests |
| `pe/dlls/ntdll/ntdll_names.h` | XInput names, `sysx64` path, always-logged DLL names (Part H3/H5/H6) | `loader.c`, tests |
| `pe/dlls/wow64/wow64_window.h` | guest window conversions, other-process base cache, exception parameter catalogue, small rules (Part J) | wow64, tests |
| `pe/dlls/wow64win/wow64win_window.h` | atom-keeping conversions, `NtUserCall*` and message tables (Part K) | wow64win, tests |

## Part A: what is not carried over

Upstream 11.4 contains none of the JIT, emulator hand-off, `ios_xp_nt` or tracing items Part A
lists, so nothing was removed; none was added. `ntdll.spec` and `ntdll_misc.h` don't declare
`ios_xp_nt`; `RtlQueryPerformanceCounter` records nothing. ntdll's unix calls 8-12 are not called.

## Spec 00 (App Store rules)

Only the no-JIT path exists: the CPU modules are the app's interpreters, x86/x64 code is data,
and nothing here writes host code or asks for executable memory of its own (the Windows-level
protection requests it forwards are handled by the unix side under spec 10's no-JIT rules).
No process creation, no private API, no privacy-sensitive or required-reason API.
`tests/run-host-tests.sh` runs `check-app-store.sh` on this part; it prints "clean".

## Part B: exports and the CPU-module contract

| item | file | function / symbol | change |
|---|---|---|---|
| B1 | `ntdll.spec` | `RtlWow64SuspendThread` | `stdcall -arch=win64` export |
| B1 | `ntdll.spec`, `loader.c`, `ntdll_misc.h` | `ios_teb_tsd_offset` | `extern -private -arch=win64` data export, `unsigned int`, 0 (= not known); defined in `loader.c` (64-bit builds) |
| B1 | `process.c` | `RtlWow64SuspendThread` | calls `NtSuspendThread` (64-bit builds) |
| B2 | `signal_arm64ec.c` | `arm64ec_process_init_dispatchers` | `ExitToX64`, `DispatchJump`, `RetToEntryThunk`, `BeginSimulation` and the three required functions (`ProcessInit`, `ThreadInit`, `BTCpu64IsProcessorFeaturePresent`) must exist, else `STATUS_PROCEDURE_NOT_FOUND` (logged); every other function is optional as upstream |
| B3 | - | `Wow64SystemServiceEx`, `Wow64PassExceptionToGuest`, `Wow64RaiseException`, `Wow64ProcessPendingCrossProcessItems`, `__wine_unix_call_dispatcher` | stay exported (unchanged upstream exports); `BTCpuNotifyProcessExecuteFlagsChange`: Part J8 |
| B4 | all | - | PE sources reach the TEB/PEB the Windows way (`x18`, `NtCurrentTeb()`); no `TPIDRRO_EL0` reads were added |

## Part C: starting an ARM64EC process and its threads

| item | file | function | change |
|---|---|---|---|
| C1 | `loader.c` | `loader_init` (EC) | after `build_ntdll_module`: `alloc_tls_slot` for the main executable (slot 0, `TlsIndex = -1`), then `locale_init()`, then the CPU module |
| C1 | `loader.c` | `fixup_imports` | skips `alloc_tls_slot` when `TlsIndex` is already -1 |
| C2 | `loader.c` | `load_arm64ec_module(context)` | phases: `load_dll`; `arm64ec_process_init_dispatchers`; `reapply_hybrid_metadata` (new: `arm64ec_update_hybrid_metadata` for every ARM64EC module in load order except ntdll); `alloc_thread_tls` for the first thread (marked by the new flag `first_thread_tls_done`, so `loader_init` doesn't allocate the blocks again; later slots are added by `alloc_tls_slot`); `process_attach` on the module's node (pinned, `lpReserved` = context); `arm64ec_process_init`. Every failure logs and ends the process |
| C2 | `signal_arm64ec.c` | `arm64ec_process_init_dispatchers` (new), `arm64ec_process_init` | upstream's function split in two; phase 2 sets the dispatchers, resolves every pointer through `arm64ec_redirect_ptr`, fills `ChpeV2ProcessInfo`, saves the dispatcher thunk bytes; phase 4 runs `ProcessInit`, the feature table, the cross-process list, `ThreadInit`, then sets `__os_arm64x_check_*`. The notification pointers (memory notifications, cache flushes, `ProcessTerm`/`ThreadTerm`, `UpdateProcessorInformation`) are resolved in phase 2 but installed at the start of phase 4 (see QUESTIONS.md, spec 18 Q1) |
| C2 | `ntdll_misc.h` | - | declares both functions |
| C3 | `loader.c` | `loader_init` (EC, later threads) | `arm64ec_thread_init` failure: log, leave the loader lock, `NtTerminateThread` on the own thread with the status |
| C4 | `signal_arm64ec.c` | `enter_syscall_callback`, `leave_syscall_callback` | NULL `ChpeV2CpuAreaInfo`: "entered" without touching anything / nothing |
| C5 | `signal_arm64ec.c` | `KiUserExceptionDispatcher` | `cbz` on `__os_arm64x_dispatch_call_no_redirect`: NULL dispatches directly |

## Part D: binding ARM64EC imports

| item | file | function | change |
|---|---|---|---|
| D1 | `signal_arm64ec.c`, `arm64ec_redirect.h` | `arm64ec_redirect_ptr` (now exported to the loader), `get_module_view`, `find_loaded_ec_module` | table hit only with an in-image destination; then the fast-forward sequence; then `FF 25` slots that look bound, redirected again with the module holding the value (load-order walk without lock) or returned as is; else unchanged; NULL stays NULL. Bytes are read only inside the module image and, when the module has a code map, only inside x64 ranges (so redirecting an already native address is a no-op); depth limit 8 |
| D2 | `loader.c` | `RtlFindExportedRoutineByName` (EC) | returns the redirected address for an ARM64EC module |
| D3 | `loader.c`, `arm64ec_x64_export_iat.c` | `import_dll`, `arm64ec_bound_import` (new), `arm64ec_iat_slot_is_x64_export` (new) | ARM64EC importer + ARM64EC exporter: store the redirected address, by name and by ordinal, unless the importer is x64 or the slot is read by one of the importer's own x64 export stubs (`FF 25` inside an x64 code range, bounds-checked); unresolved imports keep upstream's stub |
| D4 | `loader.c` | `alloc_module` (EC) | the entry point of an ARM64EC image is redirected |
| D5 | `loader.c` | `LdrResolveDelayLoadedAPI` (EC) | the resolved address goes through `arm64ec_bound_import` before it is stored and returned |
| D6 | `loader.c` | `LdrLoadDll`, `arm64ec_bind_delay_imports` (new), `bind_module_delay_imports` (new) | after a successful `load_dll`, before `process_attach`: every ARM64EC module's delay-load entries whose target DLL is already loaded (`LdrGetDllHandle`) and whose slot still points into the importer are resolved with a failure hook that returns NULL; not before `imports_fixup_done`, not with a NULL dispatcher, not re-entrantly |

## Part E: ARM64EC system-call wrappers (`signal_arm64ec.c`)

| item | function | change |
|---|---|---|
| E1 | `NtDeviceIoControlFile` | now a wrapped call: NULL, misaligned or `>= 0x8000000000` status block returns `STATUS_ACCESS_VIOLATION` without the system call; first 16 logged (handle, code, caller) |
| E2 | `NtProtectVirtualMemory`, `ProcessPendingCrossProcessEmulatorWork`, `is_huge_data_protect` (new) | a change of at least 1 GB without execute rights is not passed to `NotifyMemoryProtect` (before, after, cross-process item); first 20 logged |
| E3 | others | unchanged |

## Part F: exceptions and unwinding for x64 code

| item | file | function | change |
|---|---|---|---|
| F1 | `signal_arm64ec.c` | `init_stack_window` (new), `call_seh_handlers`, `RtlUnwindEx` | window from `Rsp` (original / captured context): the whole allocation holding it (regions with the same `AllocationBase`), the emulator range from the CPU area, budget = window/16 in [256, 65536]; kept on the walker's stack |
| F2 | `signal_arm64ec.c` | `is_valid_arm64ec_frame(window, frame)` | replaces upstream's; with a window: window or emulator stack; without: TEB stack, emulator stack (no NULL area dereference), last resort committed private RW/WC memory (first 20 accepts logged) |
| F3 | `signal_arm64ec.c` | `call_seh_handlers` | `ControlPc == 0` after a step ends the search quietly (unhandled); no progress or > 65536 steps abandons with `EXCEPTION_STACK_INVALID`, the same pair 16 times in a row (process-wide `seh_livelock`) ends the process with the code; the terminal frame is accepted once; an invalid first-step frame ends the thread with the code; later invalid frames: `EXCEPTION_STACK_INVALID` |
| F4 | `signal_arm64ec.c` | `RtlUnwindEx` | step guard before each frame check (budget, 4 repeats, downward move except across the guest/emulator stack switch); on stop: log, `EXCEPTION_STACK_INVALID` on the original record, leave the loop |
| F5 | `signal_arm64ec.c` | `RtlIsEcCode` | coverage measured once from the bitmap's allocation (at most 4096 queries; bytes from the bitmap to the allocation's end × 8 × 4096), else 2^47 with one log line; NULL bitmap or address at/above the coverage → FALSE |
| F6 | `signal_arm64ec.c` | `arm64x_check_call` | a target with a bit at or above 39 takes the exit-thunk path |
| F7 | `signal_arm64ec.c` | `unwind_one_frame`, `do_setjmpex` | the unwind reports whether a function entry was found; the x64 caller's registers are copied only then, with a non-zero, non-ARM64EC `Rip` |
| F8 | `exception.c` | `call_vectored_handlers` (EC), `vectored_rsp_usable`, `allocation_base_of` (new) | context saved before each handler; restored (first 20 logged) when the handler didn't continue execution but moved `Rip`/`Rsp` and the new `Rsp` is outside the thread's stack and the allocation of the saved `Rsp` |

## Part G: the ARM64 ntdll as WoW64 host

| item | file | function | change |
|---|---|---|---|
| G1 | `locale.c` | `casemap`, `casemap_lower` (new), `RtlDowncaseUnicodeString`, `towlower` | NULL upper table: ASCII upper case; lower-case callers go through `casemap_lower` (ASCII lower case without a table) |
| G2 | `loader.c` | `loader_init` (A64) | first thread with `WowTebOffset`: `locale_init()` right before `init_wow64()` |
| G3 | `signal_arm64.c` | `unwind_stalled` (new), `call_seh_handlers`, `RtlUnwindEx` | WoW64 threads only: third step in a row with unchanged `Pc` and `Sp` stops (phase one: unhandled; phase two: `STATUS_INVALID_DISPOSITION`); first 16 logged; `MADEIRA_UNWIND_GUARD=0` (read at the first stall) keeps walking |
| G4 | `exception.c` | `is_unconverted_guest_ptr` (new), `dispatch_exception` | A64 only (the others always say no): a non-zero parameter below 4 GB in a process with a guest window (class 1010, queried once) is not dereferenced for `EXCEPTION_WINE_STUB`, `EXCEPTION_WINE_NAME_THREAD`, `DBG_PRINTEXCEPTION_C`/`_WIDE_C`; first 16 logged |

## Part H: rules for every build

| item | file | function | change |
|---|---|---|---|
| H1 | `exception.c` | `fast_fail` (new), `dispatch_exception` | `STATUS_STACK_BUFFER_OVERRUN` before anything else: log code and address, `NtTerminateProcess` (own), endless `NtTerminateThread`; a second entry logs that termination didn't take |
| H2 | `thread.c` | `fls_value_is_placeholder` (new), `RtlFlsFree`, `RtlProcessFlsData` | slot value `(void *)-1` is never passed to the callback (first 32 logged) |
| H3 | `loader.c`, `ntdll_names.h` | `load_dll`, `LdrResolveDelayLoadedAPI` | failed `load_dll`: first 60 per process and always the subsystem names; failed delay-load resolution: importer base, DLL, name or `#ordinal`, status (first 100) |
| H4 | `loader.c` | `LdrShutdownThread` | no module for the image base: log (first 8), skip its TLS callbacks |
| H5 | `loader.c`, `ntdll_names.h` | `use_builtin_xinput` (new), `find_dll_file` | the six XInput names (relative, any case) are searched in `C:\windows\system32` only; `MADEIRA_NATIVE_XINPUT=1` (read once) restores the search |
| H6 | `loader.c`, `ntdll_names.h` | `find_sysx64_dll` (new), `find_dll_file` (EC) | after `STATUS_INVALID_IMAGE_FORMAT`/`STATUS_DLL_NOT_FOUND`: `\??\C:\windows\sysx64\<base>[.dll]` if it fits `MAX_PATH`; success replaces the NT name and is logged |
| H7 | `time.c`, `qpc_arith.h` | `RtlQueryPerformanceCounter` (A64, EC) | `CNTVCT_EL0` after `isb`, `CNTFRQ_EL0` once (0 → system call forever), conversion to 100 ns, offset to `NtQueryPerformanceCounter` measured when missing or after ≥ 1 s of counter time, adopted only if first or larger by > 1 ms (lock-free CAS); i386 unchanged; `RtlQueryPerformanceFrequency` unchanged |
| H8 | `process.c` | `RtlWow64SuspendThread` | Part B1 |

## Part I: 32-bit heaps (`heap.c`, i386 only; 64-bit builds compile upstream's behaviour)

| item | function | change |
|---|---|---|
| I1 | `heap_init_madeira_policy` (new, declared in `ntdll_misc.h`), `loader_init` | reads the four switches (exactly `1`), logs one line if any is on; called once after `init_user_process_params` |
| I2 | `find_free_block` | grow sizes from `heap_policy.h`: COMPACT caps the grow size at 2 MB and retries with half, never below the need rounded to 64 KB, failing at that size; otherwise upstream's rule |
| I3 | `allocate_region` | COMBINED: committed size == reserved size → one `MEM_RESERVE | MEM_COMMIT` call |
| I4 | `allocate_region` | any policy on: a failed commit releases the reservation |
| I5 | `heap_reclaim_lfh` (new), `RtlAllocateHeap` | RECLAIM: on `STATUS_NO_MEMORY`, once per call, under the heap lock: per bin, flush the shared list and empty the affinity slots, release groups whose free mask (acquire load) is all free, push the others back; retry once if anything was released |
| I6 | `struct heap` (`large_allocs`, `large_frees`), `heap_allocate_large`, `heap_free_large`, `RtlAllocateHeap`, `heap_census` (new) | the allocation site in `ARENA_LARGE.__pad[0]` (0 for the heap's own blocks), per-heap counters; STATS: census every 65536th large allocation and on failure, under the heap lock, without allocating (static table guarded by a flag) |

## Part J: wow64.dll

Because B is 4 GB aligned, host → guest is the low 32 bits of any address in the window, so
upstream's `PtrToUlong` writes were already right; they are spelled `host_to_guest` where the spec
names them. Every guest → host direction was changed. Unchanged upstream files (`registry.c`,
`struct32.h`) are not delivered.

| item | file | function | change |
|---|---|---|---|
| J1 | `syscall.c` | `process_init`, `guest_base` (new) | `B` from class 1010 on the own process, first thing; error → 0 |
| J2 | `wow64_private.h`, `wow64_window.h` | `guest_to_host`, `host_to_guest`, `*_base` variants, `get_ptr`, `get_guest_value` (new), `addr_32to64(_base)`, `put_addr(_base)`, `unicode_str_32to64`, `secdesc_32to64` (absolute only), `objattr_32to64`, `token_*_32to64` | conversions with B; handles, sizes, flags, the packed APC parameter, the `IO_STATUS_BLOCK` cookie, self-relative offsets, completion keys and ceilings unchanged |
| J2 | `file.c` | `segments_32to64` (new), `NtReadFileScatter`, `NtWriteFileGather` | segment buffers (guest addresses in 64-bit fields) converted in a temporary copy |
| J2 | `process.c` | `NtCreateThreadEx` | start routine and parameter stay guest numbers (they go into 32-bit registers) |
| J3 | `syscall.c`, `wow64_window.h` | `wow_guest_base_for_process` (new), `query_process_pid`, `query_process_base` | own B 0 → 0; own/NULL → own B; 8-entry handle cache (release/acquire publication); a 0 base is not cached (QUESTIONS.md spec 18 Q11) |
| J4 | `process.c` | `process_params_32to64`, `ps_attributes_32to64`, `put_ps_attributes` | environment, package dependency data, strings; image name (maximum length = length before the redirect check), handle lists, group affinity, outputs and return lengths |
| J4 | `security.c` | token groups, object-type lists, label, default DACL, output SIDs | converted |
| J4 | `sync.c`, `system.c`, `virtual.c`, `process.c` | object/type names, directory entries, section transfer address, process names, mapped file name, image and thread names | written as `host_to_guest` (the caller's buffer) |
| J5 | `syscall.c` | `init_image_mapping`, `process_init`, `thread_init`, `check_gate` (new) | `BTCpuGetBopCode` / `__wine_get_unix_opcode` stored unchanged into `Wow64Transition`, the 32-bit dispatchers and `WOW32Reserved`; with a window each checked once (non-zero, below 4 GB), else one contract-violation line |
| J6 | `syscall.c` | `process_init` | `ntdll_handle` read through its low 32 bits and converted |
| J7 | `syscall.c` | `exception_record_32to64`, `exception_record_64to32`, `Wow64RaiseException`, `call_user_exception_dispatcher`, `call_raise_user_exception_dispatcher`, `Wow64ApcRoutine`, `Wow64KiUserCallbackDispatcher`, `Wow64PrepareForException` | address, nested record and the catalogue's parameters (wow64_window.h) both ways, at most 15; raised records host-side (`B + Eip`, `B + Eip − 1`, `B + 0xffffffff`); frames at `B + Esp`/`Sp` with guest addresses in registers and fields (i386 and ARM32); `BTCpuResetToConsistentState` only if bound |
| J8 | `syscall.c`, `process.c` | `process_init`, `wow64_NtSetInformationProcess` | `BTCpuNotifyProcessExecuteFlagsChange` resolved (optional) and called after a successful `ProcessExecuteFlags` set with a `ULONG` |
| J9 | `syscall.c` | `load_64bit_module`, `try_load_64bit_module`, `load_cpu_module`, `get_default_cpu_dll_name`, `log_module_failure` (new) | failures logged by name (ASCII) and the process ends with `STATUS_DLL_NOT_FOUND`; the registry's CPU module first, then the platform default if it differs |
| J10 | `syscall.c` | `wow64_NtContinueEx` | the raw value decides BOOLEAN (≤ 0xff) or pointer; only a pointer converts |
| J11 | `syscall.c` | `get_initial_context_i386` (new), `thread_init` | failed status or `Esp` 0: log, up to 64 retries with `NtYieldExecution`, then end the process; `MADEIRA_WOW_INIT_CTX_RETRY=0` (read once) keeps upstream's call |
| J12 | `virtual.c` | every virtual-memory thunk | target addresses with the target's B, the caller's buffers with its own, results back with the target's B; `MEM_ADDRESS_REQUIREMENTS` pointer converts, contents stay ceilings; free below 0x10000 passes through without notifications (with a window); `ArbitraryUserPointer` converts; `NtQueryVirtualMemory` per class, range checks and clamping on guest values; notifications get host addresses |
| J13 | `process.c` | `NtQueryInformationProcess`, `owner_window_base` (new), `put_ps_attributes` | PEB address with the target's B + 0x1000; `ProcessWow64Information` with the target's B, the other three classes not; TEB from `ThreadBasicInformation` low 32 bits + 0x2000; `PS_ATTRIBUTE_TEB_ADDRESS` in the owning process's window (uncached query; 0 → low 32 bits, logged once); stack base back to guest |
| J14 | `file.c` | `get_file_redirect` | with a window, a name buffer below 4 GB is logged (first 16) and not redirected |

## Part K: wow64win.dll

As in wow64, host → guest is the low 32 bits (and a value below 0x10000 is unchanged either way),
so upstream's 64 → 32 copies were already right (K8); every 32 → 64 site was checked.

| item | file | function | change |
|---|---|---|---|
| K1 | `wow64win_private.h` | `guest_window_base` (new) | class 1010 on the own process on the first conversion, cached (acquire/release), error → 0 |
| K2 | `wow64win_private.h`, `wow64win_window.h` | `guest_to_host`, `host_to_guest`, `get_ptr`, `addr_32to64`, `put_addr`, `unicode_str_32to64`, `secdesc_32to64`, `objattr_32to64` | NULL and values below 0x10000 unchanged, else B + value |
| K3 | `user.c`, `gdi.c` | `wow64_NtUserGetCursorFrameInfo`, `wow64_NtGdiGetRegionData`, `wow64_NtGdiGetTextCharsetInfo` | the cursor, region and DC read with `get_handle`; `HIMC` and `hwndTarget` stay handles |
| K4 | `user.c` | client menu names, create/MDI-create structures, window classes, menu item data, `COPYDATASTRUCT`, window positions, callback parameter blocks, IME parameters (not `himc`), cursor frame descriptors, clipboard data, `NtUserCallHwndParam` structures, menu info | every `UlongToPtr` of a pointer field → `guest_to_host` (module addresses included) |
| K4 | `gdi.c` | D3DKMT descriptors, print documents | every `p*`/`ph*` field, allocation-info arrays and members, monitored-fence CPU address, object attributes, handle and fence arrays; four `p*` fields upstream read with `UlongToHandle` |
| K5 | `user.c` | `wow64_NtUserCallOneParam`, `wow64_NtUserCallTwoParam`, `wow64_NtUserCallHwndParam` | per-code conversion from `wow64win_window.h` (the listed codes; the structure codes convert outer and nested pointers; everything else unchanged) |
| K6 | `user.c` | `NtUserGetProp`, `NtUserSetProp`, `NtUserRemoveProp` | strings and atoms both work through K2 |
| K7 | `user.c` | `message_call_32to64` | `lParam` of the listed messages (and `wParam` of `EM_GETSEL`, `SBM_GETRANGE`, `CB_GETEDITSEL`) converted once on entry and used for everything below |
| K8 | `user.c` | 64 → 32 callbacks | unchanged (truncation is host → guest) |

## Spec 18 clarification 1: the aarch64 farm in an x64 session

| file | function | change |
|---|---|---|
| `loader.c` | `find_farm_dll` (was `find_sysx64_dll`), `find_dll_file` | now compiled in both 64-bit builds: the ARM64EC build tries `\??\C:\windows\sysx64\<base name>` as before (Part H6), the aarch64 build tries `\??\C:\windows\sysaa64\<base name>`. Only after the normal search ended in `STATUS_INVALID_IMAGE_FORMAT` or `STATUS_DLL_NOT_FOUND`; same name rule and `MAX_PATH` check; on success the NT name is replaced. The aarch64 build logs the substitution once per DLL name (a 128-name table under the loader lock; a full table logs nothing more) |
| `ntdll_names.h` | `names_farm_path` (new; `names_sysx64_path` now calls it), `names_farm_fallback_due`, `names_log_once` (new) | the name rule for any farm folder, the two statuses, the log-once set |

## Spec 18 clarification 2: the fallback's log level

| file | function | change |
|---|---|---|
| `loader.c` | `find_farm_dll` | the substitution is logged with `WARN` on the `module` channel instead of `ERR`, once per DLL name in both 64-bit builds (the ARM64EC build logged every substitution before) |

## Clarifications to other specs

| item | file | change |
|---|---|---|
| spec 10 clarification 4 | see `out/09-ntdll-processes/CHANGES.md` | the unix side writes `ios_teb_tsd_offset` into each ntdll image |
| spec 10 clarification 5 | see `out/09-ntdll-processes/CHANGES.md` | the code bitmap is one allocation sized from the reported address space |
| spec 12 clarification 5 | see `out/12-win32u-ios/CHANGES.md` | win32u doesn't add the base to what wow64win already converted |
