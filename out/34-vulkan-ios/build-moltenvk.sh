#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 34 Part B (and clarification 1): UTM's MoltenVK fork, with the adopted patches, as one
# static library for iOS arm64, with MoltenVK's own build files, on the macOS CI with Xcode.
#
#   build-moltenvk.sh [--dry-run] [--platform ios|macos] <moltenvk-src> <spirv-cross-src> <work-dir> <out-dir>
#
# --platform macos   the same sources and patches for macOS arm64 (13.0), for the macOS CI's render
#                    test only (tests/macos/; clarification 1, item 6): steps 1-3 with the macOS
#                    scheme and SDK, the private-API and mesh-path checks, libmoltenvk.a, include/,
#                    LICENSES/, BUILDINFO.txt; not the App Store checks or the size test, which are
#                    about the iOS product. The default is ios.
# <moltenvk-src>     a MoltenVK checkout at the commit moltenvk-sources.txt pins (fetch-moltenvk.sh);
#                    read only: it is copied to <work-dir>/MoltenVK first.
# <spirv-cross-src>  a SPIRV-Cross checkout at its pin (fetch-moltenvk.sh <dest> SPIRV-Cross); read
#                    only: copied to <work-dir>/SPIRV-Cross.
# <work-dir>         the copies, MoltenVK's External/ dependencies and Xcode's build products.
# <out-dir>          libmoltenvk.a, include/, LICENSES/, BUILDINFO.txt (Part B4).
# --dry-run          checks the arguments, the pins and the source trees, prints every step and the
#                    xcodebuild command, and runs nothing else (any host; tests/test-build-scripts.sh).
#
# Steps:
#   1. Sources (B1): both commits are checked, and SPIRV-Cross's must be the one MoltenVK's
#      ExternalRevisions/ names. moltenvk-patches/*.patch apply in name order (C locale) to the
#      MoltenVK copy and spirv-cross-patches/*.patch to the SPIRV-Cross copy, each with no fuzz
#      (steamac's adopted patches 0001-0047, then ours from 0100, then Part F's from 0200).
#      MVKPatchRevision.h is written for steamac's 0018 (the pipeline cache UUID): the first 32 bits
#      of the SHA-256 of both series. MoltenVK's fetchDependencies --ios then fetches and builds the
#      other dependencies at the revisions of ExternalRevisions/, with SPIRV-Cross taken from the
#      patched copy (--spirv-cross-root), and each checkout is checked against them. Nothing comes
#      from a prebuilt release.
#   2. Build (B2): xcodebuild of MoltenVKPackaging.xcodeproj's iOS-only package scheme, Release,
#      generic iOS, arm64 only, IPHONEOS_DEPLOYMENT_TARGET=17.0, MVK_USE_METAL_PRIVATE_API=0
#      (the fork's default, set explicitly anyway). The static XCFramework's ios-arm64 library is
#      taken, with SPIRV-Cross merged in if MoltenVK's archive doesn't hold it, debug info stripped.
#   3. Checks (B3, each fails the build): a compile-time check against MoltenVK's configuration
#      header that MVK_USE_METAL_PRIVATE_API is 0; the geometry-shader mesh path was compiled for
#      iOS (the build log compiles MVKPipeline.mm and MTLRenderPipelineDescriptor+MoltenVK.m for
#      iphoneos arm64, and the archive refers to MTLMeshRenderPipelineDescriptor; clarification 1,
#      item 3); moltenvk-checks.sh (members, imports).
#   4. Product (B4), with BUILDINFO.txt: versions and source hashes, the patches, the archive's
#      size, the __TEXT size of a dead-stripped test link that calls only vkGetInstanceProcAddr,
#      the required-reason APIs referenced.
# No directory inside refs/ is ever written (CLAUDE.md): every path is checked first.

set -eu

dry=0
platform=ios
while [ $# -gt 0 ]; do
    case $1 in
        --dry-run) dry=1; shift ;;
        --platform) [ $# -ge 2 ] || break; platform=$2; shift 2 ;;
        *) break ;;
    esac
