<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 the MYIOSDECK author -->

# Spec 14: tests

## Host tests

Run here in WSL (Ubuntu, gcc 15 / clang 21) or in CI:

```bash
wsl.exe -d Ubuntu -- bash -lc "cd /path/to/pocketgames-engine/out/14-media-audio-ios && ./tests/run-host-tests.sh"
```

`run-host-tests.sh [<configured Wine 11.4 build dir>]` compiles with `-Wall -Wextra -Werror
-Wno-unused-parameter` and `-fsanitize=address,undefined` (`-fno-sanitize-recover=undefined`,
leak detection off: retired streams are leaked on purpose) and exits non-zero on any failure.
The first run builds a host FFmpeg from `refs/ffmpeg-7.1.1` with `build-host-ffmpeg.sh` into
`~/.cache/myiosdeck/ffmpeg-7.1.1-host` (A2's components plus, for making test data only, the
wmav1/wmav2/mp2 encoders and the mp4 muxer; all LGPL; a tarball is checked by SHA-256).

Status on 2026-10-19 in WSL, gcc 15 and clang 21: all six pass — `test_audio_pure` (247
checks), `test_wg_pure` (511), `test_wma_params` (52,299; `wma_derive()` matches FFmpeg's decoder
on all 10,368 parameter sets of the grid), `test_wma_e2e` (11,704; every tone comes back at
440.0–440.3 Hz, RMS 0.353–0.354), `test_parser` (2,401), `test_audio_wine` (1,239). The Wine
group uses Wine's generated headers (`mfobjects.h`, `strmif.h`, `audioclient.h`, … from `widl`):
from the argument or `WINE_BUILD`, else `~/wine-build` (this machine's configured host build of
refs/wine-11.4), else headers the runner makes once into `~/.cache/myiosdeck/wine-11.4-headers`
when bison and flex are installed; without any of these it is skipped with a message.
`wg_platform_apple.c` and `ios_audio_apple.c` build only for iOS (CI).

| Group | Needs |
|---|---|
| pure | nothing |
| ffmpeg | the host FFmpeg (built automatically) |
| wine | the host FFmpeg and Wine's generated headers |

