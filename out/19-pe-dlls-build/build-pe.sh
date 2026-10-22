#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 19 Part B: builds the three PE farms the app ships (arm64ec-windows, aarch64-windows,
# i386-windows) from refs/wine-11.4 plus this repository's overlays, checks them and hands them to
# the owner's packaging as <dest>/.
#
#   build-pe.sh [options] <step>
#
# Steps (each finished step leaves a stamp in <work>/stamps and is skipped next time unless its
# inputs changed; a step runs the earlier steps it needs):
#   toolchain  B1: llvm-mingw 20260421 (UCRT), downloaded and checked by SHA-256, or --toolchain
#   tree       B2: <work>/src, a copy of the Wine tree with the overlays applied
#   plan       builds nothing: the overlays and their files, B2's errors, each farm's targets
#   arm64ec    B3: the arm64ec tree (it also builds Wine's host tools for the other two)
#   aarch64    B3: the aarch64 tree
#   i386       B3: the i386 tree
#   collect    B4 closure, B5: strip, pecheck, NLS tables, MANIFEST.txt, BUILDINFO.txt
#   kit        spec 00 E1: the LGPL relinking kit, <work>/pe-relink-kit.tar.xz (the exact
#              overlaid source tree the farms were built from, this script, the farm lists,
#              pecheck, BUILDINFO.txt and MANIFEST.txt, and how to rebuild)
#   all        every step above in order (plan excepted)
#
# Options:
#   --wine <tree>            the Wine 11.4 tree (default: refs/wine-11.4 next to out/)
#   --out-root <dir>         this repository's out/ (default: the folder above this script's)
#   --work <dir>             sources, build trees, toolchain, logs (default: build/pe-work next
#                            to out/; in WSL use a Linux folder such as ~/pe-work, /mnt/c is slow).
#                            It and --dest may not resolve into refs/ or the Wine tree (read-only)
#   --dest <dir>             the product (default: <work>/dest)
#   --toolchain <dir>        an unpacked llvm-mingw; without it the B1 asset for this host is
#                            downloaded into <work> and checked
#   --extra <farm>=<dir>     copy the files of <dir> into that farm before the checks (repeatable)
#   --jobs <n>               parallel jobs (default: the CPU count)
#   --allow-missing-overlays log each missing overlay and carry on (checking before every spec
#                            is implemented)
#
# Runs on macOS's bash 3.2 (CI) and on Linux (WSL, for checking): no associative arrays, no
# mapfile, no case conversion in expansions; BSD and GNU sed, find and cp alike.

set -euo pipefail

# ---- B1: the toolchain pin ----
LLVM_MINGW_RELEASE=20260421
LLVM_MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/$LLVM_MINGW_RELEASE"
LLVM_MINGW_MACOS_ASSET=llvm-mingw-20260421-ucrt-macos-universal.tar.xz
LLVM_MINGW_MACOS_SHA256=bd85a3975723815cef28dbbd2ca2cb0c926f6b348a12a0453f39f7af273cb3f7
LLVM_MINGW_LINUX_ASSET=llvm-mingw-20260421-ucrt-ubuntu-22.04-x86_64.tar.xz
LLVM_MINGW_LINUX_SHA256=f8b8cce779affeab47bcaec6ce6e9e768b166094a366123ef64b2d0b389cc121

# ---- B2: the overlays, applied in this order ----
# <name>|<source, relative to --out-root>|<path in the tree ("." = the tree root)>|<spec>
# A source that is a file is a single-file row: only that file is taken.
overlay_table() {
    cat <<'EOF'
spec17-include|09-ntdll-processes/include|include|17
spec17-unixlib|09-ntdll-processes/ntdll-unix/unixlib.h|dlls/ntdll/unixlib.h|17
spec14-metahost|14-media-audio-ios/pe/metahost.c|dlls/mscoree/metahost.c|14
spec15|15-controllers/pe|.|15
spec16|16-sync-ios/pe|.|16
spec18|18-pe-ntdll-wow64/pe|.|18
spec19|19-pe-dlls-build/pe|.|19
EOF
}

# ---- B3: configure ----
CONFIGURE_COMMON="--disable-tests --without-x --without-freetype --without-gnutls --without-vulkan --without-wayland --enable-winegstreamer"
# The compiler. aarch64 and i386: --with-mingw=llvm-mingw, configure's <cpu>-w64-mingw32-clang
# (the GNU triple). ARM64EC: the toolchain's plain clang (--with-mingw=<toolchain>/bin/clang),
# for which configure targets "arm64ec-windows", the MSVC environment. With the GNU triple, clang
# defines __x86_64__ next to __arm64ec__ and no _MSC_VER, so winnt.h's GNU branch compiles the x86
# inline assembly of __fastfail ("int $0x29" with a "c" operand), which clang rejects for ARM64EC
# in every file; the MSVC environment takes winnt.h's _MSC_VER/__arm64ec__ branches instead.
# (QUESTIONS 19-13; configure.ac: a plain clang sets no target prefix, so llvm_target is
# "$mingw_cpu-windows". The extra x86_64 compiler of the ARM64EC tree is the same clang.)
mingw_option() { # <arch>
    if [ "$1" = arm64ec ]; then echo "--with-mingw=$toolchain_dir/bin/clang"
    else echo "--with-mingw=llvm-mingw"
    fi
}
CROSS_CFLAGS="-O2"
# host-only programs upstream enables for the build host's architecture only (aarch64 on an
# Apple-silicon Mac, x86_64 in WSL): enabled for aarch64 in the aarch64 tree on every host
AARCH64_PROGRAMS="conhost rpcss services wineboot"

FARMS="arm64ec-windows aarch64-windows i386-windows"
# B4 item 3: the i386 farm's file extensions
PE_EXTS="dll exe drv cpl acm ax ocx tlb msstyles com"

# ---- options ----
script_dir=$(cd "$(dirname "$0")" && pwd)
script_path="$script_dir/$(basename "$0")"
out_root=$(cd "$script_dir/.." && pwd)
wine=""
work=""
dest=""
toolchain_opt=""
extras=""
jobs=""
allow_missing=0
step=""

usage() {
    sed -n '5,/^# Runs on/p' "$script_path" | sed '$d' | sed 's/^# \{0,1\}//'
    exit 2
}

