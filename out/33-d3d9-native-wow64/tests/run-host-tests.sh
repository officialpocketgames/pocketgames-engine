#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 33 Part J (J1-J3): the host tests (Linux; here WSL). Everything compiles with
# -Wall -Wextra -Werror; the dispatch test runs under ASan and UBSan. Exits non-zero on any failure.
#
#   run-host-tests.sh [<configured Wine 11.4 build dir>]
#
# Groups:
#   gen       J1  the table matches Wine 11.4's d3d9.h (every method, order, argument kinds) and gen/
#                 is what the generator writes; broken tables are refused
#   header    J1  gen/d3d9w_header_check.c: on i686 every method's i386 argument bytes equal its pop;
#                 on aarch64 and the host every method sits in its vtable slot
#   stubs     J1  the assembled stubs encode the ids d3d9.h gives (tests/check_stubs.py)
#   layout    J2  native/d3d9w_layout_check.c for i686-w64-mingw32 (Wine's and llvm-mingw's
#                 headers), aarch64 and the host
#   thunk     E   the i386 thunk compiles and links with llvm-mingw; its exports and ordinals are the
#                 full frontend's; GetNPatchMode's stub returns in st(0) (clarification 3)
#   dispatch  J3  tests/test_dispatch.c against tests/fake_native.c
#   pure      E3  tests/test_thunk_pure.c
# The i686 groups need llvm-mingw (LLVM_MINGW, or i686-w64-mingw32-clang on PATH); without it they
# are skipped with a message (the CI cross-compiles them). The aarch64 checks need clang.
#
# Environment: CC (default cc), WINE_SRC (default refs/wine-11.4), WINE_BUILD (a configured Wine
# 11.4 build dir with its generated headers; default ~/.cache/myiosdeck/wine-11.4-headers, made
# once with bison and flex when missing), LLVM_MINGW, CLANG (default clang), TEST_OUT (default
# tests/build), TEST_VERBOSE=1.

set -u

T=$(cd "$(dirname "$0")" && pwd)
P=$(cd "$T/.." && pwd)
REPO=$(cd "$P/../.." && pwd)
OUT=${TEST_OUT:-"$T/build"}
WINE=${WINE_SRC:-"$REPO/refs/wine-11.4"}
# a git worktree (.claude/worktrees/<name> inside the main checkout) has no refs/wine-11.4 of its
# own, as it is not committed: use the nearest one above
if [ -z "${WINE_SRC:-}" ] && [ ! -d "$WINE" ]; then
    d=$REPO
    while [ "$d" != "/" ] && [ "$d" != "." ]; do
        d=$(dirname "$d")
        [ -f "$d/refs/wine-11.4/include/d3d9.h" ] && { WINE="$d/refs/wine-11.4"; break; }
    done
fi
WBUILD=${1:-${WINE_BUILD:-}}
CC=${CC:-cc}
CLANG=${CLANG:-clang}

