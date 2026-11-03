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
#   libangle.a       the EGL and GLES entry points and what they need: of the objects of
#                    libEGL_static's and libGLESv2_static's link closure (GN's project.json), the
#                    ones a link asking for every egl*/gl* function loads (C1.2; the rest is left
#                    out)
#   include/         EGL, GLES2, GLES3, KHR
#   args.gn          the final GN arguments (also printed)
#   NOTICES.txt      ANGLE's licence and that of every third-party folder whose code is in the
#                    archive; third-party.txt lists those folders and their licences
#   LINK.txt         what linking the archive needs (frameworks, libraries; GN's view added)
#   checks.txt       C1.3: imports (with the sources that import them), sources, selectors, size
#   logs/            the links' and tools' output, the archive's members with their sources
#                    (archive-members.tsv), the objects left out (left-out.txt)
# The script fails on a GPL or LGPL-3.0 folder, a forbidden import (ci/forbidden-imports.txt
# "fail") of the archive or of a dead-stripped program linking it, MAP_JIT /
# pthread_jit_write_protect / mprotect with PROT_EXEC / dlopen of an absolute path in a compiled
# source, an Objective-C selector the SDK's public headers don't declare, or a failed link.
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
# C1.1: switches for tracing (Perfetto, whose base library imports fork and exec) and for back
# ends other than Metal. Each one this ANGLE revision declares and has on is turned off (args.gn
# is the base; this list only adds to it, for names a revision may or may not have).
ANGLE_OFF="angle_enable_trace angle_enable_trace_events angle_enable_trace_android_and_chrome
 angle_enable_trace_android_logcat angle_use_perfetto angle_enable_perfetto angle_has_frame_capture
 angle_has_histograms angle_enable_vulkan angle_enable_vulkan_system_info angle_use_vulkan_display
 angle_use_vulkan_null_display angle_enable_vulkan_validation_layers angle_enable_vulkan_api_dump_layer
 angle_enable_swiftshader angle_enable_gl angle_enable_gl_null angle_enable_null angle_enable_wgpu
 angle_enable_d3d9 angle_enable_d3d11"

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
stamp="$(cat "$0" "$here/args.gn" "$here/objc_selectors.py" "$here/angle_closure.py" | shasum -a 256 | cut -c1-16)-$target"
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
# depot_tools bootstraps itself (its Python, CIPD's tools, python3_bin_reldir.txt) in the update
# step its wrappers run first, which DEPOT_TOOLS_UPDATE=0 skips, and ANGLE's DEPS hook
# update_depot_tools_toggle --disable turns off. Bootstrap the clone once without updating it:
# the toggle first (so nothing pulls), then ensure_bootstrap; gclient's hooks then all run (the
# siso configuration needs that Python, lastchange, the flex/bison download, the toolchain check).
if [ ! -f "$depot/python3_bin_reldir.txt" ]; then
    [ -f "$depot/update_depot_tools_toggle.py" ] && python3 "$depot/update_depot_tools_toggle.py" --disable
    if [ -x "$depot/ensure_bootstrap" ]; then
        "$depot/ensure_bootstrap"
    else
        # older layout: the update step bootstraps; the toggle above keeps it from pulling
        DEPOT_TOOLS_UPDATE=1 "$depot/update_depot_tools"
    fi
fi
[ -f "$depot/python3_bin_reldir.txt" ] || {
    echo "build-angle: depot_tools at $depot isn't bootstrapped (no python3_bin_reldir.txt)" >&2; exit 1; }
echo "build-angle: depot_tools bootstrapped (python $(cat "$depot/python3_bin_reldir.txt"))"

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
# --ide=json: GN also writes <build>/project.json, every target's description (C1.2 reads it)
gn gen "$build" --ide=json
# the arguments this revision declares, with their values: ANGLE_OFF's that are on go off, and
# gn gen runs again
gn args "$build" --list --short > "$work/args-list-$target.txt"
off=""
for a in $ANGLE_OFF; do
    if grep -qx "$a = true" "$work/args-list-$target.txt"; then off="$off $a"; fi
done
if [ -n "$off" ]; then
    {
        echo ""
        echo "# build-angle.sh: declared by this revision and on, turned off (tracing, back ends other than Metal)"
        for a in $off; do echo "$a = false"; done
    } >> "$build/args.gn"
    gn gen "$build" --ide=json
    gn args "$build" --list --short > "$work/args-list-$target.txt"
fi
echo "build-angle: turned off:${off:- nothing}"
gn args "$build" --list --short --overrides-only > "$out/args.gn"
echo "build-angle: args.gn"
cat "$out/args.gn"

# ---------------------------------------------------------------- C1.2: build
autoninja -C "$build" libEGL_static libGLESv2_static

