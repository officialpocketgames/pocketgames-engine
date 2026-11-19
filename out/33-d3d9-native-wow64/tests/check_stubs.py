# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 33 J1: the assembled i386 stubs encode the right ids. Reads `objdump -d` of the stubs'
# object file and checks every stub is exactly
#     b8 <id>          mov eax, id
#     ff 25 <gate>     jmp dword ptr [d3d9w_gate]
# with id = IOS_WOW_NATIVE_ID(1, words << 16 | kind << 8 | slot), where kind, slot and words are
# worked out here from d3d9.h itself (the interface's place among the table's interfaces, the
# method's vtable index, 1 + its argument count) and not taken from the generator.
#
#   python3 -I check_stubs.py <d3d9.h> <objdump -d output> <table>

import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
from d3d9w_parse import parse_d3d9_header  # noqa: E402

SHORT = {'IDirect3D9Ex': 'D3D9', 'IDirect3DDevice9Ex': 'Device', 'IDirect3DSwapChain9Ex': 'SwapChain',
         'IDirect3DSurface9': 'Surface', 'IDirect3DTexture9': 'Texture',
         'IDirect3DCubeTexture9': 'CubeTexture', 'IDirect3DVolumeTexture9': 'VolumeTexture',
         'IDirect3DVolume9': 'Volume', 'IDirect3DVertexBuffer9': 'VertexBuffer',
         'IDirect3DIndexBuffer9': 'IndexBuffer', 'IDirect3DVertexDeclaration9': 'VertexDecl',
         'IDirect3DVertexShader9': 'VertexShader', 'IDirect3DPixelShader9': 'PixelShader',
         'IDirect3DStateBlock9': 'StateBlock', 'IDirect3DQuery9': 'Query'}


def main(argv):
    header, _ = parse_d3d9_header(argv[1])
    with open(argv[2]) as f:
        dump = f.read()
    # the interface order is the table's: kind = position
    order = []
    with open(argv[3]) as f:
        for line in f:
            m = re.match(r'^interface\s+(\w+)\s+\w+', line)
            if m:
                order.append(m.group(1))
    expected = {}
    for kind, name in enumerate(order):
        for slot, method in enumerate(header[name].methods):
            words = 1 + len(method.args)
            expected['d3d9w_s_%s_%s' % (SHORT[name], method.name)] = \
                (1 << 24) | (words << 16) | (kind << 8) | slot

    # objdump -d: "<symbol>:" then lines "  addr: b8 xx xx xx xx   movl ..."
    found = {}
    cur = None
    for line in dump.splitlines():
        m = re.match(r'^[0-9a-f]+ <_?(d3d9w_s_\w+)>:', line)
        if m:
            cur = m.group(1)
            found[cur] = []
            continue
        m = re.match(r'^\s*[0-9a-f]+:\s+((?:[0-9a-f]{2} )+)', line)
        if cur and m:
            found[cur].extend(int(b, 16) for b in m.group(1).split())
    errors = []
    for name, want in expected.items():
        code = found.get(name)
        if code is None:
            errors.append('%s: no stub' % name)
            continue
        got = code[0] == 0xb8 and int.from_bytes(bytes(code[1:5]), 'little')
        if got != want:
            errors.append('%s: id %#x, expected %#x' % (name, got or 0, want))
        if code[5:7] != [0xff, 0x25]:
            errors.append('%s: not followed by jmp dword ptr [gate]' % name)
    # the exports and the thunk's calls: words equal their arguments (no This)
    extras = [n for n in found if n.startswith('d3d9w_s_Exports_')]
    for name in extras:
        code = found[name]
        if code[0] != 0xb8 or code[5:7] != [0xff, 0x25] or (code[4] != 1) or code[2] != 0x0f:
            errors.append('%s: not an exports stub of library 1' % name)
    if len(found) != len(expected) + len(extras):
        errors.append('%d stubs, expected %d interface stubs and the extras'
                      % (len(found), len(expected)))
    if errors:
        print('check_stubs: ' + '\n  '.join(errors))
        return 1
    print('check_stubs: %d interface stubs and %d export stubs encode their ids'
          % (len(expected), len(extras)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
