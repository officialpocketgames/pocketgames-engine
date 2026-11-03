#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 34 Part M5 (B3), on the host: moltenvk-checks.sh on small fake archives built with the host
# compiler (ELF: no symbol prefix). A good archive passes and lists its mmap reference and its
# required-reason API; one with a glslang object, one importing dlopen and one without
# SPIRV-Cross each fail with the reason. SPIRV-Cross only as local symbols (MoltenVK's prelinked
# object) passes; unresolved SPIRV-Cross references and a SPIRV-Tools call fail. moltenvk-lib.sh:
# its SPIRV-Cross decision on each archive, its SPIRV-Tools list, and its XCFramework slice choice
# (lower-case slice folders, case ignored, simulator and Catalyst never, none or two refused).

set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
PART=$(cd "$HERE/.." && pwd)
CC=${CC:-cc}
T=${TMPDIR:-/tmp}/vk34-mvkchecks.$$
mkdir -p "$T"
trap 'rm -rf "$T"' EXIT
failures=0
bad() { echo "FAIL: $*"; failures=$((failures + 1)); }

cat > "$T/mvk.c" <<'EOF'
extern unsigned long long mach_absolute_time( void );
extern void *mmap( void *addr, unsigned long len, int prot, int flags, int fd, long off );
void *vkGetInstanceProcAddr( void *instance, const char *name )
{
    (void)instance; (void)name;
    return mach_absolute_time() ? mmap( 0, 16384, 3, 0x1002, -1, 0 ) : 0;
}
EOF
cat > "$T/cross.c" <<'EOF'
int MVK_spirv_cross_compile( int x ) { return x + 1; }
EOF
cat > "$T/glslang.c" <<'EOF'
int glslang_compile_shader( int x ) { return x * 2; }
EOF
cat > "$T/loader.c" <<'EOF'
extern void *dlopen( const char *path, int mode );
void *mvk_load( void ) { return dlopen( "libsomething.dylib", 1 ); }
EOF
# MoltenVK's prelinked object: SPIRV-Cross only as local symbols (hidden), which nm -g doesn't show
cat > "$T/prelinked.c" <<'EOF'
static int MVK_spirv_cross_compile_local( int x ) { return x + 1; }
int mvk_convert( int x ) { return MVK_spirv_cross_compile_local( x ); }
EOF
# a converter that calls SPIRV-Cross without having it, and one that calls SPIRV-Tools
cat > "$T/needs_cross.c" <<'EOF'
extern int MVK_spirv_cross_compile( int x );
int mvk_convert2( int x ) { return MVK_spirv_cross_compile( x ); }
EOF
cat > "$T/tools.c" <<'EOF'
extern void *spvContextCreate( int env );
void *mvk_log_spirv( void ) { return spvContextCreate( 0 ); }
EOF
for f in mvk cross glslang loader prelinked needs_cross tools; do
    $CC -std=c11 -O0 -Wall -Wextra -Werror -c "$T/$f.c" -o "$T/$f.o"
done
ar rcs "$T/good.a" "$T/mvk.o" "$T/cross.o"
ar rcs "$T/glslang.a" "$T/mvk.o" "$T/cross.o" "$T/glslang.o"
ar rcs "$T/dlopen.a" "$T/mvk.o" "$T/cross.o" "$T/loader.o"
ar rcs "$T/nocross.a" "$T/mvk.o"
ar rcs "$T/prelinked.a" "$T/mvk.o" "$T/prelinked.o"
ar rcs "$T/unresolved.a" "$T/mvk.o" "$T/needs_cross.o"
ar rcs "$T/resolved.a" "$T/mvk.o" "$T/needs_cross.o" "$T/cross.o"
ar rcs "$T/tools.a" "$T/mvk.o" "$T/cross.o" "$T/tools.o"

run() { NM=nm AR=ar SYM_PREFIX= sh "$PART/moltenvk-checks.sh" "$T/$1.a" "$T/$1.report" > "$T/$1.out" 2>&1; }

if run good; then
    grep -q '^  mmap$' "$T/good.report" || bad "the mmap reference isn't listed"
    grep -q '^  mach_absolute_time$' "$T/good.report" || bad "mach_absolute_time isn't listed as a required-reason API"
else
    bad "a good archive failed: $(cat "$T/good.out")"