logs="$out/logs"
rm -rf "$logs"
mkdir -p "$logs"
checks="$out/checks.txt"
: > "$checks"
note() { echo "$*"; echo "$*" >> "$checks"; }
failed=0
tab=$(printf '\t')
sdk_name=iphoneos
[ "$target" = macos ] && sdk_name=macosx
if [ "$target" = ios ]; then cflags="-arch arm64 -miphoneos-version-min=17.0"; else cflags="-arch arm64 -mmacosx-version-min=13.0"; fi
printf 'int main(void) { return 0; }\n' > "$work/size_empty.c"

cp "$work/args-list-$target.txt" "$logs/gn-args-list.txt"
note "arguments turned off (declared by this revision, on by default):${off:- none}"
{ grep -E '^[a-z0-9_]*(trace|perfetto|vulkan|volk)[a-z0-9_]* = ' "$logs/gn-args-list.txt" || true; } \
    | sed 's/^/  declared: /' | tee -a "$checks"

# the link closure of both targets as GN describes it (project.json): every static library,
# source set and group a program linking them links too, and their compiled sources' objects
# (closure-*.tsv: object, source). Not ninja's graph: siso builds the out dir, and depot_tools'
# ninja refuses one siso has used; ninja -t inputs also missed whole sources (GLES1State.cpp).
if ! python3 -I "$here/angle_closure.py" gn-objects "$build/project.json" "$build" //:libEGL_static //:libGLESv2_static \
        > "$work/closure-$target.tsv" 2> "$logs/gn-objects.log"; then
    note "FAIL: GN's link closure of libEGL_static and libGLESv2_static (logs/gn-objects.log):"
    sed -n '1,20p' "$logs/gn-objects.log" | sed 's/^/  /' | tee -a "$checks"
    echo "build-angle: checks failed (see $checks; logs in $logs)" >&2
    exit 1
fi
ncl=$(wc -l < "$work/closure-$target.tsv" | tr -d ' ')
note "closure: $(tail -n 1 "$logs/gn-objects.log" | sed 's/^angle_closure: //')"
# why a target is in the closure: the chain of deps from the two libraries (closure-paths.txt);
# the ones for tracing and Vulkan are named here
python3 -I "$here/angle_closure.py" gn-paths "$build/project.json" //:libEGL_static //:libGLESv2_static \
    > "$logs/closure-paths.txt" 2>> "$logs/gn-objects.log" || true
{ grep -iE 'perfetto|volk|vulkan|trace' "$logs/closure-paths.txt" || true; } | sed -n '1,12p' \
    | while IFS="$tab" read -r l p; do note "in the closure: $l, through $p"; done
nmiss=$({ grep -c '^no object: ' "$logs/gn-objects.log" || true; })
[ "$nmiss" = 0 ] || note "REVIEW: compiled sources of the closure without an object (logs/gn-objects.log): $nmiss"

# the closure in one archive, members renamed so no two share a name (members-*.txt: member,
# object, source)
objs="$work/objs-$target"
rm -rf "$objs"
mkdir -p "$objs"
: > "$work/members-$target.txt"
i=0
while IFS="$tab" read -r o s; do
    i=$((i + 1))
    m="$(printf '%05d' "$i")_$(basename "$o")"
    cp "$build/$o" "$objs/$m"
    printf '%s\t%s\t%s\n' "$m" "$o" "$s" >> "$work/members-$target.txt"
done < "$work/closure-$target.tsv"
closure="$work/libangle-closure-$target.a"
rm -f "$closure"
cut -f1 "$work/members-$target.txt" | sed "s|^|$objs/|" > "$work/filelist-closure-$target.txt"
xcrun -sdk "$sdk_name" libtool -static -o "$closure" -filelist "$work/filelist-closure-$target.txt" 2> "$logs/libtool-closure.log" || {
    cat "$logs/libtool-closure.log"; exit 1; }

rm -rf "$out/include"
mkdir -p "$out/include"
for h in EGL GLES2 GLES3 KHR; do cp -R "include/$h" "$out/include/"; done

# ---------------------------------------------------------------- C1.2: LINK.txt
# what linking the two targets needs: the frameworks of the Metal backend, and what GN gives the
# targets a program linking them links too (angle_closure.py gn-link)
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
} > "$work/link-base-$target.txt"
if python3 -I "$here/angle_closure.py" gn-link "$build/project.json" //:libEGL_static //:libGLESv2_static \
       > "$work/link-gn-$target.txt" 2> "$logs/gn-link.log"; then
    note "LINK.txt: the base list and what GN gives the link closure of libEGL_static and libGLESv2_static"
    # left out: Chromium's clang runtime (Xcode's clang links its own), any other source-tree path
    { grep '^dropped: ' "$logs/gn-link.log" || true; } | while read -r line; do
        case "$line" in
            *"clang runtime"*) note "LINK.txt: $line" ;;
            *) note "REVIEW: LINK.txt: $line" ;;
        esac
    done
