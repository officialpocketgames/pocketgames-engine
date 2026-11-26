<!-- SPDX-License-Identifier: LGPL-2.1-or-later -->
<!-- Copyright (c) 2026 the MYIOSDECK author -->

# OpenAL Soft for openal32.dll (spec 14 round 4)

`openal32.dll` (our own code, `../pe/dlls/openal32/`) forwards every OpenAL 1.1, ALC, EFX and
supported extension export through one unix call to `../openal-unix/`, which calls
**OpenAL Soft 1.24.3**, built unmodified for iOS arm64 by `build-openal.sh` with its CoreAudio
backend only. OpenAL Soft plays through its own CoreAudio output unit.

| | |
|---|---|
| Library | OpenAL Soft 1.24.3, unmodified |
| Licence | LGPL-2.1-or-later (plus the notices of included parts, collected into `NOTICES.txt`) |
| Source | https://openal-soft.org/openal-releases/openal-soft-1.24.3.tar.bz2 |
| Linking | dynamic only: `OpenALSoft.framework` (the dylib, wrapped, since an iOS app may not embed a loose dylib) |
| Not built | the utilities (`alsoft-config`), examples, tests; every backend but CoreAudio; backend dlopen |

## What the build produces (`build-openal.sh <tarball> <prefix>`)

- `<prefix>/OpenALSoft.framework`: link with `-F<prefix> -framework OpenALSoft`, embed it in the
  app's `Frameworks/` (signed with the app), and give the app `-rpath @executable_path/Frameworks`.
- `<prefix>/lib/libopenal.1.dylib`: the library as built, install name `@rpath/libopenal.1.dylib`.
- `<prefix>/NOTICES.txt`: goes into the app's third-party notices (acknowledgements) as is.
- `<prefix>/openal-relink-kit.tar.gz`: goes into the app's LGPL relinking kit beside the other
  LGPL parts. It holds the unmodified tarball, `build-openal.sh`, this README and
  `openal32_funcs.h` (the export check reads it). To use a modified OpenAL Soft, a user rebuilds
  with the script (after changing its pinned SHA-256) and replaces `OpenALSoft.framework`'s
  binary in the app, then re-signs it.

The script refuses a library whose dependencies are not Apple's system libraries (so no
liblzma), whose install name is wrong, or that lacks any export `openal32.dll` forwards.

## CI and the app

The tarball's SHA-256 is pinned in `build-openal.sh`. `ci/ios-part.sh` (part 14) fetches the
tarball, runs this script and publishes `OpenALSoft.framework`, `NOTICES.txt` and
`openal-relink-kit.tar.gz` in the `ios-14-media-audio-ios` artifact under `openal/`;
`ci/link-check.sh` links the framework. What the app must do (link, embed and sign, notices,
kit, `pg_openal_set_app_active`): `../CHANGES.md`, "What the app must do for OpenAL".

Spec 19 builds the DLL: overlay row `spec14-openal32`, `dlls/openal32` registered in the tree's
configure (`build-pe.sh` `new_modules`), `openal32.dll` in the ARM64EC and i386 farms (skipped
for aarch64: Wine's own programs don't use it). Spec 09's `unixlibs.txt` lists `openal32.dll`.
