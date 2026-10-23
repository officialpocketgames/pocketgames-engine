#!/bin/sh
# SPDX-License-Identifier: MIT
# Pocket Games engine: fail on API that an App Store build must not use (the App Store rules).
# Usage: check-app-store.sh <dir>...   (default: out/)
# A line is exempt when it is a comment, or carries the marker "app-store-ok: <reason>".
set -u
[ $# -gt 0 ] || set -- "$(dirname "$0")/out"
pattern='MAP_JIT|pthread_jit_write_protect|\bcsops\b|mach_memory_entry_ownership|\btask_for_pid\b|\bptrace\(|\bfork\(|\bvfork\(|\bexecv[pe]?\(|\bexecl[pe]?\(|posix_spawn|[^_a-zA-Z]system\(|\bpopen\(|dlopen\( *"/(usr|System)|com\.apple\.private|processor_set_tasks|dynamic-codesigning|get-task-allow|"/usr/lib/[^"]*.dylib"|"/System/Library/[^"]*"'
status=0
for dir in "$@"; do
    hits=$(grep -rnE "$pattern" "$dir" \
        --include='*.c' --include='*.h' --include='*.m' --include='*.mm' --include='*.cpp' \
        --include='*.hpp' --include='*.swift' --include='*.S' --include='*.s' \
        --include='*.entitlements' --include='*.plist' 2>/dev/null |
        grep -v '/tests/' | grep -v 'app-store-ok:' |
        grep -vE ':[0-9]+:\s*(//|/?\*|#)')
    if [ -n "$hits" ]; then
        echo "$hits"
        status=1
    fi
done
[ $status -eq 0 ] && echo "check-app-store: clean" || echo "check-app-store: forbidden API found (the App Store rules)"
exit $status