# refs/ is read-only: no build or work folder of this script may resolve into it.
REFS=$(cd "$REPO/refs" 2> /dev/null && pwd -P)
refuse_refs() {   # refuse_refs <what> <path>
    p=$2; rest=""
    while [ ! -d "$p" ]; do rest="/$(basename "$p")$rest"; p=$(dirname "$p"); done
    case "$(cd "$p" && pwd -P)$rest/" in
        "${REFS:-/nonexistent}"/*) echo "refusing $1 inside refs/ ($2): refs/ is read-only" >&2; exit 1 ;;
    esac
}
refuse_refs "TEST_OUT" "$OUT"
mkdir -p "$OUT" || exit 1

status=0
skipped=""
fail() { echo "FAILED: $1"; status=1; }
ok() { echo "ok: $1"; }

# run <name> <command...>: output to $OUT/<name>.log, shown on failure (or with TEST_VERBOSE)
run() {
    name=$1; shift
    if "$@" > "$OUT/$name.log" 2>&1; then
        ok "$name"
        [ -n "${TEST_VERBOSE:-}" ] && cat "$OUT/$name.log"
        return 0
    fi
    fail "$name"
    cat "$OUT/$name.log"
    return 1
}
# run_fails <name> <command...>: the command must fail
run_fails() {
    name=$1; shift
    if "$@" > "$OUT/$name.log" 2>&1; then
        fail "$name (succeeded, should have failed)"
        cat "$OUT/$name.log"
    else
        ok "$name"
    fi
}

[ -f "$WINE/include/d3d9.h" ] || { echo "no Wine 11.4 tree at $WINE (WINE_SRC)"; exit 1; }
D3D9H="$WINE/include/d3d9.h"

# ---------------------------------------------------------------- Wine's generated headers
if [ -z "$WBUILD" ]; then
    WBUILD="$HOME/.cache/myiosdeck/wine-11.4-headers"
    if [ ! -f "$WBUILD/include/unknwn.h" ] && command -v bison > /dev/null && command -v flex > /dev/null; then
        echo "configuring Wine 11.4 for its generated headers in $WBUILD (once)"
        mkdir -p "$WBUILD"
        ( cd "$WBUILD" && "$WINE/configure" --enable-win64 --without-mingw --without-x --without-freetype \
            --without-gstreamer --disable-tests > "$OUT/wine-configure.log" 2>&1 &&
          make -j"$(nproc 2> /dev/null || echo 4)" include/all > "$OUT/wine-headers.log" 2>&1 ) ||
            { tail -20 "$OUT/wine-configure.log" "$OUT/wine-headers.log" 2> /dev/null; echo "no generated Wine headers"; }
    fi
fi
refuse_refs "the Wine build" "$WBUILD"
[ -f "$WBUILD/include/unknwn.h" ] || { echo "no generated Wine headers in $WBUILD (WINE_BUILD)"; exit 1; }

# Wine's headers as a unix library sees them (the dispatch's view), and as PE code does.
WINE_UNIX="-DWINE_UNIX_LIB -D__WINESRC__ -isystem $WBUILD/include -isystem $WINE/include"
WINE_PE="-D__WINESRC__ -nostdinc -isystem $WINE/include/msvcrt -isystem $WBUILD/include -isystem $WINE/include"
INC="-I$P/include -I$P/native -I$P/gen -I$P/thunk -I$T -I$REPO/interfaces"
WARN="-Wall -Wextra -Werror"

# ---------------------------------------------------------------- llvm-mingw
if [ -z "${LLVM_MINGW:-}" ]; then
    if command -v i686-w64-mingw32-clang > /dev/null; then
        LLVM_MINGW=$(cd "$(dirname "$(command -v i686-w64-mingw32-clang)")/.." && pwd)
    else
        for d in "$HOME"/.cache/myiosdeck/llvm-mingw* "$HOME"/*/llvm-mingw-*-ubuntu-*-x86_64; do
            [ -x "$d/bin/i686-w64-mingw32-clang" ] && { LLVM_MINGW=$d; break; }
        done
    fi
fi
MINGW_CC=""
[ -n "${LLVM_MINGW:-}" ] && [ -x "$LLVM_MINGW/bin/i686-w64-mingw32-clang" ] && MINGW_CC="$LLVM_MINGW/bin/i686-w64-mingw32-clang"
command -v "$CLANG" > /dev/null || CLANG=""

# ---------------------------------------------------------------- gen (J1)
if command -v python3 > /dev/null; then
    run gen-check python3 -I "$P/tools/gen_d3d9w.py" "$D3D9H" --check
    # broken tables are refused: a method moved, a method missing, a wrong argument kind
    TAB="$P/table/d3d9w_table.txt"
    awk '/^SetSamplerState / { held = $0; next } /^ValidateDevice / { print; print held; next } { print }' "$TAB" > "$OUT/bad-order.txt"
    run_fails gen-refuses-order python3 -I "$P/tools/gen_d3d9w.py" "$D3D9H" --verify --table "$OUT/bad-order.txt"
    grep -v '^GetNPatchMode ' "$TAB" > "$OUT/bad-missing.txt"
    run_fails gen-refuses-missing python3 -I "$P/tools/gen_d3d9w.py" "$D3D9H" --verify --table "$OUT/bad-missing.txt"
    sed 's/^SetRenderState              hr     v v$/SetRenderState              hr     v out:DWORD/' "$TAB" > "$OUT/bad-kind.txt"
    run_fails gen-refuses-kind python3 -I "$P/tools/gen_d3d9w.py" "$D3D9H" --verify --table "$OUT/bad-kind.txt"
    sed 's/^SetTexture                  hr     v if:BaseTexture$/SetTexture                  hr     v if:Surface/' "$TAB" > "$OUT/bad-iface.txt"
    run_fails gen-refuses-iface python3 -I "$P/tools/gen_d3d9w.py" "$D3D9H" --verify --table "$OUT/bad-iface.txt"
else
    skipped="$skipped gen(no python3)"
fi

# ---------------------------------------------------------------- header (J1) and layout (J2)
# shellcheck disable=SC2086
run header-host $CC -std=gnu11 $WARN $WINE_UNIX $INC -fsyntax-only "$P/gen/d3d9w_header_check.c"
# shellcheck disable=SC2086
run layout-host $CC -std=gnu11 $WARN $WINE_UNIX $INC -fsyntax-only "$P/native/d3d9w_layout_check.c"
if [ -n "$CLANG" ]; then
    # aarch64 with Wine's own C headers (no SDK here): the layout an iOS build sees
    # shellcheck disable=SC2086
    run header-aarch64 $CLANG --target=arm64-apple-ios17.0 $WARN $WINE_PE $INC -fsyntax-only "$P/gen/d3d9w_header_check.c"
    # shellcheck disable=SC2086
    run layout-aarch64 $CLANG --target=arm64-apple-ios17.0 $WARN $WINE_PE $INC -fsyntax-only "$P/native/d3d9w_layout_check.c"
else
    skipped="$skipped aarch64(no clang)"
fi
if [ -n "$MINGW_CC" ]; then
    # shellcheck disable=SC2086
    run header-i686 "$MINGW_CC" $WARN $WINE_PE $INC -fsyntax-only "$P/gen/d3d9w_header_check.c"
    # shellcheck disable=SC2086
    run layout-i686-wine "$MINGW_CC" $WARN $WINE_PE $INC -fsyntax-only "$P/native/d3d9w_layout_check.c"
    # the thunk is compiled against llvm-mingw's own headers: they must lay these out the same
    # shellcheck disable=SC2086
    run layout-i686-mingw "$MINGW_CC" $WARN $INC -fsyntax-only "$P/native/d3d9w_layout_check.c"
else
    skipped="$skipped i686(no llvm-mingw)"
fi

# ---------------------------------------------------------------- stubs (J1) and the thunk (E)
if [ -n "$MINGW_CC" ]; then
    PE="$OUT/pe"
    rm -rf "$PE"
    mkdir -p "$PE"
    MINGW_FLAGS="-O2 -std=gnu11 $WARN -Wno-unused-parameter -Wno-cast-function-type -DNOMINMAX -D_WIN32_WINNT=0xa00 -I$P/thunk -I$P/gen -I$P/include"
    objs=""
    pe_ok=1
    for src in thunk/d3d9_thunk.c thunk/d3d9_thunk_device.c thunk/d3d9_thunk_resource.c \
               thunk/d3d9_thunk_stubs.S gen/d3d9w_stubs_i386.S gen/d3d9w_vtables_i386.c tests/pe_wine_stubs.c; do
        o="$PE/$(basename "$src").o"
        # shellcheck disable=SC2086
        run "thunk-cc-$(basename "$src")" "$MINGW_CC" $MINGW_FLAGS -c "$P/$src" -o "$o" || pe_ok=0
        objs="$objs $o"
    done
    OBJDUMP="$LLVM_MINGW/bin/i686-w64-mingw32-objdump"
    if [ $pe_ok = 1 ]; then
        # shellcheck disable=SC2086
        run thunk-link "$MINGW_CC" -shared -o "$PE/d3d9.dll" $objs "$P/thunk/d3d9.def" -static \
            -Wl,--file-alignment=4096 -Wl,--enable-stdcall-fixup -Wl,--kill-at -luser32 -lgdi32
        run thunk-exports sh "$P/tools/check_exports.sh" "$OBJDUMP" "$PE/d3d9.dll" "$P/thunk/d3d9.def"
        run thunk-exports-full sh "$P/tools/check_exports.sh" "$OBJDUMP" "$PE/d3d9.dll" \
            "$REPO/out/05-dxmt-ios/dxmt/src/d3d9/d3d9.def"
        "$OBJDUMP" -d "$PE/d3d9w_stubs_i386.S.o" > "$PE/stubs.dis" 2>&1
        if command -v python3 > /dev/null; then
            run stubs-ids python3 -I "$T/check_stubs.py" "$D3D9H" "$PE/stubs.dis" "$P/table/d3d9w_table.txt"
        fi
        # GetNPatchMode: push This, call the bits stub, push eax, fld dword [esp], add esp 4, ret 4
        "$OBJDUMP" -d "$PE/d3d9_thunk_stubs.S.o" > "$PE/npatch.dis" 2>&1
        run thunk-npatch sh -c "grep -q 'ff 74 24 04' '$PE/npatch.dis' && grep -q 'd9 04 24' '$PE/npatch.dis' && grep -q 'c2 04 00' '$PE/npatch.dis'"
        # the vtables hold every slot: 15 tables, their lengths those of d3d9.h
        run thunk-vtables sh -c "\"$OBJDUMP\" -t '$PE/d3d9w_vtables_i386.c.o' | grep -c 'd3d9w_vtbl_' | grep -qx 15"
        # clarification 12: CreateDevice(Ex) sets the x87 control word in guest code
        "$OBJDUMP" -d "$PE/d3d9_thunk_device.c.o" > "$PE/device.dis" 2>&1
        run thunk-fpu sh -c "grep -q fnstcw '$PE/device.dis' && grep -q fldcw '$PE/device.dis'"
    fi
else
    skipped="$skipped thunk(no llvm-mingw)"
fi

# ---------------------------------------------------------------- dispatch (J3) and pure (E3)
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer"
# clang's -fsanitize=function would flag the COM calls through IUnknown (Release, QueryInterface)
# on an object whose own vtable types its methods with the derived interface: the COM ABI, not a
# defect. Every other native call goes through the object's own interface type.
if $CC -fsanitize=function -fno-sanitize=function -x c -c /dev/null -o /dev/null 2> /dev/null; then
    SAN="$SAN -fno-sanitize=function"
fi
# shellcheck disable=SC2086
run dispatch-build $CC -std=gnu11 -g -O1 $WARN -Wno-unused-parameter $SAN $WINE_UNIX $INC \
    "$P/native/d3d9w_dispatch.c" "$P/native/d3d9w_marshal.c" "$P/native/d3d9w_special.c" \
    "$P/native/d3d9w_extras.c" "$P/native/d3d9w_unix.c" "$P/gen/d3d9w_dispatch_gen.c" \
    "$T/fake_native.c" "$T/test_dispatch.c" -o "$OUT/test_dispatch" -lpthread &&
    run dispatch "$OUT/test_dispatch"
# Clarification 14: the iOS SDK defines htonl, htons, ntohl and ntohs as macros, and Wine's
# winsock.h (reached through windows.h unless WIN32_LEAN_AND_MEAN) can't be compiled after them.
# glibc doesn't define them as macros, so this check defines them the SDK's way and compiles every
# dispatch source build-ios.sh builds, with build-ios.sh's definitions: no __WINESRC__, which would
# make winsock.h prefix its names (USE_WS_PREFIX) and hide the clash. As in the SDK, each expands
# to a parenthesised expression: a bare `__builtin_bswap32(x)` would turn winsock.h's
# `ULONG WINAPI htonl(ULONG);` into a redeclaration of the builtin, which gcc accepts.
sdk_macros() {
    for src in native/d3d9w_dispatch.c native/d3d9w_marshal.c native/d3d9w_special.c \
               native/d3d9w_extras.c native/d3d9w_unix.c gen/d3d9w_dispatch_gen.c; do
        # shellcheck disable=SC2086
        $CC -std=gnu11 -O1 $WARN -Wno-unused-parameter -DWINE_UNIX_LIB -D_REENTRANT \
            -isystem "$WBUILD/include" -isystem "$WINE/include" $INC \
            '-Dhtonl(x)=((unsigned int)__builtin_bswap32(x))' '-Dhtons(x)=((unsigned short)__builtin_bswap16(x))' \
            '-Dntohl(x)=((unsigned int)__builtin_bswap32(x))' '-Dntohs(x)=((unsigned short)__builtin_bswap16(x))' \
            -c "$P/$src" -o /dev/null || { echo "with the SDK's byte-order macros: $src fails"; return 1; }
    done
}
run dispatch-sdk-macros sdk_macros
# shellcheck disable=SC2086
run pure-build $CC -std=gnu11 -g -O1 $WARN $SAN $WINE_UNIX $INC "$T/test_thunk_pure.c" -o "$OUT/test_thunk_pure" &&
    run pure "$OUT/test_thunk_pure"

# ---------------------------------------------------------------- alloc (Part C, session N)
# Clarification 12: this runner also runs the allocator's tests. alloc/ is session N's and comes
# with its branch; until then it is skipped with a message.
if [ -f "$P/alloc/tests/run-host-tests.sh" ]; then
    run alloc env CC="$CC" WINE_SRC="$WINE" WINE_BUILD="$WBUILD" sh "$P/alloc/tests/run-host-tests.sh"
else
    skipped="$skipped alloc(alloc/tests/run-host-tests.sh is not here yet: session N's branch brings it)"
fi

[ -n "$skipped" ] && echo "skipped:$skipped"
if [ $status = 0 ]; then echo "run-host-tests: all passed"; else echo "run-host-tests: FAILED"; fi
exit $status
