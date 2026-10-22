<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 the MYIOSDECK author -->

# Spec 14: what was changed and added

Upstream base: Wine 11.4 (`refs/wine-11.4`). Layout of this folder:

| Path | What | Includes Wine headers |
|---|---|---|
| `build-ios.sh` | Part A1: `libwinemedia_unix.a` | |
| `build-ffmpeg.sh`, `ffmpeg/README.md` | Part A2: the static, LGPL-only FFmpeg | |
| `common/` | guest pointers (A4), logging | no |
| `winegstreamer-unix/` | Part B | only `wg_ios_*.c`, `wg_transform_ios.c`, `wg_parser_ios.c` |
| `audio-unix/` | Part C | only `ios_audio.c` |
| `dwrite-unix/` | Part D: wrapper around upstream `freetype.c` | yes |
| `pe/metahost.c` | Part E: modified `dlls/mscoree/metahost.c` (PE) | yes |
| `tests/` | Part F: host tests | |

## Part A: build

- **A1.** `build-ios.sh <wine tree> <wine build dir> <ffmpeg prefix> <out dir>` compiles every
  `.c` of `common/`, `winegstreamer-unix/`, `audio-unix/` and the dwrite wrapper for iOS arm64
  (iOS 17.0, `-O2`, `-fvisibility=hidden`, `WINE_IOS=1`, `WINE_UNIX_LIB`, `__WINESRC__`) into
  `libwinemedia_unix.a`. Generated headers come from the build directory's `include/`. Files
  that must not see Wine headers (the platform-decoder object `wg_platform_apple.c`, the RemoteIO
  object `ios_audio_apple.c`, and the pure files shared with the host tests) are compiled with
  no Wine include path. They talk to the rest through plain-C interfaces
  (`winegstreamer-unix/wg_platform.h`, `audio-unix/ios_audio_pure.h`).
- **Name clashes.** Every external symbol of the new code has a prefix of its own (`wg_ios_`,
  `wma_`, `wgp_`, `ia_`, `iom_`, `ios_audio_`); upstream's entry points (`wg_transform_create`
  and so on) are `static` here. The only upstream file compiled, `dlls/dwrite/freetype.c`, has
  only static functions besides its two tables, which the wrapper renames
  (`__wine_unix_call_funcs` → `dwrite_upstream_unused_funcs`; the wrapper defines the real
  `dwrite_unix_call_funcs`). FFmpeg is built with `-fvisibility=hidden`; its external names
  (`av_*`, `avcodec_*`, `swr_*`, `ff_*`) are not defined by specs 08–13's libraries. CI's
  link of all of them is the check.
- **Linking.** The call tables are reached only by `dlsym(RTLD_DEFAULT, …)` (spec 09 Part A):
  the app links `libwinemedia_unix.a` with `-Wl,-force_load,…`, then FFmpeg's
  `libavformat.a libavcodec.a libswresample.a libavutil.a` in that order, and the frameworks
  AudioToolbox, VideoToolbox, CoreMedia, CoreVideo and CoreFoundation. The tables are
  `__attribute__((used, visibility("default")))`. `build-ios.sh` checks with `nm` that every
  table is defined in the library.
- **A2.** `build-ffmpeg.sh` checks the tarball's SHA-256, applies no patches, configures with
  exactly the options of the spec (see `ffmpeg/README.md` for each one and why), refuses to
  build when FFmpeg's `config.h` reports anything but "LGPL version 2.1 or later", installs the
  four static libraries, and prints the enabled decoders, demuxers, parsers, protocols and
  muxers from `config_components.h`. `--disable-autodetect` is added to the listed disables so
  no other external library sneaks in.
