#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 34 Part M5 (B3), on the host: moltenvk-checks.sh on small fake archives built with the host
# compiler (ELF: no symbol prefix). A good archive passes and lists its mmap reference and its
# required-reason API; one with a glslang object, one importing dlopen and one without
# SPIRV-Cross each fail with the reason.

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
for f in mvk cross glslang loader; do
    $CC -std=c11 -Wall -Wextra -Werror -c "$T/$f.c" -o "$T/$f.o"
done
ar rcs "$T/good.a" "$T/mvk.o" "$T/cross.o"
ar rcs "$T/glslang.a" "$T/mvk.o" "$T/cross.o" "$T/glslang.o"
ar rcs "$T/dlopen.a" "$T/mvk.o" "$T/cross.o" "$T/loader.o"
ar rcs "$T/nocross.a" "$T/mvk.o"

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

[ $failures -eq 0 ] || exit 1
echo "test-moltenvk-checks: all passed"
