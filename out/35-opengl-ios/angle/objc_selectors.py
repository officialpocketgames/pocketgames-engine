# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 35 Part C1.3 (selectors) and spec 00 B1: every Objective-C selector a static archive uses
# must be declared in the iOS SDK's public headers.
#
#   python3 -I objc_selectors.py sdk <SDK path>              -> the selectors the public headers declare
#   python3 -I objc_selectors.py check <sdk list> <otool dir> -> the archive's unknown selectors
#
# "sdk" reads the headers of Metal, QuartzCore, Foundation, IOSurface, CoreFoundation and the
# Objective-C runtime (usr/include/objc): method declarations (- and +, in classes, categories
# and protocols) and properties (their getter and, unless readonly, setter, with getter= and
# setter= honoured). "check" reads, per archive member, the output of
# `otool -v -s __TEXT __objc_methname` (<member>.methname: every selector the member names) and
# `otool -ov` (<member>.ov: the methods the member implements itself, which are its own
# selectors), and prints each selector used that is neither declared nor implemented, with the
# member, one per line. Exit status 1 when there is one.

import os
import re
import sys

FRAMEWORKS = ("Metal", "QuartzCore", "Foundation", "IOSurface", "CoreFoundation")


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def skip_parens(text, i):
    """text[i] is '(': the index after the matching ')'"""
    depth = 0
    while i < len(text):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return i


def method_selector(text, i):
    """text[i] is '-' or '+' starting a declaration: (selector, end index) or (None, i + 1)"""
    j = i + 1
    while j < len(text) and text[j].isspace():
        j += 1
    if j >= len(text) or text[j] != "(":
        return None, i + 1
    j = skip_parens(text, j)
    parts = []
    while j < len(text):
        while j < len(text) and text[j].isspace():
            j += 1
        m = re.match(r"[A-Za-z_]\w*", text[j:])
        if not m:
            break
        word = m.group(0)
        k = j + len(word)
        while k < len(text) and text[k].isspace():
            k += 1
        if k < len(text) and text[k] == ":":
            parts.append(word + ":")
            k += 1
            while k < len(text) and text[k].isspace():
                k += 1
            if k < len(text) and text[k] == "(":
                k = skip_parens(text, k)
            while k < len(text) and text[k].isspace():
                k += 1
            m2 = re.match(r"[A-Za-z_]\w*", text[k:])   # the argument's name
            if m2:
                k += len(m2.group(0))
            j = k
            continue
        if not parts:
            parts.append(word)
        break
    if not parts:
        return None, i + 1
    return "".join(parts), j


PROPERTY = re.compile(r"@property\s*(\(([^)]*)\))?([^;]*);", re.S)


def property_selectors(attrs, decl):
    attrs = attrs or ""
    names = re.findall(r"[A-Za-z_]\w*", re.sub(r"\([^)]*\)", " ", decl))
    names = [n for n in names if n not in ("const", "nonnull", "nullable", "_Nonnull", "_Nullable",
                                          "__kindof", "API_AVAILABLE", "API_UNAVAILABLE")]
    if not names:
        return []
    name = names[-1]
    getter = re.search(r"getter\s*=\s*(\w+)", attrs)
    setter = re.search(r"setter\s*=\s*(\w+:)", attrs)
    out = [getter.group(1) if getter else name]
    if "readonly" not in attrs:
        out.append(setter.group(1) if setter else "set" + name[0].upper() + name[1:] + ":")
    return out


def header_selectors(text):
    text = strip_comments(text)
    found = set()
    for m in PROPERTY.finditer(text):
        found.update(property_selectors(m.group(2), m.group(3)))
    i = 0
    while i < len(text):
        c = text[i]
        if c in "-+" and (i == 0 or text[i - 1] in "\n;}{ \t"):
            sel, i = method_selector(text, i)
            if sel:
                found.add(sel)
            continue
        i += 1
    return found


def sdk(path):
    dirs = [os.path.join(path, "System/Library/Frameworks", f + ".framework", "Headers") for f in FRAMEWORKS]
    dirs.append(os.path.join(path, "usr/include/objc"))
    found = set()
    for d in dirs:
        if not os.path.isdir(d):
            print("objc_selectors.py: no headers at %s" % d, file=sys.stderr)
            continue
        for root, _, files in os.walk(d):
            for f in files:
                if f.endswith(".h"):
                    with open(os.path.join(root, f), errors="replace") as fh:
                        found.update(header_selectors(fh.read()))
    for s in sorted(found):
        print(s)


def check(sdk_list, otool_dir):
    with open(sdk_list) as fh:
        known = set(line.strip() for line in fh if line.strip())
    unknown = []
    for f in sorted(os.listdir(otool_dir)):
        if not f.endswith(".methname"):
            continue
        member = f[: -len(".methname")]
        used = set()
        with open(os.path.join(otool_dir, f), errors="replace") as fh:
            for line in fh:
                m = re.match(r"^[0-9a-fA-F]+\s+(\S+)\s*$", line)
                if m:
                    used.add(m.group(1))
        own = set()
        ov = os.path.join(otool_dir, member + ".ov")
        if os.path.exists(ov):
            with open(ov, errors="replace") as fh:
                for line in fh:
                    m = re.match(r"^\s*name\s+0x[0-9a-fA-F]+\s+(\S+)\s*$", line)
                    if m:
                        own.add(m.group(1))
        for s in sorted(used - known - own):
            unknown.append((member, s))
    for member, s in unknown:
        print("%s %s" % (s, member))
    return 1 if unknown else 0


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "sdk":
        sdk(sys.argv[2])
        return 0
    if len(sys.argv) == 4 and sys.argv[1] == "check":
        return check(sys.argv[2], sys.argv[3])
    print("usage: objc_selectors.py sdk <SDK> | check <sdk list> <otool dir>", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
