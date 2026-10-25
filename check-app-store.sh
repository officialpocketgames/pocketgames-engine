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

# Part G.4: the old working name (any case) and "winios" (any case) appear nowhere in the
# engine, comments and tests included, outside refs/; the one exception is Wine's time zone
# Atlantic/<the old word>. The word is assembled here so this script doesn't carry it.
old_word='made''ira'
old_zone='Atlantic/Made''ira'
names=$(dirname "$0")/interfaces
for dir in "$@" "$names"; do
    [ -e "$dir" ] || continue
    hits=$(grep -rnIiE "${old_word}|winios" "$dir" 2>/dev/null |
        grep -v '/refs/' | sed "s#${old_zone}##g" | grep -iE "${old_word}|winios")
    if [ -n "$hits" ]; then
        echo "$hits"
        echo "check-app-store: an old name is left (the App Store rules Part G)"
        status=1
    fi
done

[ $status -eq 0 ] && echo "check-app-store: clean" || echo "check-app-store: forbidden API or name found (the App Store rules)"
exit $status
