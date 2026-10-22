# Spec 12: changes to Wine 11.4's `dlls/win32u/` (unix side)

The base is `refs/wine-11.4/dlls/win32u/` (LGPL-2.1-or-later), minus `main.c` (the Windows half).

**How the changes are delivered.** This repository holds no copy of the upstream files. It holds:

- `patches/*.patch`: unified diffs against Wine 11.4, applied by `build-ios.sh` in name order
  to a copy of the upstream tree it makes at build time (`A-*` for Part A, `B-*` for Part B,
  and so on). Every patched file gets a "Changed for MYIOSDECK" line in its licence header.
  Every change sits under `#ifdef WINE_IOS`, so the patched files still build upstream's
  behaviour without it. The hunks are small: they call into the new files, or include a
  `<file>_ios.h` (declarations and macros, right after the file's statics) and a
  `<file>_ios.inc` (the iOS code that needs the file's statics, at the end of the file).
- `win32u-unix/`: the new files (MIT), copied next to the patched upstream files by
  `build-ios.sh`, and `freetype.c`, a wrapper that compiles the unchanged upstream file.

(The spec asks for the modified files themselves. The session that wrote this could not copy
upstream files into the repository, so modified files exist only in the build directory,
`<out>/work/tree/dlls/win32u/`. See QUESTIONS.md, Spec 12, question 2.)

One line per change, by Part.

## Part A: build

- `build-ios.sh <wine tree> <wine build dir> <out dir>`: copies `dlls/win32u/` and
  `dlls/win32u/dibdrv/` minus `main.c`, and `include/ntuser.h`; applies `patches/`; adds
  `win32u-unix/`; compiles every `.c` for arm64, iOS 17.0, `-O2`, with `WINE_IOS=1`,
  `WINE_UNIX_LIB`, `__WINESRC__` and `_WIN32U_` into `libwin32u_unix.a`. Prints
  `<name> OK/FAILED` per file and the errors of failed files.
- A1: objects of `dibdrv/` are named `dibdrv-<file>.o`, so `dibdrv/bitblt.c` and `bitblt.c` don't
  overwrite each other.
- A2, `w32ios_rename.h` (forced include): `__wine_unix_lib_init` → `win32u_unix_lib_init`;
  `init_startup_info` (also in ntdll's unix side) and the eleven names the wineserver also
  defines (`alloc_user_handle`, `destroy_thread_windows`, `free_user_handle`,
  `get_virtual_screen_rect`, `get_window_thread`, `is_desktop_class`, `is_message_class`,
  `is_window_visible`, `mirror_region`, `send_notify_message`, `shared_session`) get a `win32u_`
  prefix. Found by comparing the external definitions of the three upstream trees.
- A2, `w32ios_init.c`: `win32u_unix_call_funcs` and `win32u_unix_call_wow64_funcs`, one entry
  each (`STATUS_NOT_IMPLEMENTED`): win32u has no unix calls, its entry points are system calls
  registered by `win32u_unix_lib_init`. Spec 09's loader must call `win32u_unix_lib_init`
  (QUESTIONS.md, Spec 12, question 1).
- A2, `patches/A-01-syscall.patch`: `win32u_unix_lib_init` calls `w32ios_unix_lib_init()`
  (the iOS per-process and app-wide set-up of the later Parts) after registering the system
  calls.
- A3, `w32ios_config.h` (forced include, first): the build's `config.h`, then
  `SONAME_LIBFONTCONFIG`, `HAVE_FONTCONFIG_FONTCONFIG_H`, `SONAME_LIBEGL`, `SONAME_LIBVULKAN`,
  `SONAME_LIBMOLTENVK`, `SONAME_LIBGNUTLS` undefined; `SONAME_LIBFREETYPE` and `HAVE_FT2BUILD_H`
  defined.
- A4, `build-ios.sh`: FreeType 2.13.3 compiled unchanged with its default module list and default
  options (bundled zlib; bzip2, png, HarfBuzz, Brotli off) into `libfreetype.a`, merged into
  `libwin32u_unix.a` with `libtool -static` (`ar` as fallback).
- A4, `freetype.c` (wrapper) + `w32ios_freetype.c`: upstream `freetype.c` is compiled unchanged
  with `dlopen`/`dlsym`/`dlclose` mapped to `w32ios_ft_dlopen` (succeeds for a name containing
  `freetype`, returns a token), `w32ios_ft_dlsym` (a table of the 31 FreeType functions
  `freetype.c` loads; `FT_MulFix` only when FreeType doesn't inline it, the same condition
  `freetype.c` uses) and `w32ios_ft_dlclose`.
- A5, `freetype.c` (wrapper): `CTFontCollectionCreateFromAvailableFonts` answers NULL, so
  upstream's `load_mac_fonts` adds no system font; only the prefix's fonts and Wine's font
  directory are scanned.

## Part B: one win32u for every pseudo-process

- `w32ios_proc.c`: `struct w32ios_proc` (`w32ios.h`) holds every per-process global of the audit
  below, one per pseudo-process. The registry has 64 slots keyed by (PEB from
  `ios_current_peb()`, process id); lookups are lock-free (a seqlock per slot,
  `w32ios_pp_find` in `w32ios_pure.c`); a new slot is chosen and written under a mutex and
  published last (`w32ios_pp_publish`, release store). A slot is recycled when its process has
  ended: a new process with the same PEB (two live processes can't share one), or, when all 64
  are live, a process the server reports as exited (`NtOpenProcess` /
  `ProcessBasicInformation`). Every thread caches its slot keyed by TEB, PEB and process id.
- In the patched files the upstream name of each per-process global becomes a macro for its
  field (`#define dce_list (w32ios_proc_current()->pp_dce_list)`, placed right after the
  upstream definition, which stays unused). Fields are named `pp_<name>` so no macro can rewrite
  a field access. Where the upstream name is also a struct member name (`dpi_context`,
  `disable_ime`), the few uses are patched instead.
- B1, `w32ios_rename.h` + `w32ios_proc.c`: `zero_bits` is a macro for `w32ios_zero_bits()`:
  a thread with `WowTebOffset != 0` gets `HighestUserAddress | 0x7fffffff` of
  `SystemEmulationBasicInformation`, any other 0; cached per thread (keyed by TEB).
  `patches/B-01-syscall.patch`: the old global stays 0 (its assignment in the init is compiled
  out). Uses: DC attribute buckets (dc.c), DIB sections (dib.c), the reply buffer of messages
  (message.c), Vulkan (compiled without the library).
- B2, `class_ios.h/.inc`: `NtUserInitializeClientPfnArrays` fills the caller's
  `client_procs` and `user32`; the first process also fills `winproc_array` and `user32_module`
  (fallback). `get_winproc_ptr`, `find_winproc` and `proc_to_handle` resolve the first
  `NTUSER_NB_PROCS` handles in the caller's table (`w32ios_procs_table`, `w32ios_procs_match`,
  `w32ios_procs_index`); `user32_module` reads as the caller's.
- B3: `register_builtin_classes` registers once per pseudo-process under the process's own
  mutex, held during registration; re-entry on the registering thread returns
  (`w32ios_builtin_action`). A refusal by the server (class exists) isn't treated as an error.
- B4/B5, `w32ios_desktop.c` (`get_desktop_window` calls it): the server is asked with `force`
  set; explorer.exe is never started; no desktop window gives 0 and the next call asks again. A
  thread of a process other than the session's first (recorded when `init_user` finishes):
  once per process, `register_desktop_class()`, ask again, `winstation_init()` if the thread
  has no desktop, ask once more; logged. `MADEIRA_CHILD_DESKTOP=0` turns this off.
- B6: `ios_wow_fixup_peb64_ptrs()` is called before `init_user` (which reads the code page
  tables through the PEB).
- B7: `get_winproc_ptr` first maps a value above 4 GB whose offset from `ios_wow_base()` is
  below 4 GB with `0xffff` in its upper 16 bits to that handle (`w32ios_winproc_handle`).
  `MADEIRA_WINPROC_HANDLE=0` turns it off.
- B8: `get_shared_desktop`, `get_shared_queue` (winstation.c), `get_shared_class` and
  `get_shared_window_class` (class.c): an object whose id reads 0 drops the cached pointer and
  returns `STATUS_INVALID_HANDLE` instead of spinning. `find_class` stops after 8192 entries
  (logged once) and answers "not found".
- B9: message.c reads the tick count through `w32ios_usd()` = `ios_get_real_usd()` (a zeroed
  page before it exists), never at `0x7ffe0000`.
- `w32ios_mem.c`, the GDI handle table: allocated once with `mmap` (outside ntdll's
  bookkeeping, so no process's death frees it); each process's `PEB->GdiSharedHandleTable`
  points at it when its slot is created (also from `win32u_unix_lib_init`); a 32-bit process
  gets a second mapping of the same pages (`vm_remap`, shared) in memory reserved below its guest
  ceiling, since 32-bit gdi32 reads the PEB64 field truncated to 32 bits.
- `w32ios_mem.c`, session memory: each view of `\KernelObjects\__wine_session` is replaced
  right after mapping by a second mapping that belongs to no process, and the view is
  unmapped, so the shared session blocks survive the process that mapped them.
- `w32ios_mem.c`, NLS data: the ANSI/OEM tables of font.c and `get_cptable`'s tables are
  copies (`NtGetNlsSectionPtr` gives the size), and the locale file of `get_locale_data` is a
  copy of its view (which is unmapped), not pointers into one process's views.
- Per-process registry handles: `hkcu_key`, `wine_fonts_key`, `wine_fonts_cache_key`
  (`font_ios.inc`), `config_key`, `volatile_base_key` (`sysparams_ios.inc`) are opened on first
  use in each process other than the session's first, after `init_user` has run.

### Audit of the globals and statics of `dlls/win32u/` (and `dibdrv/`)

Classes: **app** = truly app-wide (one session, one server, one registry, one display, one
font set, or a handle table whose handles are unique across processes); **thread** = per thread
already (in the TEB's `Win32ClientInfo` / `user_thread_info` or other thread data; nothing to
change); **process** = per process upstream, now per pseudo-process (field of
`struct w32ios_proc` unless the note says otherwise). Constant tables (`static const`) are
app-wide and not listed one by one.

| File | Global or static | Class | Notes |
|---|---|---|---|
| class.c | `system_info` | app | the machine's basic information |
| class.c | `winproc_array` | app / process | handles session-wide; entries `< NTUSER_NB_PROCS` resolved in the caller's table (B2), the global ones are the fallback |
| class.c | `winproc_used`, `winproc_lock` | app | handle allocation |
| class.c | `class_list` | process | `pp_class_list`; the server keeps classes per process |
| class.c | `user32_module` | process | B2 (`user32`); the global is the fallback |
| class.c | `init_once` (NtUserInitializeClientPfnArrays) | app | `init_user` runs once for the session (B4) |
| class.c | `init_once` (register_builtin_classes) | process | replaced by B3 |
| clipboard.c | `clipboard_mutex`, `cached_formats`, `formats_to_free` | app | cache of the session's clipboard data in unix memory, keyed by format and server sequence number |
| cursoricon.c | `icon_cache` | process | `pp_icon_cache`; keyed by module address and resource, handles owned by the loading process |
| d3dkmt.c | `d3dkmt_lock`, `objects`, `objects_end`, `objects_next` | app | D3DKMT handle table; handles unique across processes |
| d3dkmt.c | `d3dkmt_vidpn_sources` | app | VidPN ownership keyed by device handle |
| d3dkmt.c | `d3dkmt_vulkan_instance`, `once` | app | no Vulkan in this build |
| dc.c | `dc_attr_lock` | app | |
| dc.c | `dc_attr_buckets` | process | `pp_dc_attr_buckets`; memory of the process, below its ceiling |
| dce.c | `dce_list` | process | `pp_dce_list`; a DCE's DC has attributes in its process's memory |
| dce.c | `window_surfaces` | process | `pp_window_surfaces`; surface bits belong to the process |
| dce.c | `surfaces_lock` | app | |
| dce.c | `dummy_surface`, `header`/`dummy_data` (1x1 bitmap) | app | constant |
| dce.c | `last_idle` (flush_window_surfaces) | process | `pp_last_idle` |
| dce.c | `locked_hwnd` (LockWindowUpdate) | app | one locked window system-wide, as on Windows |
| defwnd.c | `f10_key`, `menu_sys_key` | process | `pp_f10_key`, `pp_menu_sys_key` |
| defwnd.c | `mesh`, `virtual_rect` | app | constant |
| driver.c | `null_user_driver`, `lazy_load_driver`, `user_driver`, `driver_load_error`, `warned` | app | one user driver (Part E installs it) |
| font.c | `hkcu_key`, `wine_fonts_key`, `wine_fonts_cache_key` | process | handles: `font_keys[]`, opened per process (`font_ios.inc`) |
| font.c | `font_smoothing`, `subpixel_orientation`, `antialias_fakes`, `font_gamma_ramp`, `ff_roman/modern/swiss_default`, `associated_charset`, `quietfixme`, `cache_num` | app | read once from the session's registry |
| font.c | `ansi_cp`, `oem_cp`, `utf8_cp`, `tables` (get_cptable) | app | one locale for the session; tables are copies (`w32ios_mem.c`) |
| font.c | `font_lock`, `font_subst_list`, `family_name_tree`, `family_second_name_tree`, `face_full_name_tree`, `font_links`, `gdi_font_list`, `unused_gdi_font_list`, `unused_font_count` | app | one font set (the prefix's fonts) for the session |
| font.c | `font_handles`, `next_free`, `next_unused` | app | font handle table, unique across processes |
| font.c | `locale_table`, `lcids_index`, `header` (get_locale_data) | app | the locale file is a heap copy, its view unmapped (`w32ios_copy_view`), so it outlives the process that loaded it |
| font.c | `font_funcs` | app | the FreeType backend |
| opengl.c, vulkan.c | `driver_funcs` | app | no EGL / Vulkan in this build |
| freetype.c | `library`, `FT_Version`, `FT_SimpleVersion`, `ft_handle`, `mappings_list`, `default_aa_flags`, `system_lcid`, `enabled` (x2) | app | one FreeType |
| freetype.c | `fontconfig_enabled`, `pattern_*` | app | not compiled (no fontconfig) |
| gdiobj.c | `gdi_shared` | app | one table, a view per 32-bit process (`w32ios_mem.c`) |
| gdiobj.c | `next_free`, `next_unused`, `debug_count`, `gdi_lock` | app | |
| imm.c | `thread_data_list`, `imm_mutex` | app | a list of per-thread entries |
| imm.c | `ime_updates`, `ime_update_count` | app | keyed by window |
| imm.c | `disable_ime` | process | `pp_disable_ime` (its two uses patched: also a member name) |
| input.c | `clipping_cursor` | app | a count tested only together with the calling thread's own clipping flag, so sharing it changes no answer |
| input.c | `enable_mouse_in_pointer` | process | `pp_enable_mouse_in_pointer` |
| input.c | `grab_pointer`, `grab_fullscreen` | app | host driver options, read once |
| input.c | `caret` | process | `pp_caret` |
| menu.c | `top_popup`, `top_popup_hmenu`, `exit_menu` | process | `pp_*` |
| menu.c | `menucharsize`, `od_item_height`, `menu_font`, `menu_font_bold`, `arrow_bitmap`, `sys_menu_bmp` | app | from the system font; GDI objects in unix memory |
| message.c | `user_shared_data` | app | B9 |
| message.c | `clk_msg` (process_mouse_message) | process | `pp_clk_msg` |
| opengl.c | `devices_egl`, `display_egl`, `display_funcs`, `pixel_formats`, `formats_count`, `onscreen_count`, `wgl_extensions`, `init_once` | app | no EGL in this build |
| palette.c | `SystemPaletteUse` | app | system-wide setting |
| palette.c | `hPrimaryPalette`, `hLastRealizedPalette` | process | `pp_*` |
| rawinput.c | `registered_devices`, `registered_device_count` | process | `pp_*`; freed when the slot is recycled |
| rawinput.c | `devices`, `rawinput_mutex`, `last_check` | app | the session's HID devices |
| scroll.c | `g_tracking_info`, `scroll_moving_thumb`, `scroll_timer_running`, `prev_pt`, `track_thumb_pos`, `last_click_pos`, `last_mouse_pos` | process | `pp_*` |
| spy.c | `spy_exclude` | app | debug option |
| syscall.c | `zero_bits` | process | B1: per thread through `w32ios_zero_bits()`; the global stays 0 |
| syscall.c | `syscalls`, `arguments`, `syscall_names`, `usercall_names` | app | |
| sysparams.c | `dpi_context` | process | `pp_dpi_context` (three uses patched: also a member name) |
| sysparams.c | `video_key`, `enum_key`, `control_key`, `config_key`, `volatile_base_key`, `base_keys`, `volatile_keys` | process | handles: `pp_*` / `base_keys[]`; `config_key`, `volatile_base_key` opened on use |
| sysparams.c | `gpus`, `sources`, `monitors`, `last_query_display_time`, `monitor_update_serial`, `display_lock`, `virtual_monitor`, `work_area` | app | one display (Part C) |
| sysparams.c | `emulate_modeset`, `decorated_mode` | app | host driver options, read once |
| sysparams.c | `thunk_lock_callback` | process | `pp_thunk_lock_callback` (each process's user32 sets it) |
| sysparams.c | `system_dpi` | app | |
| sysparams.c | `process_layout` | process | `pp_process_layout` |
| sysparams.c | `display_dc` | process | `pp_display_dc`; a DC's attributes are in its process's memory |
| sysparams.c | `display_dc_lock`, `user_mutex`, `user_lock_thread`, `user_lock_rec` | app | one USER lock |
| sysparams.c | `desk_pattern_path`, `desk_wallpaper_path`, `user_prefs`, `spi_loaded`, `system_colors`, `entry_*` (the parameter entries) | app | session settings (HKCU) |
| sysparams.c | `exiting_thread_id` | process | `pp_exiting_thread_id` |
| sysparams.c | `old_virtual_rect`, `hbitmap` (get_display_bitmap), `cx`, `cy` (get_dialog_base_units), `warn`, `brush_55aa` | app | GDI objects in unix memory, system font metrics |
| vulkan.c | `p_vk*`, `vulkan_handle`, `vulkan_funcs`, `once`, `frames`, `frames_total`, `prev_time`, `start_time`, `init_once` | app | no Vulkan in this build |
| window.c | `client_objects` | app | indexed by session-unique user handle; every lookup checks the owner process |
| window.c | `startup_info_flags`, `startup_show_window` | process | `pp_*`, initialised from the process's parameters |
| window.c | `surfaces_lock` | app | |
| window.c | `client_surfaces` | process | `pp_client_surfaces` |
| winstation.c | `session_lock`, `session_blocks`, `shared_session` | app | views detached from their process (`w32ios_mem.c`) |
| dibdrv/graphics.c | `font_cache`, `font_cache_lock` | app | glyph cache in unix memory |
| dibdrv/dc.c | `info`, `bits` | app | constant 1x1 bitmap |
| (thread data) | `user_thread_info`, `session_thread_data`, IMM thread data | thread | in the TEB / per-thread allocations |
| w32ios_proc.c (new) | `table`, `procs`, `table_lock`, `overflow_proc`, `overflow_once`, `session_peb`, `session_ready` | app | the registry itself |
| w32ios_proc.c (new) | `cache`, `ceiling` | thread | `__thread`, keyed by TEB |
| w32ios_mem.c (new) | `gdi_table`, `gdi_size`, `gdi_once` | app | |
| w32ios_*.c (new) | one-shot log flags and cached environment switches (`logged`, `warned`, `enabled`, `quiet`, `count`) | app | |
| w32ios_display.c (new) | `default_size`, `current_size`, `reported`, `first_session` | app | Part C, one monitor per session |
| sysparams_ios.inc (new) | `registry_gpu`, `software_luid` | app | Part C6/C7, written under the display lock |
| sysparams_ios.inc (new) | `display_lock_depth` | thread | `__thread` |

## Part C: the virtual monitor

`w32ios_display.c` holds the sizes; `sysparams_ios.inc` the code that needs sysparams.c's
statics; `patches/C-01-sysparams.patch` the hooks.

- C1: `lock_display_devices` takes upstream's virtual-monitor path (the one it uses for
  services): the monitor list holds `virtual_monitor` alone, gpus and sources stay empty, no
  driver is asked; `set_winstation_monitors` runs when forced or when the desktop's monitor
  serial moved. `unlock_display_devices` keeps track of the holder.
- C2: the session default is `MADEIRA_SCREEN_W` x `MADEIRA_SCREEN_H` (1024 x 768 unless both are
  positive numbers), logged with `MADEIRA_SCREEN_SRC`. It is read when a session starts
  (clarification 6): a pseudo-process with no parent creates its slot
  (`ProcessBasicInformation.InheritedFromUniqueProcessId` is 0, `w32ios_is_session_root`), which
  only the program the app booted through `__wine_main` has; so a second program of the same app
  run starts from the default again, while children (wineboot, system processes, a game started
  by its launcher) never reset it. (Before clarification 6 the trigger was "no other process
  alive", which `services.exe` and friends make never true.)
  Default and current size are single 64-bit atomics: no lock to read them. 32 bpp, 60 Hz; the
  virtual monitor's rectangle and work area follow the current size; dpi stays upstream's
  system dpi (96 by default).
- C3: `NtUserEnumDisplayDevices`: with no device name, index 0 is `\\.\DISPLAY1` (attached to
  the desktop, primary, VGA compatible) with the registry GPU's name, PCI id
  (`PCI\VEN_xxxx&DEV_xxxx&SUBSYS_00000000&REV_00`) and device key
  (`...\Control\Video\{guid}\0000`); for `\\.\DISPLAY1`, index 0 is `\\.\DISPLAY1\Monitor0`
  ("Generic Non-PnP Monitor", attached and active, `MONITOR\Default_Monitor\{monitor class}\0000`
  or its interface name with `EDD_GET_DEVICE_INTERFACE_NAME`). Everything else fails.
  `MONITORINFOEX.szDevice` of the virtual monitor is `\\.\DISPLAY1` (not upstream's "WinDisc");
  `get_display_rect("\\.\DISPLAY1")` is the monitor's rectangle. Device names are matched
  case-insensitively, both the adapter's and the monitor's, NULL/empty meaning the primary
  (`w32ios_match_device`).
- C4: `NtUserEnumDisplaySettings`: `ENUM_CURRENT_SETTINGS` the current size,
  `ENUM_REGISTRY_SETTINGS` the session default, index 0.. the list of `w32ios_build_modes`
  (current first, then the 37 standard modes in the spec's order whose pixels are at most the
  default's, four times with `MADEIRA_EXTENDED_MODES=1`, the current size once); past the end
  `ERROR_NO_MORE_FILES`. Unknown device: FALSE.
- C5: `NtUserChangeDisplaySettings`: unknown device `DISP_CHANGE_BADPARAM`; then
  `w32ios_mode_decision`: no devmode or no width/height field restores the default; a size in
  the list, the default or a standard mode up to four times the default's pixels is accepted
  (one missing field keeps the current value); anything else `DISP_CHANGE_BADMODE`. `CDS_TEST`
  only checks. `MADEIRA_VIRTUAL_MODE_SET=0` accepts and keeps the size. A change, outside any
  display lock: `update_display_cache(TRUE)` (the server gets the new monitor through
  `set_winstation_monitors`), the cached work area is dropped, `winios_display_mode_changed(w, h)`
  if the app has it, then `WM_DISPLAYCHANGE` (32, w | h << 16) to the desktop window and as a
  broadcast, as upstream broadcasts it.
- C5, last rule: `get_system_metrics(SM_CXSCREEN)` reports the session default to the server
  once per session, when the thread knows its desktop window and holds neither the user lock
  nor the display lock.
- C6: once per process (first `lock_display_devices` of the process, under the display lock),
  `w32ios_write_registry_gpu`: the other identity's entries are removed (its Enum\PCI device key,
  its DeviceClasses links, `Control\Video\{its guid}`, `DirectX\{its guid}`); then upstream's
  `write_gpu_to_registry` writes Enum\PCI\VEN_xxxx&DEV_xxxx&SUBSYS_00000000&REV_00\00000000,
  `Class\{display}\0000` with `DriverVersion`, `DirectX\{guid}`, keeping an earlier launch's GUID
  and LUID; `Control\Video\{guid}\0000` is created as a link to the Class key (as upstream's source
  writer does), so it holds the same values. Nothing is added to the topology. Identity: Apple
  `106B:0001` "Apple GPU", or NVIDIA `10DE:2544` "NVIDIA GeForce RTX 3060" with
  `DXMT_ENABLE_NVEXT=1`. `driver_vendor_to_version` gives `w32ios_driver_version` (NVIDIA
  `32.0.15.8157`, AMD `35.0.21025.1024`, Intel `35.0.101.6314`, others `35.0.10.1000`) and the
  `DirectX\{guid}` `DriverVersion` QWORD is that string packed (`w32ios_pack_version`). Memory
  4096 MB (Part D: `vram-mb`).
- C7: `D3DKMTOpenAdapterFromGdiDisplayName` for `\\.\DISPLAY1` (or its monitor's name, or empty)
  opens through `NtGdiDdDDIOpenAdapterFromLuid` with the registry GPU's LUID, or one LUID
  allocated once under the display lock (logged as a software identity) if no registry GPU
  could be written; `VidPnSourceId` 1. Failures are returned; other names fail.
- C8: at the end of `normalize_nonclientmetrics` (both `SPI_GETNONCLIENTMETRICS` variants):
  a caption height <= 0 or > 200 replaces the metrics with Windows' defaults (border 1, padded
  border 4 when the structure has it, scroll bars 17, caption 22 x 22, small caption 22 x 16,
  menu 18 x 19), logged once.

## Part D: a D3DKMT adapter (opt-in)

`w32ios_kmt.c` (Wine side), `w32ios_kmt_pure.c/.h` (layouts and answers, host-tested),
`w32ios_metal.c` (Metal and the settings file, no Wine headers), `patches/D-01-d3dkmt.patch`,
`patches/D-02-sysparams.patch`. Everything is gated by `MADEIRA_KMT_ADAPTER` (1, on, true, yes;
default off). Off, no answer changes: the registry GPU keeps its own LUID and 4096 MB.

- D1: the LUID is `bswap64(MTLDevice.registryID)` of `MTLCreateSystemDefaultDevice()`
  (`dlsym` + `objc_msgSend` cast to the method's type; the device is released). No Metal device:
  no adapter, logged once.
- D2: dedicated memory: `vram-mb` of `<Documents>/madeira.cfg` (`MADEIRA_DOCS_DIR`, else
  `$HOME/Documents`; spec 02 Part G format, read with `w32ios_cfg_find`) when it is 256 or more,
  else 4096 MB. The registry GPU (C6) gets this LUID and size.
- D3: `EnumAdapters2` (and `EnumAdapters`, which calls it) lists the adapter (`NumOfSources` 1);
  `OpenAdapterFromHdc` opens it for any DC (`VidPnSourceId` 1); `OpenAdapterFromDeviceName`
  opens it for its interface name
  `\\?\PCI#VEN_xxxx&DEV_xxxx&SUBSYS_00000000&REV_00#00000000#{1CA05180-A699-450A-9A0C-DE4FBE3DDD89}`
  (case-insensitive, `#` or `\`); `OpenAdapterFromGdiDisplayName` opens it through C7 (the
  registry GPU carries its LUID). Adapter objects remember the LUID they were opened for.
- D4: `QueryAdapterInfo` for that adapter (`w32ios_kmt_answer`): `DRIVERVERSION`
  `KMT_DRIVERVERSION_WDDM_3_1`; `UMD/KMD_DRIVER_VERSION` the C6 QWORD; `ADAPTERTYPE` render +
  display, not software; `PHYSICALADAPTERCOUNT` 1; `PHYSICALADAPTERDEVICEIDS` vendor and device
  (sub-ids, revision, bus type 0); `ADAPTERADDRESS` 0/0/0; `WDDM_1_2_CAPS` (primitive / dispatch
  boundary preemption, non-VGA, smooth rotation, per-engine TDR, CCD, gamma ramp, hardware
  cursor, hardware vsync), `WDDM_1_3_CAPS` (P-states, cross-adapter resources), `WDDM_2_0_CAPS`
  (64-bit atomics, GPU MMU), `WDDM_2_7_CAPS` (hardware scheduling supported and enabled),
  `WDDM_2_9_CAPS` (hardware scheduling stable and enabled), `WDDM_3_0_CAPS` 0 (no hardware flip
  queue), `WDDM_3_1_CAPS` 0 (no native GPU fence); `DRIVER_DESCRIPTION` and
  `ADAPTERREGISTRYINFO` the registry strings (name; DAC "Integrated RAMDAC"); `NODEMETADATA` node
  0 = 3D engine "3D" with GPU MMU; `GETSEGMENTSIZE` / `GETSEGMENTGROUPSIZE` the dedicated size;
  `ADAPTERPERFDATA(_CAPS)` an idle desktop (810 MHz memory clock of 7.5 GHz, 1 GB/s memory and
  250 MB/s PCIe traffic, fan stopped, 5 % power, 38 degrees; caps 360 GB/s, 31.5 GB/s,
  3000 rpm, 93/88 degrees). A short buffer or a non-zero physical adapter / node index:
  `STATUS_INVALID_PARAMETER`. Every other type keeps upstream's answer.
- D5: `QueryStatistics` for the adapter's LUID: adapter (1 segment, 1 node, 1 source), process
  adapter, process, segment 0 (commit limit = dedicated size, local aperture), node 0, sources;
  other ids `STATUS_INVALID_PARAMETER`. `QueryVideoMemoryInfo`: local budget = dedicated size,
  half of it available for reservation, usage 0; non-local budget 0 (upstream's answer without
  Vulkan).
- D6: the SDK layouts Wine lacks are `w32ios_kmt_pure.h`'s structs with `_Static_assert`s on
  their sizes (4, 12, 4, 4, 4, 4, 4, 4, 8, 4, 28, 12, 8192, 2080, 80, 24, 56, 64, 40 bytes).

## Part E: the iOS user driver

`w32ios_driver.c`, `w32ios_app.h` (the contract's weak hooks), `patches/E-01-message.patch`,
`patches/E-02-dibdrv-bitblt.patch`.

- Install: `win32u_unix_lib_init` → `w32ios_driver_install()` → `__wine_set_user_driver` with the
  iOS table, once, before anything could lazily load another driver (so no explorer, no
  `GraphicsDriver` lookup). Entries not set get upstream's null-driver entries. Desktop mode is
  `MADEIRA_DESKTOP=1`, game mode otherwise.
- `pUpdateDisplayDevices` describes the virtual monitor (the identity's GPU, one source, one
  monitor of the current size, the mode list); the Part C lock path never asks for it.
- E1: `winios_drv_post_mouse` sends an `INPUT_MOUSE` (x, y, flags, data, tick count) through
  `send_hardware_message` with no window. `winios_drv_post_key` sends an `INPUT_KEYBOARD`: `wScan`
  the low byte of `NtUserMapVirtualKeyEx(vk, MAPVK_VK_TO_VSC_EX, current layout)`,
  `KEYEVENTF_EXTENDEDKEY` added for a `0xe0`/`0xe1` prefix or a navigation key
  (`w32ios_key_extended`; `MADEIRA_NAV_KEYS_E0=0` keeps those unextended). Failures are counted;
  the first 8 events and every 256th are logged (`MADEIRA_QUIET=1` silences the event lines).
- E2 (message.c): `process_driver_events` calls the driver's `ProcessEvents` on every poll. A
  pure poll (nothing delivered, no wake or changed mask) skips `set_queue_mask` unless this
  thread's last such request is a second old (`w32ios_skip_queue_mask`); other polls keep
  upstream's rule. `wait_message` waits through `w32ios_wait_objects`: in desktop mode, or for a
  thread marked by E5, a wait-any wakes every 16 ms (`w32ios_slice_deadline`), runs
  `ProcessEvents`, and returns the queue handle's index when it delivered something; a timeout
  is returned only at the caller's own deadline. Other waits are upstream's.
- E3: `pSetCursor` converts the cursor's first frame to straight-alpha BGRA, at most 256 x 256
  (larger ones cropped to the top-left 256 x 256, hot spot clamped): colour with alpha as is,
  colour without alpha takes alpha from the AND mask, monochrome maps AND-only to transparent,
  XOR to white, neither to black (`w32ios_cursor_color` / `w32ios_cursor_mono`). NULL hides the
  cursor; the cursor last sent is only shown again. The image goes to
  `winios_cursor_set/show` in desktop mode, to `winios_direct_cursor_set/show` in game mode when
  `winios_direct_cursor_wanted()`, otherwise the app's `winios_pSetCursor` gets the handle. In
  game mode with the direct cursor, `pSetCursorPos` and every posted mouse move report the
  shared desktop object's cursor position through `winios_direct_cursor_pos`.
  `pDestroyCursorIcon` forgets a destroyed cursor as "last sent".
- E4: in desktop mode, and in game mode unless `MADEIRA_GAME_WINDOWS=0` (then upstream's null
  driver: an offscreen surface nobody presents): `pCreateWindowSurface` makes a surface on
  win32u's generic code backed by a 32 bpp top-down DIB of the rectangle's right x bottom; a
  surface with the same rectangle is kept; in game mode a window whose visible rectangle covers
  the virtual screen gets no surface (its picture is Metal's). Flushing calls
  `winios_surface_present` (whole surface, dirty rectangle); 0 makes the flush fail, so win32u
  keeps the damage. `pWindowPosChanging` remembers the visible rectangle for that decision.
  `pWindowPosChanged` calls `winios_window_frame` (visible rectangle, client rectangle, visible
  unless `SWP_HIDEWINDOW` or an empty visible rectangle) for every window in desktop mode and
  for top-level windows in game mode, invalidates (frame and children) a visible game-mode window
  with a surface that was just shown or reframed, then calls the app's `winios_pWindowPosChanged`.
- E5: in game mode, the first time a thread's own visible top-level window of at least 32 x 32
  that doesn't cover the virtual screen gets a surface, the thread is marked (thread-local, keyed
  by TEB) and its message waits slice (E2); the first 8 are logged. `MADEIRA_GAME_INPUT_WAKE=0`
  turns it off.
- E6: `w32ios_foreground_fix()` (called by Part F for slot 0's state): at most once a second per
  process, when the foreground window isn't the process's own, its largest visible top-level
  window of at least 320 x 200 becomes the foreground window. `MADEIRA_FOREGROUND_FIX=0` turns it
  off.
- E7: `pCreateWindow`, `pDestroyWindow`, `pShowWindow`, `pDestroyCursorIcon`, `pProcessEvents`
  forward to the app's hooks when linked, else upstream's null-driver behaviour (TRUE, nothing,
  `~0`, nothing, FALSE).
- Contract Section B: `dibdrv_PutImage` calls `winios_dump_srcbits` with the source bits
  (top row pointer, width, height, stride; the stride is negative for a bottom-up source).
- A new session forgets the cursor last sent.

## Part F: XInput through win32u

- F1, `patches/F-01-ntuser-h.patch` (`include/ntuser.h`): `NtUserCallTwoParam_GetGamepadState`
  appended after `NtUserAllocWinProc` (value 10, nothing inserted before it), and
  `static inline BOOL NtUserGetGamepadState( UINT index, UINT op, void *buffer )` (arg1 = index
  in bits 0-7, op in bits 8-15; arg2 = the buffer) with `NTUSER_GAMEPAD_STATE/CAPS/SET_VIBRATION`.
  Not under `WINE_IOS`: the header is shared with the Windows side, and an unknown code already
  answers 0 ("no controller").
- F2, `patches/F-02-sysparams.patch` + `w32ios_gamepad.c`: `NtUserCallTwoParam` answers the code
  with `w32ios_gamepad_answer` (`w32ios_pure.c`): index above 3 or no buffer → 0; State: when
  `winios_gamepad_get_state` reports a connected pad, `XINPUT_STATE` (16 bytes: packet number =
  `pad.packet`, buttons, triggers, sticks) and 1; Caps (connected): `XINPUT_CAPABILITIES`
  (20 bytes: type and sub-type gamepad, flags 0, buttons `0xf3ff`, triggers `0xff`, sticks
  `0xffc0`, motors `0xff` when `winios_gamepad_rumble_caps()` else 0) and 1; SetVibration: reads
  `XINPUT_VIBRATION` (two WORDs), calls `winios_gamepad_set_vibration`, 1. No allocation, no
  server request. Slot 0's State runs the foreground fix first (E6).
- `_Static_assert`s: `XINPUT_GAMEPAD` 12, `XINPUT_STATE` 16, `XINPUT_VIBRATION` 4,
  `XINPUT_CAPABILITIES` 20 bytes (`w32ios_pure.h`), `struct winios_gamepad` 20 bytes
  (`w32ios_pure.c`, against the contract header itself).

## Part G: tests

- `tests/test_w32ios.c` + `tests/run-host-tests.sh`: host tests of every pure part the spec
  lists (mode list, mode-change decision, device names, driver-version QWORD, cursor
  conversions, extended-key rule, slicing arithmetic, per-PEB tables, client procedures,
  built-in class registration, ceiling, winproc handles, XInput packing, Part D answers) plus the
  session default, the settings reader, `vram-mb` and the LUID swap. The logic they test lives in
  `w32ios_pure.c` and `w32ios_kmt_pure.c`, which the Wine-side files call.
- `TESTS.md`: the build, the host tests, and the on-device checks the spec lists, each with the
  log lines to look for.

## App Store rules (spec 00)

- D2: `w32ios_metal.c` no longer falls back to `dlopen` of Metal.framework by path: the app links
  Metal, `MTLCreateSystemDefaultDevice`, `sel_registerName` and `objc_msgSend` are looked up with
  `dlsym(RTLD_DEFAULT, ...)` (public SDK symbols, B1), and a missing one means no D3DKMT adapter
  (logged). The selectors used are the public `registryID` and `release`.
- A1: no executable memory anywhere in this part. `vm_remap` (`w32ios_mach.c`) only makes a
  second read/write (or read-only) mapping of data pages: the GDI handle table and the session
  views. Nothing adds `PROT_EXEC`.
- C1: no `fork`/`exec`/`spawn`; the explorer.exe start of upstream's `get_desktop_window` is
  never reached (Part B5).
- D1: the only file this part reads is `<Documents>/madeira.cfg`; it writes no file.
- D4/D5: no privacy-sensitive API and no API on Apple's required-reason list in this part's own
  code (no file timestamps, boot time, disk space, user defaults or keyboard lists).
- E: FreeType 2.13.3 under the FreeType License (FTL), Wine's win32u under LGPL-2.1+, linked
  statically: `build-ios.sh` with `patches/` and `win32u-unix/` is the relinking kit (it rebuilds
  `libwin32u_unix.a` from the upstream sources), see PROVENANCE.md.
- `tests/run-host-tests.sh` runs `check-app-store.sh` on this part after the host tests. The
  checker doesn't look at `.inc` and `.patch` files; the same pattern finds nothing in them
  either.

## How the patches were checked

No compiler or `patch` could run where this was written. Two awk checks were run on every
patch: each hunk's line counts match its header, and each hunk's old side (context and removed
lines) occurs verbatim in `refs/wine-11.4` (at the header's line for the first patch of a file,
offset by exactly the lines earlier patches add for later ones). No hunk's context includes a
line added by an earlier patch. The compile itself is CI's.

# Spec 17: smaller changes in win32u

Same delivery as spec 12: `patches/S17-*.patch`, applied after spec 12's (`S` sorts after
`A`-`F`), made with `diff -u` against the tree as GNU patch 2.8 leaves it after spec 12's
patches, and new MIT files in `win32u-unix/`. Each entry is tagged "spec 17" and the Part. The
pure rules are in `win32u-unix/w32ios_s17_pure.c` (host-tested, `tests/test_w32ios_s17.c`).

## Part F: GDI objects that 32-bit programs share (spec 17)

Spec 12 Part B already keeps one GDI handle table for the session outside every process's
memory, with a second mapping of the same pages inside each 32-bit process's guest window, and
gives every pseudo-process its own DC attribute buckets and DCE cache (above). Spec 17's rules are
added on top of that design rather than replacing it (QUESTIONS.md, spec 17 #5).

- spec 17 F1, `S17-F-gdi-shared.patch` (`win32u_private.h`): one inline function,
  `win32u_alloc_ceiling()`, returns the calling thread's ceiling (`w32ios_zero_bits()`, spec 12
  B1), and `zero_bits` is a macro for it, defined there after `wine/server.h`. Fix: spec 12 had
  that macro in the forced-include `w32ios_rename.h`, where it also rewrote the `zero_bits`
  members of the server protocol's request structs, so every file including
  `wine/server_protocol.h` failed to compile (found with a host syntax check); the macro is gone
  from `w32ios_rename.h`. Uses: `dc.c`, `dib.c`, `gdiobj.c` (compiled out on iOS), `message.c`,
  `vulkan.c`.
- spec 17 F1, `dib.c` `NtGdiCreateDIBSection`: a caller's section is mapped with the caller's
  ceiling (upstream passes none), so a 32-bit program gets bits inside its window.
- spec 17 F2, `w32ios_mem.c` `w32ios_gdi_session_32bit()`: `MADEIRA_GDI_SHARED_SECTION` set and
  not `0` → yes, `0` → no (both final, logged once); unset → `ios_wow_session_armed()` (spec 10
  clarification 3, weak; absent: no); a "yes" is final, a "no" is asked again until the session
  arms (QUESTIONS.md, spec 17 #6).
- spec 17 F4, `gdiobj.c` `handle_entry` (lookups, frees, deletes, client pointers, stock objects)
  and `alloc_gdi_handle`, `dc.c` `alloc_dc_attr`: `w32ios_gdi_publish()` first: a thread-local
  marker holds the PEB the thread last published for (one load and one compare when it matches);
  otherwise `w32ios_gdi_publish_slow()` writes the table's address into the PEB: the master for a
  64-bit pseudo-process (every session), for a 32-bit one in a 32-bit session its view inside the
  window, remembered by (process id, PEB) in 8 slots under a lock, the oldest overwritten. A view
  that can't be made is logged once for that process and the master is published. Spec 12's
  per-process attach (`w32ios_gdi_attach_proc`, from the unix library init) now goes through the
  same function. The view is spec 12's `vm_remap` alias, not a named section (spec 17 #5).
- spec 17 F5, `dc.c`: each DC attribute bucket records its creator (process id, PEB, 32-bit); in
  a 32-bit session `alloc_dc_attr` skips a bucket whose owner differs from the caller's (process
  id, PEB) when either side is 32-bit (`w32ios_s17_owner_ok`). The buckets stay per pseudo-process
  (spec 12), so this is a second guard.
- spec 17 F6, `dce.c`: each DCE records its creator (`alloc_dce`); in a 32-bit session
  `NtUserGetDCEx` neither recycles nor counts a cache DCE that isn't the caller's by the same
  rule, so the cache limit is per process for 32-bit programs. The DCE list stays per
  pseudo-process (spec 12).
- New: `w32ios_s17_pure.c/.h` (session decision, publish choice, view slots, owner rule, the
  G2 walk); audit: `w32ios_mem.c` `session_32bit`, `gdi_views`, `gdi_views_lock` (app),
  `w32ios_gdi_published_peb` (thread).
- spec 17 F, fix of the first commit: `S17-F-gdi-shared.patch` as first committed still had the
  comment that pointed at `w32ios_rename.h` and lacked the `zero_bits` macro in
  `win32u_private.h`; remade (the patches are checked to apply with no fuzz and to give the
  edited tree exactly).

## Part G: windows, clicks and the DIB driver (spec 17)

`patches/S17-G-windows-input.patch`:

- spec 17 G1, `window.c` `get_shared_window`: a window's shared object whose id reads 0 (freed
  with a dead pseudo-process) returns `STATUS_INVALID_HANDLE` instead of `STATUS_PENDING`, so the
  caller's loop under `user_lock` ends (spec 12 B8's rule, for windows).
- spec 17 G2, `window.c` `NtUserGetAncestor(GA_ROOT)`: when `list_window_parents` fails and the
  window exists, walk up with `GA_PARENT` while the current window has `WS_CHILD` (the desktop
  ends the walk; at most 64 steps) and return the full handle where it stops
  (`w32ios_s17_root_walk`); the first occurrence is logged (`GetAncestor(..., GA_ROOT): no parent
  list, root ...`). A window that doesn't exist still gives 0.
- spec 17 G3, `message.c` (`process_mouse_message`): after `WM_MOUSEACTIVATE` answered
  `MA_ACTIVATE` (or 0), a failed `set_foreground_window` no longer eats the click; the first
  occurrence is logged (`click on ...: top window ... not activated, click delivered`).
  `MA_ACTIVATEANDEAT` / `MA_NOACTIVATEANDEAT` are still eaten.
- spec 17 G4, `input.c` `NtUserActivateKeyboardLayout`: unsupported flags are logged once per app
  run.
- spec 17 G5, `dibdrv/bitblt.c` `dibdrv_PutImage`: `winios_dump_srcbits` is declared weak and
  called only when it is linked, there are source bits and the destination's visible rectangle
  is at least 640 x 400, as `(bits, w, h, w * 4)` with the visible rectangle's size. This
  replaces spec 12's unconditional call with the DIB's own size and stride (the contract's
  "hard dependency" becomes optional, as spec 17 says).

## Part A1 (spec 17)

- spec 17 A1 (clarification 1), `build-ios.sh`: `out/09-ntdll-processes/include/` (the shared
  `wine/unixlib.h` and `winternl.h`) comes before the build's and the Wine tree's `include/`.

## Clarification 5: what arrives converted from 32-bit programs (spec 18 Part K; checked in the spec 18 session)

No change needed. None of this tree's patches or `win32u-unix/` files adds the window base to a
pointer argument, an `NtUserCall*` argument (including `NtUserCallTwoParam_GetGamepadState`'s
buffer) or a message's `lParam`/`wParam`; the only guest-window logic on this path is Part B7's
`w32ios_winproc_handle`, which recognises the `B + 0xffffNNNN` window-procedure handles that
wow64win now sends.

## Spec 22 Part E (session reuse) and spec 12 clarification 7

- `patches/S22-E-window.patch` (`window.c`, after spec 12's and spec 17's patches): a parallel
  `client_owners[MAX_USER_HANDLES]` records the pseudo-process (its Windows process id, never
  reused, spec 16 H1) that put each slot's client object there (`ios_set_client_object`, used by
  `alloc_user_handle` and `set_user_handle_ptr`). `get_user_handle_ptr` and
  `next_thread_user_object` go through `ios_client_object`: the object counts only for its own
  process (checked before the object is read) and, for a window, only while its own full handle
  (`WND.handle`) equals the window's full handle from the shared entry. Enumeration skips every
  other slot instead of stopping at it; dispatch (`WIN_GetPtr` → `get_user_handle_ptr`) gets NULL,
  as upstream for a window with no client object.
- `w32ios_pure.c` `w32ios_client_object_ok` (new): the rule.
- Clarification 7's monitor reset: no change needed. `create_proc` (`w32ios_proc.c`) already starts
  a session (`w32ios_display_session_start`, `MADEIRA_SCREEN_W/H` read again) for any process with
  no parent (clarification 6), and every program the app starts in a kept session has none (spec
  08 Part C1 / spec 22 Part D2).
- Host test `tests/test_w32ios_s22.c` (in `run-host-tests.sh`).

## Clarifications 8 and 9: stand-in unix tables (opengl32, winspool, netapi32, kerberos, msv1_0)

Their upstream unix sides need what iOS doesn't have (OpenGL, CUPS, Samba, Kerberos, ntlm_auth)
and aren't built. Without tables the DLLs' init fails (`0xC0000142`), or their PE side calls
through a zero handle (Unity: a fault in `__wine_unix_call_dispatcher` at 0x8 after
`winspool.drv has no table`). Everything is in `unix-tables/`:

- `tables.txt`: one line per library: name, its header in the Wine tree, the enum, the count
  entry, the setup entries.

  | Library | Header | Entries | Setup entries (return `STATUS_SUCCESS`) |
  |---|---|---|---|
  | `opengl32.dll` | `dlls/opengl32/unixlib.h` (`enum unix_funcs`) | 3106 | `unix_process_attach`, `unix_thread_attach`, `unix_process_detach` |
  | `winspool.drv` | `dlls/winspool.drv/wspool.h` (`enum cups_funcs`) | 4 | `unix_process_attach` |
  | `netapi32.dll` | `dlls/netapi32/unixlib.h` (`enum samba_funcs`) | 7 | `unix_netapi_init` |
  | `kerberos.dll` | `dlls/kerberos/unixlib.h` (`enum unix_funcs`) | 12 | `unix_process_attach` |
  | `msv1_0.dll` | `dlls/msv1_0/unixlib.h` (`enum ntlm_funcs`) | 3 | none (QUESTIONS Spec 12 q18) |

- `<name>_ios.c` (`opengl32_ios.c`, `winspool_ios.c`, `netapi32_ios.c`, `kerberos_ios.c`,
  `msv1_0_ios.c`): each includes its library's upstream header from the Wine tree (never copied),
  after the headers its upstream unix file includes. It names the two tables
  `<name>_unix_call_funcs` and `<name>_unix_call_wow64_funcs` (what spec 09's `ios_unixlib_symbol`
  gives for the DLL file name) and includes `unix_table_ios.inc`.
- `unix_table_ios.inc`: the two tables of one library. Setup entries return `STATUS_SUCCESS` and
  do nothing. Every other entry has its own two-line function that passes only the library's
  report and its index to `w32ios_unix_unsupported`, and never reads its parameter block; the
  WoW64 table uses the same functions. `C_ASSERT`s, as in upstream's unix files: both tables have
  the enum's count of entries, and the generated list has every entry (no NULL holes).
- `gen-entries.sh`: writes `<name>_ios_entries.h` from the enum at build time
  (`W32IOS_UNIX_SETUP( unix_<name> )` and `W32IOS_UNIX_UNSUPPORTED( unix_<name> )` lines, and
  `W32IOS_UNIX_ENTRY_COUNT`). It fails on anything but plain `unix_<name>,` lines or a missing
  setup entry, and refuses to write into `refs/`.
- `unix_tables_log.c`: `w32ios_unix_unsupported` returns `STATUS_NOT_SUPPORTED`. The first such
  call of a pseudo-process (its PEB from `ios_current_peb()` and process id) in each library logs
  `<head>: <entry index> returns STATUS_NOT_SUPPORTED` through `__wine_dbg_output`. The heads are
  `[opengl32] no OpenGL on iOS`, `[winspool] no printing on iOS`, `[netapi32] no Samba on iOS`,
  `[kerberos] no Kerberos on iOS` and `[msv1_0] no NTLM on iOS`. No win32u slot is created for the
  process. A per-thread cache per library skips the lock after the first call.
- `unix_tables_pure.c`: `w32ios_unix_first_report`, the remembered (PEB, process id) pairs of one
  library: 32 of them, the oldest forgotten after that (a 33rd process at once would make an old
  one log again).
- `build-ios.sh` step 4b: for each line of `tables.txt`, runs `gen-entries.sh` on the given tree's
  header and compiles `<name>_ios.c` (`WINE_IOS`, `WINE_UNIX_LIB`, `__WINESRC__`, without win32u's
  forced includes, the library's own directory first on the include path); then it compiles
  `unix_tables_log.c` and `unix_tables_pure.c`. All of them go into `libwin32u_unix.a` with
  win32u's objects. `build-ios.sh` also refuses an out dir that is (or would be created) inside
  `refs/`.
- `out/09-ntdll-processes/unixlibs.txt`: the five DLLs added (as that file asks), so spec 09's
  `exports.txt` lists their ten table names and the app exports them.

Clarification 8 first lived in `opengl32-unix/`, with `opengl32_ios_log.c`, `opengl32_ios_pure.c`
and `w32ios_gl_unsupported`; clarification 9 made that code generic and moved it here.

## Clarification 10: each top-level program's own screen size (relayed 2026-10-22)

The app lets each game pick its shape and resolution, so a later top-level program of a kept
session may ask (`MADEIRA_SCREEN_W/H` in its launch environment) for another size than the
session's current one.

- `w32ios_display.c` `w32ios_display_session_start` (still run when a program with no parent
  creates its slot, clarifications 6 and 7, before the program's first win32u call is answered)
  now decides with `w32ios_launch_size`:
  - the first program of the app run: default and current from the environment, as before
    (`virtual monitor WxH, 32 bpp, 60 Hz (...)`);
  - the same size as the current one: the default becomes it (the modes list is built on it),
    nothing else (verbose line);
  - another size, no other user program running: default and current become it, the one-time
    report is armed again, `w32ios_monitor_resized()` runs, and `[win32u] virtual monitor WxH
    (was W0xH0) for a new program` is logged; no `WM_DISPLAYCHANGE` and no app callback;
  - another size while another user program runs: nothing changes, `[win32u] virtual monitor
    stays WxH: another program is running`.
  Before, every program with no parent reset the size unconditionally, but the shared work area
  and the server's monitor list kept the earlier program's size.
- `w32ios_proc.c` `w32ios_other_user_program_running` (new): reads the server's process list
  (`SystemProcessInformation`) only when the size would change. A process counts when it has a
  running thread, isn't the caller and `w32ios_is_user_program` says so for its
  `ProcessImageFileNameWin32` path and name. TRUE (size kept) when the list can't be read.
  Includes `ddk/wdm.h` (before win32u's headers, as `message.c`) for `NtSystemRoot`.
- `w32ios_pure.c` (new): `w32ios_launch_size` (the four cases) and `w32ios_is_user_program`:
  images inside the Windows directory (`KUSER_SHARED_DATA.NtSystemRoot`) are the session's
  system processes; helpers by name ("crash", "handler", "report", "helper", as spec 09's
  live-children count) don't count either; an unreadable path counts (QUESTIONS Spec 12 q19).
- `sysparams_ios.inc` `w32ios_monitor_resized` (new): marks the work area for recomputing
  (`spi_loaded[SPI_SETWORKAREA_IDX]`, app-wide, as after a mode change) and the monitor as
  pending for the server. `w32ios_lock_display_devices` sends it (`set_winstation_monitors`, as
  forced) on the first display query from a thread that knows its desktop window, before that
  query is answered. The display device, mode list, current and registry modes,
  `EnumDisplaySettings`, `GetSystemMetrics` and the monitor rectangles already read the shared
  size on every call.
- `tests/syntax-check-patched.sh` (new): a host `-fsyntax-only` check of the patched tree (TESTS.md).
- Nothing new is needed from spec 02 or spec 25: `MADEIRA_SCREEN_W/H` are read with `getenv` when
  the program's slot is created, so the bridge only has to set them in the process environment
  before `ios_session_start_program`, as it does before `__wine_main`.
