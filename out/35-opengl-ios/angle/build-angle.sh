#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# build-angle.sh <work dir> <out dir> <ios|macos>
#
# Spec 35 Part C1: ANGLE at the pinned commit, unchanged, with its standalone GN build
# (scripts/bootstrap.py, gclient sync), Metal its only backend (args.gn), for iOS arm64 (the app)
# or macOS arm64 (the render test, Part J3). Runs on macOS with Xcode.
#
# <out dir> receives:
#   libangle.a       the objects of libEGL_static's and libGLESv2_static's whole dependency
#                    closure in one archive (EGL and GLES entry points)
#   include/         EGL, GLES2, GLES3, KHR
#   args.gn          the final GN arguments (also printed)
#   NOTICES.txt      ANGLE's licence and that of every third-party folder whose code is in the
#                    archive; third-party.txt lists those folders and their licences
#   LINK.txt         what linking the archive needs (frameworks, libc++)
#   checks.txt       C1.3: imports, sources, selectors, size
# The script fails on a GPL or LGPL-3.0 folder, a forbidden import (ci/forbidden-imports.txt
# "fail"), MAP_JIT / pthread_jit_write_protect / mprotect with PROT_EXEC / dlopen of an absolute
# path in a compiled source, or an Objective-C selector the SDK's public headers don't declare.
#
# Nothing in refs/ is read or written. The checkout and the build live in <work dir>; the
# products in <out dir> carry a stamp of this script, args.gn and the target, and a second run
# with the same stamp does nothing (CI caches <out dir>).
#
# Environment: DEPOT_TOOLS (an existing depot_tools checkout; default <work>/depot_tools).

set -euo pipefail

ANGLE_URL=https://chromium.googlesource.com/angle/angle
ANGLE_COMMIT=751417514a521b2e92d6928260c62122138824d3   # head of chromium/8103 (Chromium 157), 2026-11-02
DEPOT_TOOLS_URL=https://chromium.googlesource.com/chromium/tools/depot_tools.git

[ $# -eq 3 ] || { echo "usage: $0 <work dir> <out dir> <ios|macos>" >&2; exit 2; }
target=$3
case "$target" in ios|macos) ;; *) echo "$0: target must be ios or macos" >&2; exit 2 ;; esac

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)

# refs/ is read only: refuse a work or out dir inside it (nearest existing parent, links resolved)
refs=$(cd "$repo/refs" 2>/dev/null && pwd -P) || refs=""
for d in "$1" "$2"; do
    [ -n "$refs" ] || break
    p=$d; rest=""
    while [ ! -d "$p" ]; do rest="/$(basename "$p")$rest"; p=$(dirname "$p"); done
    case "$(cd "$p" && pwd -P)$rest/" in
        "$refs/"*) echo "$0: refusing a directory inside refs/: $d" >&2; exit 1 ;;
    esac
done

mkdir -p "$1" "$2"
work=$(cd "$1" && pwd)
out=$(cd "$2" && pwd)
stamp="$(cat "$0" "$here/args.gn" "$here/objc_selectors.py" | shasum -a 256 | cut -c1-16)-$target"
if [ -f "$out/.stamp" ] && [ "$(cat "$out/.stamp")" = "$stamp" ] && [ -f "$out/libangle.a" ]; then
    echo "build-angle: $out already built ($stamp)"
    exit 0
fi
rm -f "$out/.stamp"

# ---------------------------------------------------------------- the checkout
depot=${DEPOT_TOOLS:-"$work/depot_tools"}
if [ ! -x "$depot/gclient" ]; then
    git clone --depth 1 "$DEPOT_TOOLS_URL" "$depot"
fi
export PATH="$depot:$PATH"
export DEPOT_TOOLS_UPDATE=0
echo "build-angle: depot_tools $(git -C "$depot" rev-parse HEAD)"

src="$work/angle"
if [ ! -d "$src/.git" ]; then
    mkdir -p "$src"
    git -C "$src" init -q
    git -C "$src" remote add origin "$ANGLE_URL"
fi
if [ "$(git -C "$src" rev-parse HEAD 2>/dev/null || true)" != "$ANGLE_COMMIT" ]; then
    git -C "$src" fetch -q --depth 1 origin "$ANGLE_COMMIT"
    git -C "$src" checkout -q --force FETCH_HEAD
fi
[ "$(git -C "$src" rev-parse HEAD)" = "$ANGLE_COMMIT" ] || { echo "build-angle: not at $ANGLE_COMMIT" >&2; exit 1; }
echo "build-angle: ANGLE $ANGLE_COMMIT"

cd "$src"
[ -f .gclient ] || python3 scripts/bootstrap.py
if [ "$target" = ios ] && ! grep -q "target_os" .gclient; then
    echo "target_os = ['ios']" >> .gclient
fi
gclient sync --no-history --shallow -D

