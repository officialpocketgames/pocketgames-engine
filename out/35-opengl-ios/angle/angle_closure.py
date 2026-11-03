# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 35 Part C1.2 and C1.3: what of ANGLE's build goes into libangle.a, and what linking it
# needs, from GN's own description of the build. build-angle.sh calls it on macOS;
# tests/test_angle_closure.py runs it on samples.
#
# <gn json> is GN's project.json (`gn gen <out> --ide=json`: {"build_settings": {...,
# "default_toolchain": ...}, "targets": {label: description}}) or `gn desc --format=json`'s
# {label: description}. Labels of the default toolchain are used without "(toolchain)". The link
# closure of a label is the targets a program linking it links too: deps and public_deps through
# static_library, source_set and group targets (an executable, a shared library or an action is a
# build step, not linked; another toolchain's target neither).
#
#   python3 -I angle_closure.py gn-link <gn json> <label>...
#       the closure's frameworks, weak frameworks and libraries as linker flags, one per line
#       (-framework X, -weak_framework X, -lx, or a library's path), each once. A path in the
#       source tree ("//...") is left out, Chromium's clang runtime (libclang_rt.*) among them:
#       the app links with Xcode's clang, which brings its own; each is named on standard error
#       ("dropped: <item> (<reason>)").
#   python3 -I angle_closure.py gn-paths <gn json> <label>...
#       for each target of the closure, "label<TAB>start > ... > label": the shortest chain of
#       deps that brings it in (why a target such as Perfetto's or volk's is there).
#   python3 -I angle_closure.py gn-objects <gn json> <build dir> <label>...
#       the object files of the closure's static libraries and source sets, found in <build dir>
#       where Chromium's toolchains put them (obj/<dir>/<target>/<name>.o, or
#       obj/<dir>/<target>.<name>.o), one per line as "object<TAB>source": the object relative to
#       <build dir>, the source relative to the source root ("?" if no source of the target has
#       that name). On standard error, each compiled source with no object ("no object: ..."),
#       then a count as the last line. Exit 3 if no object is found.
#   python3 -I angle_closure.py map-members <link map> [<archive name>]
#       the archive members a link loaded, from ld's -map output ("# Object files:" lines
#       "[  n] /path/lib.a(member.o)", or "lib.a[k](member.o)"), in order, once each; only those of
#       <archive name> when given.
# gn-link, gn-paths and gn-objects exit 2 if a label is missing or not linkable.

import glob
import json
import os
import re
import sys

