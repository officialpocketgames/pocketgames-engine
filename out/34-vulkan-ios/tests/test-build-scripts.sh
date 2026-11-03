#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 34 Part M5 on the host: what of the iOS build can be checked without a Mac.
#   1. every script of the part, ci/ios-part.sh and ci/link-check.sh pass `sh -n`; no .c file is
#      given to clang++ (it would be compiled as C++)
#   2. fetch-moltenvk.sh --print gives the pins (UTM's forks, clarification 1); build-moltenvk.sh
#      --dry-run on fake MoltenVK and SPIRV-Cross trees prints the build with arm64, iOS 17.0,
#      MVK_USE_METAL_PRIVATE_API=0, the patch series and the mesh-path check, and refuses a tree at
#      another commit or a SPIRV-Cross pin MoltenVK's ExternalRevisions doesn't name
#   2b. the patch series: our patch last, author lines kept, the skipped ones absent, no game
#      names; with refs/'s copies of the forks, every patch applies in order with no fuzz
#   3. the scripts that write refuse a destination inside refs/, and create nothing there
#   4. gen-features.sh on a header spaced as Khronos' vulkan_core.h (the iOS build's input), and
#      (4b) on one with the header's own conditionals (VK_ENABLE_BETA_EXTENSIONS, #if 0 / #else):
#      the table compiles against it with and without the define, with the right entries
#   5. spec 34's win32u patches apply with no fuzz after spec 12's (macOS's patch doesn't fuzz)
# Environment: WINE_SRC (the Wine 11.4 tree, read only), MOLTENVK_REF and SPIRV_CROSS_REF (copies
# of the pinned forks, read only; default refs/moltenvk-utm-0560446 and refs/spirv-cross-utm-939b40b),
# TMPDIR.

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PART=$(cd "$HERE/.." && pwd)
REPO=$(cd "$PART/../.." && pwd)
WINE=${WINE_SRC:-"$REPO/refs/wine-11.4"}
T=${TMPDIR:-/tmp}/vk34-scripts.$$
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT
failures=0
bad() { echo "FAIL: $*"; failures=$((failures + 1)); }