while [ $# -gt 0 ]; do
    case "$1" in
        --wine) wine=$2; shift 2 ;;
        --out-root) out_root=$2; shift 2 ;;
        --work) work=$2; shift 2 ;;
        --dest) dest=$2; shift 2 ;;
        --toolchain) toolchain_opt=$2; shift 2 ;;
        --extra) extras="$extras$2
"; shift 2 ;;
        --jobs) jobs=$2; shift 2 ;;
        --allow-missing-overlays) allow_missing=1; shift ;;
        -h|--help) usage ;;
        -*) echo "build-pe: unknown option $1" >&2; usage ;;
        *) [ -z "$step" ] || usage; step=$1; shift ;;
    esac
done
[ -n "$step" ] || usage

# physical paths (symlinks resolved): a symlinked --wine must never make <work>/src a link to
# the reference tree, or the overlays would be written into it
abspath() { (cd -P "$1" && pwd -P); }
# the physical path of a folder that may not exist yet: its nearest existing parent, resolved
physpath() {
    local p=$1 rest=""
    while [ ! -d "$p" ]; do
        rest="/$(basename "$p")$rest"
        p=$(dirname "$p")
    done
    echo "$(abspath "$p")$rest"
}
# is <path> the folder <dir> or inside it?
inside() {
    case "$1/" in "$2"/*) return 0 ;; esac
    return 1
}

out_root=$(abspath "$out_root")
[ -n "$wine" ] || wine="$out_root/../refs/wine-11.4"
[ -d "$wine" ] || { echo "build-pe: no Wine tree at $wine (--wine)" >&2; exit 1; }
wine=$(abspath "$wine")
[ -n "$work" ] || work="$out_root/../build/pe-work"
work=$(physpath "$work")
[ -n "$dest" ] || dest="$work/dest"
dest=$(physpath "$dest")
# refs/ is read-only (CLAUDE.md), and so is the Wine tree given: nothing this script writes may
# resolve into either, and the Wine tree may not lie inside the work folder (the tree step
# replaces <work>/src). Checked before anything is created.
refs_dir=$(physpath "$out_root/../refs")
for written in "$work" "$dest"; do
    for ro in "$refs_dir" "$wine"; do
        if inside "$written" "$ro"; then
            echo "build-pe: refusing to write $written: it lies inside $ro, which is read-only" >&2
            exit 1
        fi
    done
done
if inside "$wine" "$work"; then
    echo "build-pe: refusing: the Wine tree $wine lies inside the work folder $work" >&2
    exit 1
fi
mkdir -p "$work"
farms_dir="$script_dir/farms"
src="$work/src"
stamps="$work/stamps"
logs="$work/logs"
mkdir -p "$stamps" "$logs"

# ---- host helpers ----
if command -v sha256sum >/dev/null 2>&1; then SHA256="sha256sum"
elif command -v shasum >/dev/null 2>&1; then SHA256="shasum -a 256"
else echo "build-pe: neither sha256sum nor shasum found" >&2; exit 1
fi
sha256_file() { $SHA256 "$1" | awk '{print $1}'; }
sha256_stdin() { $SHA256 | awk '{print $1}'; }
lower() { tr 'ABCDEFGHIJKLMNOPQRSTUVWXYZ' 'abcdefghijklmnopqrstuvwxyz'; }
file_size() { wc -c < "$1" | tr -d ' '; }

if [ -z "$jobs" ]; then
    jobs=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
fi

TAB=$(printf '\t')

# the host C compiler for tools/pecheck.c, resolved now: setup_path later puts the toolchain's
# bin/ first, whose clang targets Windows (a CC=clang must stay the host's clang)
host_cc=""
for c in "${CC:-cc}" cc clang gcc; do
    if host_cc=$(command -v "$c" 2>/dev/null); then break; fi
    host_cc=""
done
current_log="$logs/build-pe.log"

say() { echo "build-pe: $*"; }
# fail <message>: every failure names the step and the log
fail() {
    echo "build-pe: step '$step_name' failed: $*" >&2
    echo "build-pe: log: $current_log" >&2
    exit 1
}
step_name=$step

tmp=$(mktemp -d "${TMPDIR:-/tmp}/build-pe.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

farm_arch() {
    case "$1" in
        arm64ec-windows) echo arm64ec ;;
        aarch64-windows) echo aarch64 ;;
        i386-windows) echo i386 ;;
        *) echo "build-pe: unknown farm $1" >&2; exit 1 ;;
    esac
}
farm_triple() {
    case "$1" in
        arm64ec) echo arm64ec-w64-mingw32 ;;
        aarch64) echo aarch64-w64-mingw32 ;;
        i386) echo i686-w64-mingw32 ;;
    esac
}

# a farm list: names, lower case, without comments
read_list() { sed -e 's/#.*//' "$1" | lower | awk 'NF { print $1 }'; }

# ---- B2: the overlay map ----
# writes $tmp/map (tree path, overlay, source file, spec; tab-separated) and $tmp/missing
compute_overlay_map() {
    : > "$tmp/map"
    : > "$tmp/missing"
    overlay_table | while IFS='|' read -r name rel to spec; do
        [ -n "$name" ] || continue
        from="$out_root/$rel"
        if [ -f "$from" ]; then
            printf '%s\t%s\t%s\t%s\n' "$to" "$name" "$from" "$spec" >> "$tmp/map"
        elif [ -d "$from" ]; then
            (cd "$from" && find . -type f ! -name '.*' | sed 's|^\./||' | LC_ALL=C sort) | while read -r f; do
                if [ "$to" = . ]; then p=$f; else p="$to/$f"; fi
                printf '%s\t%s\t%s\t%s\n' "$p" "$name" "$from/$f" "$spec" >> "$tmp/map"
            done
        else
            printf '%s\t%s\n' "$name" "$from" >> "$tmp/missing"
        fi
    done
}

# B2 items 2 and 3: prints the errors to stdout, returns 1 if there is any
overlay_errors() {
    local errors=0 path names
    while IFS="$TAB" read -r name from; do
        if [ $allow_missing = 1 ]; then
            echo "missing overlay $name ($from): skipped (--allow-missing-overlays)"
        else
            echo "ERROR: missing overlay $name: $from"
            errors=1
        fi
    done < "$tmp/missing"
    for path in $(cut -f1 "$tmp/map" | LC_ALL=C sort | uniq -d); do
        names=$(awk -F'\t' -v p="$path" '$1 == p { printf "%s%s", sep, $2; sep = " and " }' "$tmp/map")
        echo "ERROR: $path is delivered by two overlays: $names (one owner per file: merge the changes in one of them)"
        errors=1
    done
    for path in $(awk -F'\t' '$1 ~ /^dlls\/[^\/]+\/unix\// || $1 ~ /^server\// { print $1 "|" $2 }' "$tmp/map"); do
        echo "ERROR: ${path%%|*} (overlay ${path#*|}) is a unix-side file: those are built into the app by specs 08-17, not here"
        errors=1
    done
    return $errors
}

overlay_hash() {
    {
        sha256_file "$script_path"
        while IFS="$TAB" read -r path name from spec; do
            printf '%s %s %s\n' "$path" "$name" "$(sha256_file "$from")"
        done < "$tmp/map"
    } | sha256_stdin
}

# module index of a tree: name<TAB>dir
module_index() {
    (cd "$1" && awk '/^MODULE[ \t]*=/ { d = FILENAME; sub(/\/Makefile\.in$/, "", d); print tolower($3) "\t" d }' \
        dlls/*/Makefile.in programs/*/Makefile.in)
}

# the index plan uses before the tree exists: the overlays' Makefile.in files first
plan_module_index() {
    awk -F'\t' '$1 ~ /^(dlls|programs)\/[^\/]+\/Makefile\.in$/ { print $1 "\t" $3 }' "$tmp/map" |
        while IFS="$TAB" read -r path from; do
            awk -v d="${path%/Makefile.in}" '/^MODULE[ \t]*=/ { print tolower($3) "\t" d }' "$from"
        done
    module_index "$wine"
}

# targets of a 64-bit farm list: name<TAB>target, or "?" for a name with no module
list_targets() { # <list file> <arch> <index file>
    read_list "$1" | while read -r m; do
        d=$(awk -F'\t' -v m="$m" '$1 == m { print $2; exit }' "$3")
        if [ -n "$d" ]; then printf '%s\t%s/%s-windows/%s\n' "$m" "$d" "$2" "$m"
        else printf '%s\t?\n' "$m"
        fi
    done
}

# B4 item 3: is a file name in the i386 skip list?
i386_skipped() {
    local name=$1 pat
    while read -r pat; do
        # shellcheck disable=SC2254
        case "$name" in $pat) return 0 ;; esac
    done < "$tmp/i386-skip"
    return 1
}