else
    : > "$work/link-gn-$target.txt"
    note "REVIEW: angle_closure.py gn-link failed (logs/gn-link.log); LINK.txt is the base list"
fi
cat "$work/link-base-$target.txt" "$work/link-gn-$target.txt" | awk 'NF && !seen[$0]++' > "$out/LINK.txt"
note "LINK.txt: $(tr '\n' ' ' < "$out/LINK.txt")"

# ---------------------------------------------------------------- C1.2: the archive
# libangle.a holds what the EGL and GLES entry points need. A probe program asks for every egl*
# and gl* function the closure defines (-u) and every Objective-C class and category (-ObjC); the
# members the linker loads (its -map) make the archive. What nothing there reaches stays out:
# tools for the build machine, test and process helpers, code for backends that are off.
{ nm -gU "$closure" 2>/dev/null || true; } | awk '{ print $NF }' | { grep -E '^_(egl|gl)[A-Z]' || true; } \
    | sort -u > "$work/entry-$target.txt"
nent=$(wc -l < "$work/entry-$target.txt" | tr -d ' ')
note "entry points (egl*, gl*) the closure defines: $nent"
sed 's/^/-Wl,-u,/' "$work/entry-$target.txt" > "$work/probe-u-$target.rsp"
# shellcheck disable=SC2086,SC2046
if [ "$nent" -gt 0 ] &&
   xcrun -sdk "$sdk_name" clang $cflags -o "$work/probe-$target" "$work/size_empty.c" @"$work/probe-u-$target.rsp" \
       -ObjC -Wl,-map,"$work/probe-$target.map" "$closure" $(cat "$out/LINK.txt") > "$logs/probe-link.log" 2>&1; then
    python3 -I "$here/angle_closure.py" map-members "$work/probe-$target.map" "$(basename "$closure")" \
        > "$work/kept-members-$target.txt"
else
    note "FAIL: the probe link (every entry point of the closure) failed; libangle.a is the whole closure. logs/probe-link.log:"
    { grep -E 'error|Undefined|duplicate|ld:|warning: ignoring' "$logs/probe-link.log" || head -n 20 "$logs/probe-link.log"; } \
        | sed -n '1,40p' | sed 's/^/  /' | tee -a "$checks"
    failed=1
    cut -f1 "$work/members-$target.txt" > "$work/kept-members-$target.txt"
fi
awk -F "$tab" 'NR == FNR { keep[$1] = 1; next } ($1 in keep)' "$work/kept-members-$target.txt" "$work/members-$target.txt" \
    > "$work/kept-$target.txt"
awk -F "$tab" 'NR == FNR { keep[$1] = 1; next } !($1 in keep) { print $2 }' "$work/kept-members-$target.txt" \
    "$work/members-$target.txt" > "$logs/left-out.txt"
nkept=$(wc -l < "$work/kept-$target.txt" | tr -d ' ')
[ "$nkept" -gt 0 ] || { echo "build-angle: the probe link loaded no member of the closure" >&2; exit 1; }
note "libangle.a: $nkept of the closure's $ncl objects (left out: logs/left-out.txt, by folder:"
sed -E 's|^obj/||; s|/[^/]*$||' "$logs/left-out.txt" | sort | uniq -c | sort -rn | sed -n '1,25p' | sed 's/^/  /' | tee -a "$checks"
note "  )"
cut -f1 "$work/kept-$target.txt" | sed "s|^|$objs/|" > "$work/filelist-$target.txt"
rm -f "$out/libangle.a"
xcrun -sdk "$sdk_name" libtool -static -o "$out/libangle.a" -filelist "$work/filelist-$target.txt" 2> "$logs/libtool.log" || {
    cat "$logs/libtool.log"; exit 1; }

# each member's source (GN's): logs/archive-members.tsv is member, object, source
cp "$work/kept-$target.txt" "$logs/archive-members.tsv"
{ cut -f3 "$logs/archive-members.tsv" | grep -v '^?$' || true; } | sort -u > "$work/sources-$target.txt"
nsrc=$(wc -l < "$work/sources-$target.txt" | tr -d ' ')
nunk=$({ cut -f3 "$logs/archive-members.tsv" | grep -c '^?$' || true; })
note "sources of the archive's members: $nsrc (members with no source of that name: $nunk)"

# ---------------------------------------------------------------- spec 35 A1: licences
# the third-party folders whose code is in the archive: a member's source, or for one without,
# its object's path (obj/<source dir>/... mirrors the source tree); the folder is the nearest one
# with a README.chromium
awk -F "$tab" '{ if ($3 != "?") print $3; else { sub(/^obj\//, "", $2); print $2 } }' "$logs/archive-members.tsv" \
        | while read -r o; do
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
# who imports what (symbol, object, source), to name the source of a hit
: > "$work/member-imports-$target.tsv"
while IFS="$tab" read -r m o s; do
    { nm -u "$objs/$m" 2>/dev/null || true; } | awk -v o="$o" -v s="$s" -v OFS="$tab" 'NF { print $1, o, s }' \
        >> "$work/member-imports-$target.tsv"