- **A3, checked against this track's spec 09 implementation** (`out/09-ntdll-processes`): its
  loader turns a DLL name into `<lower-case base name>_unix_call_funcs` /
  `…_wow64_funcs`, binds the 32-bit table for the Wow64 info classes, and afterwards calls
  `<name>_unix_lib_init` if one is linked, returning its status. `MemoryWineUnloadUnixLib`
  succeeds and does nothing; the "handle" is the table's address.
  - `winegstreamer.dll` → `winegstreamer_unix_call_funcs` / `…_wow64_funcs`. Spec 09 binds the
    64-bit table for every 64-bit caller, so the gate is `winegstreamer_unix_lib_init`: a caller
    with no guest window (`ios_wow_base() == 0`, a 64-bit process) gets `STATUS_DLL_NOT_FOUND`
    unless `MADEIRA_WG_64BIT=1` (QUESTIONS 14-1).
  - `DWrite.dll` → spec 09 lower-cases the name → `dwrite_unix_call_funcs` / `…_wow64_funcs`.
  - The audio driver: mmdevapi asks for `wine<name>.drv` by name. `winecoreaudio.drv` →
    `winecoreaudio_unix_call_funcs` / `…_wow64_funcs`, and `wineios.drv` → `wineios_unix_call_funcs`
    / `…_wow64_funcs` (the same entries). Spec 09 matches the derived symbol name exactly, not
    "contains" (QUESTIONS 14-2). `winepulse`, `winealsa`, `wineoss` have no table and fail as
    upstream, so `coreaudio` is chosen from the default list.
- **A4.** `common/ios_media_wow.h`: `iom_guest_ptr()` (rule 1: 0 stays NULL, else
  `ios_wow_base() + value`) and `iom_guest_addr()` (a pointer written back must be inside the
  caller's window, `ios_wow_in_window()`; otherwise 0 is stored and the caller fails the call
  with `E_FAIL`). Every 32-bit layout written here has `_Static_assert`s on its size and
  offsets. Handles and 64-bit object handles are passed through unchanged.
- **A5.** The audit of globals is per Part below ("Globals").
- **A6.** Nothing to build here: the i386 Wine build needs `--enable-winegstreamer` so that
  `winegstreamer.dll` and `wmadmod.dll` are built without GStreamer; their PE code is upstream's.

## Part B: winegstreamer's unix side (`winegstreamer-unix/`)

New code; none of upstream's GStreamer files are compiled. Upstream's `unixlib.h` is used
unchanged for the argument blocks.

