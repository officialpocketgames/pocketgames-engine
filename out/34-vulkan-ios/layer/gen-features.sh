#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 34 Part E3: writes the feature-struct table of the layer's refusal log from a Vulkan
# header.
#
#   gen-features.sh <vulkan header> <out.h>
#
# <vulkan header> is Khronos' vulkan_core.h (MoltenVK's include/vulkan/, the iOS build) or
# Wine's include/wine/vulkan.h (the host tests); both are generated from vk.xml and declare the
# same structs, only spaced differently. The output has one line per struct:
#   IVK_CORE_FEATURES(<count>, "<name>" "\0" "<name>" ...)           VkPhysicalDeviceFeatures
#   IVK_FEATURE_STRUCT(<type>, <sType>, <count>, <first member>, "<name>" "\0" ...)
# for every "typedef struct VkPhysicalDevice...Features..." whose members are sType, pNext and
# VkBool32s only. A struct's sType constant is found by spelling its name in capitals with
# underscores: underscores always go between a lower-case and an upper-case letter and before the
# last capital of an acronym followed by a lower-case letter ("ASTCDecode": ASTC_DECODE); between
# letters and digits both spellings are tried ("Float16Int8": FLOAT16_INT8, "Robustness2":
# ROBUSTNESS_2), "Vulkan<d><d>" is VULKAN_<d>_<d> and "ASTCHDR" is ASTC_HDR. Only a spelling that the header's
# VkStructureType enumeration contains is used; a struct with none is left out and named on
# stderr. A struct or sType the header declares under a preprocessor condition (Khronos puts
# provisional extensions under #ifdef VK_ENABLE_BETA_EXTENSIONS) is emitted between #if <the same
# condition> and #endif, so the table names nothing the layer's compile doesn't declare.
# ios_vk_features.c checks every struct's size and first member at compile time.

set -eu

[ $# -eq 2 ] || { echo "usage: $0 <vulkan header> <out.h>" >&2; exit 2; }
hdr=$1
out=$2
[ -f "$hdr" ] || { echo "$0: no header $hdr" >&2; exit 1; }

# never write into refs/ (CLAUDE.md): checked on the out file's existing parent, links resolved
here=$(cd "$(dirname "$0")" && pwd)
refs=$(cd "$here/../../../refs" 2>/dev/null && pwd -P) || refs=""
parent=$(cd "$(dirname "$out")" && pwd -P) || { echo "$0: no directory for $out" >&2; exit 1; }
if [ -n "$refs" ]; then
    case "$parent/" in "$refs/"*) echo "$0: refusing to write into refs/: $out" >&2; exit 1 ;; esac
fi

awk '
# the preprocessor conditionals the header declares things under: a stack of conditions, an
# #ifndef that is the first directive being the include guard (no condition). Khronos puts a provisional
# extension (VK_ENABLE_BETA_EXTENSIONS) under #ifdef in vulkan_core.h: its enumerators, and in
# some revisions its structs. Whatever is conditional in the header is emitted under the same
# condition, so the table never names what the compile does not declare.
function directive(line,    d, kw, rest) {
    if (line !~ /^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)([^a-z_]|$)/) return 0
    d = line
    sub(/^[ \t]*#[ \t]*/, "", d)
    sub(/\/\*.*/, "", d); sub(/\/\/.*/, "", d)
    match(d, /^[a-z]+/); kw = substr(d, 1, RLENGTH); rest = substr(d, RLENGTH + 1)
    sub(/^[ \t]+/, "", rest); sub(/[ \t]+$/, "", rest)
    ndir++
    if (kw == "ifdef") { cond[++depth] = "defined(" rest ")"; taken[depth] = cond[depth] }
    else if (kw == "ifndef") {
        # the include guard: an #ifndef that is the first directive of the file
        if (ndir == 1) { cond[++depth] = ""; taken[depth] = "" }
        else { cond[++depth] = "!defined(" rest ")"; taken[depth] = cond[depth] }
    }
    else if (kw == "if") { cond[++depth] = "(" rest ")"; taken[depth] = cond[depth] }
    else if (kw == "elif" && depth > 0) {
        cond[depth] = "!(" taken[depth] ") && (" rest ")"; taken[depth] = "(" taken[depth] ") || (" rest ")"
    }
    else if (kw == "else" && depth > 0) cond[depth] = (taken[depth] == "") ? "0" : "!(" taken[depth] ")"
    else if (kw == "endif" && depth > 0) depth--
    return 1
}
function current(    i, c) {
    c = ""
    for (i = 1; i <= depth; i++) if (cond[i] != "") c = c (c == "" ? "" : " && ") cond[i]
    return c
}

# the second pass reads the header again from its start
FNR == 1 && NR > 1 { depth = 0; ndir = 0 }

# pass 1: every VkStructureType enumerator (also the aliases), with its condition
FNR == NR {
    if (directive($0)) next
    if (match($0, /^[ \t]*VK_STRUCTURE_TYPE_[A-Z0-9_]+[ \t]*=/)) {
        s = substr($0, RSTART, RLENGTH)
        sub(/^[ \t]*/, "", s); sub(/[ \t]*=$/, "", s)
        stypes[s] = 1
        scond[s] = current()
    }
    next
}
directive($0) { next }

