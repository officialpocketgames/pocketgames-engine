# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 33 F2: reads the COM interfaces of Wine 11.4's include/d3d9.h (the DECLARE_INTERFACE_IID_
# blocks and their STDMETHOD lines) into plain Python data, for the generator and its checks.
# Nothing here knows about marshalling; it only reports what the header declares.

import re

_BLOCK = re.compile(
    r'DECLARE_INTERFACE_IID_\(\s*(\w+)\s*,\s*(\w+)\s*,\s*"([0-9a-fA-F-]+)"\s*\)\s*\{(.*?)\n\};',
    re.S)
_METHOD = re.compile(r'STDMETHOD(_?)\((.*?)\)\s*\((.*?)\)\s*PURE\s*;', re.S)
_COMMENT = re.compile(r'/\*.*?\*/', re.S)


class Arg:
    def __init__(self, ctype, name):
        self.ctype = ctype      # normalised C type, e.g. "const RECT *" or "IDirect3DSurface9 **"
        self.name = name

    def base(self):
        """The type without const, struct and pointer stars."""
        t = self.ctype.replace('const ', '').replace('struct ', '').replace('*', '')
        return t.strip()

    def stars(self):
        return self.ctype.count('*')

    def __repr__(self):
        return '%s %s' % (self.ctype, self.name)


class Method:
    def __init__(self, name, ret, args):
        self.name = name
        self.ret = ret
        self.args = args

    def __repr__(self):
        return '%s %s(%s)' % (self.ret, self.name, ', '.join(map(repr, self.args)))


class Interface:
    def __init__(self, name, base, iid, methods):
        self.name = name
        self.base = base
        self.iid = iid
        self.methods = methods

    def index(self, name):
        for i, m in enumerate(self.methods):
            if m.name == name:
                return i
        return -1


def _norm_type(t):
    t = ' '.join(t.split())
    t = re.sub(r'\s*\*\s*', ' *', t)
    t = t.replace('* *', '**')
    t = re.sub(r'\*\s+\*', '**', t)
    return t.strip()


def _split_args(text):
    text = ' '.join(text.split())
    if text in ('THIS', ''):
        return []
    if not text.startswith('THIS_'):
        raise ValueError('method arguments do not start with THIS_: ' + text)
    text = text[len('THIS_'):].strip()
    out = []
    for n, part in enumerate(text.split(',')):
        part = part.strip()
        m = re.match(r'^(.*?)(\w+)$', part)
        if part.endswith('*') or not m or not m.group(1).strip() or m.group(1).strip() in ('const', 'struct'):
            # an unnamed argument: the whole text is the type
            out.append(Arg(_norm_type(part), 'arg%d' % (n + 1)))
        else:
            out.append(Arg(_norm_type(m.group(1)), m.group(2)))
    return out


def parse_d3d9_header(path):
    with open(path, encoding='utf-8', errors='replace') as f:
        text = f.read()
    text = _COMMENT.sub('', text)
    interfaces = {}
    order = []
    for blk in _BLOCK.finditer(text):
        name, base, iid, body = blk.groups()
        methods = []
        for m in _METHOD.finditer(body):
            underscore, head, args = m.groups()
            if underscore:
                ret, mname = [x.strip() for x in head.split(',')]
            else:
                ret, mname = 'HRESULT', head.strip()
            methods.append(Method(mname, _norm_type(ret), _split_args(args)))
        interfaces[name] = Interface(name, base, iid.lower(), methods)
        order.append(name)
    return interfaces, order


if __name__ == '__main__':
    import sys
    ifs, order = parse_d3d9_header(sys.argv[1])
    for n in order:
        i = ifs[n]
        print('interface %s : %s {%s}' % (n, i.base, i.iid))
        for k, m in enumerate(i.methods):
            print('  %3d %r' % (k, m))
