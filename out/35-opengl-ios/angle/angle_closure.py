# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 35 Part C1.2 and C1.3: what of ANGLE's build goes into libangle.a, and what linking it
# needs. build-angle.sh calls it on macOS; tests/test_angle_closure.py runs it on samples.
#
#   python3 -I angle_closure.py gn-link <gn desc json> <label>...
#       `gn desc <out> "//*" --format=json` read; from each label, the targets a program linking
#       it links too (deps and public_deps through static_library, source_set and group targets;
#       an executable, a shared library or an action is a build step, not linked); prints the
#       frameworks, weak frameworks and libraries of those targets as linker flags, one per line
#       (-framework X, -weak_framework X, -lx), then exits 0. Exit 2 if a label is missing or
#       not linkable.
#   python3 -I angle_closure.py map-members <link map> [<archive name>]
#       the archive members a link loaded, from ld's -map output ("# Object files:" lines
#       "[  n] /path/lib.a(member.o)", or "lib.a[k](member.o)"), in order, once each; only those of
#       <archive name> when given.
#   python3 -I angle_closure.py sources <compdb json> <objects file>
#       `ninja -t compdb` read; for each object path of <objects file> (relative to the build
#       directory, one per line) prints "object<TAB>source", the source relative to the build
#       directory as the compile command names it, or "?" when no command writes that object.

import json
import os
import re
import sys

LINKED = ("static_library", "source_set", "group")


def gn_link(desc, labels):
    """the linker flags of the link closure of labels, and the closure's size"""
    todo = list(labels)
    seen = set()
    for label in labels:
        target = desc.get(label)
        if target is None or target.get("type") not in LINKED:
            raise KeyError(label)
    frameworks, weak, libs = set(), set(), set()
    while todo:
        label = todo.pop()
        if label in seen:
            continue
        seen.add(label)
        target = desc[label]
        frameworks.update(target.get("frameworks", []))
        weak.update(target.get("weak_frameworks", []))
        libs.update(target.get("libs", []))
        for dep in target.get("deps", []) + target.get("public_deps", []):
            sub = desc.get(dep)
            if sub is not None and sub.get("type") in LINKED and dep not in seen:
                todo.append(dep)

    def name(f):
        return f[: -len(".framework")] if f.endswith(".framework") else f

    flags = ["-framework " + name(f) for f in sorted(frameworks)]
    flags += ["-weak_framework " + name(f) for f in sorted(weak)]
    for lib in sorted(libs):
        if "/" in lib or lib.endswith((".a", ".dylib", ".tbd")):
            flags.append(lib)
        else:
            flags.append("-l" + lib)
    return flags, len(seen)


OBJECT_LINE = re.compile(r"^\[\s*\d+\]\s+(.*)\(([^()]+)\)\s*$")


def map_members(text, archive=None):
    members = []
    seen = set()
    inside = False
    for line in text.splitlines():
        if line.startswith("# Object files:"):
            inside = True
            continue
        if inside and line.startswith("#"):
            break
        if not inside:
            continue
        m = OBJECT_LINE.match(line)
        if not m:
            continue
        path = re.sub(r"\[\d+\]$", "", m.group(1).strip())
        if archive is not None and os.path.basename(path) != archive:
            continue
        member = m.group(2)
        if member not in seen:
            seen.add(member)
            members.append(member)
    return members


def compdb_outputs(entries):
    """object (relative to the build directory) -> source, from compile commands"""
    out = {}
    for e in entries:
        directory = e.get("directory", "")
        output = e.get("output")
        if output is None:
            args = e.get("arguments") or e.get("command", "").split()
            for i, a in enumerate(args[:-1]):
                if a == "-o":
                    output = args[i + 1]
                    break
        if output is None:
            continue
        source = e.get("file", "")
        if os.path.isabs(source) and directory:
            source = os.path.relpath(source, directory)
        if os.path.isabs(output) and directory:
            output = os.path.relpath(output, directory)
        out[os.path.normpath(output)] = source
    return out


def main(argv):
    if len(argv) >= 3 and argv[1] == "gn-link":
        with open(argv[2], encoding="utf-8") as f:
            desc = json.load(f)
        try:
            flags, n = gn_link(desc, argv[3:])
        except KeyError as e:
            print("angle_closure: %s is not a linkable target in %s" % (e.args[0], argv[2]), file=sys.stderr)
            return 2
        for flag in flags:
            print(flag)
        print("angle_closure: %d targets in the link closure" % n, file=sys.stderr)
        return 0
    if len(argv) in (3, 4) and argv[1] == "map-members":
        with open(argv[2], encoding="utf-8", errors="replace") as f:
            text = f.read()
        for member in map_members(text, argv[3] if len(argv) == 4 else None):
            print(member)
        return 0
    if len(argv) == 4 and argv[1] == "sources":
        with open(argv[2], encoding="utf-8") as f:
            table = compdb_outputs(json.load(f))
        with open(argv[3], encoding="utf-8") as f:
            for line in f:
                obj = line.strip()
                if obj:
                    print("%s\t%s" % (obj, table.get(os.path.normpath(obj), "?")))
        return 0
    print("usage: angle_closure.py gn-link <json> <label>... | map-members <map> [<archive>] | "
          "sources <compdb> <objects>", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