function kind(c) {
    if (c ~ /[A-Z]/) return "U"
    if (c ~ /[a-z]/) return "L"
    if (c ~ /[0-9]/) return "D"
    return "S"
}

# the sType of a struct name, or "" when no spelling is an enumerator
function stype_of(name,    s, i, n, c, p, q, k, piece, pieces, np, amb, total, m, cand, j) {
    s = substr(name, 3)   # drop "Vk"
    if (match(s, /Vulkan[0-9][0-9]/))
        s = substr(s, 1, RSTART - 1) "Vulkan_" substr(s, RSTART + 6, 1) "_" substr(s, RSTART + 7, 1) substr(s, RSTART + 8)
    sub(/ASTCHDR/, "ASTC_HDR", s)   # two acronyms in a row, the one case the rules above miss
    n = length(s); np = 0; piece = ""
    for (i = 1; i <= n; i++) {
        c = substr(s, i, 1); k = kind(c)
        if (i > 1) {
            p = kind(substr(s, i - 1, 1)); q = (i < n) ? kind(substr(s, i + 1, 1)) : ""
            if (k == "S" || p == "S") { }                            # an underscore of its own
            else if (p == "L" && k == "U") piece = piece "_"
            else if (p == "U" && k == "U" && q == "L") piece = piece "_"
            else if ((p == "D") != (k == "D")) { pieces[++np] = piece; piece = "" }   # letter/digit: both
        }
        piece = piece c
    }
    pieces[++np] = piece
    amb = np - 1
    if (amb > 10) return ""
    total = 1; for (i = 0; i < amb; i++) total *= 2
    for (m = 0; m < total; m++) {
        cand = pieces[1]; j = m
        for (i = 2; i <= np; i++) { cand = cand ((j % 2) ? "_" : "") pieces[i]; j = int(j / 2) }
        cand = "VK_STRUCTURE_TYPE_" toupper(cand)
        if (cand in stypes) return cand
    }
    return ""
}

function finish(    i, names, st, c) {
    if (!ok || nm < 1) return
    names = ""
    for (i = 1; i <= nm; i++) names = names (i > 1 ? " \"\\0\" " : "") "\"" member[i] "\""
    if (sname == "VkPhysicalDeviceFeatures") {
        if (!have_stype) printf "IVK_CORE_FEATURES(%d, %s)\n", nm, names
        return
    }
    if (!have_stype) return
    st = stype_of(sname)
    if (st == "") { print "gen-features.sh: no sType found for " sname ", left out" > "/dev/stderr"; return }
    if (st in done) return
    done[st] = 1
    # the struct'"'"'s condition and its sType'"'"'s, both (once when they are the same)
    c = tcond
    if (scond[st] != "" && scond[st] != c) c = c (c == "" ? "" : " && ") scond[st]
    if (c != "") { printf "#if %s\n", c; nguarded++ }
    printf "IVK_FEATURE_STRUCT(%s, %s, %d, %s, %s)\n", sname, st, nm, member[1], names
    if (c != "") print "#endif"
}

# pass 2: the structs
/^typedef struct VkPhysicalDevice[A-Za-z0-9]*Features[A-Z]*[ \t]*[{]?[ \t]*$/ {
    sname = $3; sub(/[{].*/, "", sname)
    instruct = 1; ok = 1; nm = 0; have_stype = 0; field = 0; tcond = current()
    next
}
instruct && /^[ \t]*[{][ \t]*$/ { next }
instruct && /^[ \t]*[}]/ { finish(); instruct = 0; next }
instruct {
    line = $0
    sub(/\/\/.*/, "", line); gsub(/\*/, " * ", line); sub(/;.*/, "", line)
    nf = split(line, w, /[ \t]+/)
    # drop empty words from leading space
    nw = 0; for (i = 1; i <= nf; i++) if (w[i] != "") ww[++nw] = w[i]
    if (nw == 0) next
    field++
    if (field == 1 && nw == 2 && ww[1] == "VkStructureType" && ww[2] == "sType") { have_stype = 1; next }
    if (field == 2 && have_stype && nw == 3 && ww[1] == "void" && ww[2] == "*" && ww[3] == "pNext") next
    if (nw == 2 && ww[1] == "VkBool32") { member[++nm] = ww[2]; next }
    ok = 0
}
' "$hdr" "$hdr" > "$out.tmp"

{
    echo "/* generated by gen-features.sh from $(basename "$hdr"); do not edit */"
    cat "$out.tmp"
} > "$out"
rm -f "$out.tmp"
grep -q '^IVK_CORE_FEATURES(' "$out" || { echo "$0: VkPhysicalDeviceFeatures not found in $hdr" >&2; exit 1; }
echo "gen-features.sh: $(grep -c '^IVK_FEATURE_STRUCT(' "$out") feature structs from $(basename "$hdr") ($(grep -c '^#if ' "$out" || true) under the header's own conditions)"