# ---- 1
for s in "$PART"/*.sh "$PART"/layer/*.sh "$PART"/winevulkan-unix/*.sh "$PART"/tests/*.sh "$PART"/tests/windows/*.sh \
         "$PART"/tests/common/*.sh "$PART"/tests/macos/*.sh "$REPO/ci/ios-part.sh" "$REPO/ci/link-check.sh"; do
    sh -n "$s" || bad "sh -n $s"
done
# a C source given to clang++ is compiled as C++ (its calls mangled: the first CI run's size test
# couldn't find vkGetInstanceProcAddr); C goes through clang, clang++ only links or compiles .mm
c_to_cxx() {
    # each command with its continuation lines joined, comments left out
    awk '/^[[:space:]]*#/ { next }
         { if (sub(/\\$/, "")) { cmd = cmd $0; next } print cmd $0; cmd = "" }' "$1" |
        grep 'clang++' | grep -E '\.c("|[[:space:]]|$)' || true
}
for s in "$PART"/*.sh "$PART"/tests/macos/*.sh "$REPO/ci/link-check.sh"; do
    [ -z "$(c_to_cxx "$s")" ] || bad "$(basename "$s"): a .c file given to clang++: $(c_to_cxx "$s" | head -n 1)"
done
# the guard itself, on the first CI run's size test
printf '%s\n' 'if ! xcrun --sdk iphoneos clang++ -arch arm64 -Wl,-dead_strip \' '        "$work/size-test.c" -lc++ -o x; then' > "$T/cxx.sh"
[ -n "$(c_to_cxx "$T/cxx.sh")" ] || bad "the .c-through-clang++ guard misses a .c on a continuation line"

# ---- 2
pin=$(sh "$PART/fetch-moltenvk.sh" --print)
commit=$(echo "$pin" | awk '{ print $3 }')
[ "$pin" = "MoltenVK https://github.com/utmapp/MoltenVK.git 05604465d691118cfd20f53a48ecf1aad9c12f93 geometry-shaders" ] || bad "pin: $pin"
sc_pin=$(sh "$PART/fetch-moltenvk.sh" --print SPIRV-Cross)
sc_commit=$(echo "$sc_pin" | awk '{ print $3 }')
[ "$sc_pin" = "SPIRV-Cross https://github.com/utmapp/SPIRV-Cross.git 939b40b33a44443c404c4078823c406e3c94866f -" ] || bad "SPIRV-Cross pin: $sc_pin"
sh "$PART/fetch-moltenvk.sh" --print Nothing > /dev/null 2>&1 && bad "fetch-moltenvk.sh printed a pin for an unknown name"
fake="$T/MoltenVK"
fakesc="$T/SPIRV-Cross"
mkdir -p "$fake/ExternalRevisions" "$fake/MoltenVKPackaging.xcodeproj" "$fakesc"
: > "$fake/fetchDependencies"; : > "$fake/LICENSE"; : > "$fakesc/spirv_msl.cpp"; : > "$fakesc/LICENSE"
for d in Vulkan-Headers cereal; do echo "0123456789abcdef0123456789abcdef0123$d" > "$fake/ExternalRevisions/${d}_repo_revision"; done
echo "$sc_commit" > "$fake/ExternalRevisions/SPIRV-Cross_repo_revision"
echo "$commit" > "$fake/.ios-vk-commit"
echo "$sc_commit" > "$fakesc/.ios-vk-commit"
nmvk=$(ls "$PART"/moltenvk-patches/*.patch | grep -c .)
nsc=$(ls "$PART"/spirv-cross-patches/*.patch | grep -c .)
if sh "$PART/build-moltenvk.sh" --dry-run "$fake" "$fakesc" "$T/work" "$T/out" > "$T/dry.out" 2>&1; then
    for want in "MoltenVK geometry-shaders ($commit), SPIRV-Cross $sc_commit" "ARCHS=arm64" \
                "IPHONEOS_DEPLOYMENT_TARGET=17.0" "MVK_USE_METAL_PRIVATE_API=0" "MVK_EXCLUDE_SPIRV_TOOLS=1" \
                "SPIRV-Cross at $sc_commit" "Vulkan-Headers at 0123456789abcdef0123456789abcdef0123Vulkan-Headers" \
                "patches: $nmvk MoltenVK, $nsc SPIRV-Cross; MVK_PATCH_REVISION 0x" \
                "fetchDependencies --ios --spirv-cross-root $T/work/SPIRV-Cross" \
                "MVKPipeline.mm MTLRenderPipelineDescriptor+MoltenVK.m, MTLMeshRenderPipelineDescriptor" \
                "moltenvk-checks.sh"; do
        grep -qF "$want" "$T/dry.out" || bad "dry run doesn't say: $want"
    done
    [ ! -e "$T/work" ] || bad "the dry run created its work dir"
else
    bad "dry run failed: $(cat "$T/dry.out")"
fi
# the macOS build of the same sources, for the CI's render test (clarification 1, item 6)
if sh "$PART/build-moltenvk.sh" --dry-run --platform macos "$fake" "$fakesc" "$T/work" "$T/out" > "$T/drym.out" 2>&1; then
    for want in "platform macos" "fetchDependencies --macos --spirv-cross-root" "'MoltenVK Package (macOS only)'" \
                "MACOSX_DEPLOYMENT_TARGET=13.0" "MVK_USE_METAL_PRIVATE_API=0" "MVK_EXCLUDE_SPIRV_TOOLS=1" "no App Store checks"; do
        grep -qF "$want" "$T/drym.out" || bad "macOS dry run doesn't say: $want"
    done
    grep -qF "IPHONEOS_DEPLOYMENT_TARGET" "$T/drym.out" && bad "the macOS dry run has iOS settings"
else
    bad "macOS dry run failed: $(cat "$T/drym.out")"
fi
sh "$PART/build-moltenvk.sh" --dry-run --platform tvos "$fake" "$fakesc" "$T/work" "$T/out" > /dev/null 2>&1 &&
    bad "build-moltenvk.sh took --platform tvos"
echo 0000000000000000000000000000000000000000 > "$fake/.ios-vk-commit"
if sh "$PART/build-moltenvk.sh" --dry-run "$fake" "$fakesc" "$T/work" "$T/out" > "$T/dry2.out" 2>&1; then
    bad "build-moltenvk.sh took a MoltenVK tree at another commit"
fi
echo "$commit" > "$fake/.ios-vk-commit"
echo 0000000000000000000000000000000000000000 > "$fakesc/.ios-vk-commit"
if sh "$PART/build-moltenvk.sh" --dry-run "$fake" "$fakesc" "$T/work" "$T/out" > "$T/dry3.out" 2>&1; then
    bad "build-moltenvk.sh took a SPIRV-Cross tree at another commit"
fi
echo "$sc_commit" > "$fakesc/.ios-vk-commit"
echo 0123456789abcdef0123456789abcdef01234567 > "$fake/ExternalRevisions/SPIRV-Cross_repo_revision"
if sh "$PART/build-moltenvk.sh" --dry-run "$fake" "$fakesc" "$T/work" "$T/out" > "$T/dry4.out" 2>&1; then
    bad "build-moltenvk.sh took a SPIRV-Cross pin that MoltenVK's ExternalRevisions doesn't name"
fi
echo "$sc_commit" > "$fake/ExternalRevisions/SPIRV-Cross_repo_revision"

# ---- 2b: the patch series (clarification 1, item 2): name order, file names, author lines; on
# the pinned forks' trees when refs/ has them, every patch applies in order with no fuzz
ours=$(ls "$PART"/moltenvk-patches/0100-*.patch 2> /dev/null | head -n 1)
[ -n "$ours" ] && grep -q '^SPDX-License-Identifier: Apache-2.0' "$ours" || bad "no 0100 patch with its licence line"
after=$(ls "$PART"/moltenvk-patches/*.patch | LC_ALL=C sort | awk -F/ '{ print $NF }' | sed -n '/^0100-/,$p' | grep -c '^00' || true)
[ "$after" = 0 ] || bad "a steamac patch sorts after ours"
for p in "$PART"/moltenvk-patches/0[0-9][0-9][0-9]-*.patch "$PART"/spirv-cross-patches/*.patch; do
    case "$(basename "$p")" in 01*|02*) continue ;; esac
    grep -q '^From: ' "$p" && grep -q '^Subject: ' "$p" || bad "$(basename "$p") has lost its author lines"
done
for n in 0001 0012 0025 0031 0039 0043 0046 0047; do
    case $n in
        0001) ls "$PART"/moltenvk-patches/$n-*.patch > /dev/null 2>&1 || bad "MoltenVK $n (prerequisite of 0002) is missing" ;;
        *) ls "$PART"/moltenvk-patches/$n-*.patch > /dev/null 2>&1 && bad "MoltenVK $n should be skipped (CHANGES.md)" ;;
    esac
done
grep -q -i -E 'Stellar Blade|Counter-Strike|Left 4 Dead|Truck Simulator|Olden Era|Rogue Trader|TF2' \
    "$PART"/moltenvk-patches/*.patch "$PART"/spirv-cross-patches/*.patch && bad "a patch names a game"
U=${MOLTENVK_REF:-"$REPO/refs/moltenvk-utm-0560446"}
S=${SPIRV_CROSS_REF:-"$REPO/refs/spirv-cross-utm-939b40b"}
if [ -f "$U/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm" ] && [ -f "$S/spirv_msl.cpp" ]; then
    mkdir -p "$T/series"
    cp -R "$U" "$T/series/mvk"
    cp -R "$S" "$T/series/sc"
    for p in $(ls "$PART"/moltenvk-patches/*.patch | LC_ALL=C sort); do
        patch -p1 -N -s -F 0 -d "$T/series/mvk" < "$p" > "$T/patch.out" 2>&1 || bad "$(basename "$p") doesn't apply with no fuzz: $(head -n 5 "$T/patch.out")"
    done
    for p in $(ls "$PART"/spirv-cross-patches/*.patch | LC_ALL=C sort); do
        patch -p1 -N -s -F 0 -d "$T/series/sc" < "$p" > "$T/patch.out" 2>&1 || bad "SPIRV-Cross $(basename "$p") doesn't apply with no fuzz: $(head -n 5 "$T/patch.out")"
    done
    d="$T/series/mvk/MoltenVK/MoltenVK/GPUObjects/MVKDevice.mm"
    [ "$(grep -c 'mvkSupportsMeshEmulation(_gpuCapabilities)' "$d")" = 4 ] || bad "the mesh gating isn't on all four places of MVKDevice.mm"
    grep -q '_features.geometryShader = true' "$d" && bad "geometryShader is still reported as true everywhere"
    rm -rf "$T/series"
else
    echo "SKIPPED: applying the patch series (no refs/moltenvk-utm-0560446 or refs/spirv-cross-utm-939b40b)"
fi

# ---- 3
if [ -d "$REPO/refs" ]; then
    r="$REPO/refs/vk34-must-not-exist"
    sh "$PART/build-moltenvk.sh" --dry-run "$fake" "$fakesc" "$r/work" "$T/out" > /dev/null 2>&1 && bad "build-moltenvk.sh took a work dir in refs/"
    sh "$PART/build-moltenvk.sh" --dry-run "$fake" "$fakesc" "$T/work" "$r/out" > /dev/null 2>&1 && bad "build-moltenvk.sh took an out dir in refs/"
    sh "$PART/fetch-moltenvk.sh" "$r/src" SPIRV-Cross > /dev/null 2>&1 && bad "fetch-moltenvk.sh took a SPIRV-Cross destination in refs/"
    sh "$PART/fetch-moltenvk.sh" "$r/src" > /dev/null 2>&1 && bad "fetch-moltenvk.sh took a destination in refs/"
    sh "$PART/build-ios.sh" "$WINE" "$T" "$r/out" "$T" > /dev/null 2>&1 && bad "build-ios.sh took an out dir in refs/"
    sh "$PART/winevulkan-unix/build-ios.sh" "$WINE" "$T" "$r/out" > /dev/null 2>&1 && bad "winevulkan-unix/build-ios.sh took an out dir in refs/"
    sh "$PART/layer/gen-features.sh" "$WINE/include/wine/vulkan.h" "$REPO/refs/vk34.h" > /dev/null 2>&1 && bad "gen-features.sh wrote into refs/"
    [ ! -e "$r" ] && [ ! -e "$REPO/refs/vk34.h" ] || bad "something was created in refs/"
else
    echo "SKIPPED: the refs/ refusals (no refs/ folder)"
fi

# ---- 4
cat > "$T/vulkan_core.h" <<'EOF'
typedef enum VkStructureType {
    VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES = 49,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES = 1000082000,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ASTC_DECODE_FEATURES_EXT = 1000067001,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR = 1000286000,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR,
    VK_STRUCTURE_TYPE_MAX_ENUM = 0x7FFFFFFF
} VkStructureType;
typedef struct VkPhysicalDeviceFeatures {
    VkBool32    robustBufferAccess;
    VkBool32    geometryShader;
} VkPhysicalDeviceFeatures;
typedef struct VkPhysicalDeviceVulkan11Features {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           storageBuffer16BitAccess;
    VkBool32           multiview;
} VkPhysicalDeviceVulkan11Features;
typedef struct VkPhysicalDeviceShaderFloat16Int8Features {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           shaderFloat16;
    VkBool32           shaderInt8;
} VkPhysicalDeviceShaderFloat16Int8Features;
typedef struct VkPhysicalDeviceASTCDecodeFeaturesEXT {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           decodeModeSharedExponent;
} VkPhysicalDeviceASTCDecodeFeaturesEXT;
typedef struct VkPhysicalDeviceRobustness2FeaturesKHR {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           robustBufferAccess2;
} VkPhysicalDeviceRobustness2FeaturesKHR;
typedef struct VkPhysicalDeviceMixedFeaturesEXT {
    VkStructureType    sType;
    void*              pNext;
    uint32_t           notABool;
} VkPhysicalDeviceMixedFeaturesEXT;
typedef struct VkPhysicalDeviceUnknownFeaturesEXT {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           something;
} VkPhysicalDeviceUnknownFeaturesEXT;
EOF
sh "$PART/layer/gen-features.sh" "$T/vulkan_core.h" "$T/gen.h" 2> "$T/gen.err" > /dev/null || bad "gen-features.sh failed on a Khronos-style header"
grep -qx 'IVK_CORE_FEATURES(2, "robustBufferAccess" "\\0" "geometryShader")' "$T/gen.h" || bad "core features: $(grep CORE "$T/gen.h")"
grep -q '^IVK_FEATURE_STRUCT(VkPhysicalDeviceVulkan11Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, 2, storageBuffer16BitAccess,' "$T/gen.h" || bad "Vulkan 1.1 struct"
grep -q '^IVK_FEATURE_STRUCT(VkPhysicalDeviceShaderFloat16Int8Features, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES, 2,' "$T/gen.h" || bad "Float16Int8 struct"
grep -q '^IVK_FEATURE_STRUCT(VkPhysicalDeviceASTCDecodeFeaturesEXT, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ASTC_DECODE_FEATURES_EXT, 1,' "$T/gen.h" || bad "ASTC decode struct"
grep -q '^IVK_FEATURE_STRUCT(VkPhysicalDeviceRobustness2FeaturesKHR, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR, 1,' "$T/gen.h" || bad "Robustness2 struct"
grep -q 'Mixed' "$T/gen.h" && bad "a struct with a non-VkBool32 member was taken"
grep -q 'no sType found for VkPhysicalDeviceUnknownFeaturesEXT' "$T/gen.err" || bad "an unknown sType isn't reported"
grep -q '^#if' "$T/gen.h" && bad "an unconditional header gave conditional entries"

# ---- 4b: the header's own conditionals (the CI's first layer build: a provisional extension's
# sType is under #ifdef VK_ENABLE_BETA_EXTENSIONS in vulkan_core.h; the table named it anyway).
# The generated table must compile without the define, and hold both entries with it.
cat > "$T/beta_core.h" <<'EOF'
#ifndef VULKAN_CORE_H_
#define VULKAN_CORE_H_ 1
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
typedef uint32_t VkBool32;
typedef enum VkStructureType {
    VK_STRUCTURE_TYPE_APPLICATION_INFO = 0,
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES = 1000082000,
#ifdef VK_ENABLE_BETA_EXTENSIONS
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_METERING_FEATURES_NV = 1000613001,
#endif
#ifdef VK_ENABLE_BETA_EXTENSIONS
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR = 1000163000,
#endif
#if 0
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OTHER_FEATURES_EXT = 1000999000,
#else
    VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ELSE_FEATURES_EXT = 1000999001,
#endif /* 0 */
    VK_STRUCTURE_TYPE_MAX_ENUM = 0x7FFFFFFF
} VkStructureType;
typedef struct VkPhysicalDeviceFeatures {
    VkBool32    robustBufferAccess;
} VkPhysicalDeviceFeatures;
typedef struct VkPhysicalDeviceShaderFloat16Int8Features {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           shaderFloat16;
} VkPhysicalDeviceShaderFloat16Int8Features;
typedef struct VkPhysicalDevicePresentMeteringFeaturesNV {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           presentMetering;
} VkPhysicalDevicePresentMeteringFeaturesNV;
#ifdef VK_ENABLE_BETA_EXTENSIONS
typedef struct VkPhysicalDevicePortabilitySubsetFeaturesKHR {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           constantAlphaColorBlendFactors;
} VkPhysicalDevicePortabilitySubsetFeaturesKHR;
#endif
typedef struct VkPhysicalDeviceOtherFeaturesEXT {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           other;
} VkPhysicalDeviceOtherFeaturesEXT;
typedef struct VkPhysicalDeviceElseFeaturesEXT {
    VkStructureType    sType;
    void*              pNext;
    VkBool32           elseFeature;
} VkPhysicalDeviceElseFeaturesEXT;
#ifdef __cplusplus
}
#endif
#endif
EOF
if sh "$PART/layer/gen-features.sh" "$T/beta_core.h" "$T/beta_gen.h" > /dev/null 2>&1; then
    grep -q '^#if defined(VK_ENABLE_BETA_EXTENSIONS)$' "$T/beta_gen.h" || bad "conditionals: the provisional entries aren't under VK_ENABLE_BETA_EXTENSIONS: $(cat "$T/beta_gen.h")"
    grep -q '^#if defined(VK_ENABLE_BETA_EXTENSIONS) && defined(VK_ENABLE_BETA_EXTENSIONS)' "$T/beta_gen.h" &&
        bad "conditionals: one condition repeated"
    grep -q '^#if !((0))$' "$T/beta_gen.h" || bad "conditionals: the #else branch isn't !((0)): $(cat "$T/beta_gen.h")"
    grep -B1 'VkPhysicalDeviceShaderFloat16Int8Features' "$T/beta_gen.h" | grep -q '^#if' && bad "conditionals: an unconditional entry got a condition"
    cat > "$T/beta_use.c" <<'EOF'
