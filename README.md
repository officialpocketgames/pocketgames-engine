# Pocket Games engine

The open-source parts of [Pocket Games](https://officialpocketgames.github.io/) for iPhone and iPad:
Wine 11.4 and DXMT ported to iOS, and the build that produces the libraries the app ships with.

- `out/`: the iOS parts (wineserver, ntdll, win32u, networking, audio, controllers, DXMT on Metal,
  the Windows-side DLL build).
- `ci/` and `.github/workflows/engine.yml`: the build. Every push builds the iOS static libraries,
  the Windows DLLs and the relinking kit, and runs the host tests.
- `refs/`: the upstream sources the build uses (downloaded and checked by `ci/fetch-refs.sh`).

## Licences

Each file's licence is in its SPDX header. New code is under the MIT licence; code derived from
Wine or DXMT is under the GNU Lesser General Public License, as upstream. The sources under `refs/`
keep their own licences.

This repository, with the `pe-relink-kit` artifact of each build, is the corresponding source of
the LGPL parts in Pocket Games.

Contact: officialpocketgame@gmail.com
