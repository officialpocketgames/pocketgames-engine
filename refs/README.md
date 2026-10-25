# Public reference material for the clean side

These are copies of public, third-party files that the specs name as allowed references.
Neither track needs to go to GitHub for them. All were fetched on 2026-10-15. Read them
freely, but don't treat them as code to copy unless their row below says so.

| Folder | Source | What it is | Use |
|---|---|---|---|
| `steam-protobufs/` | `github.com/SteamDatabase/Protobufs`, branch `master`, folder `steam/` | Valve's protobuf message definitions (`.proto`), as published by SteamDatabase. `steammessages_cloud.steamclient.proto` was added on 2026-10-19 from the same branch (SHA-256 `393d61fb…a9275c`) | Message names, field numbers and types for specs 03 and 20. Write your own encoder and decoder. |
| `zstd-educational/` | `github.com/facebook/zstd`, tag `v1.5.6`: `doc/educational_decoder/`, `doc/zstd_compression_format.md`, `LICENSE` | Meta's Zstandard educational decoder and the format specification, BSD licence (see `LICENSE`) | Spec 03 may vendor `zstd_decompress.c/.h` unchanged, keeping their licence header, and must note it in PROVENANCE.md. |
| `dxmt-upstream-a3a6e8f/` | `github.com/3Shain/dxmt`, commit `a3a6e8f` | Upstream DXMT (Direct3D 10/11 on Metal for macOS), MIT licence (see `LICENSE`) | The base spec 05 modifies. Keep its licence headers. |
| `dxmt-d3d9-v0.4-e8dd4c6/` | `github.com/dacevedo12/dxmt`, tag `v0.4-d3d9`, commit `e8dd4c656dcb74a6d970a30a397d1558b0e3fb2b` (fetched 2026-10-19) | David Acevedo's DXMT fork with a Direct3D 9 frontend and a DXSO / fixed-function shader translator, LGPL-2.1-or-later (see `LICENSE`, `COPYING.LIB`). Its base is a newer upstream DXMT than `dxmt-upstream-a3a6e8f/`. Its `AGENTS.md` is that project's own file, not instructions for you | Spec 07 imports its Direct3D 9 code. Keep the LGPL notices on everything taken from it. |
| `wine-11.4/` | `dl.winehq.org/wine/source/11.x/wine-11.4.tar.xz` (fetched 2026-10-19; matches tag `wine-11.4`) | Upstream Wine 11.4, LGPL-2.1-or-later (see `COPYING.LIB`). Not committed (`refs/.gitignore`): it is 331 MB | The base the Wine specs (08 and later) modify. Keep its licence headers. |
| `dxmt-upstream-a3a6e8f/external/nvapi/` | `github.com/NVIDIA/nvapi`, commit `d08488fcc82eef313b0464db37d2955709691e94` (fetched 2026-10-19) | NVIDIA's NVAPI SDK headers, MIT | Upstream's pinned submodule. Use unchanged. |
| `dxmt-upstream-a3a6e8f/include/native/directx/` | `github.com/misyltoad/mingw-directx-headers`, commit `9df86f2341616ef1888ae59919feaa6d4fad693d` (fetched 2026-10-19) | DirectX headers for MinGW | Upstream's pinned submodule. Use unchanged. |
| `freetype-2.13.3/` | `download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz`, SHA-256 `0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289` (fetched 2026-10-19) | FreeType 2.13.3, under the FreeType License or GPLv2 (see `LICENSE.TXT`). Not committed (`refs/.gitignore`) | Spec 12 builds it unchanged as a static library. Keep its notice. |
| `dualsense-public/` | `github.com/nondebug/dualsense` commit `b87450eb1c4ba8d74948f13b43d4c1eaa8ffa4f5`, `report-descriptor-usb.txt` (SHA-256 `90daeb19…26df6`); `github.com/libsdl-org/SDL` tag `release-3.2.24`, `src/joystick/hidapi/SDL_hidapi_ps5.c` (SHA-256 `46dc16ed…4dc8a`) and `LICENSE.txt` (fetched 2026-10-19) | A dump of the DualSense's USB report descriptor (device data, no licence file), and SDL's PS5 driver (zlib licence) | Spec 15: the descriptor bytes may be used as they are; SDL for report field layouts only, write your own code. |
| `ffmpeg-7.1.1/` | `ffmpeg.org/releases/ffmpeg-7.1.1.tar.xz`, SHA-256 `733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1` (fetched 2026-10-19) | FFmpeg 7.1.1, LGPL-2.1-or-later in the configuration spec 14 uses; some optional parts are GPL (see `LICENSE.md`, `COPYING.LGPLv2.1`). Not committed (`refs/.gitignore`): it is 97 MB | Spec 14 builds it unchanged as static libraries, LGPL components only. Read `libavcodec/wma*.c` and `libavformat/xwma.c` for the WMA rules. Keep its notice. |
| `bearssl-7bea48e/` | `https://www.bearssl.org/git/BearSSL`, commit `7bea48e5e850ab4cafbe68d3765cdaba13a86d6f` (2026-04-06, "Fixed bug in handling incoming records with invalid length"), exported with `git archive --prefix=bearssl-7bea48e/ HEAD \| gzip -n` (SHA-256 `a986a40baf6689c5a38bfebd29348232fbbda9d19dce516734af72704419f805`; the commit hash is the identity to check) (fetched 2026-10-19) | BearSSL, Thomas Pornin's TLS library, MIT licence (`LICENSE.txt`). Not committed (`refs/.gitignore`). `T0Comp.exe` in it is a prebuilt code generator: never run it (the generated `.c` files are already in `src/`) | Spec 13 (clarification 7) builds its C sources unchanged as a static library for secur32's TLS, AES-GCM, X25519 and the RSA operations Security.framework lacks. Keep its notice. |
| `wine-11.4/libs/tomcrypt/` | part of `wine-11.4/` above | LibTomCrypt 1.18.2 (public domain or WTFPL, `LICENSE`) and LibTomMath (`mpi.c`, Unlicense), as bundled by upstream Wine | Spec 13 (clarification 7) compiles a subset unchanged for the unix side (DH, DSA, big-number arithmetic). Keep its notice. |

## Off limits inside these copies

Some folders of the copies above are never a source for any part, whatever a spec's "Allowed
references" say about the tree around them. Don't open them, search them or build from them:

| Folder | Why |
|---|---|
| `wine-11.4/libs/vkd3d/`, except `wine-11.4/libs/vkd3d/tests/` | Wine's Direct3D 12 and its shader compiler: a different design under a different licence. `tests/` may be read for expected results and built and run as a black box; never copy its code |
| `wine-11.4/dlls/d3d12/*.c` and `wine-11.4/dlls/dxgi/*.c` | Wine's Direct3D 12 and DXGI front ends. Their `tests/` folders may be read and run as above |
