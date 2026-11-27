#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 05 item 91, on the host: the unix side's sources as the iOS build preprocesses them
# (DXMT_IOS defined) name none of the mach-port sharing selectors (createMachPort,
# initWithMachPort:, eventPort, newSharedEventWithMachPort:), while the macOS preprocessing still
# does (so the check sees the code it should). The real preprocessor runs over the sources, with
# every system header replaced by an empty one (the Apple SDK isn't here); comments are gone, and a
# name that is part of a longer one (_MTLSharedEvent_createMachPort) doesn't count. The device
# build checks its objects too (check-no-mach-port-selectors.sh in build-ios.sh).
#
#   check-ios-sources-mach-ports.sh <dxmt/src/winemetal/unix>
#
# Needs clang (an Objective-C preprocessor); says "skipped" without it.

set -u

dir=${1:?usage: $0 <winemetal/unix directory>}
cpp=${OBJC_CPP:-clang}
if ! command -v "$cpp" >/dev/null 2>&1; then
  echo "check-ios-sources-mach-ports: skipped (no $cpp)"
  exit 0
fi

work=$(mktemp -d) || exit 2
trap 'rm -rf "$work"' EXIT INT TERM
mkdir -p "$work/sys"

# the include paths build-ios.sh gives the unix side (the tree is dxmt/, three levels up)
tree=$(cd "$dir/../../.." && pwd) || exit 2
includes="-I$tree/src/winemetal -I$tree/src/airconv -I$tree/include -I$tree/libs -I$tree/include/native/windows \
  -I$tree/include/native/directx"

# preprocess <source> <out> [defines]: a header the preprocessor can't find becomes an empty
# stand-in when the tree has no file of that name (a system header); one the tree has means a
# wrong include path, which fails
preprocess() {
  src=$1
  out=$2
  shift 2
  tries=0
  while :; do
    # shellcheck disable=SC2086
    if "$cpp" -E -P -x objective-c -nostdinc -isystem "$work/sys" $includes "$@" -w "$src" >"$out" 2>"$work/err"; then
      return 0
    fi
    missing=$(sed -n "s/.*fatal error: '\([^']*\)' file not found.*/\1/p" "$work/err" | head -n 1)
    tries=$((tries + 1))
    if [ -z "$missing" ] || [ "$tries" -gt 200 ]; then
      cat "$work/err"
      return 1
    fi
    if [ -n "$(find "$tree" -name "$(basename "$missing")" -print 2>/dev/null | head -n 1)" ]; then
      echo "check-ios-sources-mach-ports: $missing is in the tree but not on the include path"
      return 1
    fi
    mkdir -p "$work/sys/$(dirname "$missing")" && : >"$work/sys/$missing"
  done
}

pattern='(^|[^A-Za-z0-9_])(createMachPort|initWithMachPort|eventPort|newSharedEventWithMachPort)([^A-Za-z0-9_]|$)'
status=0
for src in winemetal_unix.c ios_support.m; do
  [ -f "$dir/$src" ] || continue
  if ! preprocess "$dir/$src" "$work/ios.i" -DDXMT_IOS=1; then
    echo "check-ios-sources-mach-ports: $src: the iOS preprocessing failed"
    status=1
    continue
  fi
  if grep -nE "$pattern" "$work/ios.i" >"$work/hits"; then
    echo "check-ios-sources-mach-ports: $src names a mach-port selector in the iOS build:"
    cat "$work/hits"
    status=1
  fi
done

# the macOS preprocessing of winemetal_unix.c keeps its mach-port path
if preprocess "$dir/winemetal_unix.c" "$work/mac.i"; then
  if ! grep -qE "$pattern" "$work/mac.i"; then
    echo "check-ios-sources-mach-ports: the macOS preprocessing has no mach-port selector either: the check sees nothing"
    status=1
  fi
else
  echo "check-ios-sources-mach-ports: the macOS preprocessing failed"
  status=1
fi

[ "$status" -eq 0 ] && echo "check-ios-sources-mach-ports: clean (iOS preprocessing of the unix side)"
exit "$status"