has_pe_ext() {
    local ext
    for ext in $PE_EXTS; do
        case "$1" in *."$ext") return 0 ;; esac
    done
    return 1
}

# every PE module file a configured tree has a rule for: dlls/x/<arch>-windows/file
tree_rules() { # <build dir> <arch>
    grep -oE "^(dlls|programs)/[^/ :]+/$2-windows/[^/ :]+:" "$1/Makefile" | sed 's/:$//' | LC_ALL=C sort -u
}

i386_targets() { # <rules file>
    read_list "$farms_dir/i386-skip.txt" > "$tmp/i386-skip"
    while read -r t; do
        n=$(basename "$t" | lower)
        has_pe_ext "$n" || continue
        i386_skipped "$n" && continue
        printf '%s\t%s\n' "$n" "$t"
    done < "$1"
}

# ---- toolchain (B1) ----
host_asset() {
    case "$(uname -s)" in
        Darwin) echo "$LLVM_MINGW_MACOS_ASSET $LLVM_MINGW_MACOS_SHA256" ;;
        Linux)
            case "$(uname -m)" in
                x86_64) echo "$LLVM_MINGW_LINUX_ASSET $LLVM_MINGW_LINUX_SHA256" ;;
                *) echo "build-pe: no pinned llvm-mingw asset for Linux $(uname -m); pass --toolchain" >&2; return 1 ;;
            esac ;;
        *) echo "build-pe: no pinned llvm-mingw asset for $(uname -s); pass --toolchain" >&2; return 1 ;;
    esac
}

toolchain_dir=""
toolchain_info=""

check_toolchain() {
    local t
    for t in aarch64 arm64ec i686 x86_64; do
        [ -x "$1/bin/$t-w64-mingw32-clang" ] || fail "toolchain $1 has no bin/$t-w64-mingw32-clang"
        [ -x "$1/bin/$t-w64-mingw32-strip" ] || fail "toolchain $1 has no bin/$t-w64-mingw32-strip"
    done
    # the ARM64EC tree's plain clang (MSVC target) links with lld-link
    [ -x "$1/bin/clang" ] || fail "toolchain $1 has no bin/clang"
    [ -x "$1/bin/lld-link" ] || fail "toolchain $1 has no bin/lld-link"
}

step_toolchain() {
    local pair asset sum url top part
    step_name=toolchain
    current_log="$logs/toolchain.log"
    if [ -n "$toolchain_opt" ]; then
        toolchain_dir=$(abspath "$toolchain_opt")
        check_toolchain "$toolchain_dir"
        toolchain_info="--toolchain $toolchain_dir"
        return 0
    fi
    pair=$(host_asset) || fail "no pinned toolchain for this host"
    asset=${pair% *}
    sum=${pair#* }
    top="$work/${asset%.tar.xz}"
    toolchain_info="$asset sha256 $sum"
    if [ -f "$stamps/toolchain" ] && [ "$(cat "$stamps/toolchain")" = "$asset $sum" ] && [ -d "$top/bin" ]; then
        toolchain_dir=$top
        return 0
    fi
    say "toolchain: $asset"
    mkdir -p "$work/downloads"
    part="$work/downloads/$asset.part"
    url="$LLVM_MINGW_URL/$asset"
    if [ ! -f "$work/downloads/$asset" ]; then
        curl -fL --retry 3 -o "$part" "$url" > "$current_log" 2>&1 || fail "download of $url"
        mv "$part" "$work/downloads/$asset"
    fi
    if [ "$(sha256_file "$work/downloads/$asset")" != "$sum" ]; then
        rm -f "$work/downloads/$asset"
        fail "SHA-256 of $asset is not $sum: the download was removed, nothing unpacked"
    fi
    rm -rf "$top"
    tar -xJf "$work/downloads/$asset" -C "$work" >> "$current_log" 2>&1 || fail "unpacking $asset"
    [ -d "$top/bin" ] || fail "$asset did not unpack to $top"
    check_toolchain "$top"
    toolchain_dir=$top
    echo "$asset $sum" > "$stamps/toolchain"
}

# PATH for every build step: the toolchain first (configure must never pick a GCC mingw), then
# Homebrew's bison and flex on macOS
setup_path() {
    local d
    for d in /usr/local/opt/flex/bin /opt/homebrew/opt/flex/bin /usr/local/opt/bison/bin /opt/homebrew/opt/bison/bin; do
        [ -d "$d" ] && PATH="$d:$PATH"
    done
    PATH="$toolchain_dir/bin:$PATH"
    export PATH
    command -v make >/dev/null 2>&1 || fail "make not found"
    command -v flex >/dev/null 2>&1 || fail "flex not found"
    command -v bison >/dev/null 2>&1 || fail "bison not found"
    case "$(bison --version | head -n 1 | awk '{print $NF}')" in
        [012].*) fail "bison 3 or newer is needed (macOS's own is 2.3: brew install bison)" ;;
    esac
}

