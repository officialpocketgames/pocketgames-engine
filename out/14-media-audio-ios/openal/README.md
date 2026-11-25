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

## CI (a change I can't make here)

1. **Pin the tarball.** `build-openal.sh` has `EXPECTED_SHA256=` empty and refuses to build
   until it is set. Once, on a trusted machine: download the URL above, compare it against the
   release announced on openal-soft.org, and put its `shasum -a 256` into `EXPECTED_SHA256`.
2. **A step in `engine.yml`'s macOS job**, before the app link, with CMake on the runner:
   ```sh
   curl -fsSL -o "$RUNNER_TEMP/openal-soft-1.24.3.tar.bz2" \
     https://openal-soft.org/openal-releases/openal-soft-1.24.3.tar.bz2
   sh out/14-media-audio-ios/openal/build-openal.sh "$RUNNER_TEMP/openal-soft-1.24.3.tar.bz2" "$RUNNER_TEMP/openal"
   ```
   Cache `$RUNNER_TEMP/openal` keyed on the script's hash. Upload `NOTICES.txt` and
   `openal-relink-kit.tar.gz` with the other notices and kit pieces.
3. **The app link**: `-F$RUNNER_TEMP/openal -framework OpenALSoft`, the framework embedded
   (Embed & Sign) and `@executable_path/Frameworks` in the rpaths. `libwinemedia_unix.a` now
   references OpenAL Soft's functions (`openal-unix/openal_link.c`), so the link needs it.

## Outside this folder

- **Spec 19 (PE build).** `dlls/openal32` is a new module, so the Wine tree needs it in
  `configure.ac` (`WINE_CONFIG_MAKEFILE(dlls/openal32)`, after `dlls/opengl32`) and in the
  generated `configure` (`wine_fn_config_makefile dlls/openal32 enable_openal32`), an overlay row
  `spec14-openal32|14-media-audio-ios/pe/dlls/openal32|dlls/openal32|14` in `build-pe.sh`, and
  `openal32.dll` in `farms/arm64ec-windows.txt` (64-bit set) and the i386 farm (32-bit set). In
  `farms/unixlibs-skip.txt`: `aarch64-windows openal32.dll` (Wine's own programs don't use it).
- **The app.** Call `pg_openal_set_app_active(0)` when the app resigns active and
  `pg_openal_set_app_active(1)` when it becomes active again, where the rest of the audio
  follows the app's state (QUESTIONS 14-40). It pauses every OpenAL device
  (`alcDevicePauseSOFT`) and resumes the ones the program didn't pause itself.
- **Spec 09.** Add `openal32.dll` to `unixlibs.txt` when it is turned on (its tables are in
  `libwinemedia_unix.a` built with `WITH_OPENAL=1`). Until then it is off: `CHANGES.md`, round 4.
