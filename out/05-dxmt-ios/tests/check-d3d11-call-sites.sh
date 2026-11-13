#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 05 clarification 63 (DXMT_D11_CALL_STATS), coverage, second half: every method of
# dxmt/src/d3d11/d3d11_call_list.h is counted exactly once, as the first statement of the
# method that implements it. test_d3d11_call_list.c checks the lists against the headers'
# vtables; this script checks:
#   - in the device-context template (d3d11_context_impl.cpp), the immediate context
#     (d3d11_context_imm.cpp) and the deferred one (d3d11_context_def.cpp), each public
#     method (STDMETHODCALLTYPE, override or final) of the context list starts with
#     D11_CTX_CALL(<its name>), and no D11_CTX_CALL stands anywhere else;
#   - for each kind of context every listed method is counted exactly once: by the context's
#     own definition, or else by the template's (only QueryInterface is defined in both; the
#     immediate context's calls the template's uncounted body);
#   - the device (MTLD3D11DeviceImpl in d3d11_device.cpp) counts each method of the device
#     list once, first thing, with D11_DEVICE_CALL;
#   - the swap chain counts Present and Present1 once each; the device children's
#     QueryInterface, AddRef and Release are counted.
# Usage: check-d3d11-call-sites.sh [src/d3d11 directory]. POSIX sh and awk.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
d3d11=${1:-"$here/../dxmt/src/d3d11"}

for f in d3d11_call_list.h d3d11_context_impl.cpp d3d11_context_imm.cpp d3d11_context_def.cpp d3d11_device.cpp \
    d3d11_swapchain.cpp d3d11_device_child.hpp; do
    [ -f "$d3d11/$f" ] || { echo "check-d3d11-call-sites: $d3d11/$f missing" >&2; exit 1; }
done