# ---- tree (B2) ----
# steps already run in this invocation (a step runs the earlier ones it needs, once)
done_steps=""
once() { # <name> <command...>
    case " $done_steps " in *" $1 "*) return 0 ;; esac
    # not "name": bash's locals are dynamically scoped, and the steps read into a "name"
    local once_step=$1
    shift
    "$@"
    done_steps="$done_steps $once_step"
}

step_tree() {
    local hash path old_list
    step_name=tree
    current_log="$logs/tree.log"
    compute_overlay_map
    overlay_errors > "$tmp/errors" || { cat "$tmp/errors" >&2; fail "overlay errors (above)"; }
    cat "$tmp/errors"
    hash=$( { overlay_hash; cat "$wine/VERSION"; } | sha256_stdin)
    if [ -f "$stamps/tree" ] && [ "$(cat "$stamps/tree")" = "$hash" ] && [ -d "$src" ] && [ ! -L "$src" ] &&
       [ -z "$(find "$wine" -newer "$stamps/tree" -type f 2>/dev/null | head -n 1)" ]; then
        return 0
    fi
    say "tree: copying $wine to $src and applying $(wc -l < "$tmp/map" | tr -d ' ') overlay files"
    rm -rf "$src.new"
    # the tree's contents into a new real folder: never a link to the reference tree
    mkdir "$src.new"
    cp -pR "$wine/." "$src.new/" || fail "copying $wine"
    [ -d "$src.new" ] && [ ! -L "$src.new" ] || fail "$src.new is not a folder of its own"
    while IFS="$TAB" read -r path name from spec; do
        mkdir -p "$src.new/$(dirname "$path")"
        cp "$from" "$src.new/$path" || fail "copying $from"
    done < "$tmp/map"
    # keep the old time of every overlaid path whose content didn't change, so make rebuilds
    # only what did; a path that changed, or went back to upstream's file, is new
    old_list="$stamps/tree.map"
    [ -f "$old_list" ] || : > "$old_list"
    { cut -f1 "$tmp/map"; cut -f1 "$old_list"; } | LC_ALL=C sort -u | while read -r path; do
        [ -f "$src.new/$path" ] || continue
        if [ -f "$src/$path" ] && cmp -s "$src/$path" "$src.new/$path"; then
            touch -r "$src/$path" "$src.new/$path"
        else
            touch "$src.new/$path"
        fi
    done
    rm -rf "$src"
    mv "$src.new" "$src"
    cp "$tmp/map" "$old_list"
    echo "$hash" > "$stamps/tree"
}

# ---- build trees (B3) ----
build_dir() { echo "$work/build-$1"; }

configure_args() { # <arch>
    local p args="$(mingw_option "$1") $CONFIGURE_COMMON --enable-archs=$1 CROSSCFLAGS=$CROSS_CFLAGS"
    [ "$1" = arm64ec ] || args="$args --with-wine-tools=$(build_dir arm64ec)"
    if [ "$1" = aarch64 ]; then
        for p in $AARCH64_PROGRAMS; do args="$args enable_$p=aarch64"; done
    fi
    echo "$args"
}

# configure when its inputs changed: the arguments, configure itself, every Makefile.in an
# overlay delivers (a new source file exists only if its Makefile.in was in place)
configure_tree() { # <arch>
    local arch=$1 dir hash
    dir=$(build_dir "$arch")
    hash=$( {
        configure_args "$arch"
        sha256_file "$src/configure"
        awk -F'\t' '$1 ~ /Makefile\.in$/ { print $1 }' "$stamps/tree.map" | while read -r p; do
            echo "$p $(sha256_file "$src/$p")"
        done
        echo "$toolchain_dir"
    } | sha256_stdin)
    if [ -f "$stamps/configure-$arch" ] && [ "$(cat "$stamps/configure-$arch")" = "$hash" ] && [ -f "$dir/Makefile" ]; then
        return 0
    fi
    say "$arch: configure"
    mkdir -p "$dir"
    # shellcheck disable=SC2046
    (cd "$dir" && "$src/configure" $(configure_args "$arch")) > "$logs/configure-$arch.log" 2>&1 ||
        { current_log="$logs/configure-$arch.log"; fail "configure of the $arch tree"; }
    echo "$hash" > "$stamps/configure-$arch"
    rm -f "$stamps/build-$arch"
}

# the first tree builds Wine's host tools; the other two use them
ensure_tools() {
    local dir
    dir=$(build_dir arm64ec)
    configure_tree arm64ec
    if [ ! -x "$dir/tools/winebuild/winebuild" ] || [ ! -x "$dir/tools/makedep" ] || [ ! -f "$stamps/tools" ]; then
        say "arm64ec: Wine's host tools"
        (cd "$dir" && make -j"$jobs" __tooldeps__) > "$logs/tools.log" 2>&1 ||
            { current_log="$logs/tools.log"; fail "building Wine's host tools in $dir"; }
        touch "$stamps/tools"
    fi
}

