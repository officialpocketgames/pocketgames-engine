#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 05 item 91: no iOS object file references the mach-port sharing selectors, which aren't
# public API: createMachPort, initWithMachPort:, eventPort, newSharedEventWithMachPort:. An
# Objective-C selector is kept in the object as its own string (the method-name section), so a
# string that is exactly one of these names is a reference; a C symbol that merely contains one
# (_MTLSharedEvent_createMachPort, the unix-call handler's name) is not.
#
#   check-no-mach-port-selectors.sh <object or archive>...   (build-ios.sh: the unix side's objects)
#   check-no-mach-port-selectors.sh --self-test <work-dir>   (host: on objects compiled here)
#
# Prints "check-no-mach-port-selectors: clean (<n> files)" or each file and name found, and exits
# non-zero when any is found.

set -u

SELECTORS="createMachPort initWithMachPort: eventPort newSharedEventWithMachPort:"

# check <file>...: 0 when none of the files has a selector string
check() {
  found=0
  count=0
  for f in "$@"; do
    count=$((count + 1))
    if [ ! -f "$f" ]; then
      echo "check-no-mach-port-selectors: no such file: $f"
      found=1
      continue
    fi
    for s in $SELECTORS; do
      if strings -a "$f" | grep -qxF -- "$s"; then
        echo "check-no-mach-port-selectors: $f references $s"
        found=1
      fi
    done
  done
  if [ "$found" -eq 0 ]; then
    echo "check-no-mach-port-selectors: clean ($count files)"
  fi
  return "$found"
}

if [ "${1:-}" = "--self-test" ]; then
  dir=${2:?usage: $0 --self-test <work-dir>}
  mkdir -p "$dir" || exit 2
  cc=${CC:-cc}
  # one object with a selector-like string, one with only symbols that contain the names
  printf 'const char *volatile dxmt_sel = "initWithMachPort:";\n' >"$dir/mach_sel_bad.c"
  printf 'int MTLSharedEvent_createMachPort(void) { return 0; }\nconst char *volatile dxmt_n = "an eventPort here";\n' \
    >"$dir/mach_sel_good.c"
  $cc -c -o "$dir/mach_sel_bad.o" "$dir/mach_sel_bad.c" || exit 1
  $cc -c -o "$dir/mach_sel_good.o" "$dir/mach_sel_good.c" || exit 1
  status=0
  if check "$dir/mach_sel_bad.o" >/dev/null; then
    echo "check-no-mach-port-selectors: self-test FAILED (a selector string went unseen)"
    status=1
  fi
  if ! check "$dir/mach_sel_good.o" >/dev/null; then
    echo "check-no-mach-port-selectors: self-test FAILED (a symbol name was taken for a selector)"
    status=1
  fi
  [ "$status" -eq 0 ] && echo "check-no-mach-port-selectors: self-test passed"
  exit "$status"
fi

if [ $# -eq 0 ]; then
  echo "usage: $0 <object or archive>... | --self-test <work-dir>" >&2
  exit 2
fi
check "$@"