| File | Wine headers | What |
|---|---|---|
| `wg_ios_main.c` | yes | B1: both tables (designated initialisers by `enum unix_funcs` index, `_Static_assert` on the count), `wg_init_gstreamer` succeeds, the seven `wg_muxer_*` return `STATUS_NOT_IMPLEMENTED`; A3's `winegstreamer_unix_lib_init` gate |
| `wg_ios.h` | yes | the 32-bit `struct wg_media_type32` (24 bytes) and `_Static_assert`s on `wg_media_type` (32), `wg_format` (120), `wg_parser_buffer` (32), `wg_sample` (48) |
| `wg_handles.c` | no | B1: handles = magic 0x5747, type, generation, slot; looked up in a table, never dereferenced, so a parser handle passed as a stream or a stale handle gives `STATUS_INVALID_HANDLE` |
| `wg_transform_ios.c` | yes | B2's `wg_transform_*` entries and their 32-bit thunks |
| `wma_core.c` | no | B2 rules 3–14 on libavcodec/libswresample |
| `wma_params.c` | no | B2 rules 2, 7, 8, 9 as pure functions (FFmpeg's derived values re-derived) |
| `wg_avlog.c` | no | B2 rule 13: FFmpeg's warnings and errors to the log, tagged by the owner set on the calling thread, at most 64 per app |
| `wg_parser_ios.c` | yes | B3: the parser and its 32-bit thunks |
| `wg_parser_pure.c` | no | B3's pure rules: frame layouts and conversions, S24 packing, reorder, trims, queue limits, the AAC PTS queue, channel masks |
| `wg_platform.h` | no | the plain-C interface to the platform decoders (`wgp_ops`, replaceable by host tests) |
| `wg_platform_pure.c` | no | B4: avcC / hvcC parsing, the ES_Descriptor cookie |
| `wg_platform_apple.c` | no | B4: VideoToolbox and AudioConverter |

- **B1, 32-bit entries.** Own thunks for: parser connect (URI), push_data (buffer),
  get_current_format / get_codec_format / enable (`struct wg_format *`), get_buffer
  (`struct wg_parser_buffer *`), copy_buffer (destination), get_tag (buffer and size);
  transform create (two `wg_media_type32`), get/set_output_type (`wg_media_type32`),
  push_data / read_data (`struct wg_sample *`, and `wg_sample.data`, a guest address). Every
  pointer goes through `iom_guest_ptr()`. All other entries share the 64-bit entry; their blocks
  have `_Static_assert`s showing they hold only handles and fixed-width scalars at the same
  offsets. No entry writes a pointer back into a 32-bit field.
- **B2 notes.** The decoder is opened with rate, block align, the bit rate of rule 2,
  `bits_per_coded_sample` = `wBitsPerSample` and the channel layout from the mask (when it has as
  many bits as channels) or the default one. Packets are copied into an `av_new_packet` buffer
  (zero padding included). Each packet is decoded into a scratch buffer, judged by rule 8, then
  committed; a packet with no kept frames commits silence (rule 6), one that decoded to the wrong
  length keeps its samples. The search (rule 7) retries the same packet; candidates come from
  `wma_build_candidates()` rebuilt before each step with the cursor kept as an index. When the
  list runs out with a best candidate, the decoder goes back to it, re-decodes the packet once
  and conceals from then on (QUESTIONS 14-6). `MADEIRA_WMA_SEARCH=0` disables the search; the
  family's other codecs never search. Logs: `[wma] accepted bit rate … flags …` once per
  transform; failure lines at most 8 per transform; a summary at drain, flush and destroy only
  when packets failed or tail bytes were dropped since the previous summary, with the counts
  since then (Clarification 3); a clean stream prints none. `MADEIRA_WMA_DUMP` writes the
  rule-14 file. A read whose buffer can't hold one whole frame answers like no PCM: size 0,
  `MF_E_TRANSFORM_NEED_MORE_INPUT`, the PCM stays queued (Clarification 2).
- **App Store (spec 00).** No JIT, exec, fork or private API; `check-app-store.sh` is clean
  and runs from `tests/run-host-tests.sh`. `dlopen`/`dlsym` appear only as the dwrite wrapper's
  own static table (no system library is opened). No privacy-sensitive API (render only, no
  capture). Required-reason API: `mach_absolute_time` (`ios_audio_apple.c`, the audio clock) is
  on Apple's "system boot time" list — reason **35F9.1** (elapsed time between events in the
  app). FFmpeg 7.1.1 is LGPL-2.1+ and linked statically: the app must ship a relinking kit
  (the object files of `libwinemedia_unix.a` and the app, and `build-ffmpeg.sh`/`build-ios.sh`)
  so a user can relink against a modified FFmpeg (spec 00 E1).
- **B3 notes.** The read protocol has its own lock, so the PE side's read thread never waits for
  the parser lock that a thread blocked in a read holds; `disconnect` closes the protocol (wakes
  both sides) before taking the parser lock. Reads ask for at most 1 MiB; a read at or past the
  connect's file size answers end of file without a request. The demuxer gets a 64 KiB I/O buffer
  and `probesize` 256 KiB; streams that can't be exposed are `AVDISCARD_ALL` before analysis.
  MP3/WAV timestamps are made relative to the stream's start time (QUESTIONS 14-4).
- **B4 notes.** VideoToolbox decodes synchronously (flags 0, then
  `VTDecompressionSessionWaitForAsynchronousFrames`); the output callback retains the picture
  and hands it to the parser with its packet's PTS (passed through the frame refcon, not
  VideoToolbox's timestamps). `kVTInvalidSessionErr` drops the session (`WGP_E_SESSION_LOST`,
  logged by the parser); the next packet makes a new one. AudioToolbox's input callback hands
  over the one packet, then answers its own "no more input" status, which counts as success.
- **Globals (A5).** All app-wide: the handle table (`wg_handles.c`, holding objects of every
  pseudo-process; a handle is only meaningful to the process that got it), the `[wma]` and
  av-log caps, the parser's dedupe table, the transform and dump counters, `wgp_ops`, the
  once-installed av_log callback, `MADEIRA_DIAG`. Per object: everything else. A transform or
  parser belongs to the pseudo-process that created it; one left behind by a pseudo-process that
  ended is not freed (nothing tells this library; QUESTIONS 14-13).

## Part C: the audio driver (`audio-unix/`)