LC_ALL=C awk '
function fail(msg) { print "FAIL " msg; failures++ }
function base(path,   p) { p = path; sub(/.*\//, "", p); return p }
# the argument of the first MACRO(...) on a line, or ""
function macro_arg(line, macro,   s) {
    if (!match(line, macro "\\([A-Za-z0-9_]+\\)")) return ""
    s = substr(line, RSTART + length(macro) + 1, RLENGTH - length(macro) - 2)
    return s
}

{ sub(/\r$/, "") }
FNR == 1 {
    nfile++; file = base(FILENAME); in_range = 0; done_range = 0; capturing = 0; expect = 0
    if (file == "d3d11_context_impl.cpp") { tag = "impl"; start_re = "class MTLD3D11DeviceContextImplBase"; end_re = "^class MTLD3D11ContextExt :"; macro = "D11_CTX_CALL"; list = "CONTEXT" }
    else if (file == "d3d11_context_imm.cpp") { tag = "imm"; start_re = "^class MTLD3D11ImmediateContext "; end_re = "^};"; macro = "D11_CTX_CALL"; list = "CONTEXT" }
    else if (file == "d3d11_context_def.cpp") { tag = "def"; start_re = "^class MTLD3D11DeferredContext "; end_re = "^};"; macro = "D11_CTX_CALL"; list = "CONTEXT" }
    else if (file == "d3d11_device.cpp") { tag = "dev"; start_re = "^class MTLD3D11DeviceImpl final"; end_re = "^class MTLD3D11DXGIDevice final"; macro = "D11_DEVICE_CALL"; list = "DEVICE" }
    else { tag = "" }
}

# ---- the lists ----
nfile == 1 {
    if ($0 ~ /^#define D11_CALLS_/) { cur = $2; sub(/^D11_CALLS_/, "", cur); sub(/\(X\)$/, "", cur); next }
    if ($0 ~ /^  X\(/) {
        s = $0; sub(/^  X\(/, "", s); sub(/\).*/, "", s)
        if ((cur, s) in listed) fail("list " cur ": " s " listed twice")
        listed[cur, s] = 1; order[cur, ++count[cur]] = s
    }
    next
}

# ---- the swap chain and the child base: counted somewhere ----
file == "d3d11_swapchain.cpp" {
    a = macro_arg($0, "D11_PRESENT_CALL"); if (a != "") present[a]++
    next
}
file == "d3d11_device_child.hpp" {
    a = macro_arg($0, "D11_CHILD_CALL"); if (a != "") child[a]++
    next
}

# ---- the context and device classes ----
tag != "" {
    a = macro_arg($0, "D11_CHILD_CALL"); if (a != "") child[a]++
    if (!in_range && !done_range && $0 ~ start_re) { in_range = 1; next }
    if (in_range && $0 ~ end_re) { in_range = 0; done_range = 1 }
    if (!in_range) next

    if (expect && $0 !~ /^[ \t]*$/) {
        arg = macro_arg($0, macro)
        if (pub && ((list, name) in listed)) {
            if (arg != name) fail(file ":" FNR ": " name " does not start with " macro "(" name ")")
            else counted[tag, name]++
        } else if (arg != "") {
            fail(file ":" FNR ": " macro "(" arg ") in a method that is not a listed public one (" name ")")
        }
        expect = 0
        next
    }
    if (macro_arg($0, macro) != "") { fail(file ":" FNR ": " macro " not the first statement of its method"); next }

    if (!capturing && $0 ~ /STDMETHODCALLTYPE/) { capturing = 1; sig = "" }
    if (capturing) {
        sig = sig " " $0
        if ($0 ~ /\{/) {
            capturing = 0
            s = sig; sub(/.*STDMETHODCALLTYPE[ \t]*/, "", s)
            name = ""; if (match(s, /^[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) { name = substr(s, 1, RLENGTH); sub(/[ \t]*\($/, "", name) }
            pub = (sig ~ /\)[ \t]*(const[ \t]*)?(override|final)/)
            expect = 1
        } else if ($0 ~ /;[ \t]*$/) {
            capturing = 0
        }
    }
}

END {
    for (i = 1; i <= count["CONTEXT"]; i++) {
        m = order["CONTEXT", i]
        for (t = 1; t <= 3; t++) {
            kind = (t == 1 ? "impl" : (t == 2 ? "imm" : "def"))
            if (counted[kind, m] > 1) fail(kind ": " m " counted " counted[kind, m] " times")
        }
        if (counted["impl", m] && counted["imm", m] && m != "QueryInterface") fail("imm: " m " counted by the template and the immediate context")
        if (counted["impl", m] && counted["def", m] && m != "QueryInterface") fail("def: " m " counted by the template and the deferred context")
        imm = counted["imm", m] ? counted["imm", m] : counted["impl", m]
        def = counted["def", m] ? counted["def", m] : counted["impl", m]
        if (imm != 1) fail("immediate context: " m " counted " imm + 0 " times")
        if (def != 1) fail("deferred context: " m " counted " def + 0 " times")
        checks += 2
    }
    for (i = 1; i <= count["DEVICE"]; i++) {
        m = order["DEVICE", i]
        if (counted["dev", m] != 1) fail("device: " m " counted " counted["dev", m] + 0 " times")
        checks++
    }
    if (present["Present"] != 1) fail("SwapChain::Present counted " present["Present"] + 0 " times")
    if (present["Present1"] != 1) fail("SwapChain::Present1 counted " present["Present1"] + 0 " times")
    if (!child["AddRef"] || !child["Release"] || !child["QueryInterface"]) fail("child AddRef, Release or QueryInterface not counted")
    checks += 3
    printf "check-d3d11-call-sites: %d methods (%d context, %d device), %d checks, %d failures\n", count["CONTEXT"] + count["DEVICE"], count["CONTEXT"], count["DEVICE"], checks, failures + 0
    exit failures ? 1 : 0
}
' "$d3d11/d3d11_call_list.h" "$d3d11/d3d11_swapchain.cpp" "$d3d11/d3d11_device_child.hpp" \
  "$d3d11/d3d11_context_impl.cpp" "$d3d11/d3d11_context_imm.cpp" "$d3d11/d3d11_context_def.cpp" \
  "$d3d11/d3d11_device.cpp"
