#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 34 Part B3: the checks of the MoltenVK archive. Each failure fails the build.
#
#   moltenvk-checks.sh <archive> <report file>
#
# 1. Members: only MoltenVK and SPIRV-Cross (with MoltenVK's own SPIR-V to MSL converter). An
#    object whose name or whose symbols (defined or referenced) come from glslang or SPIRV-Tools
#    fails. vkGetInstanceProcAddr must be defined, and SPIRV-Cross's code must be in it.
# 2. Undefined symbols contain none of: dlopen, fork, vfork, execve, posix_spawn*, system,
#    popen, ptrace, task_for_pid, pthread_jit_write_protect_np.
# 3. Every mmap and mprotect reference is listed in the report for review, and the required-
#    reason APIs referenced (spec 00 D5) are named.
# The report is appended to (build-moltenvk.sh's BUILDINFO.txt).
#
# Environment: NM, AR (default: xcrun nm / xcrun ar on macOS, nm / ar elsewhere); SYM_PREFIX,
# the C symbol prefix of the object format ("_" for Mach-O, the default on macOS; "" for ELF, the
# host tests' fake archives).

set -eu
[ $# -eq 2 ] || { echo "usage: $0 <archive> <report file>" >&2; exit 2; }
lib=$1
report=$2
[ -f "$lib" ] || { echo "moltenvk-checks: no archive $lib" >&2; exit 1; }

if [ "$(uname -s)" = Darwin ]; then
    NM=${NM:-"xcrun nm"}; AR=${AR:-"xcrun ar"}; SYM_PREFIX=${SYM_PREFIX-_}
else
    NM=${NM:-nm}; AR=${AR:-ar}; SYM_PREFIX=${SYM_PREFIX-}
fi
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
status=0
fail() { echo "moltenvk-checks: FAIL: $*"; echo "FAIL: $*" >> "$report"; status=1; }

# names without the format's C prefix
strip_prefix() {
    if [ -n "$SYM_PREFIX" ]; then sed "s/^$SYM_PREFIX//"; else cat; fi
}

# shellcheck disable=SC2086
$AR t "$lib" > "$t/members"
# shellcheck disable=SC2086
$NM -g "$lib" 2>/dev/null > "$t/symbols" || true
# shellcheck disable=SC2086
$NM -u "$lib" 2>/dev/null | awk 'NF { print $NF }' | grep -v ':$' | strip_prefix | sort -u > "$t/undefined"
# defined (global) symbol names
awk 'NF >= 2 && $(NF-1) ~ /^[TDBSCRVW]$/ { print $NF }' "$t/symbols" | strip_prefix | sort -u > "$t/defined"

echo "== moltenvk-checks $(basename "$lib"): $(wc -l < "$t/members" | tr -d ' ') members, $(wc -l < "$t/defined" | tr -d ' ') defined, $(wc -l < "$t/undefined" | tr -d ' ') undefined symbols" | tee -a "$report"

# ---- 1. members
if grep -iE 'glslang|spvtools|spirv-tools|spirvtools' "$t/members" > "$t/bad-members"; then
    fail "glslang / SPIRV-Tools objects in the archive: $(tr '\n' ' ' < "$t/bad-members")"
fi
if grep -E 'glslang|spvtools' "$t/symbols" > "$t/bad-symbols"; then
    fail "glslang / SPIRV-Tools symbols in the archive ($(wc -l < "$t/bad-symbols" | tr -d ' ')), e.g. $(head -n 3 "$t/bad-symbols" | awk '{print $NF}' | tr '\n' ' ')"
fi
if ! grep -qx 'vkGetInstanceProcAddr' "$t/defined"; then fail "vkGetInstanceProcAddr isn't defined"; fi
if ! grep -q 'spirv_cross' "$t/defined"; then fail "no SPIRV-Cross code (no defined spirv_cross symbol)"; fi

# ---- 2. forbidden imports
for s in dlopen fork vfork execve system popen ptrace task_for_pid pthread_jit_write_protect_np; do
    if grep -qx "$s" "$t/undefined"; then fail "imports $s"; fi
done
if grep -E '^posix_spawn' "$t/undefined" > "$t/spawn"; then fail "imports $(tr '\n' ' ' < "$t/spawn")"; fi

# ---- 3. for review
{
    echo "mmap / mprotect references (spec 00 A1, for review):"
    grep -xE 'mmap|mprotect|vm_protect|mach_vm_protect|vm_allocate|mach_vm_allocate' "$t/undefined" | sed 's/^/  /' || echo "  none"
    echo "required-reason APIs referenced (spec 00 D5):"
    found=0
    for s in mach_absolute_time stat fstat lstat fstatat getattrlist fgetattrlist getattrlistat getattrlistbulk \
             statfs fstatfs statvfs fstatvfs 'OBJC_CLASS_$_NSUserDefaults'; do
        if grep -qxF "$s" "$t/undefined"; then echo "  $s"; found=1; fi
    done
    [ $found -eq 1 ] || echo "  none"
} >> "$report"

if [ $status -eq 0 ]; then echo "moltenvk-checks: passed" | tee -a "$report"; fi
exit $status