done
[ $# -eq 4 ] || { echo "usage: $0 [--dry-run] [--platform ios|macos] <moltenvk-src> <spirv-cross-src> <work-dir> <out-dir>" >&2; exit 2; }
case $platform in
    ios)
        scheme_name="MoltenVK Package (iOS only)"; fetch_flag=--ios; sdk=iphoneos; slice=ios-arm64
        minver=-miphoneos-version-min=17.0
        build_settings="-configuration Release -destination generic/platform=iOS ARCHS=arm64 ONLY_ACTIVE_ARCH=NO \
 IPHONEOS_DEPLOYMENT_TARGET=17.0 DEBUG_INFORMATION_FORMAT=dwarf" ;;
    macos)
        scheme_name="MoltenVK Package (macOS only)"; fetch_flag=--macos; sdk=macosx; slice=macos-arm64
        minver=-mmacosx-version-min=13.0
        build_settings="-configuration Release -destination generic/platform=macOS ARCHS=arm64 ONLY_ACTIVE_ARCH=NO \
 MACOSX_DEPLOYMENT_TARGET=13.0 DEBUG_INFORMATION_FORMAT=dwarf" ;;
    *) echo "build-moltenvk: --platform is ios or macos, not $platform" >&2; exit 2 ;;
esac
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
src_in=$1
sc_in=$2

say() { echo "build-moltenvk: $*"; }
die() { echo "build-moltenvk: $*" >&2; exit 1; }

# refs/ is read only: refuse a path that is, or would be created, inside it
refs=$(cd "$repo/refs" 2>/dev/null && pwd -P) || refs=""
not_in_refs() {
    [ -n "$refs" ] || return 0
    p=$1; rest=""
    while [ ! -d "$p" ]; do rest="/$(basename "$p")$rest"; p=$(dirname "$p"); done
    case "$(cd "$p" && pwd -P)$rest/" in
        "$refs/"*) die "refusing a path inside refs/: $1" ;;
    esac
}
not_in_refs "$3"
not_in_refs "$4"
[ -d "$src_in" ] || die "no MoltenVK source tree at $src_in"
[ -d "$sc_in" ] || die "no SPIRV-Cross source tree at $sc_in"
src_in=$(cd "$src_in" && pwd)
sc_in=$(cd "$sc_in" && pwd)

# ---------------------------------------------------------------- 1. sources
# commit_of <dir>: the commit a fetched tree holds (git, else fetch-moltenvk.sh's .ios-vk-commit)
commit_of() {
    c=""
    if [ -f "$1/.ios-vk-commit" ]; then c=$(cat "$1/.ios-vk-commit"); fi
    if command -v git >/dev/null 2>&1 && git -C "$1" rev-parse HEAD >/dev/null 2>&1; then
        c=$(git -C "$1" rev-parse HEAD)
    fi
    echo "$c"
}
pin=$(sh "$here/fetch-moltenvk.sh" --print MoltenVK)
commit=$(echo "$pin" | awk '{ print $3 }')
tag=$(echo "$pin" | awk '{ print $4 }')
sc_pin=$(sh "$here/fetch-moltenvk.sh" --print SPIRV-Cross)
sc_commit=$(echo "$sc_pin" | awk '{ print $3 }')
have=$(commit_of "$src_in")
[ "$have" = "$commit" ] || die "$src_in is ${have:-no known commit}, moltenvk-sources.txt pins MoltenVK $commit ($tag)"
have=$(commit_of "$sc_in")
[ "$have" = "$sc_commit" ] || die "$sc_in is ${have:-no known commit}, moltenvk-sources.txt pins SPIRV-Cross $sc_commit"
for f in fetchDependencies MoltenVKPackaging.xcodeproj ExternalRevisions LICENSE; do
    [ -e "$src_in/$f" ] || die "$src_in has no $f: not a MoltenVK tree"
done
for f in spirv_msl.cpp LICENSE; do
    [ -e "$sc_in/$f" ] || die "$sc_in has no $f: not a SPIRV-Cross tree"