New code. Upstream's `dlls/mmdevapi/unixlib.h` gives the argument blocks.

| File | Wine headers | What |
|---|---|---|
| `ios_audio.c` | yes | the 37 entries in both tables (`winecoreaudio_…` and `wineios_…`, designated initialisers by `enum unix_funcs` index, `_Static_assert` on the count), streams, positions, render buffers, the timer thread, MIDI/aux answers, the 32-bit thunks |
| `ios_audio_pure.c` | no | C2 endpoint layout and mix format bytes, C3 ring capacity, C4 format classes, C6 the mixer, C8 gains, C10 the lead and the pacing decision |
| `ios_audio_apple.c` | no | C5 the RemoteIO unit (float32 packed interleaved), the monotonic clock (`mach_absolute_time`), absolute sleeps (`mach_wait_until`), the time-constraint policy (`thread_policy_set`) |

- **C1.** 32-bit thunks for every block with a pointer or a pointer-sized field: main_loop
  (event), get_endpoint_ids, create_stream, release_stream (timer thread handle),
  get_render_buffer, get_capture_buffer, is_format_supported, get_loopback_capture_device,
  get_mix_format, get_device_period, get_buffer_size, get_latency, get_current_padding,
  get_next_packet_size, get_frequency, get_position, set_volumes, set_event_handle, test_connect
  (its `priority` moves to offset 4), get_prop_value, midi_init, midi_out_message,
  midi_in_message, midi_notify_wait (`struct notify_context32`), aux_message. Handles are
  sign-extended (`LongToHandle`), never converted. start, stop, reset, timer_loop,
  release_render_buffer, release_capture_buffer, set_sample_rate, is_started, midi_get_driver,
  midi_release and process attach/detach share the 64-bit entry (asserted layouts). Every
  32-bit layout has `_Static_assert`s.
- **C2.** One render endpoint: `iOS Null` / `ios-null` laid out after the endpoint array as
  upstream's drivers do; too small → the size needed and
  `HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)`. Capture: none, `S_OK`. The mix format is
  written byte by byte (`ia_mix_format`). `get_loopback_capture_device` answers `E_NOTIMPL`.
- **C3.** Streams live in a 16-slot table; a handle is magic 0x1a, the slot's generation and
  the slot, checked on every call (`AUDCLNT_E_NOT_INITIALIZED`, logged at most 8 times). The
  render scratch of a 32-bit caller is allocated with `NtAllocateVirtualMemory` and a 2 GB
  guest limit (spec 10 E4), checked with `ios_wow_in_window()`; the 32-bit get_render_buffer
  writes its guest address or, if it isn't in the window, 0 and `E_FAIL` (the handout is
  cancelled). Upstream's state errors are kept (QUESTIONS 14-18).
- **C5.** The unit is created by the first mixable stream with that stream's rate and 2
  channels (1 if mono), kept until the app exits; a failure is never retried. A mono device gets
  (L + R) / 2. `start` stores `started` before starting the unit; the unit is stopped when the
  started count reaches 0.
- **C6.** `ia_mix` clears the output, then per stream keeps the read position's fraction
  between passes (Clarification 4): `phase` in units of 1/rd stream frame; device frame f reads
  stream frame `floor((phase + f·rs)/rd)` from the play position, the pass consumes
  `floor((phase + n·rs)/rd)` frames (or what the ring holds) and keeps the remainder, so the
  frames consumed match device frames × rs/rd within one frame (tested over 3000 passes of 44.1
  kHz on 48 kHz). The fraction restarts at 0 on `start`, `reset` and `set_sample_rate` (an
  atomic request the mixer takes at its next pass) and after an underrun pass. It folds channels
  with the gains, publishes `played` with release, clamps to ±1 (NaN → 0, counted), counts
  short passes and missing frames per stream, and advances `passes`. Writes stop at the byte
  size the unit hands over.
- **C7.** Null mode: padding 0, position = wall-clock frames since `start` accumulated across
  stops (also across `set_sample_rate`). `reset` waits for one render pass (a pass that saw the
  stream started may still run) before zeroing.