# ---------------------------------------------------------------- C1.1: arguments
build="out/$target"
mkdir -p "$build"
{
    cat "$here/args.gn"
    echo ""
    echo "# build-angle.sh, target $target"
    if [ "$target" = ios ]; then
        echo 'target_os = "ios"'
        echo 'target_environment = "device"'
        echo 'ios_deployment_target = "17.0"'
    else
        echo 'target_os = "mac"'
        echo 'mac_deployment_target = "13.0"'
    fi
    echo 'target_cpu = "arm64"'
} > "$build/args.gn"
gn gen "$build"
gn args "$build" --list --short --overrides-only > "$out/args.gn"
echo "build-angle: args.gn"
cat "$out/args.gn"

# ---------------------------------------------------------------- C1.2: build
autoninja -C "$build" libEGL_static libGLESv2_static

# the dependency closure of both targets: every object file ninja needs for them
ninja -C "$build" -t inputs libEGL_static libGLESv2_static > "$work/inputs-$target.txt"
grep -E '\.o$' "$work/inputs-$target.txt" | sort -u > "$work/objects-$target.txt"
grep -E '\.(c|cc|cpp|m|mm)$' "$work/inputs-$target.txt" | sort -u > "$work/sources-$target.txt"
nobj=$(wc -l < "$work/objects-$target.txt" | tr -d ' ')
[ "$nobj" -gt 0 ] || { echo "build-angle: no objects found for the two targets" >&2; exit 1; }
echo "build-angle: $nobj objects, $(wc -l < "$work/sources-$target.txt" | tr -d ' ') sources"

# one archive; members renamed so no two share a name
objs="$work/objs-$target"
rm -rf "$objs"
mkdir -p "$objs"
i=0
while read -r o; do
    i=$((i + 1))
    cp "$build/$o" "$objs/$(printf '%05d' "$i")_$(basename "$o")"
done < "$work/objects-$target.txt"
sdk_name=iphoneos
[ "$target" = macos ] && sdk_name=macosx
rm -f "$out/libangle.a"
find "$objs" -name '*.o' | sort > "$work/filelist-$target.txt"
xcrun -sdk "$sdk_name" libtool -static -o "$out/libangle.a" -filelist "$work/filelist-$target.txt" 2> "$work/libtool-$target.log" || {
    cat "$work/libtool-$target.log"; exit 1; }

rm -rf "$out/include"
mkdir -p "$out/include"
for h in EGL GLES2 GLES3 KHR; do cp -R "include/$h" "$out/include/"; done

{
    echo "-framework Metal"
    echo "-framework QuartzCore"
    echo "-framework IOSurface"
    echo "-framework CoreGraphics"
    echo "-framework Foundation"
    echo "-framework CoreFoundation"
    [ "$target" = ios ] && echo "-framework UIKit"
    [ "$target" = macos ] && echo "-framework Cocoa"
    echo "-lc++"
} > "$out/LINK.txt"

# ---------------------------------------------------------------- spec 35 A1: licences
checks="$out/checks.txt"
: > "$checks"
note() { echo "$*"; echo "$*" >> "$checks"; }
failed=0

# the third-party folders whose objects are in the archive: obj/<source dir>/... mirrors the
# source tree; the folder is the nearest one with a README.chromium
: > "$work/tp-$target.txt"
sed -n 's|^obj/||p' "$work/objects-$target.txt" | while read -r o; do
    d=$(dirname "$o")
    while [ "$d" != . ] && [ "$d" != / ]; do
        case "$d" in third_party/*) ;; *) break ;; esac
        if [ -f "$d/README.chromium" ]; then echo "$d"; break; fi
        d=$(dirname "$d")
    done
done | sort -u > "$work/tp-$target.txt"

{
    echo "ANGLE ($ANGLE_URL, $ANGLE_COMMIT): BSD-3-Clause"
    echo ""
    cat LICENSE
} > "$out/NOTICES.txt"
: > "$out/third-party.txt"
while read -r d; do
    lic=$(sed -n 's/^License: *//p' "$d/README.chromium" | head -n 1)
    files=$(sed -n 's/^License File: *//p' "$d/README.chromium" | head -n 1)
    echo "$d: ${lic:-unknown} (${files:-no licence file named})" >> "$out/third-party.txt"
    case "$lic" in
        *LGPL-3*|*"LGPL v3"*|*LGPLv3*) note "FAIL: $d is $lic (LGPL-3.0)"; failed=1 ;;
        *LGPL*) note "REVIEW: $d is $lic (LGPL: the relinking kit must carry it)" ;;
        *GPL*) note "FAIL: $d is $lic (GPL)"; failed=1 ;;
        "") note "REVIEW: $d names no licence in README.chromium" ;;
    esac
    {
        echo ""
        echo "==== $d: $lic"
        for f in $(echo "$files" | tr ',' ' '); do
            [ -f "$d/$f" ] && cat "$d/$f"
            [ -f "$f" ] && [ ! -f "$d/$f" ] && cat "$f"
        done
    } >> "$out/NOTICES.txt"
done < "$work/tp-$target.txt"
note "third-party folders in the archive: $(wc -l < "$work/tp-$target.txt" | tr -d ' ') (third-party.txt)"