/* the generated table used as ios_vk_features.c uses it, against the header it came from */
#include <stddef.h>
#include "beta_core.h"
struct entry { VkStructureType stype; size_t size; unsigned int n; const char *names; };
#define IVK_CORE_FEATURES(n, names)
#define IVK_FEATURE_STRUCT(type, stype, n, first, names) { stype, sizeof(type), n, names },
static const struct entry entries[] = {
#include "beta_gen.h"
};
/* the count, read through an entry so the table is used (clang's -Wunneeded-internal-declaration) */
int main( void ) { return entries[0].size ? (int)(sizeof(entries) / sizeof(entries[0])) : 100; }
EOF
    n=0
    if ${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -I"$T" "$T/beta_use.c" -o "$T/beta_use" 2> "$T/beta_use.err"; then
        "$T/beta_use" || n=$?
        [ "$n" = 2 ] || bad "conditionals: $n entries without VK_ENABLE_BETA_EXTENSIONS, wanted 2 (Float16Int8, Else)"
    else
        bad "conditionals: the table doesn't compile without VK_ENABLE_BETA_EXTENSIONS: $(head -n 3 "$T/beta_use.err")"
    fi
    n=0
    if ${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -DVK_ENABLE_BETA_EXTENSIONS -I"$T" "$T/beta_use.c" -o "$T/beta_use2" 2> "$T/beta_use.err"; then
        "$T/beta_use2" || n=$?
        [ "$n" = 4 ] || bad "conditionals: $n entries with VK_ENABLE_BETA_EXTENSIONS, wanted 4"
    else
        bad "conditionals: the table doesn't compile with VK_ENABLE_BETA_EXTENSIONS: $(head -n 3 "$T/beta_use.err")"
    fi
else
    bad "gen-features.sh failed on a header with conditionals"
fi

# ---- 5
if [ -f "$WINE/dlls/win32u/vulkan.c" ]; then
    P="$REPO/out/12-win32u-ios/patches"
    mkdir -p "$T/w/dlls/win32u"
    cp "$WINE/dlls/win32u/vulkan.c" "$WINE/dlls/win32u/d3dkmt.c" "$T/w/dlls/win32u/"
    # the earlier patches of these files, as syntax-check-patched.sh applies them
    for p in "$P"/*.patch; do
        case "$(basename "$p")" in S34-*) continue ;; esac
        grep -qE '^\+\+\+ b/dlls/win32u/(vulkan|d3dkmt)\.c' "$p" || continue
        patch -p1 -N -s -F 5 -d "$T/w" < "$p" > /dev/null 2>&1 || bad "earlier patch $(basename "$p") doesn't apply to vulkan.c/d3dkmt.c"
    done
    for p in "$P"/S34-*.patch; do
        patch -p1 -N -s -F 0 -d "$T/w" < "$p" > "$T/patch.out" 2>&1 || bad "$(basename "$p") doesn't apply with no fuzz: $(cat "$T/patch.out")"
    done
else
    echo "SKIPPED: the win32u patch check (no Wine tree)"
fi

[ $failures -eq 0 ] || exit 1
echo "test-build-scripts: all passed"
