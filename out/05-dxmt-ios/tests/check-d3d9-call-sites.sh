#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 07 clarification 3 (Part D), coverage, second half: every (interface,
# method) pair of dxmt/src/d3d9/d3d9_call_list.h is counted exactly once, as the
# first statement of the function that implements it. test_d3d9_call_list.c
# checks the d3d9.h lists against d3d9.h's vtables; this script checks:
#   - every class in src/d3d9 that derives from ComObject<I> is in the list's
#     D9_CALL_IMPLEMENTERS table under I (no handed-out object is left out);
#   - each implementer defines every method of its lists out of line
#     ("STDMETHODCALLTYPE" line, then "Class::Method("), and defines no other
#     STDMETHODCALLTYPE method; no d3d9 header has an inline one;
#   - the first statement of each such definition is D9_CALL(label, method),
#     or D9_CALL_PRESENT(...) for the three Presents;
#   - the shader validator list matches d3d9.cpp's IDirect3DShaderValidator9Vtbl
#     and its initialiser, and each shader_validator_<method> starts with
#     D9_CALL(IDirect3DShaderValidator9, method);
#   - the export list matches d3d9.def in order, and each export starts with
#     D9_CALL_EXPORT(name);
#   - the IDxmtDiag9 list matches d3d9_diag.hpp;
#   - no pair is counted anywhere else: one macro per pair in all of src/d3d9.
# Usage: check-d3d9-call-sites.sh [src/d3d9 directory]. POSIX sh and awk.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
d3d9=${1:-"$here/../dxmt/src/d3d9"}

for f in d3d9_call_list.h d3d9.def d3d9_diag.hpp d3d9.cpp; do
    [ -f "$d3d9/$f" ] || { echo "check-d3d9-call-sites: $d3d9/$f missing" >&2; exit 1; }
done