- **C10.** `drv_timer_loop`: the lead from `MADEIRA_AUDIO_LEAD_MS` (`ia_parse_lead_ms`), the
  time-constraint policy (10 ms / 0.5 ms / 5 ms, preemptible) when the lead is on, the decision
  of `ia_pace_step`, absolute deadlines, and a pacing line per stream every 10 s.
- **C11.** `release_stream`: dead and not started, join and close the timer thread, unpublish,
  wait for two passes (500 ms bound; none when the unit is stopped), stop the unit if idle, free.
  `process_detach` retires the calling pseudo-process's streams (stop, unpublish, free the
  slot) without freeing their memory. This track's spec 09 has no per-process cleanup hook for
  other unix libraries: a pseudo-process that ends without detaching leaves its streams
  published; they underrun to silence once their ring drains. `ios_audio_process_ended(void
  *peb)` does the same retiring and is exported for such a hook (QUESTIONS 14-14).
- **C12.** MIDI and aux answers as the spec says, each kind logged once.
- **Globals (A5).** App-wide: the output unit and its state, the mixer and its slots, the stream
  table and generations, the started count, the lead and downmix switches (read once), the log
  flags and the stale-handle log count. Per stream (owned by the pseudo-process that created it,
  `struct stream.owner`): everything in `struct stream`.

## Part D: dwrite's unix side (`dwrite-unix/dwrite_freetype_ios.c`)

Upstream `dlls/dwrite/freetype.c` is compiled unchanged: `build-ios.sh` copies it to
`work/dwrite/freetype_upstream.inc` and the wrapper includes it.

- **D1.** Before the include: `config.h`, then `HAVE_FT2BUILD_H`, `HAVE_FREETYPE` and
  `SONAME_LIBFREETYPE` defined; `<dlfcn.h>` included first so its prototypes keep their names,
  then `dlopen`/`dlsym`/`dlclose` mapped to static functions: "open" succeeds for a name
  containing `freetype`, "symbol" looks the name up in a table of exactly the 28 FreeType
  functions `freetype.c` loads (unknown names → NULL), "close" does nothing. The FreeType code is
  spec 12's static copy in `libwin32u_unix.a`; this library only uses FreeType's headers.
- **Renames.** Upstream's `__wine_unix_call_funcs` / `__wine_unix_call_wow64_funcs` become
  `dwrite_upstream_unused_funcs` / `…_wow64_funcs` (still compiled, not looked up), its
  `process_attach` / `process_detach` become `dwrite_upstream_process_attach` / `…_detach`. The
  wrapper defines `dwrite_unix_call_funcs` / `dwrite_unix_call_wow64_funcs` (default-visible,
  `used`), whose other entries are upstream's static functions.
- **D2.** Eight 32-bit thunks of the wrapper's own replace upstream's (`ULongToPtr`): font data
  and the object out-slot (create; the UINT64 handle itself is not converted), the outline block
  and its tag and point arrays (outline; the counts are written back), and the count, advance,
  has-contours, bbox, bitmap, is-1bpp and metrics out-slots, all through `iom_guest_ptr()`. Every
  32-bit layout has `_Static_assert`s; release_font_object shares the 64-bit entry.
- **D3.** `process_attach` runs upstream's attach (which loads the symbols and calls
  `FT_Init_FreeType`) under a lock the first time, and only until it succeeds; later calls
  succeed without re-initialising. `process_detach` succeeds and does nothing.
- **Globals (A5).** App-wide: upstream's `ft_handle`, `library` and the `pFT_*` pointers (one
  FreeType library for the app), the wrapper's `ft_ready` flag. Font objects (`FT_Face`) belong
  to the pseudo-process that created them and stay allocated if it ends without releasing them.

## Part E: Wine Mono (`pe/metahost.c`)

Wine Mono has no unix-side part here. `pe/metahost.c` is upstream's `dlls/mscoree/metahost.c`
with its LGPL header kept, a "Changed for MYIOSDECK" note added, and three marked changes, for
the 32- and 64-bit PE builds of mscoree:

- `ios_mono_set_defaults()` runs in `load_mono` right before `LoadLibraryW` of the Mono DLL.
  Unless `MADEIRA_MONO_DEFAULTS=0`: `MONO_THREADS_SUSPEND` unset → `coop`; `MONO_DEBUG` unset →
  `keep-delegates`, set without the `keep-delegates` option → `,keep-delegates` appended (options
  compared as comma-separated tokens). The previous values (or "unset") are saved and the
  resulting values logged with `MESSAGE` (`mscoree: loading Mono with MONO_THREADS_SUSPEND=…
  MONO_DEBUG=…`).