| Test | Covers |
|---|---|
| `test_audio_pure` | C8 gains: mono, stereo, 5.1 by mask (back and side) and without, 7.1, 7.1.4, a short mask, centre-type speakers, `MADEIRA_AUDIO_DOWNMIX=0`, more than 12 channels; C6 the mixer on synthetic rings: float/s16/s32, rate mapping (24 kHz on 48 kHz), underrun with short-pass and missing-frame counts, clamping and its count, the pass counter, a stream not started, the byte limit, a mono device; C10 the lead (`MADEIRA_AUDIO_LEAD_MS` values) and the pacing decision as a function of padding, lead, capacity and the last signal (unanswered signals, 1/10 ms looks, 1–10 ms sleeps); C3 ring capacity clamps; C2 the endpoint buffer layout and its too-small answer, the mix format bytes; C4 the format classes. |
| `test_audio_wine` | the driver through its tables with a fake unit and a virtual clock: endpoint ids, mix format, periods, `test_connect`, `get_prop_value`; every entry non-NULL in both tables, the `wineios_` alias; MIDI and aux answers (DRV_FAILURE, NOTSUPPORTED, quit, `send_notify` cleared, logged once); a stream: zeroed render buffers, OUT_OF_ORDER, BUFFER_TOO_LARGE, start/stop/reset state errors, mixing through the unit's render callback, padding and position, the silent flag, the unit stopped when idle, stale handles; null mode's wall-clock position across stops and `set_sample_rate`; the 16-stream limit; the timer thread signalling below the lead and being joined by `release_stream`; `process_detach` of one pseudo-process only and `ios_audio_process_ended`; the 32-bit thunks (create, a render buffer inside the window, 0 and `E_FAIL` outside it, get_position, test_connect's offset, midi_notify_wait, endpoint ids). |
| `test_wg_pure` | B1 handles (wrong type, a parser as a stream, stale, reused slot); B3 frame layouts; NV12 → NV12/I420/YV12/YUY2/BGRx/BGRA/RGBA with both matrices and both ranges, bottom-up, padding and cropping, the odd-width YUY2 rule; S24 packing (also in place); channel masks; trims; queue limits; the AAC PTS queue; the reorder rule; the log dedupe (2nd/4th/8th, 64 lines); B4 avcC and hvcC parsing (valid, truncated, wrong version, too few sets, SEI dropped) and the ES_Descriptor cookie bytes. |
| `test_wma_params` | B2: `wma_derive()` against FFmpeg's own decoder (`wma_reference.c` opens it and reads its private context) for a grid of v1/v2, 8 rates, 1–3 channels, 18 bit rates, 12 flag words and three private-data sizes; the candidate list (first entry, order of the alternative rate, dedupe, the cap, the byte_offset_bits limit and the 7–12 middles); the good-packet rule with each slack; the implied rate; the nearest declared rate; the xWMA synthetic-data rule and xwma.c's rows. |
| `test_wma_e2e` | B2 end to end through the `wg_transform` entries as `wma_decoder.c` calls them: WMA v1 and v2 tones (440 Hz, amplitude 0.5) from FFmpeg's encoders at 22050/32000/44100/48000 Hz, mono and stereo, to s16 and float, search on and off; pushes of 1000 bytes (staging), back-pressure (`MF_E_NOTACCEPTING`, `accepts_input` 0), reads of 4099 bytes (whole frames, `INCOMPLETE`), drain; the fundamental (±6 Hz) and RMS (0.354 ± 0.08) come back; flush and its discontinuity; refusals (rate change, 24-bit output, AAC, video); two-phase `get_output_type`, `set_output_type`; `[wma] accepted` logged; the muxer entries answer `STATUS_NOT_IMPLEMENTED`; the same decode through the 32-bit table with guest addresses (including `wg_sample.data`). FFmpeg's encoders can't make bit-reservoir streams: `MADEIRA_WMA_DUMP` files from a device fill that gap. |
| `test_parser` | B3 with stub platform decoders: WAV in every PCM format handed on unchanged (S24 packed back), conversion to s16 and to 44.1 kHz mono, a refused format disabling the stream; MPEG layer II (440 Hz tone) and its codec format; MP3 (silent layer III frames) with and without ID3 cover art; a synthetic MP4 (stub H.264 in B-frame order, stub AAC answering one packet late) with formats, codec formats, presentation order, PTS of late AAC answers, stream 0 ordering, a bottom-up BGRx enable, no scaler; the read protocol: short reads, end of file, a read error ending the stream and a seek starting over, `push_data` with no request, disconnect while a read waits; refusals: a low probe score (logged once), compressed output, `MADEIRA_WG_VIDEO=0`, `MADEIRA_WG_PARSER=0`; seek with exact trim (also unaligned) and stop, a stop beyond the duration; `get_tag`; the 32-bit table end to end. |

## On-device checks

Run with the app's normal logging (no `MADEIRA_QUIET`). Each check names the lines to look for.

1. **MP4 intro video through Media Foundation** (a game whose intro or menu video is H.264 + AAC
   in MP4): plays with sound, and the menu that waits for it appears.
   `[wg-parser] connect: mp4 (score 100, … bytes), video h264 (VideoToolbox) 1920x1080 30/1 NV12 BT.709, audio aac (AudioToolbox) 48000 Hz 2 ch …, duration … s`.
   A 64-bit game also needs `MADEIRA_WG_64BIT=1` and a 64-bit `winegstreamer.dll`; without them
   `[winegstreamer] 64-bit caller: no unix library (set MADEIRA_WG_64BIT=1 to enable it)`.
2. **xWMA through XAudio2** (a 32-bit game with xWMA music and dialogue): clean, no static.
   `[wma] transform N: tag 0x161 … (xWMA, header rate kept as a candidate) …`, then
   `[wma] accepted bit rate … flags …`, and no summary lines (a summary is printed only after
   failed packets or a dropped tail, Clarification 3).
   With `MADEIRA_WMA_DUMP=1`, `wma-dump-0.bin` … appear in Documents.
3. **MP3 music in a 32-bit DirectShow game**: plays.
   `[wg-parser] connect: mp3 (score …), audio mp3 (libavcodec) 44100 Hz 2 ch …`. The MPEG-I
   splitter's attempt first logs `[wg-parser] refused mp3 …: compressed output of mp3 is not supported (only PCM)`.
4. **Two WASAPI clients at once** (a cutscene over menu music): both are heard, and the cutscene
   ends. Two `[ios_audio] stream N created by …` lines, one `[ios_audio] output unit created: RemoteIO, float32, 48000 Hz, 2 ch`,
   later `[ios_audio] stream N released (…)`.
5. **5.1 dialogue**: a 5.1 client's centre channel is audible:
   `[ios_audio] stream N gains (L/R): 0:1.000/0.000 1:0.000/1.000 2:0.707/0.707 3:0.316/0.316 4:0.707/0.000 5:0.000/0.707`.
6. **FMOD clock with the sound off** (an unmixable format, or the unit failed): the intro
   advances. `[ios_audio] … the stream runs in null mode` or `output unit creation failed (…)`.
7. **Pacing**: an hour of gameplay; every 10 s `[ios_audio] pacing stream N (…): … device short 0 ms …`
   once the lead is in place (`MADEIRA_AUDIO_LEAD_MS` default 60).
8. **Backgrounding during a video**: return to the app; the picture resumes at the next keyframe:
   `[wg-parser] stream 0: the video decoder session was lost (app in the background?); recovering at the next keyframe`.
9. **dwrite**: a Chromium-based launcher or store page shows its text, in a 32-bit and a 64-bit
   program (no "Wine cannot find the FreeType font library" message).
10. **.NET Framework game** starts and reaches its menu:
    `mscoree: loading Mono with MONO_THREADS_SUSPEND=coop MONO_DEBUG=keep-delegates`.
11. **Two programs one after the other** each get sound, and a killed program's stream stops
    playing. The second program logs its own `stream N created`; a killed program's stream goes
    silent once its ring drains (QUESTIONS 14-14 until spec 09 calls `ios_audio_process_ended`).

## Build and link checks (CI)

- `build-ffmpeg.sh` prints `enabled DECODERS: …` etc.; the list must match `ffmpeg/README.md`.
- `build-ios.sh` ends with `unix call tables present`.
- The app links `-Wl,-force_load,libwinemedia_unix.a`, FFmpeg's four libraries in order and the
  frameworks, together with specs 08–12's libraries, without duplicate symbols (one FreeType).

## Spec 22 Part F (kept sessions)

Host: `test_audio_pure` `test_kept_session` (the two rules); `test_audio_wine`
`test_session_reuse`: the hook registered once at the first attach; the hook with an ended
process's PEB unpublishes its stream (`process ended: 1 stream(s) stopped and unpublished`), and
without the setting the unit stops; with `MYIOSDECK_SESSION_REUSE=1` the unit keeps running after
the last stop, the last release and a game's end; unset again, it stops; a started event stream
with a full buffer whose unit failed to start still gets events every 10 ms.

On the device: a game with sound, Quit game, another game with sound: the log has `no stream
plays: the output unit keeps running (silent) for the kept session` once, the first game's end
`process ended: N stream(s) stopped and unpublished`, and the second game's audio plays from its
start (no start of the output unit between the games).
