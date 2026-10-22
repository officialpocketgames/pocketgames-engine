#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 19 Part C (slow, opt-in): build-pe.sh's tree step (B2) with fixture overlays against the
# real Wine tree. It copies the tree three times, a few minutes in WSL; run-host-tests.sh runs it
# only with SLOW=1.
#   test-tree.sh <scratch dir, on a Linux file system>
# Checks: a second run with nothing changed is skipped; after one overlay file changed and
# another was removed, the unchanged overlay file keeps its time, the changed one is newer, the
# removed one is upstream's content again with a newer time (so make rebuilds what changed and
# only that), and an upstream file keeps its time.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/.." && pwd)
repo=$(cd "$top/../.." && pwd)
wine=${WINE_SRC:-"$repo/refs/wine-11.4"}
dir=$1
work="$dir/work"
fx="$dir/out"
failures=0

bad() {
    echo "FAIL: $*" >&2
    failures=$((failures + 1))
}

if [ ! -f "$wine/VERSION" ]; then
    echo "SKIPPED: test-tree.sh (no Wine 11.4 tree at $wine)"
    exit 0
fi
# file modification time in seconds, GNU or BSD stat
mtime() {
    stat -c %Y "$1" 2>/dev/null || stat -f %m "$1"
}
run() {
    bash "$top/build-pe.sh" --wine "$wine" --out-root "$fx" --work "$work" --allow-missing-overlays tree 2>&1 |
        grep -v 'missing overlay'
}

rm -rf "$dir"
mkdir -p "$fx/19-pe-dlls-build/pe/dlls/nsi" "$fx/19-pe-dlls-build/pe/dlls/user32" "$fx/15-controllers/pe/dlls/dinput"
echo "/* one */" > "$fx/19-pe-dlls-build/pe/dlls/nsi/nsi.c"
echo "/* two */" > "$fx/19-pe-dlls-build/pe/dlls/user32/misc.c"
echo "/* three */" > "$fx/15-controllers/pe/dlls/dinput/dinput.c"

run > "$dir/run1.out"
grep -q copying "$dir/run1.out" || bad "run 1 didn't copy: $(cat "$dir/run1.out")"
cmp -s "$work/src/dlls/nsi/nsi.c" "$fx/19-pe-dlls-build/pe/dlls/nsi/nsi.c" || bad "run 1: nsi.c not overlaid"
a=$(mtime "$work/src/dlls/nsi/nsi.c")
b=$(mtime "$work/src/dlls/user32/misc.c")
c=$(mtime "$work/src/dlls/dinput/dinput.c")
k=$(mtime "$work/src/dlls/kernel32/heap.c")
[ "$k" = "$(mtime "$wine/dlls/kernel32/heap.c")" ] || bad "run 1: an upstream file didn't keep its time"
sleep 2

run > "$dir/run2.out"
grep -q copying "$dir/run2.out" && bad "run 2 copied the tree again with nothing changed"
sleep 2

echo "/* two, changed */" > "$fx/19-pe-dlls-build/pe/dlls/user32/misc.c"
rm "$fx/15-controllers/pe/dlls/dinput/dinput.c"
run > "$dir/run3.out"
grep -q copying "$dir/run3.out" || bad "run 3 didn't copy after an overlay changed"
[ "$(mtime "$work/src/dlls/nsi/nsi.c")" = "$a" ] || bad "run 3: the unchanged overlay file got a new time"
[ "$(mtime "$work/src/dlls/user32/misc.c")" -gt "$b" ] || bad "run 3: the changed overlay file isn't newer"
[ "$(mtime "$work/src/dlls/dinput/dinput.c")" -gt "$c" ] || bad "run 3: the file back to upstream isn't newer"
cmp -s "$work/src/dlls/dinput/dinput.c" "$wine/dlls/dinput/dinput.c" || bad "run 3: dinput.c isn't upstream's again"
[ "$(mtime "$work/src/dlls/kernel32/heap.c")" = "$k" ] || bad "run 3: an upstream file got a new time"

rm -rf "$dir"
if [ $failures -gt 0 ]; then
    echo "test-tree.sh: $failures failure(s)" >&2
    exit 1
fi
echo "test-tree.sh: all passed"
