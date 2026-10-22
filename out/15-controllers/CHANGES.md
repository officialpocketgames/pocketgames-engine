# Spec 15: controllers on the Windows side

Base: upstream Wine 11.4 (`refs/wine-11.4`, LGPL-2.1-or-later). New files are MIT; modified
Wine files keep their LGPL headers and carry a "Changed for MYIOSDECK" line.

| Folder | Part | Goes into |
|---|---|---|
| `hidpad/` | A | both the wineserver library (spec 08) and ntdll's unix library (spec 09) |
| `server/` | B | the wineserver library (spec 08) |
| `ntdll/` | C | ntdll's unix library (spec 09) |
| `pe/` | D, E | the Windows-side DLLs (a later spec builds them) |
| `tests/` | F | host tests: `tests/run-host-tests.sh` |

One line per change, by Part.

## Part A: the HID pad's identity and reports

- A1: `hidpad/ios_hidpad.c` `ios_hidpad_identity_select` / `_get`: `dualsense` (054C:0CE6, `HID\VID_054C&PID_0CE6&MI_03`, Sony strings, serial `02a14d616465`) and `generic` (1209:4D47, `HID\VID_1209&PID_4D47`, product from `MADEIRA_HIDPAD_NAME` filtered to printable ASCII, else "MYIOSDECK Gamepad"; manufacturer "MYIOSDECK"; no serial); instance `9&4d616465&0&0000`; a fixed container GUID each.
- A1: `ios_hidpad_hardware_id`: the six hardware ids in A1's order (`&REV_0100` inserted before `&MI_`).
- A2: `ios_hidpad_link`, `_user_path` (`\\?\`), `_symlink_path` (`\??\`), `_registry_link_key` (`##?#`), `_instance_id`; `IOS_HIDPAD_DEVICE_OBJECT` = `\Device\MyiosdeckHidPad0`.
- A3: `ios_hidpad_dualsense_descriptor` (the 273 bytes of `refs/dualsense-public`); `ios_hidpad_generic_descriptor` (our own Game Pad, report 1 = 14 bytes: X Y Rx Ry Z 16-bit, hat 1–8 with null state, 11 buttons). Axes use −32767…32767 (QUESTIONS 15-1).
- A4: `ios_hidpad_build_dualsense_input` (report 0x01, 64 bytes: sticks 0–255 with Y flipped, triggers, counter, hat + face buttons, shoulder/trigger/Create/Options/stick bits, PS/touchpad/mute, gyro/accel from the store or flat at rest, timestamp µs×3, two touch points with new 7-bit ids per touch-down, battery byte) and `ios_hidpad_build_generic_input`; a disconnected store builds a pad at rest (battery unknown, QUESTIONS 15-2).
- A5: `ios_hidpad_build_feature`: 0x05 calibration (zero bias, ±8640 at 540 °/s, ±8192 = 1 g), 0x09 pairing info (address reversed), 0x20 firmware info (date/time strings, 0x0224 at 44–45), any other id zero-filled.
- A6: `ios_hidpad_decode_output`: USB 0x02 (≥ 48 bytes, block from byte 1) and Bluetooth 0x31 (≥ 78 bytes, block from byte 3, CRC-32 over 0xA2 + 74 bytes); folds rumble, trigger blocks, mic LED, power reduction (`& 0x77`), player LEDs, light bar when flagged (QUESTIONS 15-4, 15-5); anything else is ignored. `ios_hidpad_output_release` (B2) and `ios_hidpad_output_same`.
- A (for B3/B7): `ios_hidpad_read_ready` (the 4 ms / 1 ms rule) and `ios_hidpad_effects_log_kind` (first 8 in full, tallies at 100/1000/10000).
- A: `hidpad/ios_hidpad_contract.h` includes `interfaces/wine_unix_contract.h` with or without Wine's `windef.h` in front.

## Part B: the device, served by the wineserver

- B1: `server/ios_hidpad_server.c` `ios_hidpad_server_init()` (called once by spec 08 after the namespace and registry exist): reads `MADEIRA_HIDPAD` / `MADEIRA_HIDPAD_NAME`; with no identity it does nothing; otherwise it parses the descriptor (B6), creates the permanent device object `\Device\MyiosdeckHidPad0` (an object type of its own, no driver behind it) and the permanent symlink `\??\<link>` to it, and logs the identity (B7).
- B2: opening the device gives one file object per open (pseudo fd, `FD_TYPE_DEVICE`, the caller's options); when the last file object goes away, `ios_hidpad_output_release` stops the motors and releases the trigger effects in the snapshot and hands it to `winios_hidpad_set_output` (QUESTIONS 15-8).
- B3: reads refuse a buffer shorter than the input report (`STATUS_INVALID_BUFFER_SIZE`); complete at once by `ios_hidpad_read_ready` (4 ms since this handle's last report, or a changed store `packet` and 1 ms); otherwise wait in the handle's queue for the device clock, a 4 ms server timer armed only while a read waits, which completes one read per waiting handle per tick (QUESTIONS 15-9). Reports are built from `winios_hidpad_get_state` when delivered.
- B4: `WriteFile` and `IOCTL_HID_SET_OUTPUT_REPORT`: hidclass's checks (a buffer, an output report with byte 0's id, at least its length; else `STATUS_INVALID_USER_BUFFER` / `STATUS_INVALID_PARAMETER`), then A6 on the report's length; a changed snapshot goes to `winios_hidpad_set_output`.
- B5: IOCTLs answered as hidclass answers them: collection information (preparsed size, VID, PID, version, not polled), collection descriptor (`STATUS_INVALID_BUFFER_SIZE` when short), input report, get/set feature (A5; sets accepted), manufacturer/product/serial and indexed strings 1/2/3 (UTF-16 with the terminator, `STATUS_BUFFER_TOO_SMALL` when short), poll frequency get/set, number of input buffers get/set (2–512, stored per handle), flush queue, Wine's raw input handle (`0x100`); anything else `STATUS_NOT_SUPPORTED` (QUESTIONS 15-11). For direct and neither-method codes the request data is input then output buffer, so byte 0 is the report id either way. The volume information query answers `FILE_DEVICE_UNKNOWN`.
- B6: `server/ios_hidparse.c` compiles `refs/wine-11.4/dlls/hidparse.sys/main.c` unchanged (path in `IOS_HIDPARSE_MAIN_C`) with `HidP_GetCollectionDescription`, `HidP_FreeCollectionDescription`, `DriverEntry` and `parse_descriptor` prefixed `ios_hidparse_`, `ExAllocatePool`/`ExFreePool` mapped to `malloc`/`free`, and `TRACE`/`WARN`/`ERR`/`FIXME`/`TRACE_ON`/`wine_dbg_sprintf` compiled out; `ios_hidparse_parse` keeps a copy of the first collection's preparsed data and the report table; `ios_hidparse_find_report` is hidclass's lookup.
- B7: logs the identity at start, the first 8 effects reports in full (hex and the resulting snapshot), tallies at 100, 1000 and 10000 effects reports (rumble, no rumble bit, triggers, light bar, LEDs), Bluetooth reports with a bad CRC, and the release on last close.
- Build: `build-ios.sh` builds `libios_hidpad.a` (Part A), `libios_controllers_server.a` (Part B, compiled with upstream's server headers overlaid with spec 08's and its `ios_config.h`) and `libios_controllers_ntdll.a` (Part C); the last two must be force-loaded (QUESTIONS 15-12).

## Part C: the registry entries

- C: `ntdll/ios_hidpad_regplan.c` (pure): `ios_hidpad_registry_plan` builds the four keys: `HKLM\System\CurrentControlSet\Enum\<device id>\<instance>` (`ClassGUID` HIDClass's GUID, `Class` `HIDClass`, `DeviceDesc` "HID-compliant game controller", `Mfg`, `HardwareID` multi-string of A1's six ids, `ContainerID`, `ConfigFlags` 0); `...\Control\DeviceClasses\{4d1e55b2-…}\##?#<link>` (`DeviceInstance`); its `#` subkey (`SymbolicLink` `\?\<link>`); `#\Control` (`Linked` 1). Each key is a stable base plus the pad's own levels, created volatile (QUESTIONS 15-13).
- C: `ntdll/ios_hidpad_publish.c` `ios_hidpad_publish()` (called by spec 09 clarification 1): nothing without `MADEIRA_HIDPAD`; otherwise publishes the identity whose `\??\<link>` symlink the server created (`NtOpenSymbolicLinkObject`), creating keys level by level with `NtCreateKey` from `\Registry\Machine` and values with `NtSetValueKey`, and logs how many of the four keys were written (QUESTIONS 15-14).

## Part D: XInput asks win32u first

- D: `pe/include/ntuser.h` (full copy of upstream's): `NtUserCallTwoParam_GetGamepadState` appended after `NtUserAllocWinProc` (value 10) and `static inline BOOL NtUserGetGamepadState( UINT index, UINT op, void *buffer )` (index in bits 0–7, op in bits 8–15), as spec 12 Part F1 specifies.
- D1: `pe/dlls/xinput1_3/main.c` (shared by xinput1_1/1_2/1_4/xinputuap): `XInputGetState` / `XInputGetStateEx` answer a connected host slot with one `NtUserGetGamepadState(index, 0, …)` call, no lock, before the HID path and before the discovery thread starts; a win32u that answers 0 leaves upstream's path unchanged. Battery and DirectSound GUID queries answer for a host slot too (QUESTIONS 15-16).
- D2: `XInputGetCapabilities(Ex)` uses the Caps operation for a connected host slot (Ex adds 045E:028E v0x0114, QUESTIONS 15-15); `XINPUT_FLAG_GAMEPAD` is honoured as upstream.
- D3: `XInputSetState` for a connected host slot stores the motors per slot and sends them with the SetVibration operation; `XInputEnable(FALSE)` zeroes the reported state and silences the motors, `XInputEnable(TRUE)` sends the stored motors again.
- D4: `pe/dlls/xinput1_3/xinput_keystroke.h` (new, LGPL: upstream's edge detector moved unchanged into `xinput_keystroke_edge(index, cur, last, keystroke)`); `check_for_keystroke` asks the host slot first (its own edge memory per slot), then the HID path, both through that function.
- D5: `DllMain(DLL_PROCESS_DETACH)` stops the host motors the program left running.
- D: `pe/dlls/xinput1_{1,2,3,4}/Makefile.in`, `pe/dlls/xinputuap/Makefile.in`: `win32u` added to `IMPORTS` (for `NtUserCallTwoParam`). xinput9_1_0 has its own `main.c` and is unchanged.

## Part E: an opt-in DirectInput joystick

- E: `pe/dlls/dinput/joystick_ios.c` (new, MIT): one joystick backed by host slot 0 (`NtUserGetGamepadState(0, 0, …)`), plugged into dinput's `device.c` like the HID joystick: `ios_joystick_enum_device`, `ios_joystick_create_device`, a vtbl with poll, acquire, unacquire, enum_objects and get_property (no read, no effects).
- E1: only when `MADEIRA_DINPUT_PAD` starts with `1` (read once, logged once); disabled, no pad in slot 0, or on Wine's `HKCU\Software\Wine\DirectInput\Joysticks` disable list (`device_instance_is_disabled`): `DIERR_DEVICENOTREG`, checked on every enumeration and every `CreateDevice`.
- E2: fixed instance GUID, product GUID in dinput's PIDVID form for 1209:4D47, `DI8DEVTYPE_GAMEPAD | DI8DEVTYPEGAMEPAD_STANDARD` for version ≥ 0x800 (the HID joystick's mapping below), no `DIDC_FORCEFEEDBACK`, `guidFFDriver` GUID_NULL (QUESTIONS 15-18).
- E3: `pe/dlls/dinput/joystick_ios_map.h` (new, MIT, pure): the 19 objects in order (X, Y, Z, Rx, Ry, Rz, POV, 12 buttons; LONGs from offset 0, button bytes from 28) and `ios_joy_from_gamepad` (Y/Ry/Rz negated with −32768 clamped, Z = (LT − RT) × 128, POV 0–7 or 8, trigger buttons past 30).
- E4: signed logical ranges, scaled by `pe/dlls/dinput/joystick_scale.h` (new, LGPL: `sign_extend`, `scale_value`, `scale_axis_value` moved unchanged out of `joystick_hid.c`, which now includes it), so `DIPROP_RANGE`, `DIPROP_DEADZONE` and `DIPROP_SATURATION` apply exactly as for a HID joystick (QUESTIONS 15-1, 15-19).
- E5: `pe/dlls/dinput/dinput.c`: `EnumDevices` offers the iOS joystick before the HID loop (a separate call); `CreateDeviceEx` asks it first and falls back to the HID path on `DIERR_DEVICENOTREG`. `dinput_private.h` declares both functions.
- E6: `MADEIRA_DINPUT_TRACE=1`: at most once a second while acquired, the scaled state, the raw sample, each axis's and the POV's range / dead zone / saturation, and the game's data-format offsets (QUESTIONS 15-20).
- E: `pe/dlls/dinput/Makefile.in`, `pe/dlls/dinput8/Makefile.in`: `joystick_ios.c` added to `SOURCES`, `win32u` to `IMPORTS`.

## Part F: tests

- F: `tests/run-host-tests.sh` builds and runs `test_hidpad`, `test_regplan`, `test_dinput_ios` (no Wine headers) and `test_hidparse`, `test_keystroke` (Wine 11.4's `include/`, `WINE_SRC`); `TESTS.md` lists what each covers, the build checks and the on-device checks (XInput game moves and rumbles; `dualsense` glyphs, light bar, trigger effects; `generic` seen by a DirectInput8 HID game; `MADEIRA_DINPUT_PAD=1` with Steins;Gate; rumble stops when a game quits). (Run since: see "Clarifications (round 2)".)

## Integration

- Spec 08 calls `ios_hidpad_server_init()` (weak) after the namespace and registry exist; spec 09 calls `ios_hidpad_publish()` (weak) after the drive links. Both are defined here; link `libios_controllers_server.a` and `libios_controllers_ntdll.a` with `-force_load` and `libios_hidpad.a` normally (QUESTIONS 15-12).
- The Windows-side files in `pe/` replace upstream's when the PE DLLs are built (a later spec); `pe/include/ntuser.h` carries spec 12 Part F1's addition, so whichever copy the PE build takes must have it.

## Clarifications (round 2)

- Clarification 2 (QUESTIONS 15-16): `pe/dlls/xinput1_3/main.c` `XInputGetBatteryInformation` for a connected host slot and the gamepad answers `BATTERY_TYPE_UNKNOWN`, `BATTERY_LEVEL_EMPTY`, `ERROR_SUCCESS` (was wired/full); the headset stays disconnected.
- Clarification 3 (QUESTIONS 15-19): `pe/dlls/dinput/joystick_ios.c` `ios_joystick_lost`: a poll that finds slot 0 empty puts an acquired device in `STATUS_UNPLUGGED`, takes it off the acquired list with `dinput_hooks_unacquire_device` and returns `DIERR_INPUTLOST`, leaving the stored state and queued events alone; device.c answers `DIERR_INPUTLOST` to `Poll` / `GetDeviceState` / `GetDeviceData` until `Acquire`, which succeeds with or without a pad. The trace logs the loss.
- Spec 00: `tests/run-host-tests.sh` compiles with `-Werror` and ends with `check-app-store.sh` on this folder; PROVENANCE lists the libraries and licences. Host tests now run in WSL: all five pass with gcc and with clang (`-O1` and `-O0`).

## Spec 17 Part A1

- spec 17 A1, `build-ios.sh`: `out/09-ntdll-processes/include/` (spec 17's shared `wine/unixlib.h` and `winternl.h`) comes before the Wine tree's `include/` for the server and ntdll parts.

## CI round 1

- Build: `build-ios.sh` stages spec 11 Part A2's `ios_syscall_frame.h` (from `out/09-ntdll-processes/ntdll-unix/`) next to spec 08's server headers, as spec 08's build does (its `ios_suspend.h` → `ios_thread_layout.h` include it; without it `ios_hidpad_server.c` didn't compile and `libios_controllers_server.a` lacked `ios_hidpad_server_init`). The server objects also get spec 08's spec-16 settings (`-DHAVE_LINUX_NTSYNC_H=1 -DIOS_USYNC=1`, `out/16-sync-ios/include`), and the script refuses an out dir inside `refs/`.

## Clarification 4 (spec 21 Part B): the HID pad in every session

- `hidpad/ios_hidpad.c`, `.h`: `ios_hidpad_open_allowed(connected, &refusals, &log)`, the gate's
  rule (pure, host-tested): an open is allowed only while the HID store reports the pad
  connected; a refusal is counted, and the first 4 are flagged for the log.
- `server/ios_hidpad_server.c`: `hidpad_device_open_file` asks `winios_hidpad_get_state` and
  refuses a new handle with `STATUS_NO_SUCH_DEVICE` while the pad is unpublished, logging
  `[hid-pad] open refused: no PlayStation pad published` the first 4 times. Handles already
  open are untouched and read a pad at rest (A4). The library exports
  `int myiosdeck_hidpad_gate = 1;`, which the app links against (spec 21 Part C's
  `mid_hidpad_gate`), so an app built for the gate fails to link against a server without it.
  The registry entries (Part C) are unchanged: setupapi lists the device in every session that
  has `MADEIRA_HIDPAD`.
- `tests/test_hidpad.c`: `test_gate`. `TESTS.md`: the gate's row and device check 3.7.

## Device report of 2026-10-21 (an SDL2 game)

- B4/B5: the answers to WriteFile and every IOCTL moved, unchanged in behaviour, from `server/ios_hidpad_server.c` into `hidpad/ios_hidpad_request.{h,c}` (new, LGPL, plain C, in `libios_hidpad.a`): `ios_hidpad_write`, `ios_hidpad_ioctl`, `ios_hidpad_input_size`, and `ios_hidparse_find_report` (moved from `server/ios_hidparse.c`). The server keeps handles, asyncs, the read clock, logging and the store call (`finish_request`), and checks with `C_ASSERT` that the request rules' copies of the NTSTATUS values, IOCTL codes and `HID_COLLECTION_INFORMATION` layout match Wine's. `HID_COLLECTION_INFORMATION` and ULONG replies are now written byte by byte (little endian, the same bytes on arm64).
- F: `tests/test_sdl_sequence.c` (new): the gate, win32u's raw input enumeration (raw input handle, collection information, descriptor of the exact size), an hidapi-style enumerate and open (SetupDi path from Part C, attributes, strings, indexed string with a language id, preparsed caps, input buffers), a DualSense driver's start (feature reports with the buffer as input and output, and with the id only in the output buffer), first read = 64 bytes (USB), pacing, effects writes through WriteFile and `IOCTL_HID_SET_OUTPUT_REPORT`, hidclass's refusals, the generic identity; static checks of every copied IOCTL and status value against Wine's headers.
- Clarification 4 unchanged: a gated pad keeps refusing opens with `STATUS_NO_SUCH_DEVICE`; why, and the question about device-arrival notifications, in QUESTIONS 15-21.
- Build: `build-ios.sh` adds `hidpad/ios_hidpad_request.c` to `libios_hidpad.a`.

## Device report, second data point (physical pad works, on-screen pad doesn't)

- Clarification 4 changed for open handles (QUESTIONS 15-22, please amend the spec): a handle that sees the HID store report no pad is lost for good, as a removed device's under hidclass. `hidpad/ios_hidpad_request.c`: `ios_hidpad_handle_init`, `ios_hidpad_handle_lost`, `ios_hidpad_read_decide` (lost / now / wait, B3's pacing moved into `struct ios_hidpad_handle`), `ios_hidpad_read_report`; `ios_hidpad_ioctl` and `ios_hidpad_write` take the handle and the store snapshot and answer `STATUS_DELETE_PENDING` on a lost handle. `server/ios_hidpad_server.c`: reads (immediate and on the device clock) fail with `STATUS_DEVICE_NOT_CONNECTED` and fail every waiting read of the handle (`lose_handle`, first 4 losses logged as `[hid-pad] open handle lost: ...`); the store is read for every IOCTL and write. New opens are still refused while unpublished.
- F: `test_sdl_sequence` adds the report's case: a reader holds the DualSense, the app goes back to Xbox style, the handle's reads, IOCTLs and writes fail and stay failed after the pad is published again, a new handle works.

## Device report of build 36 (clarifications 5–7)

- Clarification 5 (PlayStation look: the pad was added but dead unless pressed during loading): presence with a 1 s grace. `hidpad/ios_hidpad_request.{h,c}`: `struct ios_hidpad_device` gains `published`, `was_published`, `unpublished_since_us`; `ios_hidpad_presence(dev, connected, now)` is true while the store reports the pad and until it has reported it unpublished for `IOS_HIDPAD_UNPUBLISH_GRACE_US` (1 s), false for a pad never published; `ios_hidpad_handle_lost(handle, present)`; `ios_hidpad_read_decide(dev, handle, pad, queued, now)`, `ios_hidpad_write(dev, handle, pad, now, …)` and `ios_hidpad_ioctl(…, pad, now, …)` lose a handle only when the pad is no longer present. An idle handle is never lost; reads keep completing (a pad at rest during a gap) with the counter and timestamp advancing. `server/ios_hidpad_server.c`: `store_present` feeds every store sample through `ios_hidpad_presence`; opens, the read clock, reads, writes and IOCTLs use it.
- Clarification 6 (hot-plug): `pe/dlls/sechost/ios_pnp.h` (new, LGPL): the channel's names, the record (`code`, class GUID, UTF-16 path) and `ios_pnp_record_to_broadcast`. `hidpad/ios_hidpad_pnp.{h,c}` (new, LGPL, in `libios_hidpad.a`): `ios_pnp_watch_update` (baseline, then HID pad presence and slots 0–3), `ios_pnp_record_build` (HID pad: its A2 user path; slot n: `\?\HID#VID_045E&PID_028E&IG_0n#9&4d616465&1&000n#{4d1e55b2-…}`; both under `GUID_DEVINTERFACE_HID`), `ios_pnp_queue_push/pop` (64 per handle, oldest dropped). `server/ios_hidpad_server.c`: `pnp_init` (called first in `ios_hidpad_server_init`, with or without a HID pad identity) creates `\Device\MyiosdeckPnp` and `\??\MyiosdeckPnp`; each open is a listener whose `ReadFile` returns one record (at once or when the next event happens; a buffer smaller than a record fails `STATUS_INVALID_BUFFER_SIZE`); a 100 ms timer samples the stores, logs `[pnp] arrival/removal: …`, delivers to every listener, and loses the HID pad's open handles once it is gone past the grace. `pe/dlls/sechost/service.c` (full copy of upstream, modified): `device_notify_proc` first tries `ios_pnp_listen`, which reads the channel and hands each record, as a `DEV_BROADCAST_DEVICEINTERFACE_W` with an empty path, to upstream's `notification_filter_matches` / `device_notify_copy` / callbacks; without the channel (desktop Wine) plugplay is used as before.
- Clarification 7 (Xbox look: SDL never adds the XInput pad): `pe/dlls/xinput1_3/xinput_host.h` (new, LGPL): the host-slot code moved out of `main.c` unchanged, plus `host_get_capabilities_ex`, `host_get_battery`, `host_get_dsound_guids` (the bodies of the Ex, battery and DirectSound entry points' host branches), over `XINPUT_HOST_CALL` / `XINPUT_HOST_LOCK` / `XINPUT_HOST_UNLOCK`, which `main.c` maps to `NtUserGetGamepadState` and `host_crit`. No change in what the DLL answers; the move lets a host test run the shipped code. The SDL finding and the `SDL_JOYSTICK_RAWINPUT=0` hint for the app: spec 15 clarification 7, QUESTIONS 15-23.
- F: `test_sdl_sequence` adds `test_presence_grace` and reworks the report's case (steady reads, a minute idle, a 0.9 s gap, loss only after 1 s, no revival, a new handle works); `tests/test_pnp.c` (new: watch, records, queue, broadcast, and a window registered for HID arrivals, one with `DEVICE_NOTIFY_ALL_INTERFACE_CLASSES`, one for USB only, through hot-plug of the HID pad and an XInput slot); `tests/test_xinput_sdl.c` (new: SDL2's XInput detection, open, update and rumble against `xinput_host.h` over a fake win32u, finding slot 0 as 045E:028E). `run-host-tests.sh` builds both and adds `-I pe/dlls/sechost`.
- Build: `build-ios.sh` adds `hidpad/ios_hidpad_pnp.c` to `libios_hidpad.a` and `pe/dlls/sechost` to the include path (the server includes `ios_pnp.h`). Spec 19 builds `pe/dlls/sechost/service.c` in place of upstream's (no Makefile change: same file name, no new imports).
