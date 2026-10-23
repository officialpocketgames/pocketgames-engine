#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Checks of the CI itself that run anywhere (WSL here): the workflow parses as YAML and has the
# expected jobs, every ci/*.sh passes `sh -n` (and shellcheck when installed), fetch-refs.sh
# resolves every gitignored ref to a URL and a hash, and link-check.sh's parsing of the linker's
# messages works on a sample. Exits non-zero on any failure.
#   ci/check-ci.sh

set -u
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
wf="$repo/.github/workflows/engine.yml"
status=0
fail() { echo "FAIL: $*"; status=1; }

# 1. YAML
if command -v python3 >/dev/null 2>&1 && python3 -c 'import yaml' 2>/dev/null; then
    if jobs=$(python3 -I -c '
import sys, yaml
d = yaml.safe_load(open(sys.argv[1]))
on = d.get("on", d.get(True))
assert "push" in on and "workflow_dispatch" in on, "triggers"
print(" ".join(d["jobs"]))
' "$wf"); then
        echo "workflow jobs: $jobs"
        for j in refs host-tests wine-host-macos ios-libs link-check pe-farms; do
            case " $jobs " in *" $j "*) ;; *) fail "job $j missing" ;; esac
        done
    else
        fail "engine.yml does not parse"
    fi
elif command -v ruby >/dev/null 2>&1; then
    ruby -ryaml -e 'YAML.load_file(ARGV[0])' "$wf" && echo "workflow parses (ruby)" || fail "engine.yml does not parse"
else
    fail "no YAML parser (python3-yaml or ruby)"
fi
if grep -n "$(printf '\t')" "$wf"; then fail "tabs in engine.yml"; fi