fi
if run glslang; then bad "an archive with glslang passed"; else grep -q 'glslang' "$T/glslang.out" || bad "glslang not named"; fi
if run dlopen; then bad "an archive importing dlopen passed"; else grep -q 'imports dlopen' "$T/dlopen.out" || bad "dlopen not named"; fi
if run nocross; then bad "an archive without SPIRV-Cross passed"; else grep -q 'SPIRV-Cross' "$T/nocross.out" || bad "SPIRV-Cross not named"; fi
run prelinked || bad "SPIRV-Cross as the local symbols of a prelinked object wasn't taken: $(cat "$T/prelinked.out")"
if run unresolved; then bad "unresolved SPIRV-Cross references passed"; else grep -q 'referred to but not defined' "$T/unresolved.out" || bad "unresolved SPIRV-Cross not named"; fi
if run tools; then bad "an archive calling SPIRV-Tools passed"; else grep -q 'spvContextCreate' "$T/tools.out" || bad "the SPIRV-Tools reference not named"; fi

# ---- moltenvk-lib.sh spirv-cross / spirv-tools: what build-moltenvk.sh decides on
lib() { NM=nm sh "$PART/moltenvk-lib.sh" "$@"; }
for c in good:inside prelinked:inside unresolved:merge resolved:inside nocross:absent; do
    got=$(lib spirv-cross "$T/${c%%:*}.a")
    [ "$got" = "${c#*:}" ] || bad "spirv-cross on ${c%%:*}.a: $got, wanted ${c#*:}"
done
[ -z "$(lib spirv-tools "$T/good.a")" ] || bad "spirv-tools found something in good.a"
[ "$(lib spirv-tools "$T/tools.a")" = spvContextCreate ] || bad "spirv-tools on tools.a: $(lib spirv-tools "$T/tools.a")"

# ---- moltenvk-lib.sh slice: the XCFramework slice folders, lower case, as the CI's log shows them
X="$T/build/Release/SPIRVCross.xcframework"
for s in ios-arm64 ios-arm64_x86_64-simulator ios-arm64_x86_64-maccatalyst macos-arm64_x86_64; do
    mkdir -p "$X/$s"; : > "$X/$s/libSPIRVCross.a"
done
mkdir -p "$T/build/Release/SPIRVTools.xcframework/ios-arm64"; : > "$T/build/Release/SPIRVTools.xcframework/ios-arm64/libSPIRVTools.a"
[ "$(lib slice "$T/build" SPIRVCross.xcframework libSPIRVCross.a ios-arm64 2>/dev/null)" = "$X/ios-arm64/libSPIRVCross.a" ] ||
    bad "slice: the ios-arm64 libSPIRVCross.a wasn't the one found"
[ "$(lib slice "$T/build" SPIRVCross.xcframework libSPIRVCross.a macos-arm64 2>/dev/null)" = "$X/macos-arm64_x86_64/libSPIRVCross.a" ] ||
    bad "slice: the macos-arm64_x86_64 libSPIRVCross.a wasn't the one found"
# without regard to case (the old lookup looked for "iOS")
mkdir -p "$T/upper/SPIRVCross.xcframework/IOS-ARM64"; : > "$T/upper/SPIRVCross.xcframework/IOS-ARM64/libSPIRVCross.a"
[ -n "$(lib slice "$T/upper" SPIRVCross.xcframework libSPIRVCross.a ios-arm64 2>/dev/null)" ] || bad "slice: an upper-case slice folder wasn't found"
# none, or two: refused, and what there is listed
mkdir -p "$T/none/SPIRVCross.xcframework/ios-arm64_x86_64-simulator"; : > "$T/none/SPIRVCross.xcframework/ios-arm64_x86_64-simulator/libSPIRVCross.a"
if lib slice "$T/none" SPIRVCross.xcframework libSPIRVCross.a ios-arm64 > /dev/null 2> "$T/none.err"; then bad "slice: a simulator-only XCFramework was taken"
else grep -q 'simulator/libSPIRVCross.a' "$T/none.err" || bad "slice: the archives there weren't listed"; fi
mkdir -p "$T/build/Debug/SPIRVCross.xcframework/ios-arm64"; : > "$T/build/Debug/SPIRVCross.xcframework/ios-arm64/libSPIRVCross.a"
if lib slice "$T/build" SPIRVCross.xcframework libSPIRVCross.a ios-arm64 > /dev/null 2> "$T/two.err"; then bad "slice: two candidates were taken"
else grep -q '2 candidates' "$T/two.err" || bad "slice: two candidates not reported: $(cat "$T/two.err")"; fi

[ $failures -eq 0 ] || exit 1
echo "test-moltenvk-checks: all passed"