# ---------------------------------------------------------------- C1.3: imports
nm -u "$out/libangle.a" 2>/dev/null | grep -E '^_' | sort -u > "$work/imports-$target.txt"
while read -r sym verdict; do
    case "$sym" in ''|'#'*) continue ;; esac
    pat=$sym
    case "$pat" in *'*') prefix=${pat%'*'}; hits=$(grep -E "^${prefix}" "$work/imports-$target.txt" || true) ;;
                   *) hits=$(grep -xF "$pat" "$work/imports-$target.txt" || true) ;; esac
    [ -n "$hits" ] || continue
    if [ "$verdict" = fail ]; then note "FAIL: import $hits"; failed=1
    else note "REVIEW: import $hits"; fi
done < "$repo/ci/forbidden-imports.txt"
if grep -qx '_dlopen' "$work/imports-$target.txt"; then
    note "REVIEW: _dlopen is imported (the sources that call it are listed below)"
fi

# ---------------------------------------------------------------- C1.3: compiled sources
while read -r s; do
    [ -f "$build/$s" ] || continue
    f="$build/$s"
    if grep -nE 'MAP_JIT|pthread_jit_write_protect' "$f" > "$work/hit.txt"; then
        note "FAIL: $s: $(head -n 1 "$work/hit.txt")"; failed=1
    fi
    if grep -nE 'mprotect\([^;]*PROT_EXEC' "$f" > "$work/hit.txt"; then
        note "FAIL: $s: $(head -n 1 "$work/hit.txt")"; failed=1
    fi
    if grep -nE 'dlopen\( *"/' "$f" > "$work/hit.txt"; then
        note "FAIL: $s: dlopen of an absolute path: $(head -n 1 "$work/hit.txt")"; failed=1
    fi
    if grep -nE '\bdlopen\(' "$f" > "$work/hit.txt"; then
        note "REVIEW: $s calls dlopen: $(head -n 1 "$work/hit.txt")"
    fi
done < "$work/sources-$target.txt"

# ---------------------------------------------------------------- C1.3: selectors
sdk=$(xcrun --sdk "$sdk_name" --show-sdk-path)
python3 -I "$here/objc_selectors.py" sdk "$sdk" > "$work/sdk-selectors-$target.txt"
note "selectors declared in the SDK's Metal/QuartzCore/Foundation/IOSurface/CoreFoundation/objc headers: $(wc -l < "$work/sdk-selectors-$target.txt" | tr -d ' ')"
ot="$work/otool-$target"
rm -rf "$ot"
mkdir -p "$ot"
for o in "$objs"/*.o; do
    n=$(basename "$o" .o)
    otool -v -s __TEXT __objc_methname "$o" > "$ot/$n.methname" 2>/dev/null || true
    otool -ov "$o" > "$ot/$n.ov" 2>/dev/null || true
done
if python3 -I "$here/objc_selectors.py" check "$work/sdk-selectors-$target.txt" "$ot" > "$work/unknown-selectors-$target.txt"; then
    note "selectors: every selector the archive uses is declared in the SDK's public headers"
else
    note "FAIL: selectors not declared in the SDK's public headers (selector member):"
    sed 's/^/  /' "$work/unknown-selectors-$target.txt" | tee -a "$checks"
    failed=1
fi

# ---------------------------------------------------------------- C1.3: size
note "libangle.a: $(ls -l "$out/libangle.a" | awk '{print $5}') bytes"
if [ "$target" = ios ]; then cflags="-arch arm64 -miphoneos-version-min=17.0"; else cflags="-arch arm64 -mmacosx-version-min=13.0"; fi
cat > "$work/size_main.c" <<'EOF'
#include <EGL/egl.h>
#include <GLES2/gl2.h>
int main( int argc, char **argv )
{
    void *volatile keep[3] = { (void *)eglGetProcAddress, (void *)eglGetPlatformDisplay, (void *)glGetString };
    (void)argv;
    return argc > 99 ? (int)(long)keep[argc % 3] : 0;
}
EOF
printf 'int main(void) { return 0; }\n' > "$work/size_empty.c"
# shellcheck disable=SC2086,SC2046
if xcrun -sdk "$sdk_name" clang $cflags -I"$out/include" -o "$work/size_with" "$work/size_main.c" "$out/libangle.a" $(cat "$out/LINK.txt") 2> "$work/size-link-$target.log" &&
   xcrun -sdk "$sdk_name" clang $cflags -o "$work/size_without" "$work/size_empty.c"; then
    with=$(stat -f %z "$work/size_with")
    without=$(stat -f %z "$work/size_without")
    note "the app binary grows by about $(( (with - without) / 1024 )) KB when it links ANGLE (an executable that uses EGL and GLES)"
else
    note "REVIEW: the size link failed: $(grep -m 1 error "$work/size-link-$target.log" || true)"
fi

if [ $failed -ne 0 ]; then
    echo "build-angle: checks failed (see $checks)" >&2
    exit 1
fi
echo "$stamp" > "$out/.stamp"
echo "build-angle: done, $out/libangle.a"