LINKED = ("static_library", "source_set", "group")
COMPILED = (".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".s", ".S", ".asm")


def load(path):
    """{label: description} with the default toolchain's labels unqualified"""
    with open(path, encoding="utf-8") as f:
        text = f.read()
    start = text.find("{")
    if start < 0:
        raise ValueError("%s holds no JSON object" % path)
    data, _ = json.JSONDecoder().raw_decode(text[start:])
    default = ""
    targets = data
    if isinstance(data.get("targets"), dict):
        targets = data["targets"]
        default = data.get("build_settings", {}).get("default_toolchain", "")

    def norm(label):
        if default and label.endswith("(" + default + ")"):
            return label[: -len(default) - 2]
        return label

    out = {}
    for label, t in targets.items():
        t = dict(t)
        for key in ("deps", "public_deps"):
            t[key] = [norm(d) for d in t.get(key, [])]
        out[norm(label)] = t
    return out


def closure(desc, labels):
    """the labels of the link closure, sorted"""
    for label in labels:
        target = desc.get(label)
        if target is None or target.get("type") not in LINKED:
            raise KeyError(label)
    todo = list(labels)
    seen = set()
    while todo:
        label = todo.pop()
        if label in seen:
            continue
        seen.add(label)
        target = desc[label]
        for dep in target.get("deps", []) + target.get("public_deps", []):
            sub = desc.get(dep)
            if sub is not None and sub.get("type") in LINKED and "(" not in dep and dep not in seen:
                todo.append(dep)
    return sorted(seen)


def dropped_reason(item):
    """why a framework or library GN names is left out of the link, or None to keep it: a path in
    the source tree ("//...") is not something the app links; Chromium's own clang runtime least of
    all (Xcode's clang links its own compiler-rt)"""
    if not item.startswith("//"):
        return None
    if os.path.basename(item).startswith("libclang_rt"):
        return "Chromium's clang runtime; Xcode's clang links its own"
    return "a path in ANGLE's source tree"


def gn_link(desc, labels):
    """the linker flags of the link closure of labels, the closure's size, and what was left out
    ([(item, reason)])"""
    frameworks, weak, libs = set(), set(), set()
    labels_seen = closure(desc, labels)
    for label in labels_seen:
        target = desc[label]
        frameworks.update(target.get("frameworks", []))
        weak.update(target.get("weak_frameworks", []))
        libs.update(target.get("libs", []))
    dropped = [(i, dropped_reason(i)) for i in sorted(frameworks | weak | libs) if dropped_reason(i)]
    gone = set(i for i, _ in dropped)

    def name(f):
        return f[: -len(".framework")] if f.endswith(".framework") else f

    flags = ["-framework " + name(f) for f in sorted(frameworks - gone)]
    flags += ["-weak_framework " + name(f) for f in sorted(weak - gone)]
    for lib in sorted(libs - gone):
        if "/" in lib or lib.endswith((".a", ".dylib", ".tbd")):
            flags.append(lib)
        else:
            flags.append("-l" + lib)
    return flags, len(labels_seen), dropped


def gn_paths(desc, labels):
    """{label: [start label, ..., label]}: for each target of the link closure, the shortest chain
    of deps that brings it in (breadth first, labels in sorted order)"""
    closure(desc, labels)
    paths = {}
    queue = []
    for label in sorted(labels):
        if label not in paths:
            paths[label] = [label]
            queue.append(label)
    while queue:
        label = queue.pop(0)
        target = desc[label]
        for dep in sorted(target.get("deps", []) + target.get("public_deps", [])):
            sub = desc.get(dep)
            if sub is not None and sub.get("type") in LINKED and "(" not in dep and dep not in paths:
                paths[dep] = paths[label] + [dep]
                queue.append(dep)
    return paths


def source_path(s):
    """a GN source ("//dir/file.cc") relative to the source root"""
    return s[2:] if s.startswith("//") else s


def gn_objects(desc, labels, build_dir):
    """[(object, source)], [compiled sources with no object], targets in the closure, with objects"""
    rows, missing = [], []
    with_objects = 0
    labels_seen = closure(desc, labels)
    for label in labels_seen:
        target = desc[label]
        if target.get("type") not in ("static_library", "source_set"):
            continue
        path, _, name = label[2:].partition(":")
        base = "obj/" + (path + "/" if path else "")
        stems = {}
        for s in target.get("sources", []):
            stem, ext = os.path.splitext(os.path.basename(s))
            if ext in COMPILED:
                stems.setdefault(stem, source_path(s))
        found = {}
        for f in sorted(glob.glob(os.path.join(build_dir, base + name, "*.o"))):
            found[os.path.basename(f)[:-2]] = os.path.relpath(f, build_dir)
        for f in sorted(glob.glob(os.path.join(build_dir, base + glob.escape(name) + ".*.o"))):
            found.setdefault(os.path.basename(f)[len(name) + 1: -2], os.path.relpath(f, build_dir))
        if found:
            with_objects += 1
        for stem in sorted(found):
            rows.append((found[stem].replace(os.sep, "/"), stems.get(stem, "?")))
        for stem in sorted(stems):
            if stem not in found:
                missing.append("%s (%s)" % (stems[stem], label))
    return rows, missing, len(labels_seen), with_objects


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


def main(argv):
    try:
        if len(argv) >= 3 and argv[1] == "gn-link":
            flags, n, dropped = gn_link(load(argv[2]), argv[3:])
            for flag in flags:
                print(flag)
            for item, reason in dropped:
                print("dropped: %s (%s)" % (item, reason), file=sys.stderr)
            print("angle_closure: %d targets in the link closure" % n, file=sys.stderr)
            return 0
        if len(argv) >= 3 and argv[1] == "gn-paths":
            paths = gn_paths(load(argv[2]), argv[3:])
            for label in sorted(paths):
                print("%s\t%s" % (label, " > ".join(paths[label])))
            return 0
        if len(argv) >= 4 and argv[1] == "gn-objects":
            rows, missing, n, with_objects = gn_objects(load(argv[2]), argv[4:], argv[3])
            for obj, src in rows:
                print("%s\t%s" % (obj, src))
            for m in missing:
                print("no object: %s" % m, file=sys.stderr)
            print("angle_closure: %d targets in the link closure, %d with objects, %d objects, "
                  "%d compiled sources without an object" % (n, with_objects, len(rows), len(missing)),
                  file=sys.stderr)
            return 0 if rows else 3
    except KeyError as e:
        print("angle_closure: %s is not a linkable target in %s" % (e.args[0], argv[2]), file=sys.stderr)
        return 2
    except (ValueError, OSError) as e:
        print("angle_closure: %s" % e, file=sys.stderr)
        return 2
    if len(argv) in (3, 4) and argv[1] == "map-members":
        with open(argv[2], encoding="utf-8", errors="replace") as f:
            text = f.read()
        for member in map_members(text, argv[3] if len(argv) == 4 else None):
            print(member)
        return 0
    print("usage: angle_closure.py gn-link <gn json> <label>... | gn-paths <gn json> <label>... | "
          "gn-objects <gn json> <build dir> <label>... | map-members <map> [<archive>]", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