# B3: when widl builds a type library that imports another one (importlib("stdole2.tlb") and
# "stdole32.tlb" in Wine's .idl files), it looks for it as dlls/<module>/<arch dir>/<file> under
# the -L dlls/* of its command line, and its arch dir for ARM64EC is aarch64-windows/
# (tools/tools.h, get_arch_dir), while the ARM64EC tree builds every .tlb under
# arm64ec-windows/. In that tree, make each dlls/*/aarch64-windows/*.tlb resolve to the tree's own
# file. The other trees' widl looks under their own folder.
link_typelibs() { # <arch> <dir>
    local arch=$1 dir=$2 ref own
    [ "$arch" = arm64ec ] || return 0
    grep -oE '^dlls/[^/ :]+/arm64ec-windows/[^/ :]+\.tlb:' "$dir/Makefile" | sed 's/:$//' | LC_ALL=C sort -u | while read -r own; do
        ref="$(dirname "$(dirname "$own")")/aarch64-windows/$(basename "$own")"
        # a path the tree can make itself needs no link
        grep -q "^$ref:" "$dir/Makefile" && continue
        # the link may point at a file not built yet: make builds an imported type library
        # before whatever imports it (the .res rules depend on the arm64ec-windows file)
        if [ ! -L "$dir/$ref" ] && [ ! -e "$dir/$ref" ]; then
            mkdir -p "$dir/$(dirname "$ref")"
            ln -s "../$arch-windows/$(basename "$ref")" "$dir/$ref"
            echo "build-pe: $arch: $ref -> $own" >> "$current_log"
        fi
    done
}

# make every target; retry each missing one once with -j1; fail listing what is still missing
make_targets() { # <arch> <dir> <targets file (name TAB target)>
    local arch=$1 dir=$2 list=$3 name target missing=0 mlog
    cut -f2 "$list" > "$tmp/targets-$arch"
    [ -s "$tmp/targets-$arch" ] || return 0
    # shellcheck disable=SC2046
    (cd "$dir" && make -k -j"$jobs" $(cat "$tmp/targets-$arch")) >> "$current_log" 2>&1 || true
    : > "$tmp/failed-$arch"
    while IFS="$TAB" read -r name target; do
        [ -f "$dir/$target" ] && continue
        mlog="$logs/$arch-$name.log"
        say "$arch: retrying $name with -j1 ($mlog)"
        (cd "$dir" && make -j1 "$target") > "$mlog" 2>&1 || true
        [ -f "$dir/$target" ] || printf '%s\t%s\n' "$name" "$mlog" >> "$tmp/failed-$arch"
    done < "$list"
    if [ -s "$tmp/failed-$arch" ]; then
        while IFS="$TAB" read -r name mlog; do
            echo "build-pe: $arch: $name failed to build (log: $mlog):" >&2
            grep -m 5 -iE 'error|undefined' "$mlog" | sed 's/^/    /' >&2 || true
        done < "$tmp/failed-$arch"
        fail "$(wc -l < "$tmp/failed-$arch" | tr -d ' ') module(s) failed to build: $(cut -f1 "$tmp/failed-$arch" | tr '\n' ' ' | sed 's/ $//')"
    fi
}

# the target list of a farm in a configured tree: name<TAB>target
farm_targets() { # <farm> <out file>
    local farm=$1 out=$2 arch dir
    arch=$(farm_arch "$farm")
    dir=$(build_dir "$arch")
    tree_rules "$dir" "$arch" > "$tmp/rules-$arch"
    if [ "$farm" = i386-windows ]; then
        i386_targets "$tmp/rules-$arch" > "$out"
        cp "$out" "$out.listed"
        return 0
    fi
    module_index "$src" > "$tmp/index"
    list_targets "$farms_dir/$farm.txt" "$arch" "$tmp/index" > "$out"
    cp "$out" "$out.listed"
    # the closure's additions of earlier runs are built too
    [ -f "$stamps/closure-$farm" ] && cat "$stamps/closure-$farm" >> "$out"
    awk -F'\t' '$2 == "?" { print "build-pe: " $1 " is not a module of the Wine tree" }' "$out" >&2
    awk -F'\t' '$2 == "?" { bad = 1 } END { exit bad }' "$out" || fail "unknown module names in farms/$farm.txt"
    cut -f2 "$out" | LC_ALL=C sort > "$tmp/want-$arch"
    LC_ALL=C comm -23 "$tmp/want-$arch" "$tmp/rules-$arch" > "$tmp/norule-$arch"
    if [ -s "$tmp/norule-$arch" ]; then
        sed 's/^/build-pe: no rule in the tree for /' "$tmp/norule-$arch" >&2
        fail "the $arch tree can't build every module of farms/$farm.txt"
    fi
}

step_build() { # <arch>
    local arch=$1 farm dir hash
    case "$arch" in
        arm64ec) farm=arm64ec-windows ;;
        aarch64) farm=aarch64-windows ;;
        i386) farm=i386-windows ;;
    esac
    once toolchain step_toolchain
    once tree step_tree
    step_name=$arch
    current_log="$logs/$arch.log"
    setup_path
    [ "$arch" = arm64ec ] || ensure_tools
    configure_tree "$arch"
    [ "$arch" = arm64ec ] && ensure_tools
    dir=$(build_dir "$arch")
    farm_targets "$farm" "$tmp/farm-$arch"
    # the listed targets; the closure's additions (built by collect) are only checked for presence
    hash=$( { cat "$stamps/configure-$arch" "$stamps/tree"; cat "$tmp/farm-$arch.listed"; } | sha256_stdin)
    if [ -f "$stamps/build-$arch" ] && [ "$(cat "$stamps/build-$arch")" = "$hash" ]; then
        if ! cut -f2 "$tmp/farm-$arch" | (cd "$dir" && while read -r t; do [ -f "$t" ] || exit 1; done); then
            rm -f "$stamps/build-$arch"
        else
            say "$arch: up to date"
            return 0
        fi
    fi
    say "$arch: building $(wc -l < "$tmp/farm-$arch" | tr -d ' ') modules with -j$jobs (log: $current_log)"
    : > "$current_log"
    link_typelibs "$arch" "$dir"
    make_targets "$arch" "$dir" "$tmp/farm-$arch"
    echo "$hash" > "$stamps/build-$arch"
}

# ---- collect (B4 closure, B5) ----
pecheck=""

