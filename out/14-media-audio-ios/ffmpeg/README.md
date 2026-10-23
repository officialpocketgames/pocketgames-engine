<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 the MYIOSDECK author -->

# The static FFmpeg (spec 14 Part A2)

`../build-ffmpeg.sh <ffmpeg-7.1.1.tar.xz> <prefix>` builds it. FFmpeg's sources are used
unchanged: no patches, no copies in this repository.

- Source: FFmpeg 7.1.1, `https://ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz`, SHA-256
  `733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1` (the script aborts on a
  mismatch).
- Licence: GNU Lesser General Public License, version 2.1 or later, in this configuration
  (FFmpeg's `LICENSE.md`, `COPYING.LGPLv2.1`). The script refuses to build when FFmpeg's
  `config.h` reports any other licence or has `CONFIG_GPL`, `CONFIG_NONFREE` or
  `CONFIG_VERSION3` set.
- Outputs: `<prefix>/lib/libavformat.a`, `libavcodec.a`, `libswresample.a`, `libavutil.a` and
  `<prefix>/include/`. The app target links them in that order, after `libwinemedia_unix.a`.

## Options and why

| Option | Why |
|---|---|
| `--target-os=darwin --arch=aarch64 --enable-cross-compile --sysroot=<iPhoneOS SDK>` | iOS arm64 cross build. |
| `--extra-cflags="-arch arm64 -miphoneos-version-min=17.0 -fvisibility=hidden"` | Same minimum OS as the Wine libraries; FFmpeg's symbols stay out of the app's export table. |
| `--disable-gpl --disable-nonfree --disable-version3` | LGPL-2.1-or-later only. Never add a GPL, version-3 or non-free component: FFmpeg is linked statically, so those would change the licence of the whole app. |
| `--disable-everything` | Nothing but the components below. |
| `--enable-decoder=wmav1,wmav2,wmapro,wmalossless,xma1,xma2` | The WMA decoder MFT (Part B2); FAudio decodes every xWMA/XMA voice through it. |
| `--enable-decoder=mp1,mp2,mp3,pcm_u8,pcm_s16le,pcm_s24le,pcm_s32le,pcm_f32le,pcm_f64le` | The parser's audio (Part B3). |
| `--enable-demuxer=mp3,wav,mov` | The containers the parser accepts (Part B3). |
| `--enable-parser=mpegaudio` | The mp3 demuxer selects it. |
| (no h264, hevc, aac decoders or parsers) | H.264/HEVC go to VideoToolbox and AAC to AudioToolbox (Part B4), called directly by `winegstreamer-unix/wg_platform_apple.c`; the mov demuxer's sample tables already carry what a parser would add. |
| `--disable-protocols --disable-muxers --disable-network` | The parser reads through its own I/O callbacks, never a URL. |
| `--disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-swscale --disable-postproc` | Not used. `avformat`, `avcodec`, `avutil` and `swresample` are kept. |
| `--disable-autodetect --disable-zlib --disable-bzlib --disable-lzma --disable-iconv --disable-sdl2` | No external libraries. |
| `--disable-audiotoolbox --disable-videotoolbox --disable-securetransport --disable-appkit --disable-avfoundation --disable-coreimage --disable-metal` | No Apple integrations inside FFmpeg; the platform decoders are called directly. |
| `--disable-asm` | The C paths are fast enough for these decoders and are what was tested. |
| `--enable-static --disable-shared --enable-pic --disable-debug` | Static libraries for the one app binary. |

At the end the script prints the enabled decoders, demuxers, parsers, protocols and muxers from
`config_components.h`, so a CI log shows exactly what was built. Expected:

```
enabled DECODERS: mp1 mp2 mp3 pcm_f32le pcm_f64le pcm_s16le pcm_s24le pcm_s32le pcm_u8 wmalossless wmapro wmav1 wmav2 xma1 xma2
enabled DEMUXERS: mov mp3 wav
enabled PARSERS: mpegaudio
enabled PROTOCOLS: (none)
enabled MUXERS: (none)
```

(The order is `config_components.h`'s, which may differ from this list.)
