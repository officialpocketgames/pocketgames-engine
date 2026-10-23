# Pocket Games: open-source components

[Pocket Games](https://officialpocketgames.github.io/) for iPhone and iPad includes the open-source
components below. This repository holds the source of the ones it builds (Wine and DXMT for iOS,
with every change made for iOS) and the scripts that build them. Each file's licence is in its
header; the licence texts are in the app under Settings › About › Open-source licences.

| Component | Licence |
|---|---|
| [Wine](https://www.winehq.org) 11.4, with the libraries it bundles | GNU LGPL 2.1 or later |
| [DXMT](https://github.com/3Shain/dxmt) | MIT; its Direct3D 9 part GNU LGPL 2.1 or later |
| [FFmpeg](https://ffmpeg.org) 7.1.1 (built without GPL or non-free parts) | GNU LGPL 2.1 or later |
| [FreeType](https://freetype.org) 2.13.3 | FreeType License |
| [BearSSL](https://www.bearssl.org) | MIT |
| [LibTomCrypt](https://www.libtom.net) 1.18.2 and LibTomMath | Public domain or WTFPL |
| New code for iOS in this repository | MIT |

## Source and relinking

- `out/`, `interfaces/`, `ci/` and `.github/workflows/engine.yml`: the source and the build. Every
  push builds the iOS libraries and the Windows DLLs.
- Each build's `pe-relink-kit` artifact holds the exact Wine tree its Windows DLLs were built from.
- `refs/` lists the upstream sources the build downloads, with their checksums.

You may change the LGPL parts and use your changed versions with the app.

Contact: officialpocketgame@gmail.com
