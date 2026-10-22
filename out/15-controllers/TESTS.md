# Spec 15: tests

## 1. Host tests (Part F)

```
CC=clang WINE_SRC=/path/to/wine-11.4 out/15-controllers/tests/run-host-tests.sh [build-dir]
```

Any C11 compiler (`CC`, default `cc`). `WINE_SRC` defaults to `refs/wine-11.4` of this
repository (not committed: fetch it as `refs/README.md` says); without it the two tests that
compile Wine's own code are skipped with a `SKIPPED:` line. Each test prints
`<name>: all N checks passed` or the failing checks with file and line; the script exits 1 if
any test fails or doesn't build.

From Git Bash on this machine:
`wsl.exe -d Ubuntu -- bash -lc "cd /path/to/pocketgames-engine/out/15-controllers && ./tests/run-host-tests.sh /tmp/t15"`.
Everything compiles with `-Wall -Wextra -Werror` (Wine's own `hidparse` code gets
`-Wno-sign-compare -Wno-unused-*` on top). The script ends with `check-app-store.sh` on this
folder. Last run (2026-10-22): all six tests pass with gcc 15 and with clang 21, and
the App Store check is clean.

What only the device can check: the wineserver device (Part B: objects, asyncs, the timer),
the registry writes through ntdll (Part C), and the Windows DLLs (Parts D and E: win32u
answers, dinput's acquire / unplugged states). The host tests cover the pure logic each of
those uses; sections 2 and 3 below cover the rest.

| Test | Needs Wine headers | Covers (spec 15 Part F) |
|---|---|---|
| `test_hidpad` | no | A1 identity lookup (`dualsense`, `generic`, name filtering, unknown values); A2 link, `\\?\`, `\??\`, `##?#`, instance id, the six hardware ids, buffer limits; A3 descriptor lengths (input 64 / output 48 / features 41, 20, 64; generic 14) with a small HID item walker; A4 DualSense report field by field (sticks incl. the flip of −32768, triggers, counter and its wrap, hat table incl. opposite directions, face/shoulder/trigger/Create/Options/stick/PS/touchpad/mute bits, gyro/accel live and at rest, timestamp ×3, touch ids new per touch-down and 7-bit wrap, 12-bit coordinates and clamping, battery byte incl. unknown/full/charging, disconnected = at rest); generic report field by field; A5 feature reports 0x05 (and SDL-style normalisation being exact), 0x09 (address order), 0x20 (0x0224), others zero; A6 output decoding (USB, Bluetooth with good and bad CRC, partial flags keep old fields, improved rumble and haptics select, non-effects reports ignored), CRC-32 check value; B2 release; B3 read pacing (4 ms / 1 ms rules); B7 log schedule; clarification 4's gate (`ios_hidpad_open_allowed`: opens while connected, refused while disconnected with the first 4 refusals logged, the count saturating; a disconnected snapshot read through an open handle is a pad at rest) |
| `test_regplan` | no | C: the four keys (paths, volatile tails), value names, types and data, the `HardwareID` multi-string, generic identity, no identity |
| `test_dinput_ios` | no (uses `pe/dlls/dinput/joystick_scale.h` with a local `struct object_properties` and `MulDiv`) | E3 object table (order, instances, offsets, usages, names); E4 axis values at rest under 0…65535 (32767) and −1000…1000 (0), with dead zone and saturation; full deflection and Y/Ry/Rz flip; Z = LT − RT; dead zone and saturation behaviour; POV 0…31500 in 4500 steps, −1 centred and for cancelled directions; trigger buttons past 30; button mapping |
| `test_hidparse` | yes | B6: the generic descriptor parsed by Wine's `hidparse.sys/main.c` (compiled through `server/ios_hidparse.c` exactly as the server compiles it): Game Pad collection, 14-byte input report, X Y Rx Ry Z caps (16-bit, −32767…32767, byte offsets 1/3/5/7/9), hat (4 bits, 1–8, null state, 0–315), 11 buttons in one range; the DualSense descriptor's report table; hidclass's report lookup (refuses output 0x31) |
| `test_sdl_sequence` | yes | The device report of 2026-10-21: the request rules (`hidpad/ios_hidpad_request.c`) and Wine's hidparse output driven in the order a session's readers use them: the gate (refused opens while no pad is published); win32u's raw input enumeration (raw input handle 0x100, collection information, descriptor of the exact size, one byte short refused); an hidapi-style enumerate and open (SetupDi path from the registry plan, attributes, serial/manufacturer/product and indexed strings, preparsed usage 1/5, input buffers); a DualSense driver's start (features 0x09, 0x20, 0x05 with the buffer as input and output or output only, short and unknown ids refused), first read 64 bytes, pacing, effects writes by WriteFile and `IOCTL_HID_SET_OUTPUT_REPORT`, the 0x31/short/empty write refusals, set feature, poll frequency, flush, unknown IOCTL; the generic identity; static checks that every copied IOCTL and status value equals Wine's |
| `test_sdl_sequence` (build 36) | yes | Clarification 5: `ios_hidpad_presence` (never published = absent, a gap under 1 s = present and forgotten on return, a full second = gone, an older sample = within the grace, published again = present); the report's case reworked: one report per 4 ms tick with nothing pressed, a minute idle without reads (not lost, the next read and a feature IOCTL succeed), a 0.9 s gap (reads complete with a pad at rest, nothing lost), unpublished for good (works until 1 s, then reads lost, IOCTLs and writes `STATUS_DELETE_PENDING`, nothing in the output snapshot, opens refused), published again (old handle stays lost, a new one works) |
| `test_pnp` | yes | Clarification 6: `ios_pnp_watch_update` (baseline without events, the HID pad first then slots in order, any non-zero = connected, up to 5 changes); `ios_pnp_record_build` (codes equal `DBT_DEVICEARRIVAL` / `DBT_DEVICEREMOVECOMPLETE`, class bytes equal Wine's `GUID_DEVINTERFACE_HID`, the HID pad's name = its A2 user path, slot 2's `IG_02` name, no HID identity refused); the queue (64 kept, the oldest dropped, order kept); `ios_pnp_record_to_broadcast` (`dbcc_size` with the terminator, type, class, an unterminated name cut); a session: windows registered for HID interfaces, with `DEVICE_NOTIFY_ALL_INTERFACE_CLASSES`, and for USB only (filters as user32 sizes them, matched as sechost matches them) get `WM_DEVICECHANGE` for the HID pad's arrival, an XInput pad's arrival and the HID pad's removal queued while sechost's thread was busy, and the XInput pad's removal; the USB window gets none |
| `test_xinput_sdl` | yes | Clarification 7: SDL2's XInput driver (detection over slots 3..0 with `XINPUT_FLAG_GAMEPAD`, `XInputGetCapabilitiesEx(1, i, 0)`, open with battery, `XInputGetStateEx` updates on packet changes, guide only through the Ex call, rumble, a second pad found, an unplugged pad dropped) against `pe/dlls/xinput1_3/xinput_host.h` over a fake win32u: slot 0 is found as 045E:028E version 0x0114, battery unknown/empty, headset disconnected, no DirectSound GUIDs, `XInputEnable(FALSE/TRUE)` zeroes the state and stops then restores the motors, process detach stops them, keystroke edges per slot, the lock never nested |
| `test_keystroke` | yes | D4: XInput's keystroke edges through `xinput_keystroke_edge` (button down/up once, one event per call in table order, guide ignored, triggers past 30, stick directions incl. the up-of-the-old-direction-first rule, precedence, release on a zeroed state) |

## 2. Build

`out/15-controllers/build-ios.sh <wine-11.4> <host build dir> <out>` prints one `CC` line per
file and `AR` per library: `libios_hidpad.a`, `libios_controllers_server.a`,
`libios_controllers_ntdll.a`. It fails if a file doesn't compile or if the server library lacks
`ios_hidpad_server_init` or the ntdll library lacks `ios_hidpad_publish`.

Link check: the app links the last two with `-Wl,-force_load,…` (QUESTIONS 15-12). With
`MADEIRA_HIDPAD=dualsense`, the server log must show the `[hidpad] dualsense 054C:0CE6 …` line
at start. If it doesn't, the weak hook stayed NULL: the library wasn't force-loaded.

The Windows-side files in `pe/` replace their upstream counterparts in the PE build (a later
spec builds the DLLs): `include/ntuser.h`, `dlls/xinput1_3/{main.c,xinput_keystroke.h}`,
`dlls/xinput1_{1,2,3,4}/Makefile.in`, `dlls/xinputuap/Makefile.in`,
`dlls/dinput/{dinput.c,dinput_private.h,joystick_hid.c,joystick_scale.h,joystick_ios.c,joystick_ios_map.h,Makefile.in}`,
`dlls/dinput8/Makefile.in`.

## 3. On the device

Logs: the server log (`[hidpad] …` lines, spec 08), ntdll's log (`[hidpad] … registry keys
written`, `[dinput-ios] …`, spec 09). Each check names what to look for.

### 3.1 An XInput game moves and rumbles

1. XInput mode (no `MADEIRA_HIDPAD`), one controller. Start an XInput game (any recent
   Windows game with controller support).
2. Sticks, triggers, buttons and the d-pad work; menus that use `XInputGetKeystroke` respond to
   single presses (one step per press).
3. Something that rumbles (a hit, an explosion) makes the controller's motors turn, if the app
   reports rumble capability (`winios_gamepad_rumble_caps`); otherwise the game sees no motors.
4. Pause menus that call `XInputEnable(FALSE)` stop the rumble; resuming brings it back.
5. Nothing about `[hidpad]` in any log, and no `[hidpad]` registry keys
   (`HKLM\System\CurrentControlSet\Enum\HID` has no `VID_054C`/`VID_1209` entry).

### 3.2 HID mode, `dualsense`: PlayStation glyphs, light bar, trigger effects

1. `MADEIRA_HIDPAD=dualsense`. Server log: `[hidpad] dualsense 054C:0CE6 v0100 "DualSense
   Wireless Controller" / "Sony Interactive Entertainment" / "02a14d616465", descriptor 273
   bytes, input report 64, output 48, feature 64, …; \??\HID#VID_054C&PID_0CE6&MI_03#… ->
   \Device\MyiosdeckHidPad0`. ntdll log: `[hidpad] dualsense: 4 of 4 registry keys written`.
2. Start a game with native DualSense support (one built on SDL 2.0.14+ or Steam Input-free
   native support). It shows PlayStation button glyphs; sticks, triggers, touchpad click and the
   PS button work.
3. The game's light bar colour reaches the pad (`[hidpad] effects report 1 (48 bytes): 02 …` then
   `snapshot: … lightbar 1 #rrggbb …`), and adaptive-trigger games stiffen the triggers
   (`triggers L1 R1` in a snapshot line). After 100 effects reports a tally line appears.
4. Gyro aiming (if the game has it and the pad has motion) follows the pad; without motion the
   game sees a pad lying flat.

### 3.3 HID mode, `generic`: a DirectInput8 HID game sees one controller

1. `MADEIRA_HIDPAD=generic`, `MADEIRA_HIDPAD_NAME` = the physical pad's name. Server log:
   `[hidpad] generic 1209:4D47 … "<that name>" / "MYIOSDECK" / ""`; ntdll log `4 of 4`.
2. A DirectInput8 game (or a joystick test tool) enumerates exactly one game controller named
   after the pad; all axes rest in the middle (no drift), the hat and 11 buttons work.

### 3.4 `MADEIRA_DINPUT_PAD=1`: a DirectInput-only game (Steins;Gate)

1. XInput mode, `MADEIRA_DINPUT_PAD=1`. ntdll log: `[dinput-ios] MADEIRA_DINPUT_PAD=1: the
   DirectInput joystick for host slot 0 is on` (once).
2. Steins;Gate (DirectInput only) sees one controller and its menus **don't scroll on their own**
   (every axis at rest reads the centre of the game's range).
3. With `MADEIRA_DINPUT_TRACE=1` the ntdll log shows, once a second while the game holds the
   device: `[dinput-ios] state X 32767 Y 32767 …` at rest (for the default range), the raw
   sample, each object's range / dead zone / saturation, and the game's data-format offsets.
4. Without `MADEIRA_DINPUT_PAD` (or with no controller connected) the game sees no controller.
5. Turn the controller off during play (clarification 3): the game reacts as to a lost device
   (it acquires again, or pauses with a "controller disconnected" prompt); with the trace on the
   log shows `[dinput-ios] slot 0 is empty: device lost until Acquire`. Turn it back on: input
   works again after the game's next `Acquire`, with no stuck buttons or drift.

### 3.5 Quitting a game while it rumbles stops the rumble

1. HID mode (`dualsense`): quit or force-quit a game while the pad rumbles. The rumble stops;
   server log: `[hidpad] last handle closed, motors stopped and trigger effects released: rumble
   1 L0 R0 …`. Trigger effects are released too.
2. XInput mode: same with an XInput game that rumbles: the motors stop when the game exits
   (xinput's process detach sends 0, 0).
3. The next game starts with still motors.

### 3.6 Read pacing (HID mode)

A game reading the HID pad gets about 250 reports a second while it reads continuously (4 ms
device clock), and a report within about 1 ms of a change; a game that reads slowly gets the
current state, not old reports (no input lag building up).

### 3.7 The gate (clarification 4, spec 21 Part B)

1. `MADEIRA_HIDPAD=dualsense` in XInput mode (the app publishes no HID pad). A game that looks
   for HID devices sees none it can open: the server log has up to four
   `[hid-pad] open refused: no PlayStation pad published` lines and no more; the game uses
   XInput. setupapi still lists the device (the registry entries are unchanged).
2. Switch to PlayStation mode, then start a DualSense game: it opens the pad and works as in 3.2.
3. Switch back to XInput mode while it runs (QUESTIONS 15-22): the game's open handle is lost
   as an unplugged pad's (server log: `[hid-pad] open handle lost: no PlayStation pad published
   any more`); the game drops the DualSense (or shows its "controller disconnected" prompt) and,
   if it also reads XInput, carries on with the XInput pad. Switching to PlayStation mode again
   doesn't revive the old handle; the game finds the pad only when it opens it again.
4. The device report's case: an SDL2 game with the app's on-screen pad, which publishes the
   DualSense at times while in Xbox style. The game must not end up with an idle controller: it
   follows the on-screen pad's presses (through XInput), as it does with a physical pad.

### 3.8 A PlayStation pad streams from the start (clarification 5)

1. PlayStation look, an SDL2 game with its own `SDL2.dll` (the build 36 report's game). Start it
   without touching the pad. Once in the menu, the pad works at once: no press during loading is
   needed. Server log: no `[hid-pad] open handle lost` line while the look stays PlayStation.
2. Leave the game idle in a menu for a few minutes, then press a button: it reacts.
3. Switch to the Xbox look: within about a second the log shows
   `[hid-pad] open handle lost: …` and `[pnp] removal: HID pad`; the game drops the PS5
   controller.
4. If (1) still fails, check the app's log for the HID store being unpublished for a second or
   more while the game loads (the engine absorbs gaps under 1 s only).

### 3.9 Hot-plug (clarification 6)

1. Start an SDL2 game in the Xbox look with no pad connected (no on-screen pad either), then
   connect a physical pad: server log `[pnp] arrival: XInput slot 0`; the game notices it
   without a restart (with `SDL_JOYSTICK_RAWINPUT=0`, see 3.10). Disconnect it: `[pnp] removal:
   XInput slot 0`; the game shows its disconnect behaviour.
2. In the PlayStation look switch Xbox → PlayStation while the game runs: `[pnp] arrival: HID
   pad`; SDL's HIDAPI rescans and adds "PS5 Controller" without a restart. PlayStation → Xbox:
   `[pnp] removal: HID pad` and the PS5 controller goes.
3. A game or tool that registers with `DEVICE_NOTIFY_ALL_INTERFACE_CLASSES` (dinput,
   windows.gaming.input) gets the same events. Only the device can show that sechost's thread
   reads the channel: a `WINEDEBUG=+service` trace has `reading device changes from the
   wineserver's channel`.

### 3.10 XInput pads under SDL2 (clarification 7)

1. Xbox look, the build 36 SDL2 game, the app's launcher setting `SDL_JOYSTICK_RAWINPUT=0`:
   the game adds "XInput Controller" (Xbox glyphs) for the on-screen pad and for a physical pad,
   and both move and rumble.
2. Without the hint (to confirm the diagnosis): the same game adds no XInput controller. If it
   does add one without the hint, the cause is elsewhere: report it with a `+xinput,+rawinput`
   trace.