build_pecheck() {
    pecheck="$work/pecheck"
    [ -n "$host_cc" ] || fail "no host C compiler for tools/pecheck.c (CC, cc, clang or gcc)"
    if [ ! -x "$pecheck" ] || [ "$script_dir/tools/pecheck.c" -nt "$pecheck" ]; then
        rm -f "$pecheck"
        "$host_cc" -std=c11 -O2 -Wall -Wextra -o "$pecheck" "$script_dir/tools/pecheck.c" >> "$current_log" 2>&1 ||
            fail "building tools/pecheck.c with $host_cc"
        [ -x "$pecheck" ] || fail "$host_cc made no $pecheck"
    fi
}

is_api_set() {
    case "$1" in api-ms-win-*|ext-ms-win-*) return 0 ;; esac
    return 1
}

# B4 item 4: add what the farm's modules import until nothing is added. Works on the built
# (unstripped) files; extras take part. Writes $tmp/closure-<farm> (name TAB target).
closure() { # <farm>
    local farm=$1 arch dir round=0 added name target file imp d
    arch=$(farm_arch "$farm")
    dir=$(build_dir "$arch")
    module_index "$src" > "$tmp/index"
    "$pecheck" --list-provided | lower > "$tmp/provided"
    cp "$tmp/farm-$arch" "$tmp/set-$farm"
    : > "$tmp/closure-$farm"
    : > "$tmp/delay-$farm"
    while :; do
        round=$((round + 1))
        cut -f1 "$tmp/set-$farm" > "$tmp/names-$farm"
        [ -f "$tmp/extra-names-$farm" ] && cat "$tmp/extra-names-$farm" >> "$tmp/names-$farm"
        : > "$tmp/new-$farm"
        {
            while IFS="$TAB" read -r name target; do echo "$dir/$target"; done < "$tmp/set-$farm"
            [ -f "$tmp/extra-files-$farm" ] && cat "$tmp/extra-files-$farm"
        } | while read -r file; do
            case "$file" in *.tlb|*.msstyles) continue ;; esac
            # a file pecheck can't read is reported by the farm check below
            "$pecheck" --imports "$file" 2>> "$current_log" | while read -r kind imp; do
                imp=$(echo "$imp" | lower)
                grep -qxF "$imp" "$tmp/names-$farm" && continue
                grep -qxF "$imp" "$tmp/provided" && continue
                is_api_set "$imp" && continue
                if [ "$kind" = delay ]; then
                    printf '%s\t%s\n' "$imp" "$(basename "$file")" >> "$tmp/delay-$farm"
                else
                    printf '%s\t%s\n' "$imp" "$(basename "$file")" >> "$tmp/new-$farm"
                fi
            done || true
        done
        [ -s "$tmp/new-$farm" ] || break
        added=0
        for imp in $(cut -f1 "$tmp/new-$farm" | LC_ALL=C sort -u); do
            d=$(awk -F'\t' -v m="$imp" '$1 == m { print $2; exit }' "$tmp/index")
            target="$d/$arch-windows/$imp"
            if [ -z "$d" ] || ! grep -qxF "$target" "$tmp/rules-$arch"; then
                fail "$farm: $(awk -F'\t' -v m="$imp" '$1 == m { print $2 }' "$tmp/new-$farm" | LC_ALL=C sort -u | tr '\n' ' ')imports $imp, which nobody can build for $arch"
            fi
            say "$farm: closure adds $imp (imported by $(awk -F'\t' -v m="$imp" '$1 == m { print $2 }' "$tmp/new-$farm" | LC_ALL=C sort -u | tr '\n' ' '| sed 's/ $//'))"
            printf '%s\t%s\n' "$imp" "$target" >> "$tmp/closure-$farm"
            printf '%s\t%s\n' "$imp" "$target" >> "$tmp/set-$farm"
            printf '%s\t%s\n' "$imp" "$target" > "$tmp/one"
            make_targets "$arch" "$dir" "$tmp/one"
            added=1
        done
        [ $added = 1 ] || break
        [ $round -lt 50 ] || fail "$farm: the import closure doesn't end"
    done
    # remember every addition (this run's and earlier ones), so the arch step builds them next time
    read_list "$farms_dir/$farm.txt" > "$tmp/listed-$farm"
    awk -F'\t' 'NR == FNR { listed[$1] = 1; next } !($1 in listed)' "$tmp/listed-$farm" "$tmp/set-$farm" \
        > "$stamps/closure-$farm"
    if [ -s "$tmp/delay-$farm" ]; then
        say "$farm: delay-imported DLLs not in the farm (reported only):"
        LC_ALL=C sort -u "$tmp/delay-$farm" | awk -F'\t' '{ a[$1] = a[$1] " " $2 } END { for (k in a) print "    " k ":" a[k] }' | LC_ALL=C sort
    fi
    cp "$tmp/set-$farm" "$tmp/farm-$arch"
}