- `ios_mono_restore_env()` runs in `get_root_domain` right after `mono_jit_init_version`: both
  variables get their saved values back, or are unset if they were unset
  (`SetEnvironmentVariableA(name, NULL)`), so child processes don't inherit them. A value the
  user set wins (it is never replaced, only `MONO_DEBUG` gains the option until the restore).
- The variables are changed in the Windows process environment (`SetEnvironmentVariableA`),
  which is where Mono's own `getenv` replacement reads them (QUESTIONS 14-19).
- `<stdlib.h>` and `<string.h>` are included explicitly for the new code.

## Part F: tests (`tests/`, `TESTS.md`)

Host tests for every pure part and end-to-end tests through the call tables, with stubs for
what ntdll and the Apple frameworks provide (`stubs_common.c`: the log sink, a fake guest window
and arena, the PEB; `stubs_wine.c`: `NtSetEvent`, `NtWaitForSingleObject`, `NtClose`,
`NtAllocateVirtualMemory` into the arena; `stubs_audio.c`: a fake output unit and a virtual
clock; `stubs_platform.c`: stub video and AAC decoders). `build-host-ffmpeg.sh` makes the host
FFmpeg; `run-host-tests.sh` builds under ASan/UBSan with `-Wall -Wextra -Werror` and runs them.
In WSL all six tests pass with gcc 15 and clang 21 against a host build of refs/wine-11.4
(`~/wine-build`) for the generated headers. `wg_platform_apple.c` and `ios_audio_apple.c` only
build for iOS. Fixes from the first Wine-header build: `ios_audio.c` includes `wingdi.h` (before
`mmreg.h`) and `wine/unixlib.h`, a missing field initialiser, a sign-compare in
`wg_parser_ios.c`; in the tests, FFmpeg 7.1's const write callback, the `ios_wow_*` prototypes,
and no PTS expected after a flush (rule 12 forgets it). `TESTS.md` lists what each test covers and the on-device checks with their log lines.

- Found by the tests: the lead of C10 was not reduced when the capacity is at most one period
  (`ia_lead_frames`); it is now `capacity − period` (or 0) raised to one period.

- Found while writing the parser test: after a read error a seek must also clear the AVIO
  context's own latched error and end-of-file flag (`wg_parser_ios.c`, seek), or every later
  read fails.

## Spec 17 Part A1

- spec 17 A1, `build-ios.sh`: `out/09-ntdll-processes/include/` (spec 17's shared
  `wine/unixlib.h` and `winternl.h`) comes before the Wine tree's `include/` in `WINEINC`.

## Spec 22 Part F (kept sessions) and spec 14 clarification 5

- F1, `audio-unix/ios_audio.c`: the first `process_attach` registers `process_end_hook` with
  `ios_process_end_hook_add` (spec 22 C6; declared in `common/ios_media_wow.h`), once. It calls
  the existing `ios_audio_process_ended(peb)`: the ended pseudo-process's streams are stopped,
  unpublished from the mixer and no longer counted (C11's open point closed).
- F2, `unit_update`: when the last stream stops and `MYIOSDECK_SESSION_REUSE` is `1` (read then),
  a running output unit is not stopped; it runs on, silent, until the app ends (logged once).
  Without it, C5's rule is unchanged. Rule `ia_keep_unit_running` (`ios_audio_pure.c`).
- F3, `drv_timer_loop`: a started stream whose unit isn't running (creation failed or start
  failed or stopped) gets the plain 10 ms beat, as null mode, instead of the pacing that waits for
  the unit to drain the buffer. Rule `ia_plain_beat`. `unit_running` is now written with release
  stores and read atomically by the timer threads.
- Tests: `test_kept_session` (`test_audio_pure.c`), `test_session_reuse` (`test_audio_wine.c`);
  stubs: `ios_process_end_hook_add` (`stubs_common.c`), `test_unit_start_fail` (`stubs_audio.c`).