# 2. shell syntax
for s in "$here"/*.sh; do
    sh -n "$s" || fail "sh -n $s"
done
if command -v shellcheck >/dev/null 2>&1; then
    shellcheck -s sh -S warning "$here"/*.sh || fail "shellcheck"
else
    echo "(shellcheck not installed)"
fi

# 3. fetch-refs.sh's reading of refs/README.md
out=$(FETCH_REFS_DRY=1 sh "$here/fetch-refs.sh" "${TMPDIR:-/tmp}/check-ci-dl.$$") || fail "fetch-refs.sh dry run"
echo "$out"
for n in $(sed -n 's|^\([A-Za-z0-9._+-]*\)/$|\1|p' "$repo/refs/.gitignore"); do
    line=$(echo "$out" | grep "^dry: $n <- ") || { fail "$n not resolved"; continue; }
    case "$line" in
        *"<- https://"*) ;;
        *) fail "$n: no URL" ;;
    esac
    case "$line" in
        *"sha256=none tree=none"*) fail "$n: neither a SHA-256 nor a tree digest" ;;
    esac
done
rmdir "${TMPDIR:-/tmp}/check-ci-dl.$$" 2>/dev/null
# the tree digests of ci/refs-trees.sha256 against the local copies, where there are any
# (FETCH_REFS_TREE_CHECK=0 skips this: it reads the whole tree)
if [ "${FETCH_REFS_TREE_CHECK:-1}" = 1 ]; then
    FETCH_REFS_TREE_ONLY=1 sh "$here/fetch-refs.sh" "${TMPDIR:-/tmp}/check-ci-dl.$$" ||
        fail "tree digests of the local refs/ differ from ci/refs-trees.sha256"
    rmdir "${TMPDIR:-/tmp}/check-ci-dl.$$" 2>/dev/null
fi

# 4. link-check.sh's parsing, on messages in ld's format (C++ names demangled, as ld prints them)
t=${TMPDIR:-/tmp}/check-ci-ld.$$
mkdir -p "$t"
cat > "$t/pass1.log" <<'LOG'
ld: warning: something harmless
duplicate symbol '_dup_one' in:
    /d/09-ntdll-processes/libntdll_unix.a[3](a.o)
    /d/13-net-crypto-ios/libnetcrypto_unix.a[7](b.o)
Undefined symbols for architecture arm64:
  "llvm::LLVMContext::LLVMContext()", referenced from:
      _airconv_x in libdxmt_unix.a[2](c.o)
  "_missing_fn", referenced from:
      _caller_one in libwineserver.a[4](d.o)
      _caller_two in libwin32u_unix.a[9](e.o)
ld: symbol(s) not found for architecture arm64
LOG
printf '%s\n' __ZN4llvm11LLVMContextC1Ev _malloc _missing_fn | sort > "$t/referenced.txt"
sed -n 's/^ *"\(.*\)", referenced from:$/\1/p' "$t/pass1.log" | sort -u > "$t/undefined-ld.txt"
sed -n "s/^duplicate symbol '\(.*\)' in:\$/\1/p" "$t/pass1.log" | sort -u > "$t/d"
if command -v c++filt >/dev/null 2>&1; then
    c++filt -_ < "$t/referenced.txt" > "$t/referenced-demangled.txt"
    paste "$t/referenced.txt" "$t/referenced-demangled.txt" |
        awk -F'\t' '{ if ($1 ~ /^__Z/) print $2 "\t" $1; else print $1 "\t" $1 }' > "$t/name-map.txt"
    awk -F'\t' 'NR == FNR { m[$1] = $2; next } { print (($0 in m) ? m[$0] : $0) "\t" $0 }' \
        "$t/name-map.txt" "$t/undefined-ld.txt" > "$t/pairs"
    [ "$(cut -f1 "$t/pairs" | sort | tr '\n' ' ')" = "__ZN4llvm11LLVMContextC1Ev _missing_fn " ] ||
        fail "name mapping: $(tr '\t\n' '| ' < "$t/pairs")"
    llvm_re=$(sed -n "s/^llvm_re='\(.*\)'\$/\1/p" "$here/link-check.sh")
    [ -n "$llvm_re" ] || fail "no llvm_re in link-check.sh"
    [ "$(grep -cE "$llvm_re" "$t/pairs")" = 1 ] || fail "LLVM classification"
else
    echo "(c++filt not installed: name mapping not checked)"
fi
[ "$(cat "$t/d")" = "_dup_one" ] || fail "duplicate parsing: $(cat "$t/d")"
rm -rf "$t"

# 5. ios-part.sh's keep-going copies of the build scripts that stop at their first failing file
for p in 08-wineserver-ios 15-controllers; do
    orig="$repo/out/$p/build-ios.sh"
    copy=$(sh "$here/ios-part.sh" --keep-going-copy "$orig")
    if [ "$copy" = "$orig" ]; then
        fail "$p: no keep-going copy made"
        continue
    fi
    echo "== $p keep-going rewrite:"
    diff "$orig" "$copy" | grep '^>' || true
    grep -q 'trap .*CI_FAILED' "$copy" || fail "$p: no EXIT trap"
    grep -q 'CI_FAILED=1;.*\(continue\|return 0\)$' "$copy" || fail "$p: failure exit not rewritten"
    rm -f "$copy"
done
# and in effect: a fake script whose second of three files fails runs all three, then exits 1
t=${TMPDIR:-/tmp}/check-ci-kg.$$
mkdir -p "$t"
cat > "$t/build-ios.sh" <<'EOF'
#!/bin/sh
set -eu
for src in a b c; do
    echo "CC $src"
    if [ "$src" = b ]; then
        echo "error: $src failed to compile:" >&2
        exit 1
    fi
done
echo "all done"
EOF
copy=$(sh "$here/ios-part.sh" --keep-going-copy "$t/build-ios.sh")
out=$(sh "$copy" 2>&1); rc=$?
case "$out" in
    *"CC c"*"all done"*"files failed to compile"*) [ $rc -eq 1 ] || fail "keep-going fake: exit $rc" ;;
    *) fail "keep-going fake: $out" ;;
esac
rm -rf "$t"

[ $status -eq 0 ] && echo "check-ci: all passed" || echo "check-ci: FAILURES"
exit $status