# The list first, then the files it is checked against, then every source.
LC_ALL=C awk '
function fail(msg) { print "FAIL " msg; failures++ }
function base(path,   p) { p = path; sub(/.*\//, "", p); return p }
# The identifier that starts s (s begins with one).
function ident(s) { match(s, /^[A-Za-z_][A-Za-z0-9_]*/); return substr(s, 1, RLENGTH) }
# "label::method" of the first D9_CALL* macro on a line, kind in MACRO_KIND.
function macro_of(line,   m, args, a) {
    MACRO_KIND = ""
    if (!match(line, /D9_CALL(_PRESENT|_EXPORT)?\([A-Za-z0-9_]+(, *[A-Za-z0-9_]+)?\)/)) return ""
    m = substr(line, RSTART, RLENGTH)
    MACRO_KIND = substr(m, 1, index(m, "(") - 1)
    args = substr(m, index(m, "(") + 1); sub(/\)$/, "", args); gsub(/ /, "", args)
    if (MACRO_KIND == "D9_CALL_EXPORT") return "d3d9::" args
    split(args, a, ","); return a[1] "::" a[2]
}

{ sub(/\r$/, "") }
FNR == 1 { nfile++; file = base(FILENAME); prev = ""; pending = 0; want_first = 0; in_struct = ""; in_init = 0 }

# ---- d3d9_call_list.h: the lists and the implementer table -----------------
nfile == 1 {
    if ($0 ~ /^#define D9_CALLS_[A-Za-z0-9_]+\(X\)/) {
        cur = substr($0, length("#define D9_CALLS_") + 1); cur = ident(cur); next
    }
    if (cur != "" && match($0, /^ *X\([A-Za-z0-9_]+, [A-Za-z0-9_]+, [A-Za-z0-9_]+\)/)) {
        s = substr($0, RSTART, RLENGTH); sub(/^ *X\(/, "", s); sub(/\)$/, "", s); gsub(/ /, "", s)
        split(s, a, ",")
        n = ++count[cur]; lab[cur, n] = a[1]; meth[cur, n] = a[3]
        pair = a[1] "::" a[3]
        if (pair in pair_list) fail("pair " pair " listed twice")
        pair_list[pair] = cur; npairs++
        if ($0 !~ /\\$/) cur = ""
        next
    }
    if ($0 ~ /^ *C\([A-Za-z0-9_]+, [A-Za-z0-9_]+\)/) {
        s = $0; sub(/^ *C\(/, "", s); sub(/\).*/, "", s); gsub(/ /, "", s); split(s, a, ",")
        impl_lists[a[2]] = impl_lists[a[2]] " " a[1]; impl_of_list[a[1]] = a[2]
    }
    if ($0 !~ /\\$/) cur = ""
    next
}

# ---- d3d9.def: the exports, in order -----------------------------------------
nfile == 2 {
    if ($1 == "EXPORTS") { in_exports = 1; next }
    if (in_exports && $1 ~ /^[A-Za-z_]/) def_export[++ndef_export] = $1
    next
}

# ---- d3d9_diag.hpp: IDxmtDiag9 methods ---------------------------------------
nfile == 3 {
    if ($0 ~ /^struct IDxmtDiag9 *\{/) { in_struct = "diag"; next }
    if (in_struct == "diag" && $0 ~ /^};/) { in_struct = ""; next }
    if (in_struct == "diag" && match($0, /STDMETHODCALLTYPE +[A-Za-z0-9_]+\(/)) {
        s = substr($0, RSTART, RLENGTH); sub(/^STDMETHODCALLTYPE +/, "", s); diag[++ndiag] = ident(s)
    }
    next
}

# ---- every d3d9 source file ---------------------------------------------------
{
    line = $0
    # Macro sites (the macros are defined in d3d9_call_stats.hpp; the list and
    # the stats files name no pair).
    if (file != "d3d9_call_stats.hpp" && file != "d3d9_call_stats.cpp" && file != "d3d9_call_list.h") {
        p = macro_of(line)
        if (p != "") { sites[p]++; site_where[p] = site_where[p] " " file ":" FNR }
    }

    if (file == "d3d9.cpp") {
        if (line ~ /^struct IDirect3DShaderValidator9Vtbl *\{/) { in_struct = "vtbl"; next }
        if (in_struct == "vtbl") {
            if (line ~ /^};/) { in_struct = ""; next }
            if (match(line, /WINAPI \*[A-Za-z0-9_]+\)/)) {
                s = substr(line, RSTART + 8); validator_slot[++nvslot] = ident(s)
            }
        }
        if (line ~ /shader_validator_vtbl = \{/) in_init = 1
        if (in_init) {
            s = line
            while (match(s, /shader_validator_[A-Za-z0-9]+/)) {
                t = substr(s, RSTART, RLENGTH)
                if (t != "shader_validator_vtbl") validator_init[++nvinit] = substr(t, length("shader_validator_") + 1)
                s = substr(s, RSTART + RLENGTH)
            }
            if (line ~ /};/) in_init = 0
        }
    }

    if (want_first) {
        # The first line after the opening brace: the first statement.
        first_stmt[want_first_key] = line
        want_first = 0
    }
    if (pending) {
        if (index(line, "{")) {
            pending = 0
            rest = substr(line, index(line, "{") + 1)
            if (rest ~ /[^ \t]/) first_stmt[want_first_key] = rest
            else want_first = 1
        } else if (index(line, ";")) {
            pending = 0  # a declaration, not a definition
        }
    }

    if (prev ~ /STDMETHODCALLTYPE[ \t]*$/ && match(line, /^[A-Za-z_][A-Za-z0-9_]*::[A-Za-z_][A-Za-z0-9_]*\(/)) {
        s = substr(line, 1, RLENGTH - 1); split(s, a, "::")
        key = a[1] "::" a[2]
        defs[key]++; def_where[key] = file ":" FNR; class_has[a[1]] = 1; methods_of[a[1]] = methods_of[a[1]] " " a[2]
        want_first_key = key; pending = 1
        if (index(line, "{")) {
            pending = 0
            rest = substr(line, index(line, "{") + 1)
            if (rest ~ /[^ \t]/) first_stmt[key] = rest
            else want_first = 1
        }
    } else if (file == "d3d9.cpp" && prev ~ /WINAPI[ \t]*$/ && match(line, /^[A-Za-z_][A-Za-z0-9_]*\(/)) {
        key = "fn::" substr(line, 1, RLENGTH - 1)
        defs[key]++; def_where[key] = file ":" FNR
        want_first_key = key; pending = 1
        if (index(line, "{")) {
            pending = 0
            rest = substr(line, index(line, "{") + 1)
            if (rest ~ /[^ \t]/) first_stmt[key] = rest
            else want_first = 1
        }
    }

    # Inline STDMETHODCALLTYPE bodies in headers would escape the checks above.
    if (file ~ /\.(hpp|h)$/ && line ~ /STDMETHODCALLTYPE/ && line ~ /\)[ a-z]*\{/)
        fail(file ":" FNR ": an inline STDMETHODCALLTYPE method; define it out of line")

    # Objects the frontend hands out.
    if (match(line, /^class [A-Za-z0-9_]+ final : public ComObject<[A-Za-z0-9_]+>/)) {
        s = substr(line, 7); cls = ident(s)
        s = substr(line, index(line, "ComObject<") + 10); comobj[cls] = ident(s)
    }
    prev = line
}

function check_first(key, want_kind, want_pair,   got) {
    if (!(key in defs)) { fail(key ": no out-of-line definition"); return }
    if (defs[key] > 1) { fail(key ": defined " defs[key] " times"); return }
    got = macro_of(first_stmt[key])
    if (got != want_pair || MACRO_KIND != want_kind || first_stmt[key] !~ /^[ \t]*D9_CALL/)
        fail(key " (" def_where[key] "): first statement is \"" first_stmt[key] "\", want " want_kind "(" want_pair ")")
}

END {
    if (npairs == 0) { fail("no pairs read from d3d9_call_list.h"); exit 1 }

    # Every ComObject-derived class is an implementer of its own interface.
    for (c in comobj) {
        if (!(c in impl_lists)) fail("class " c " (ComObject<" comobj[c] ">) is in no call list")
        else if (index(impl_lists[c] " ", " " comobj[c] " ") == 0)
            fail("class " c " implements " comobj[c] " but is listed for" impl_lists[c])
    }

    present["IDirect3DDevice9::Present"] = 1
    present["IDirect3DDevice9Ex::PresentEx"] = 1
    present["IDirect3DSwapChain9::Present"] = 1

    checked = 0
    for (c in impl_lists) {
        if (!(c in class_has)) { fail("class " c " defines no method"); continue }
        nl = split(impl_lists[c], lists, " ")
        split("", expect)
        for (k = 1; k <= nl; k++) {
            L = lists[k]
            if (!(L in count)) { fail("list " L " of " c " is empty"); continue }
            for (i = 1; i <= count[L]; i++) {
                m = meth[L, i]; pair = lab[L, i] "::" m
                expect[m] = 1
                check_first(c "::" m, (pair in present) ? "D9_CALL_PRESENT" : "D9_CALL", pair)
                checked++
            }
        }
        nm = split(methods_of[c], ms, " ")
        for (k = 1; k <= nm; k++)
            if (!(ms[k] in expect)) fail(c "::" ms[k] " (" def_where[c "::" ms[k]] "): public method in no list")
    }

    # Shader validator.
    L = "IDirect3DShaderValidator9"
    if (count[L] != nvslot) fail("validator: " count[L] " listed, " nvslot " vtable slots")
    if (count[L] != nvinit) fail("validator: " count[L] " listed, " nvinit " in the vtable initialiser")
    for (i = 1; i <= count[L]; i++) {
        m = meth[L, i]
        if (validator_slot[i] != m) fail("validator slot " i ": vtable has " validator_slot[i] ", list has " m)
        if (validator_init[i] != m) fail("validator slot " i ": initialiser has " validator_init[i] ", list has " m)
        check_first("fn::shader_validator_" m, "D9_CALL", L "::" m); checked++
    }

    # Exports.
    L = "EXPORTS"
    if (count[L] != ndef_export) fail("exports: " count[L] " listed, " ndef_export " in d3d9.def")
    for (i = 1; i <= count[L]; i++) {
        m = meth[L, i]
        if (def_export[i] != m) fail("export " i ": d3d9.def has " def_export[i] ", list has " m)
        if (lab[L, i] != "d3d9") fail("export " m ": label " lab[L, i] ", want d3d9")
        check_first("fn::" m, "D9_CALL_EXPORT", "d3d9::" m); checked++
    }

    # IDxmtDiag9.
    L = "IDxmtDiag9"
    if (count[L] != ndiag) fail("IDxmtDiag9: " count[L] " listed, " ndiag " in d3d9_diag.hpp")
    for (i = 1; i <= count[L]; i++)
        if (diag[i] != meth[L, i]) fail("IDxmtDiag9 slot " i ": header has " diag[i] ", list has " meth[L, i])

    # One macro per pair in all of src/d3d9, and no macro for an unlisted pair.
    for (p in pair_list)
        if (sites[p] != 1) fail(p ": " (sites[p] + 0) " counting sites, want 1:" site_where[p])
    for (p in sites)
        if (!(p in pair_list)) fail(p ": counted at" site_where[p] " but in no list")

    if (checked != npairs) fail(checked " entry points checked, " npairs " pairs listed")
    if (failures) { print "check-d3d9-call-sites: " failures " failures"; exit 1 }
    print "check-d3d9-call-sites: " npairs " pairs, each counted once, first thing in its method"
}
' "$d3d9/d3d9_call_list.h" "$d3d9/d3d9.def" "$d3d9/d3d9_diag.hpp" "$d3d9"/*.cpp "$d3d9"/*.hpp "$d3d9"/*.h