done < "$logs/archive-members.tsv"
importers() {
    awk -F "$tab" -v h="$1" '$1 == h { print $3 " (" $2 ")" }' "$work/member-imports-$target.tsv" | sort -u | sed -n '1,6p' \
        | paste -s -d ';' -
}

# forbidden <imports file> <what>: ci/forbidden-imports.txt against a list of imports; a "fail"
# entry fails the build, any other is listed for review; a hit of the archive names its sources
forbidden() {
    while read -r sym verdict; do
        verdict=${verdict%$'\r'}   # a checkout with CRLF line ends
        case "$sym" in ''|'#'*) continue ;; esac
        case "$sym" in *'*') hits=$(grep -E "^${sym%'*'}" "$1" || true) ;;
                       *) hits=$(grep -xF "$sym" "$1" || true) ;; esac
        for h in $hits; do
            where=""
            [ "$2" = libangle.a ] && where=": $(importers "$h")"
            if [ "$verdict" = fail ]; then note "FAIL: $2 imports $h$where"; failed=1
            else note "REVIEW: $2 imports $h$where"; fi
        done
    done < "$repo/ci/forbidden-imports.txt"
}

{ nm -u "$out/libangle.a" 2>/dev/null || true; } | { grep -E '^_' || true; } | sort -u > "$work/imports-$target.txt"
forbidden "$work/imports-$target.txt" libangle.a
if grep -qx '_dlopen' "$work/imports-$target.txt"; then
    note "REVIEW: _dlopen is imported by: $(importers _dlopen)"
fi

# ---------------------------------------------------------------- C1.3: compiled sources
while read -r s; do
    # GN's sources are relative to the source root, the current directory
    [ -f "$s" ] || continue
    f="$s"
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
while IFS="$tab" read -r m o s; do
    n=${m%.o}
    otool -v -s __TEXT __objc_methname "$objs/$m" > "$ot/$n.methname" 2>/dev/null || true
    otool -ov "$objs/$m" > "$ot/$n.ov" 2>/dev/null || true
done < "$logs/archive-members.tsv"
if python3 -I "$here/objc_selectors.py" check "$work/sdk-selectors-$target.txt" "$ot" > "$work/unknown-selectors-$target.txt"; then
    note "selectors: every selector the archive uses is declared in the SDK's public headers"
else
    note "FAIL: selectors not declared in the SDK's public headers (selector member):"
    sed 's/^/  /' "$work/unknown-selectors-$target.txt" | tee -a "$checks"
    failed=1
fi

# ---------------------------------------------------------------- C1.3: size, and the linked program
note "libangle.a: $(ls -l "$out/libangle.a" | awk '{print $5}') bytes"
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
# dead-stripped, as the app is linked
# shellcheck disable=SC2086,SC2046
if xcrun -sdk "$sdk_name" clang $cflags -Wl,-dead_strip -I"$out/include" -o "$work/size_with" "$work/size_main.c" \
       "$out/libangle.a" $(cat "$out/LINK.txt") > "$logs/size-link.log" 2>&1 &&
   xcrun -sdk "$sdk_name" clang $cflags -Wl,-dead_strip -o "$work/size_without" "$work/size_empty.c" >> "$logs/size-link.log" 2>&1; then
    with=$(stat -f %z "$work/size_with")
    without=$(stat -f %z "$work/size_without")
    note "the app binary grows by about $(( (with - without) / 1024 )) KB when it links ANGLE (an executable that uses EGL and GLES, dead-stripped)"
    { nm -u "$work/size_with" 2>/dev/null || true; } | { grep -E '^_' || true; } | sort -u > "$work/linked-imports-$target.txt"
    before=$failed
    failed=0
    forbidden "$work/linked-imports-$target.txt" "the linked program"
    [ $failed -eq 0 ] && note "the linked program imports nothing ci/forbidden-imports.txt fails"
    [ "$before" -ne 0 ] && failed=1
else
    note "FAIL: the size link failed; logs/size-link.log:"
    { grep -E 'error|Undefined|duplicate|ld:|warning: ignoring' "$logs/size-link.log" || head -n 20 "$logs/size-link.log"; } \
        | sed -n '1,40p' | sed 's/^/  /' | tee -a "$checks"
    failed=1
fi

if [ $failed -ne 0 ]; then
    echo "build-angle: checks failed (see $checks; logs in $logs)" >&2
    exit 1
fi
echo "$stamp" > "$out/.stamp"
echo "build-angle: done, $out/libangle.a"