done
deps="SPIRV-Cross Vulkan-Headers cereal"
for d in $deps; do
    [ -f "$src_in/ExternalRevisions/${d}_repo_revision" ] || die "no ExternalRevisions/${d}_repo_revision"
    say "$d at $(head -n 1 "$src_in/ExternalRevisions/${d}_repo_revision") (ExternalRevisions)"
done
want_sc=$(head -n 1 "$src_in/ExternalRevisions/SPIRV-Cross_repo_revision" | tr -d '[:space:]')
[ "$want_sc" = "$sc_commit" ] || die "MoltenVK's ExternalRevisions names SPIRV-Cross $want_sc, moltenvk-sources.txt pins $sc_commit"
patches=$(LC_ALL=C ls "$here"/moltenvk-patches/*.patch 2>/dev/null || true)
sc_patches=$(LC_ALL=C ls "$here"/spirv-cross-patches/*.patch 2>/dev/null || true)
# MVK_PATCH_REVISION: 32 bits of the SHA-256 of both series, in the order they apply
# shellcheck disable=SC2086
patch_rev=$(cat /dev/null $patches $sc_patches | { if command -v sha256sum >/dev/null 2>&1; then sha256sum; else shasum -a 256; fi; } | cut -c1-8)
say "MoltenVK $tag ($commit), SPIRV-Cross $sc_commit; patches: $(echo "$patches" | grep -c . || true) MoltenVK, $(echo "$sc_patches" | grep -c . || true) SPIRV-Cross; MVK_PATCH_REVISION 0x$patch_rev; platform $platform"

defs='$(inherited) MVK_USE_METAL_PRIVATE_API=0'
# the geometry-shader mesh path (B3): sources that must be compiled for arm64 (for iOS: in an
# iphoneos build directory; a macOS build directory carries no platform name)
mesh_sources="MVKPipeline.mm MTLRenderPipelineDescriptor+MoltenVK.m"

if [ $dry -eq 1 ]; then
    say "dry run: would copy $src_in to $3/MoltenVK and $sc_in to $3/SPIRV-Cross, apply the patches with no fuzz, write MVKPatchRevision.h"
    say "dry run: would run ./fetchDependencies $fetch_flag --spirv-cross-root $3/SPIRV-Cross"
    say "dry run: would run xcodebuild -project MoltenVKPackaging.xcodeproj -scheme '$scheme_name' $build_settings GCC_PREPROCESSOR_DEFINITIONS='$defs' build"
    if [ "$platform" = ios ]; then
        say "dry run: would check MVK_USE_METAL_PRIVATE_API == 0, the mesh path ($mesh_sources, MTLMeshRenderPipelineDescriptor), run moltenvk-checks.sh, write $4/{libmoltenvk.a,include,LICENSES,BUILDINFO.txt}"
    else
        say "dry run: would check MVK_USE_METAL_PRIVATE_API == 0, the mesh path ($mesh_sources, MTLMeshRenderPipelineDescriptor), write $4/{libmoltenvk.a,include,LICENSES,BUILDINFO.txt} (no App Store checks: the render test's library)"
    fi
    exit 0
fi

[ "$(uname -s)" = Darwin ] || die "needs macOS with Xcode (use --dry-run elsewhere)"
mkdir -p "$3" "$4"
work=$(cd "$3" && pwd)
out=$(cd "$4" && pwd)
mvk="$work/MoltenVK"
sc="$work/SPIRV-Cross"
logs="$out/logs"
rm -rf "$mvk" "$sc" "$out/libmoltenvk.a" "$out/include" "$out/LICENSES" "$out/BUILDINFO.txt"
mkdir -p "$logs"
info="$out/BUILDINFO.txt"

say "copying the sources"
cp -R "$src_in" "$mvk"
cp -R "$sc_in" "$sc"
# apply_series <tree> <patches>: in order, no fuzz, a failure stops the build
apply_series() {
    t=$1; shift
    for p in "$@"; do
        say "patch $(basename "$p")"
        patch -p1 -N -F 0 -s -d "$t" < "$p" > "$logs/patch-$(basename "$p").log" 2>&1 || { cat "$logs/patch-$(basename "$p").log"; die "patch $(basename "$p") failed"; }
    done
}
# shellcheck disable=SC2086
apply_series "$mvk" $patches
# shellcheck disable=SC2086
apply_series "$sc" $sc_patches
gpu="$mvk/MoltenVK/MoltenVK/GPUObjects"
[ -f "$gpu/MVKDevice.mm" ] || die "no MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm"
cat > "$gpu/MVKPatchRevision.h" <<EOF
// Generated by build-moltenvk.sh (spec 34): 32 bits of the SHA-256 of the patch series
#define MVK_PATCH_REVISION 0x${patch_rev}u
EOF

say "fetchDependencies $fetch_flag --spirv-cross-root (log: logs/fetchDependencies.log)"
if ! (cd "$mvk" && ./fetchDependencies "$fetch_flag" --spirv-cross-root "$sc") > "$logs/fetchDependencies.log" 2>&1; then
    tail -n 60 "$logs/fetchDependencies.log"
    die "fetchDependencies failed"
fi
for d in $deps; do
    want=$(head -n 1 "$mvk/ExternalRevisions/${d}_repo_revision" | tr -d '[:space:]')
    dir="$mvk/External/$d"
    [ -d "$dir" ] || die "fetchDependencies left no External/$d"
    got=$(git -C "$dir" rev-parse HEAD)
    want_commit=$(git -C "$dir" rev-parse "$want^{commit}" 2>/dev/null || echo "$want")
    [ "$got" = "$want_commit" ] || die "External/$d is at $got, ExternalRevisions says $want"
done
[ "$(cd "$mvk/External/SPIRV-Cross" && pwd -P)" = "$(cd "$sc" && pwd -P)" ] || die "External/SPIRV-Cross is not the patched copy"

# ---------------------------------------------------------------- 2. build
scheme=$(cd "$mvk" && xcodebuild -list -project MoltenVKPackaging.xcodeproj 2>/dev/null |
         sed 's/^ *//; s/ *$//' | grep -xF "$scheme_name" | head -n 1 || true)
[ -n "$scheme" ] || { (cd "$mvk" && xcodebuild -list -project MoltenVKPackaging.xcodeproj) || true; die "no '$scheme_name' scheme (listed above)"; }
say "xcodebuild '$scheme' (log: logs/xcodebuild.log)"
# shellcheck disable=SC2086
if ! (cd "$mvk" && xcodebuild -project MoltenVKPackaging.xcodeproj -scheme "$scheme" $build_settings \
        GCC_PREPROCESSOR_DEFINITIONS="$defs" build) > "$logs/xcodebuild.log" 2>&1; then
    grep -E 'error:|BUILD FAILED' "$logs/xcodebuild.log" | head -n 40 || true
    tail -n 40 "$logs/xcodebuild.log"
    die "xcodebuild failed"
fi
lib=$(find "$mvk/Package" -path '*static*' -path "*$slice*" ! -path '*simulator*' ! -path '*maccatalyst*' -name 'libMoltenVK.a' 2>/dev/null | head -n 1)
[ -n "$lib" ] || { find "$mvk/Package" -name '*.a' | head -n 20; die "no static $slice libMoltenVK.a in Package/"; }
say "library: ${lib#"$mvk"/}"
cp "$lib" "$work/libmoltenvk-full.a"
if ! xcrun nm -g "$work/libmoltenvk-full.a" 2>/dev/null | grep -q spirv_cross; then
    if [ "$platform" = ios ]; then ext_os=iOS; else ext_os=macOS; fi
    cross=$(find "$mvk/External/build" -name 'libSPIRVCross.a' -path "*$ext_os*" ! -path '*imulator*' 2>/dev/null | head -n 1)
    [ -n "$cross" ] || die "libMoltenVK.a has no SPIRV-Cross and External/build has no $ext_os libSPIRVCross.a"
    say "merging ${cross#"$mvk"/}"
    xcrun libtool -static -o "$work/libmoltenvk-merged.a" "$work/libmoltenvk-full.a" "$cross"
    mv "$work/libmoltenvk-merged.a" "$work/libmoltenvk-full.a"
fi
xcrun strip -S -o "$out/libmoltenvk.a" "$work/libmoltenvk-full.a"

# ---------------------------------------------------------------- 3. checks
{
    echo "# BUILDINFO of libmoltenvk.a (spec 34 Part B4), platform $platform"
    echo "built $(date -u '+%Y-%m-%d %H:%M UTC') with $(xcodebuild -version | tr '\n' ' '), $sdk SDK $(xcrun --sdk "$sdk" --show-sdk-version)"
    echo "MoltenVK (utmapp, $tag) $commit, source sha256 (git archive) $(git -C "$src_in" archive --format=tar "$commit" | shasum -a 256 | awk '{print $1}')"
    echo "SPIRV-Cross (utmapp) $sc_commit, source sha256 (git archive) $(git -C "$sc_in" archive --format=tar "$sc_commit" | shasum -a 256 | awk '{print $1}')"
    for d in Vulkan-Headers cereal; do
        h=$(git -C "$mvk/External/$d" rev-parse HEAD)
        echo "$d $(head -n 1 "$mvk/ExternalRevisions/${d}_repo_revision") = $h, source sha256 (git archive) $(git -C "$mvk/External/$d" archive --format=tar "$h" | shasum -a 256 | awk '{print $1}')"
    done
    echo "MoltenVK patches: $(for p in $patches; do printf '%s ' "$(basename "$p" | cut -c1-4)"; done)"
    echo "SPIRV-Cross patches: $(for p in $sc_patches; do printf '%s ' "$(basename "$p" | cut -c1-4)"; done)"
    echo "MVK_PATCH_REVISION 0x$patch_rev"
    echo "settings: $build_settings GCC_PREPROCESSOR_DEFINITIONS=$defs"
} > "$info"

conf=$(grep -rlE '^[[:space:]]*#[[:space:]]*define[[:space:]]+MVK_USE_METAL_PRIVATE_API' "$mvk/Common" "$mvk/MoltenVK" 2>/dev/null | head -n 1)
[ -n "$conf" ] || die "no header of MoltenVK defines MVK_USE_METAL_PRIVATE_API"
cat > "$work/private-api-check.mm" <<EOF
// spec 34 B3: MoltenVK's configuration, as the build saw it, keeps Metal's private API off
#include "$(basename "$conf")"
#if !defined(MVK_USE_METAL_PRIVATE_API) || MVK_USE_METAL_PRIVATE_API != 0
#error "MVK_USE_METAL_PRIVATE_API is not 0"
#endif
int ios_vk_private_api_off = 1;
EOF
SDK=$(xcrun --sdk "$sdk" --show-sdk-path)
if ! xcrun --sdk "$sdk" clang++ -x objective-c++ -std=c++20 -arch arm64 -isysroot "$SDK" "$minver" \
        -DMVK_USE_METAL_PRIVATE_API=0 -I"$(dirname "$conf")" -I"$mvk/Common" \
        -c "$work/private-api-check.mm" -o "$work/private-api-check.o" > "$logs/private-api-check.log" 2>&1; then
    cat "$logs/private-api-check.log"
    die "the private-API check object didn't compile (${conf#"$mvk"/})"
fi
echo "MVK_USE_METAL_PRIVATE_API == 0 (compile-time check against ${conf#"$mvk"/})" >> "$info"

# the geometry-shader mesh path, compiled for the platform (clarification 1, item 3)
if [ "$platform" = ios ]; then dir_re=iphoneos; else dir_re=.; fi
for s in $mesh_sources; do
    line=$(grep -F "CompileC " "$logs/xcodebuild.log" | grep -F "/$s normal arm64" | grep -i "$dir_re" | head -n 1 || true)
    if [ -z "$line" ]; then
        grep -F "$s" "$logs/xcodebuild.log" | head -n 10 || true
        die "the build log shows no $sdk arm64 compile of $s (the geometry-shader mesh path)"
    fi
    echo "mesh path: $s compiled for $sdk arm64" >> "$info"
done
xcrun nm -u "$out/libmoltenvk.a" 2>/dev/null | grep -q '_OBJC_CLASS_\$_MTLMeshRenderPipelineDescriptor$' ||
    die "libmoltenvk.a doesn't refer to MTLMeshRenderPipelineDescriptor: the geometry-shader mesh path is not in it"
echo "mesh path: libmoltenvk.a refers to MTLMeshRenderPipelineDescriptor" >> "$info"

if [ "$platform" = ios ]; then
    NM="xcrun nm" AR="xcrun ar" SYM_PREFIX=_ sh "$here/moltenvk-checks.sh" "$out/libmoltenvk.a" "$info"
fi

# ---------------------------------------------------------------- 4. product
inc=$(find "$mvk/Package" -type d -name include -path '*MoltenVK*' 2>/dev/null | head -n 1)
[ -n "$inc" ] && [ -f "$inc/vulkan/vulkan.h" ] || die "no MoltenVK include/ with vulkan/vulkan.h in Package/"
cp -R "$inc" "$out/include"
mkdir -p "$out/LICENSES/MoltenVK" "$out/LICENSES/SPIRV-Cross" "$out/LICENSES/Vulkan-Headers" "$out/LICENSES/cereal" "$out/LICENSES/steamac"
cp "$mvk/LICENSE" "$out/LICENSES/MoltenVK/"
[ -f "$mvk/NOTICE" ] && cp "$mvk/NOTICE" "$out/LICENSES/MoltenVK/"
for d in SPIRV-Cross Vulkan-Headers cereal; do
    for f in LICENSE LICENSE.txt LICENSE.md NOTICE NOTICE.txt; do
        [ -f "$mvk/External/$d/$f" ] && cp "$mvk/External/$d/$f" "$out/LICENSES/$d/"
    done
    [ -d "$mvk/External/$d/LICENSES" ] && cp -R "$mvk/External/$d/LICENSES" "$out/LICENSES/$d/"
    [ -n "$(ls "$out/LICENSES/$d")" ] || die "no licence file in External/$d"
done
# steamac's patches (Apache-2.0 section 4(d)): the attribution lines of its NOTICE
cp "$here/moltenvk-patches/NOTICE-steamac.txt" "$out/LICENSES/steamac/NOTICE"
if [ "$platform" != ios ]; then
    echo "archive size: $(wc -c < "$out/libmoltenvk.a" | tr -d ' ') bytes" >> "$info"
    cat "$info"
    say "done: $out/libmoltenvk.a ($platform, for the render test)"
    exit 0
fi

cat > "$work/size-test.c" <<'EOF'
// spec 34 B4: the app-size cost: a dead-stripped link that calls only vkGetInstanceProcAddr
typedef void (*PFN_vkVoidFunction)( void );
extern PFN_vkVoidFunction vkGetInstanceProcAddr( void *instance, const char *name );
int main( void ) { return vkGetInstanceProcAddr( 0, "vkCreateInstance" ) ? 0 : 1; }
EOF
if ! xcrun --sdk iphoneos clang++ -arch arm64 -isysroot "$SDK" -miphoneos-version-min=17.0 -Wl,-dead_strip \
        "$work/size-test.c" "$out/libmoltenvk.a" -framework Metal -framework QuartzCore -framework IOSurface \
        -framework Foundation -framework CoreGraphics -framework UIKit -lc++ -o "$work/size-test" > "$logs/size-test.log" 2>&1; then
    cat "$logs/size-test.log"
    die "the size test link failed"
fi
{
    echo "archive size: $(wc -c < "$out/libmoltenvk.a" | tr -d ' ') bytes"
    echo "__TEXT of the dead-stripped test link: $(xcrun size -m "$work/size-test" | awk '/^Segment __TEXT:/ { print $3 }') bytes"
} >> "$info"
cat "$info"
say "done: $out/libmoltenvk.a"
