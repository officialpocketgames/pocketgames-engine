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
- Each App Store version's engine is tagged here (`pocketgames-<version>-<build>`).
- [pocketgames-build](https://github.com/officialpocketgames/pocketgames-build) has the tools that
  turn the Windows DLLs into the files the app loads, the other Wine-derived files in the app, the
  prebuilt parts the app is built from, and, for each App Store version, a relinking kit: the rest
  of the app's executable as object code, with its link command, so that you can relink it with
  your own versions of the LGPL libraries linked into it.

You may change the LGPL parts and use your changed versions with the app; reverse engineering the
app for debugging such changes is permitted. For three years after each version's release, we will
send you its relinking material on request, at no more than the cost of sending it.

Contact: officialpocketgame@gmail.com