# --extra <farm>=<dir>: the files, and the extra names a farm already has
prepare_extras() {
    local line farm d f n
    for farm in $FARMS; do : > "$tmp/extra-files-$farm"; : > "$tmp/extra-names-$farm"; done
    printf '%s' "$extras" | while IFS= read -r line; do
        [ -n "$line" ] || continue
        farm=${line%%=*}
        d=${line#*=}
        case " $FARMS " in *" $farm "*) ;; *) fail "--extra $line: unknown farm $farm" ;; esac
        [ -d "$d" ] || fail "--extra $line: no folder $d"
        d=$(abspath "$d")
        for f in "$d"/*; do
            [ -f "$f" ] || continue
            n=$(basename "$f" | lower)
            echo "$f" >> "$tmp/extra-files-$farm"
            echo "$n" >> "$tmp/extra-names-$farm"
        done
    done
}

# B4 item 5: an extra whose name is also a built Wine module is an error, unless another part
# provides that name
check_extras() { # <farm>
    local farm=$1 arch n
    arch=$(farm_arch "$farm")
    while read -r n; do
        [ -n "$n" ] || continue
        if cut -f1 "$tmp/farm-$arch" | grep -qxF "$n" && ! grep -qxF "$n" "$tmp/provided"; then
            fail "--extra for $farm: $n is also a module built from Wine"
        fi
    done < "$tmp/extra-names-$farm"
}

# App Review guideline 2.5.2 (no downloaded code; spec 19 follow-up): these farm files may contain
# none of these strings, in ASCII or UTF-16 (resource text), compared without regard to case.
# appwiz.cpl held Wine's Gecko/Mono add-on downloader (pe/dlls/appwiz.cpl removes it).
NO_DOWNLOAD_FILES="appwiz.cpl"
NO_DOWNLOAD_STRINGS="winegecko winemono http"

check_no_download() { # <farm> <farm folder>
    local farm=$1 dir=$2 f s n found="" before
    for f in $NO_DOWNLOAD_FILES; do
        [ -f "$dir/$f" ] || continue
        before=$found
        for s in $NO_DOWNLOAD_STRINGS; do
            # dropping every NUL turns UTF-16 text into ASCII, so one search finds both; grep -c
            # reads everything (grep -q could stop early, and pipefail would hide the match)
            n=$(tr -d '\000' < "$dir/$f" | LC_ALL=C grep -a -i -c -e "$s" || true)
            [ "${n:-0}" = 0 ] || found="$found $f:$s"
        done
        [ "$found" != "$before" ] || say "$farm: $f has no download strings ($NO_DOWNLOAD_STRINGS)"
    done
    [ -z "$found" ] || fail "$farm: download strings left (App Review 2.5.2):$found"
}

origin_of() { # <tree dir of a module> -> wine, or wine+spec NN,MM
    local specs parent
    # a module also compiles the sources of its PARENTSRC folder (d3dcompiler_47: ../d3dcompiler_43)
    parent=$(sed -n 's/^PARENTSRC *= *//p' "$src/$1/Makefile.in" 2>/dev/null | head -n 1)
    case "$parent" in
        ../*) parent="$(dirname "$1")/${parent#../}" ;;
        "") ;;
        *) parent="$1/$parent" ;;
    esac
    specs=$(awk -F'\t' -v d="$1/" -v p="${parent:+$parent/}" 'index($1, d) == 1 || (p != "" && index($1, p) == 1) { print $4 }' \
        "$stamps/tree.map" | LC_ALL=C sort -u | tr '\n' ',' | sed 's/,$//')
    if [ -n "$specs" ]; then echo "wine+spec $specs"; else echo wine; fi
}

step_collect() {
    local farm arch dir triple strip winebuild name target file out n
    for farm in $FARMS; do once "build-$(farm_arch "$farm")" step_build "$(farm_arch "$farm")"; done
    step_name=collect
    current_log="$logs/collect.log"
    : > "$current_log"
    setup_path
    build_pecheck
    prepare_extras
    "$pecheck" --list-provided | lower > "$tmp/provided"
    winebuild="$(build_dir arm64ec)/tools/winebuild/winebuild"
    [ -x "$winebuild" ] || fail "no winebuild in $(build_dir arm64ec)"
    # <dest> is replaced as a whole: only ever an earlier product, or empty
    if [ -d "$dest" ] && [ ! -f "$dest/MANIFEST.txt" ] && [ -n "$(ls -A "$dest")" ]; then
        fail "$dest is not empty and holds no MANIFEST.txt: not replacing it (--dest)"
    fi

    rm -rf "$dest.new"
    mkdir -p "$dest.new/nls"
    : > "$tmp/origins"
    for farm in $FARMS; do
        arch=$(farm_arch "$farm")
        dir=$(build_dir "$arch")
        tree_rules "$dir" "$arch" > "$tmp/rules-$arch"
        farm_targets "$farm" "$tmp/farm-$arch"
        check_extras "$farm"
        [ "$farm" = i386-windows ] || closure "$farm"
        triple=$(farm_triple "$arch")
        strip="$toolchain_dir/bin/$triple-strip"
        out="$dest.new/$farm"
        mkdir -p "$out"
        # B5 item 1: strip into the farm folder, and mark the file builtin again as Wine's own
        # install does (winebuild --builtin); .tlb and .msstyles files as they are
        while IFS="$TAB" read -r name target; do
            file="$dir/$target"
            cp "$file" "$out/$name"
            case "$name" in
                *.tlb|*.msstyles) ;;
                *)
                    "$strip" --strip-debug "$out/$name" >> "$current_log" 2>&1 || fail "$farm: strip of $name"
                    "$winebuild" --builtin "$out/$name" >> "$current_log" 2>&1 || fail "$farm: winebuild --builtin $name"
                    ;;
            esac
            d=${target%/*-windows/*}
            printf '%s\t%s\t%s\n' "$farm" "$name" "$(origin_of "$d")" >> "$tmp/origins"
        done < "$tmp/farm-$arch"
        while read -r file; do
            [ -n "$file" ] || continue
            n=$(basename "$file" | lower)
            cp "$file" "$out/$n"
            printf '%s\t%s\textra:%s\n' "$farm" "$n" "$(dirname "$file")" >> "$tmp/origins"
        done < "$tmp/extra-files-$farm"
        # B5 item 2
        say "pecheck $farm"
        "$pecheck" "$farm" "$out" 2>&1 | tee -a "$current_log" || fail "pecheck $farm"
        check_no_download "$farm" "$out"
    done

    # B5 item 4: the NLS tables
    cp "$src"/nls/*.nls "$dest.new/nls/"
    for file in "$dest.new"/nls/*.nls; do printf 'nls\t%s\twine\n' "$(basename "$file")" >> "$tmp/origins"; done

    # B5 item 5: MANIFEST.txt and BUILDINFO.txt
    while IFS="$TAB" read -r farm name origin; do
        file="$dest.new/$farm/$name"
        if [ "$farm" = nls ]; then machine=-; else machine=$("$pecheck" --machine "$file"); fi
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$farm" "$name" "$(file_size "$file")" "$(sha256_file "$file")" "$machine" "$origin"
    done < "$tmp/origins" > "$dest.new/MANIFEST.txt"
    {
        echo "wine: $(cat "$src/VERSION")"
        echo "toolchain: $toolchain_info"
        echo "build host: $(uname -srm)"
        overlay_table | while IFS='|' read -r name rel to spec; do
            [ -n "$name" ] || continue
            h=$(awk -F'\t' -v n="$name" '$2 == n { print $1 " " $3 }' "$stamps/tree.map" | while read -r p f; do
                    echo "$p $(sha256_file "$f")"; done | sha256_stdin)
            if awk -F'\t' -v n="$name" '$2 == n { found = 1 } END { exit !found }' "$stamps/tree.map"; then
                echo "overlay $name (spec $spec, $rel): $h"
            else
                echo "overlay $name (spec $spec, $rel): missing"
            fi
        done
    } > "$dest.new/BUILDINFO.txt"

    # B6: exactly the four folders and the two files
    rm -rf "$dest"
    mv "$dest.new" "$dest"
    say "done: $dest"
}

# ---- kit (spec 00 E1) ----
# Wine is LGPL-2.1-or-later, and its DLLs link LGPL libraries statically (fluidsynth, mpg123,
# vkd3d from src/libs/), so the app ships a relinking kit: here the exact source the farms were
# built from, with everything needed to build them again from a changed copy. It is not part of
# <dest> (B6 fixes that folder's contents).
kit_readme() {
    cat <<'EOF'
MYIOSDECK: relinking kit for the app's Windows-side DLLs

The DLL farms the app ships (arm64ec-windows, aarch64-windows, i386-windows) are built from
src/: upstream Wine 11.4 (LGPL-2.1-or-later, see src/COPYING.LIB and src/LICENSE) with the
app's changes already applied, including the libraries Wine bundles in src/libs/ (each with its
licence file there). BUILDINFO.txt names the toolchain and the build host; MANIFEST.txt lists
every shipped file with its SHA-256.

To build the farms again, after changing anything under src/ (bash, make, bison 3, flex, a C
compiler; the script downloads its pinned llvm-mingw toolchain, or pass --toolchain <dir>):

    ./build-pe.sh --wine src --out-root out --allow-missing-overlays all

(out/ is empty on purpose: the app's overlays are already part of src/). The result is in
build/pe-work/dest/. Each file of the two 64-bit farms is packaged in the app as a signed
framework named after it (lib<file name>.dylib); the i386 farm is bundled as plain files.
EOF
}

step_kit() {
    local kit stage
    # the kit's MANIFEST.txt must describe farms built from exactly this src/
    once collect step_collect
    step_name=kit
    current_log="$logs/kit.log"
    : > "$current_log"
    kit="$work/pe-relink-kit.tar.xz"
    stage="$work/kit.new/pe-relink-kit"
    rm -rf "$work/kit.new"
    mkdir -p "$stage/out" "$stage/tools"
    cp -pR "$src" "$stage/src" || fail "copying $src"
    cp "$script_path" "$stage/build-pe.sh"
    cp -pR "$farms_dir" "$stage/farms"
    cp "$script_dir/tools/pecheck.c" "$stage/tools/pecheck.c"
    cp "$dest/BUILDINFO.txt" "$dest/MANIFEST.txt" "$stage/"
    kit_readme > "$stage/README.txt"
    (cd "$work/kit.new" && tar -cJf "$kit.new" pe-relink-kit) >> "$current_log" 2>&1 || fail "packing $kit"
    mv "$kit.new" "$kit"
    rm -rf "$work/kit.new"
    say "kit: $kit"
}

# ---- plan ----
step_plan() {
    local status=0 name rel to spec n arch farm dir
    step_name=plan
    current_log=/dev/null
    compute_overlay_map
    echo "== overlays (in order)"
    overlay_table | while IFS='|' read -r name rel to spec; do
        [ -n "$name" ] || continue
        n=$(awk -F'\t' -v n="$name" '$2 == n' "$tmp/map" | wc -l | tr -d ' ')
        if [ -e "$out_root/$rel" ]; then
            echo "overlay $name (spec $spec): $rel -> $to ($n files)"
        else
            echo "overlay $name (spec $spec): $rel -> $to (MISSING)"
        fi
        awk -F'\t' -v n="$name" '$2 == n { print $1 }' "$tmp/map" | while read -r path; do
            if [ -e "$wine/$path" ]; then echo "    $path"; else echo "    $path  new"; fi
        done
    done
    echo "== overlay errors"
    if overlay_errors > "$tmp/errors"; then
        cat "$tmp/errors"
        echo "none"
    else
        cat "$tmp/errors"
        status=1
    fi
    plan_module_index | awk -F'\t' '!seen[$1]++' > "$tmp/index"
    for farm in $FARMS; do
        arch=$(farm_arch "$farm")
        dir=$(build_dir "$arch")
        echo "== farm $farm"
        if [ "$farm" = i386-windows ]; then
            if [ -f "$dir/Makefile" ]; then
                tree_rules "$dir" "$arch" > "$tmp/rules-$arch"
                i386_targets "$tmp/rules-$arch" | cut -f2
            else
                echo "(not configured yet) every PE module the configured i386 tree has a rule for, under dlls/ and"
                echo "programs/, with the extensions .${PE_EXTS// / .}, minus the $(read_list "$farms_dir/i386-skip.txt" | wc -l | tr -d ' ') names"
                echo "and patterns of farms/i386-skip.txt"
            fi
            continue
        fi
        list_targets "$farms_dir/$farm.txt" "$arch" "$tmp/index" > "$tmp/plan-$farm"
        if [ -f "$dir/Makefile" ]; then
            tree_rules "$dir" "$arch" > "$tmp/rules-$arch"
        else
            : > "$tmp/rules-$arch"
        fi
        while IFS="$TAB" read -r name target; do
            if [ "$target" = "?" ]; then
                echo "ERROR: $name is not a module of the Wine tree"
                status=1
            elif [ -s "$tmp/rules-$arch" ] && ! grep -qxF "$target" "$tmp/rules-$arch"; then
                echo "$target  (no rule in the configured tree)"
                status=1
            else
                echo "$target"
            fi
        done < "$tmp/plan-$farm"
        [ -f "$stamps/closure-$farm" ] && awk -F'\t' '{ print "closure: " $2 }' "$stamps/closure-$farm"
        echo "(plus the import closure, B4 item 4)"
    done
    return $status
}

case "$step" in
    toolchain) step_toolchain; say "toolchain: $toolchain_dir" ;;
    tree) step_tree; say "tree: $src" ;;
    plan) step_plan ;;
    arm64ec|aarch64|i386) step_build "$step" ;;
    collect) step_collect ;;
    kit) step_kit ;;
    all) once toolchain step_toolchain; once tree step_tree; once build-arm64ec step_build arm64ec
         once build-aarch64 step_build aarch64; once build-i386 step_build i386
         once collect step_collect; step_kit ;;
    *) usage ;;
esac
