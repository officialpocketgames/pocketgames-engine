# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright (c) 2026 the MYIOSDECK author
#
# Spec 33 F2: the generator. Reads table/d3d9w_table.txt (F1) and Wine 11.4's include/d3d9.h,
# checks one against the other, and writes:
#   gen/d3d9w_ids.h              method ids, kinds and pop bytes (both sides)
#   gen/d3d9w_stubs_i386.S       the i386 stubs: mov eax, id; jmp [gate]
#   gen/d3d9w_stubs_i386.h       C prototypes of the stubs, for the thunk's hand-written code
#   gen/d3d9w_vtables_i386.c     one vtable per interface, in d3d9.h's method order
#   gen/d3d9w_dispatch_gen.c     the native dispatch function of every method, and the table
#   gen/d3d9w_header_check.c     static asserts the compiler checks against d3d9.h: vtable
#                                order, and per method the i386 argument bytes against pop
#
#   python3 -I tools/gen_d3d9w.py <wine-11.4>/include/d3d9.h [--check]
#
# --check writes nothing: it fails when gen/ differs from what would be written.

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from d3d9w_parse import parse_d3d9_header  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
PART = os.path.dirname(HERE)
TABLE = os.path.join(PART, 'table', 'd3d9w_table.txt')
GEN = os.path.join(PART, 'gen')

LIB = 1  # IOS_WOW_NATIVE_LIB_D3D9

# Kinds: the order of interfaces in the table, then the exports. Each with the C type the native
# side calls through and the header types an argument of that kind may have.
KINDS = ['D3D9', 'Device', 'SwapChain', 'Surface', 'Texture', 'CubeTexture', 'VolumeTexture',
         'Volume', 'VertexBuffer', 'IndexBuffer', 'VertexDecl', 'VertexShader', 'PixelShader',
         'StateBlock', 'Query']
KIND_EXPORTS = 15
KIND_HEADER_TYPES = {
    'D3D9': ('IDirect3D9', 'IDirect3D9Ex'),
    'Device': ('IDirect3DDevice9', 'IDirect3DDevice9Ex'),
    'SwapChain': ('IDirect3DSwapChain9', 'IDirect3DSwapChain9Ex'),
    'Surface': ('IDirect3DSurface9',),
    'Texture': ('IDirect3DTexture9',),
    'CubeTexture': ('IDirect3DCubeTexture9',),
    'VolumeTexture': ('IDirect3DVolumeTexture9',),
    'Volume': ('IDirect3DVolume9',),
    'VertexBuffer': ('IDirect3DVertexBuffer9',),
    'IndexBuffer': ('IDirect3DIndexBuffer9',),
    'VertexDecl': ('IDirect3DVertexDeclaration9',),
    'VertexShader': ('IDirect3DVertexShader9',),
    'PixelShader': ('IDirect3DPixelShader9',),
    'StateBlock': ('IDirect3DStateBlock9',),
    'Query': ('IDirect3DQuery9',),
    'BaseTexture': ('IDirect3DBaseTexture9',),
    'Resource': ('IDirect3DResource9',),
}
FAMILIES = {
    'BaseTexture': ['Texture', 'CubeTexture', 'VolumeTexture'],
    'Resource': ['Surface', 'Texture', 'CubeTexture', 'VolumeTexture', 'Volume', 'VertexBuffer',
                 'IndexBuffer'],
}
# The non-Ex interface (or base) whose methods must be a prefix of the table's interface.
PREFIX_OF = {
    'IDirect3D9Ex': ['IDirect3D9'],
    'IDirect3DDevice9Ex': ['IDirect3DDevice9'],
    'IDirect3DSwapChain9Ex': ['IDirect3DSwapChain9'],
    'IDirect3DSurface9': ['IDirect3DResource9'],
    'IDirect3DVertexBuffer9': ['IDirect3DResource9'],
    'IDirect3DIndexBuffer9': ['IDirect3DResource9'],
    'IDirect3DTexture9': ['IDirect3DBaseTexture9', 'IDirect3DResource9'],
    'IDirect3DCubeTexture9': ['IDirect3DBaseTexture9', 'IDirect3DResource9'],
    'IDirect3DVolumeTexture9': ['IDirect3DBaseTexture9', 'IDirect3DResource9'],
}

VALUE_TYPES = {
    'UINT', 'DWORD', 'BOOL', 'INT', 'int', 'float', 'UINT32', 'D3DCOLOR',
    'D3DFORMAT', 'D3DDEVTYPE', 'D3DRESOURCETYPE', 'D3DMULTISAMPLE_TYPE', 'D3DPOOL',
    'D3DBACKBUFFER_TYPE', 'D3DTEXTUREFILTERTYPE', 'D3DTRANSFORMSTATETYPE', 'D3DRENDERSTATETYPE',
    'D3DSTATEBLOCKTYPE', 'D3DTEXTURESTAGESTATETYPE', 'D3DSAMPLERSTATETYPE', 'D3DPRIMITIVETYPE',
    'D3DQUERYTYPE', 'D3DCUBEMAP_FACES', 'D3DCOMPOSERECTSOP',
}
HANDLE_TYPES = {'HWND', 'HMONITOR', 'HANDLE', 'HDC'}
RET_TYPES = {
    'hr': {'HRESULT'},
    'u32': {'ULONG', 'UINT', 'DWORD', 'BOOL', 'D3DRESOURCETYPE', 'D3DTEXTUREFILTERTYPE',
            'D3DQUERYTYPE'},
    'count': {'UINT'},
    'void': {'void'},
    'float': {'float'},
    'handle': {'HMONITOR'},
}
# Structs converted field by field (pointer or handle fields, F4); the rest are copied as bytes.
CONVERTED = {'D3DPRESENT_PARAMETERS', 'D3DDEVICE_CREATION_PARAMETERS', 'D3DLOCKED_RECT',
             'D3DLOCKED_BOX', 'D3DPRESENTSTATS'}

# The exports and the thunk's own calls (kind 15): name, return kind, argument kinds (for pop
# and documentation), and the i386 C prototype the thunk calls the stub with. Their dispatch is
# hand-written (native/d3d9w_extras.c); include/d3d9w_proxy.h describes the thunk's calls.
EXTRAS = [
    ('Direct3DCreate9', 'u32', ['v', 'out:UINT'],
     'uint32_t', 'UINT sdk_version, uint32_t *status'),
    ('Direct3DCreate9Ex', 'hr', ['v', 'ifout:D3D9', 'out:UINT'],
     'HRESULT', 'UINT sdk_version, IDirect3D9Ex **out, uint32_t *status'),
    ('D3DPERF_BeginEvent', 'u32', ['v', 'opaque'], 'int', 'D3DCOLOR color, LPCWSTR name'),
    ('D3DPERF_EndEvent', 'u32', [], 'int', 'void'),
    ('D3DPERF_GetStatus', 'u32', [], 'DWORD', 'void'),
    ('D3DPERF_QueryRepeatFrame', 'u32', [], 'BOOL', 'void'),
    ('D3DPERF_SetMarker', 'void', ['v', 'opaque'], 'void', 'D3DCOLOR color, LPCWSTR name'),
    ('D3DPERF_SetOptions', 'void', ['v'], 'void', 'DWORD options'),
    ('D3DPERF_SetRegion', 'void', ['v', 'opaque'], 'void', 'D3DCOLOR color, LPCWSTR name'),
    ('DebugSetLevel', 'void', ['v'], 'void', 'DWORD level'),
    ('DebugSetMute', 'u32', [], 'BOOL', 'void'),
    ('Init', 'u32', ['v', 'in:UINT[a3]', 'v', 'out:UINT'],
     'uint32_t', 'uint32_t version, const uint32_t *vtables, uint32_t count, uint32_t *shared'),
    ('WindowState', 'hr', ['if:Device', 'out:d3d9w_guest_window_state'],
     'HRESULT', 'IDirect3DDevice9 *device, struct d3d9w_guest_window_state *state'),
    ('FocusActivation', 'u32', ['if:Device', 'v', 'out:RECT'],
     'DWORD', 'IDirect3DDevice9 *device, BOOL active, RECT *pos'),
    ('Unpin', 'hr', ['v'], 'HRESULT', 'void *proxy'),
    ('Pin', 'hr', ['v'], 'HRESULT', 'void *proxy'),
]


class TableError(Exception):
    pass


class TArg:
    def __init__(self, text):
        self.text = text
        self.kind, _, rest = text.partition(':')
        self.type = None
        self.count = None
        if rest:
            m = re.match(r'^(\w+)(?:\[(.+)\])?$', rest)
            if not m:
                raise TableError('bad argument ' + text)
            self.type, self.count = m.group(1), m.group(2)


class TMethod:
    def __init__(self, iface, index, name, ret, args, guest, special):
        self.iface = iface
        self.index = index
        self.name = name
        self.ret = ret
        self.args = args
        self.guest = guest
        self.special = special
        self.header = None

    def nwords(self):
        # an interface method's arguments follow This; the exports and the thunk's calls have none
        return len(self.args) + (0 if self.iface.kind == KIND_EXPORTS else 1)

    def pop(self):
        return 4 * self.nwords()

    def number(self):
        return (self.nwords() << 16) | (self.iface.kind << 8) | self.index

    def macro(self):
        return 'D3D9W_M_%s_%s' % (self.iface.short, self.name)


class TIface:
    def __init__(self, name, short, kind):
        self.name = name
        self.short = short
        self.kind = kind
        self.methods = []


def read_table(path):
    ifaces = []
    cur = None
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.split('#', 1)[0].strip()
            if not line:
                continue
            words = line.split()
            if words[0] == 'interface':
                if len(words) != 3 or words[2] not in KINDS:
                    raise TableError('line %d: bad interface line' % lineno)
                cur = TIface(words[1], words[2], KINDS.index(words[2]))
                if cur.kind != len(ifaces):
                    raise TableError('line %d: interfaces must appear in kind order' % lineno)
                ifaces.append(cur)
                continue
            if cur is None:
                raise TableError('line %d: method outside an interface' % lineno)
            name, ret = words[0], words[1]
            if ret not in RET_TYPES:
                raise TableError('line %d: bad return kind %s' % (lineno, ret))
            guest = False
            special = None
            args = []
            for w in words[2:]:
                if w == '@guest':
                    guest = True
                elif w.startswith('@special='):
                    special = w[len('@special='):]
                else:
                    args.append(TArg(w))
            cur.methods.append(TMethod(cur, len(cur.methods), name, ret, args, guest, special))
    if len(ifaces) != len(KINDS):
        raise TableError('the table has %d interfaces, expected %d' % (len(ifaces), len(KINDS)))
    return ifaces


def _check_count(expr, nargs, where):
    if expr in (None, '*') or expr.isdigit():
        return
    m = re.match(r'^(?:(\d+)\*)?a(\d+)$', expr)
    if not m or not (1 <= int(m.group(2)) <= nargs):
        raise TableError('%s: bad length %s' % (where, expr))


def check_arg(targ, harg, where, nargs):
    """Fails unless the table's argument kind fits the header's C type."""
    t = harg.ctype
    base = harg.base()
    stars = harg.stars()
    k = targ.kind

    def bad(why):
        raise TableError('%s: argument %s (%s): %s' % (where, harg.name, t, why))

    if k == 'v':
        if stars or base not in VALUE_TYPES:
            bad('not a 32-bit value type')
    elif k == 'h':
        if stars or base not in HANDLE_TYPES:
            bad('not a handle')
    elif k == 'opaque':
        if t != 'void *':
            bad('opaque must be void *')
    elif k in ('in', 'out', 'inout'):
        if stars != 1 or base != targ.type:
            bad('expected a pointer to %s' % targ.type)
        if k != 'in' and t.startswith('const '):
            bad('an out pointer cannot be const')
        _check_count(targ.count, nargs, where)
    elif k == 'hout':
        if stars != 1 or base != targ.type or base not in HANDLE_TYPES:
            bad('expected a pointer to a handle')
    elif k == 'iid':
        if base not in ('REFIID', 'REFGUID'):
            bad('expected REFIID or REFGUID')
    elif k == 'if':
        if stars != 1 or base not in KIND_HEADER_TYPES.get(targ.type, ()):
            bad('expected an interface pointer of kind %s' % targ.type)
    elif k == 'ifout':
        if stars != 2 or base not in KIND_HEADER_TYPES.get(targ.type, ()):
            bad('expected an interface out pointer of kind %s' % targ.type)
    elif k == 'ifout_iid':
        if t != 'void **':
            bad('ifout_iid must be void **')
    elif k == 'ifarr':
        if stars != 2 or base not in KIND_HEADER_TYPES.get(targ.type, ()):
            bad('expected an array of interface pointers')
        _check_count(targ.count, nargs, where)
    elif k == 'shader':
        if t != 'const DWORD *':
            bad('shader tokens are const DWORD *')
    elif k == 'decl':
        if t != 'const D3DVERTEXELEMENT9 *':
            bad('a declaration is const D3DVERTEXELEMENT9 *')
    elif k == 'rgn':
        if t != 'const RGNDATA *':
            bad('a region is const RGNDATA *')
    elif k == 'shared':
        if t != 'HANDLE *':
            bad('a shared handle is HANDLE *')
    elif k == 'lockptr':
        if t != 'void **':
            bad('a lock pointer is void **')
    else:
        bad('unknown argument kind ' + k)


def check(ifaces, header):
    errors = []
    for ti in ifaces:
        hi = header.get(ti.name)
        if not hi:
            errors.append('%s is not in d3d9.h' % ti.name)
            continue
        if len(hi.methods) != len(ti.methods):
            errors.append('%s: %d methods in the table, %d in d3d9.h'
                          % (ti.name, len(ti.methods), len(hi.methods)))
        for tm, hm in zip(ti.methods, hi.methods):
            where = '%s::%s' % (ti.name, tm.name)
            if tm.name != hm.name:
                errors.append('%s: vtable slot %d is %s in d3d9.h' % (where, tm.index, hm.name))
                continue
            tm.header = hm
            if hm.ret not in RET_TYPES[tm.ret]:
                errors.append('%s: returns %s, table says %s' % (where, hm.ret, tm.ret))
            if len(hm.args) != len(tm.args):
                errors.append('%s: %d arguments in d3d9.h, %d in the table'
                              % (where, len(hm.args), len(tm.args)))
                continue
            for ta, ha in zip(tm.args, hm.args):
                try:
                    check_arg(ta, ha, where, len(tm.args))
                except TableError as e:
                    errors.append(str(e))
            kinds = [a.kind for a in tm.args]
            if 'ifout_iid' in kinds and (kinds.index('ifout_iid') == 0 or
                                         kinds[kinds.index('ifout_iid') - 1] != 'iid'):
                errors.append('%s: ifout_iid must follow an iid' % where)
        for base in PREFIX_OF.get(ti.name, []):
            bi = header[base]
            names = [m.name for m in bi.methods]
            if [m.name for m in hi.methods[:len(names)]] != names:
                errors.append('%s does not start with the methods of %s' % (ti.name, base))
    if errors:
        raise TableError('the table does not match d3d9.h:\n  ' + '\n  '.join(errors))


# ----------------------------------------------------------------------------- output helpers

HEADER = '''/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Generated by tools/gen_d3d9w.py from table/d3d9w_table.txt and Wine 11.4 include/d3d9.h
 * (spec 33 F2). Do not edit: change the table or the generator and run it again. */
'''
ASM_HEADER = HEADER.replace('/*', '//', 1).replace('\n *', '\n//').replace(' */', '')


def native_type(kind):
    return KIND_HEADER_TYPES[kind][-1]


def kind_mask(kind):
    names = FAMILIES.get(kind, [kind])
    return ' | '.join('D3D9W_KB(D3D9W_KIND_%s)' % n for n in names)


def stub_name(m):
    return 'd3d9w_s_%s_%s' % (m.iface.short, m.name)


def guest_name(m):
    return 'd3d9t_%s_%s' % (m.iface.short, m.name)


def extras_methods():
    out = []
    iface = TIface('exports', 'Exports', KIND_EXPORTS)
    for i, (name, ret, args, c_ret_type, c_args) in enumerate(EXTRAS):
        m = TMethod(iface, i, name, ret, [TArg(a) for a in args], False, 'extra')
        m.c_proto = (c_ret_type, c_args)
        out.append(m)
    iface.methods = out
    return iface


def gen_ids(ifaces, extras):
    o = [HEADER, '#ifndef D3D9W_IDS_H', '#define D3D9W_IDS_H', '',
         '/* A method number (the low 24 bits of IOS_WOW_NATIVE_ID(1, n)) is',
         ' *   bits 16-23: the stdcall argument words, This included for an interface method',
         ' *               (pop = 4 * words)',
         ' *   bits  8-15: the kind (the interface, 15 for the exports and the thunk\'s calls)',
         ' *   bits  0-7:  the vtable slot (or the export\'s number)',
         ' * so even a mismatched build pops the right number of bytes (spec 33 G3). */',
         '#define D3D9W_LIB 1u',
         '#define D3D9W_NUM_KINDS %d' % len(KINDS),
         '#define D3D9W_KIND_EXPORTS %d' % KIND_EXPORTS,
         '#define D3D9W_METHOD_WORDS(n) (((n) >> 16) & 0xffu)',
         '#define D3D9W_METHOD_SLOT(n)  ((n) & 0xffffu)',
         '#define D3D9W_ID(n) ((D3D9W_LIB << 24) | ((n) & 0xffffffu))', '']
    for i, k in enumerate(KINDS):
        o.append('#define D3D9W_KIND_%s %d' % (k, i))
    o.append('#define D3D9W_KB(k) (1u << (k))')
    for fam, members in FAMILIES.items():
        o.append('#define D3D9W_KINDS_%s (%s)' % (fam, kind_mask(fam)))
    o.append('')
    o.append('/* vtable lengths, in d3d9.h order */')
    for ti in ifaces:
        o.append('#define D3D9W_VTBL_LEN_%s %d' % (ti.short, len(ti.methods)))
    o.append('')
    for ti in ifaces + [extras]:
        o.append('/* %s */' % ti.name)
        for m in ti.methods:
            o.append('#define %-56s 0x%06xu /* pop %d */' % (m.macro(), m.number(), m.pop()))
        o.append('')
    o.append('#endif')
    return '\n'.join(o) + '\n'


def gen_stubs_asm(ifaces, extras):
    o = [ASM_HEADER,
         '// Every stub: mov eax, IOS_WOW_NATIVE_ID(1, method); jmp dword ptr [d3d9w_gate].',
         '// The gate returns to the caller as stdcall does (interfaces/ios_wow_native.h).',
         '// IDirect3DDevice9::GetNPatchMode has its own stub in thunk/d3d9_thunk_stubs.S',
         '// (clarification 3).',
         '',
         '#if defined(_WIN32) || defined(__MINGW32__)',
         '#define SYM(x) _##x',
         '#else',
         '#define SYM(x) x',
         '#endif',
         '',
         '#define STUB(name, id) \\',
         '    .globl SYM(name); \\',
         '    .p2align 4; \\',
         'SYM(name): \\',
         '    movl $id, %eax; \\',
         '    jmp *SYM(d3d9w_gate)',
         '',
         '    .text']
    for ti in ifaces + [extras]:
        for m in ti.methods:
            o.append('STUB(%s, 0x%08x)' % (stub_name(m), (LIB << 24) | m.number()))
    return '\n'.join(o) + '\n'


def c_proto_args(m):
    """The stub's C parameters for the i386 thunk, from the header's types."""
    this = '%s *This' % native_type(m.iface.short) if m.iface.kind != KIND_EXPORTS else None
    out = [this] if this else []
    for ha in m.header.args:
        t = ha.ctype
        if t.endswith('*'):
            out.append('%s%s' % (t, ha.name))
        else:
            out.append('%s %s' % (t, ha.name))
    return ', '.join(out) if out else 'void'


def c_ret(m):
    return m.header.ret


ASM_NAME = '''/* The stubs are assembly symbols without stdcall's @N decoration: each prototype names its
 * symbol (with the leading underscore of i386 Windows C symbols). */
#if defined(_WIN32) || defined(__MINGW32__)
#define D3D9W_ASM_NAME(x) "_" x
#else
#define D3D9W_ASM_NAME(x) x
#endif
#define D3D9W_STUB(name) __asm__(D3D9W_ASM_NAME(#name))
'''


def gen_stubs_h(ifaces, extras):
    o = [HEADER, '#ifndef D3D9W_STUBS_I386_H', '#define D3D9W_STUBS_I386_H', '',
         '/* The generated stubs as stdcall functions: the thunk\'s hand-written methods call',
         ' * native through these. Types are the d3d9.h ones. */', '', ASM_NAME,
         '/* The gate address the stubs jump through (Part H stores it). */',
         'extern uint32_t d3d9w_gate;', '']
    for ti in ifaces:
        for m in ti.methods:
            # GetNPatchMode's generated stub returns the float's bits in EAX (clarification 3);
            # only the hand-written stub turns them into st(0).
            ret = 'DWORD' if is_npatch(m) else c_ret(m)
            o.append('%s WINAPI %s(%s) D3D9W_STUB(%s);'
                     % (ret, stub_name(m), c_proto_args(m), stub_name(m)))
        o.append('')
    o.append('/* The exports and the thunk\'s own calls (include/d3d9w_proxy.h). */')
    for m in extras.methods:
        r, args = m.c_proto
        o.append('%s WINAPI %s(%s) D3D9W_STUB(%s);' % (r, stub_name(m), args, stub_name(m)))
    o.append('')
    o.append('#endif')
    return '\n'.join(o) + '\n'


def gen_vtables(ifaces):
    o = [HEADER, '#include "d3d9_thunk.h"', '',
         '/* One vtable per interface, in d3d9.h order (spec 33 E2). Generated stubs, except',
         ' * the @guest methods (hand-written, thunk/). */', '']
    for ti in ifaces:
        o.append('const void *const d3d9w_vtbl_%s[D3D9W_VTBL_LEN_%s] =' % (ti.short, ti.short))
        o.append('{')
        for m in ti.methods:
            fn = guest_name(m) if m.guest else stub_name(m)
            o.append('    (const void *)%s, /* %d %s */' % (fn, m.index, m.name))
        o.append('};')
        o.append('')
    o.append('const void *const *const d3d9w_vtables[D3D9W_NUM_KINDS] =')
    o.append('{')
    for ti in ifaces:
        o.append('    d3d9w_vtbl_%s,' % ti.short)
    o.append('};')
    o.append('')
    o.append('const unsigned int d3d9w_vtable_lengths[D3D9W_NUM_KINDS] =')
    o.append('{')
    for ti in ifaces:
        o.append('    D3D9W_VTBL_LEN_%s,' % ti.short)
    o.append('};')
    return '\n'.join(o) + '\n'


def guest_protos(ifaces):
    o = [HEADER, '#ifndef D3D9W_GUEST_I386_H', '#define D3D9W_GUEST_I386_H', '',
         '/* The thunk\'s hand-written @guest methods (thunk/), as the vtables name them. */', '']
    for ti in ifaces:
        for m in ti.methods:
            if not m.guest:
                continue
            # GetNPatchMode's own stub is assembly too (thunk/d3d9_thunk_stubs.S).
            label = ' D3D9W_STUB(%s)' % guest_name(m) if is_npatch(m) else ''
            o.append('%s WINAPI %s(%s)%s;' % (c_ret(m), guest_name(m), c_proto_args(m), label))
    o.append('')
    o.append('#endif')
    return '\n'.join(o) + '\n'


def is_npatch(m):
    return m.iface.short == 'Device' and m.name == 'GetNPatchMode'


# ----------------------------------------------------------------------------- the dispatch

def count_expr(c):
    if c.isdigit():
        return '(uint64_t)%s' % c
    m = re.match(r'^(?:(\d+)\*)?a(\d+)$', c)
    mul, idx = m.group(1), m.group(2)
    return '(uint64_t)%sa[%s]' % (mul + ' * ' if mul else '', idx)


def value_expr(ha, i):
    base = ha.base()
    if base == 'float':
        return 'd3d9w_as_float(a[%d])' % i
    if base in ('INT', 'int'):
        return '(%s)(int32_t)a[%d]' % (base, i)
    return '(%s)a[%d]' % (base, i)


def gen_method(m):
    """C for one method's dispatch function."""
    ti = m.iface
    hm = m.header
    nt = native_type(ti.short)
    fail = {'hr': 'D3DERR_INVALIDCALL', 'u32': '0', 'count': '0', 'void': '0', 'float': '0',
            'handle': '0'}[m.ret]
    L = []
    L.append('/* %s::%s, vtable slot %d, pop %d */' % (ti.name, m.name, m.index, m.pop()))
    L.append('static void d_%s_%s(uint32_t esp, struct ios_wow_native_ret *ret)'
             % (ti.short, m.name))
    L.append('{')
    L.append('    uint32_t a[%d];' % m.nwords())
    L.append('    %s *This;' % nt)
    if m.special:
        L.append('    if (!d3d9w_read_args(a, esp, sizeof(a)) ||')
        L.append('        !(This = d3d9w_this(a[0], D3D9W_KIND_%s)))' % ti.short)
        L.append('    {')
        L.append('        ret->eax = %s;' % fail)
        L.append('        return;')
        L.append('    }')
        L.append('    d3d9w_sp_%s(D3D9W_KIND_%s, This, a, ret);' % (m.special, ti.short))
        L.append('}')
        return '\n'.join(L)

    decl = []       # local declarations
    pre = []        # copy-in code
    callargs = []   # native call arguments
    post = []       # write-back code
    cleanup = []    # frees
    iid_local = None
    for i, (ta, ha) in enumerate(zip(m.args, hm.args), 1):
        k = ta.kind
        if k == 'v':
            callargs.append(value_expr(ha, i))
        elif k == 'h':
            callargs.append('(%s)(uintptr_t)a[%d]' % (ha.base(), i))
        elif k == 'opaque':
            callargs.append('(void *)(uintptr_t)a[%d]' % i)
        elif k == 'iid':
            decl.append('GUID g%d;' % i)
            pre.append('if (!d3d9w_read(&g%d, a[%d], sizeof(g%d))) goto refused;' % (i, i, i))
            callargs.append('&g%d' % i)
            iid_local = 'g%d' % i
        elif k in ('in', 'out', 'inout') and ta.count is None:
            t = ta.type
            if t in CONVERTED:
                decl.append('struct d3d9w_g_%s s%d; %s l%d, *p%d = NULL;' % (t, i, t, i, i))
                pre.append('if (a[%d]) {' % i)
                pre.append('    if (!d3d9w_read(&s%d, a[%d], sizeof(s%d))) goto refused;' % (i, i, i))
                pre.append('    d3d9w_in_%s(&l%d, &s%d);' % (t, i, i))
                pre.append('    p%d = &l%d;' % (i, i))
                pre.append('}')
                if k != 'in':
                    post.append('if (p%d) {' % i)
                    post.append('    d3d9w_out_%s(&s%d, &l%d);' % (t, i, i))
                    post.append('    d3d9w_write(a[%d], &s%d, sizeof(s%d));' % (i, i, i))
                    post.append('}')
            else:
                # copied as bytes: the i386 size (include/d3d9w_layout.h, F4)
                decl.append('%s l%d, *p%d = NULL;' % (t, i, i))
                pre.append('if (a[%d]) {' % i)
                pre.append('    if (!d3d9w_read(&l%d, a[%d], D3D9W_GSIZE_%s)) goto refused;' % (i, i, t))
                pre.append('    p%d = &l%d;' % (i, i))
                pre.append('}')
                if k != 'in':
                    post.append('if (p%d) d3d9w_write(a[%d], &l%d, D3D9W_GSIZE_%s);' % (i, i, i, t))
            callargs.append('p%d' % i)
        elif k in ('in', 'out', 'inout'):
            t = ta.type
            if ta.count.isdigit():
                decl.append('%s l%d[%s], *p%d = NULL;' % (t, i, ta.count, i))
                pre.append('if (a[%d]) {' % i)
                pre.append('    if (!d3d9w_read(l%d, a[%d], sizeof(l%d))) goto refused;' % (i, i, i))
                pre.append('    p%d = l%d;' % (i, i))
                pre.append('}')
                if k != 'in':
                    post.append('if (p%d) d3d9w_write(a[%d], l%d, sizeof(l%d));' % (i, i, i, i))
                callargs.append('p%d' % i)
            else:
                decl.append('D3D9W_STACK_ARRAY(b%d); void *h%d = NULL; void *p%d = NULL; uint64_t n%d;'
                            % (i, i, i, i))
                pre.append('n%d = %s * sizeof(%s);' % (i, count_expr(ta.count), t))
                pre.append('if (!d3d9w_array_in(a[%d], n%d, b%d, sizeof(b%d), &h%d, &p%d)) goto refused;'
                           % (i, i, i, i, i, i))
                if k != 'in':
                    post.append('if (a[%d]) d3d9w_write(a[%d], p%d, (size_t)n%d);' % (i, i, i, i))
                cleanup.append('free(h%d);' % i)
                callargs.append('(%s)p%d' % (ha.ctype, i))
        elif k == 'hout':
            decl.append('uint32_t v%d = 0; %s l%d, *p%d = NULL;' % (i, ta.type, i, i))
            pre.append('if (a[%d]) {' % i)
            pre.append('    if (!d3d9w_read(&v%d, a[%d], 4)) goto refused;' % (i, i))
            pre.append('    l%d = (%s)(uintptr_t)v%d;' % (i, ta.type, i))
            pre.append('    p%d = &l%d;' % (i, i))
            pre.append('}')
            post.append('if (p%d) { v%d = (uint32_t)(uintptr_t)l%d; d3d9w_write(a[%d], &v%d, 4); }'
                        % (i, i, i, i, i))
            callargs.append('p%d' % i)
        elif k == 'if':
            decl.append('void *n%d = NULL;' % i)
            pre.append('if (a[%d] && !(n%d = d3d9w_native_of(a[%d], %s))) goto refused;'
                       % (i, i, i, kind_mask(ta.type)))
            callargs.append('(%s)n%d' % (ha.ctype.replace('const ', ''), i))
        elif k in ('ifout', 'ifout_iid'):
            if m.ret != 'hr':
                raise TableError('%s::%s: an interface out needs an HRESULT method' % (ti.name, m.name))
            decl.append('void *o%d = D3D9W_UNSET;' % i)
            pre.append('if (a[%d] && !d3d9w_guest_ok(a[%d], 4)) goto refused;' % (i, i))
            callargs.append('a[%d] ? (%s)&o%d : NULL' % (i, ha.ctype.replace('const ', ''), i))
            # The out pointer is written only when native code wrote it (D3D9W_UNSET otherwise).
            if k == 'ifout_iid':
                put = 'd3d9w_put_iface_iid(a[%d], o%d, &%s, This, D3D9W_KIND_%s)' \
                    % (i, i, iid_local, ti.short)
                err = 'E_NOINTERFACE'
            else:
                put = 'd3d9w_put_iface(a[%d], o%d, %s)' % (i, i, kind_mask(ta.type))
                err = 'E_OUTOFMEMORY'
            post.append('if (a[%d] && o%d != D3D9W_UNSET && !%s) hr = %s;' % (i, i, put, err))
        elif k == 'ifarr':
            decl.append('D3D9W_STACK_ARRAY(b%d); void *h%d = NULL; void *p%d = NULL;' % (i, i, i))
            pre.append('if (!d3d9w_iface_array_in(a[%d], %s, %s, b%d, sizeof(b%d), &h%d, &p%d)) goto refused;'
                       % (i, count_expr(ta.count), kind_mask(ta.type), i, i, i, i))
            cleanup.append('free(h%d);' % i)
            callargs.append('(%s)p%d' % (ha.ctype, i))
        elif k == 'shader':
            decl.append('D3D9W_STACK_ARRAY(b%d); void *h%d = NULL; const DWORD *p%d = NULL;' % (i, i, i))
            pre.append('if (a[%d] && !d3d9w_shader_in(a[%d], b%d, sizeof(b%d), &h%d, &p%d, NULL)) goto refused;'
                       % (i, i, i, i, i, i))
            cleanup.append('free(h%d);' % i)
            callargs.append('p%d' % i)
        elif k == 'decl':
            decl.append('D3DVERTEXELEMENT9 l%d[D3D9W_MAX_DECL_ELEMENTS]; const D3DVERTEXELEMENT9 *p%d = NULL;'
                        % (i, i))
            pre.append('if (a[%d]) { if (!d3d9w_decl_in(a[%d], l%d)) goto refused; p%d = l%d; }'
                       % (i, i, i, i, i))
            callargs.append('p%d' % i)
        elif k == 'rgn':
            decl.append('D3D9W_STACK_ARRAY(b%d); void *h%d = NULL; const RGNDATA *p%d = NULL;' % (i, i, i))
            pre.append('if (a[%d] && !d3d9w_rgn_in(a[%d], b%d, sizeof(b%d), &h%d, &p%d)) goto refused;'
                       % (i, i, i, i, i, i))
            cleanup.append('free(h%d);' % i)
            callargs.append('p%d' % i)
        elif k == 'shared':
            decl.append('HANDLE l%d, *p%d = NULL;' % (i, i))
            pre.append('if (a[%d]) { if (!d3d9w_shared_in(a[%d], &l%d)) goto refused; p%d = &l%d; }'
                       % (i, i, i, i, i))
            callargs.append('p%d' % i)
        else:
            raise TableError('%s::%s: no generic code for %s' % (ti.name, m.name, k))

    for d in decl:
        L.append('    ' + d)
    if m.ret == 'hr':
        L.append('    HRESULT hr;')
    L.append('')
    L.append('    if (!d3d9w_read_args(a, esp, sizeof(a)) ||')
    L.append('        !(This = d3d9w_this(a[0], D3D9W_KIND_%s)))' % ti.short)
    L.append('        goto refused;')
    for p in pre:
        L.append('    ' + p)
    call = 'This->lpVtbl->%s(This%s)' % (m.name, ''.join(', ' + c for c in callargs))
    if m.ret == 'hr':
        L.append('    hr = %s;' % call)
    elif m.ret in ('u32', 'count'):
        L.append('    ret->eax = (uint32_t)%s;' % call)
    elif m.ret == 'handle':
        L.append('    ret->eax = (uint32_t)(uintptr_t)%s;' % call)
    elif m.ret == 'float':
        L.append('    ret->eax = d3d9w_float_bits(%s);' % call)
    else:
        L.append('    %s;' % call)
    for p in post:
        L.append('    ' + p)
    if m.ret == 'hr':
        L.append('    ret->eax = (uint32_t)hr;')
    for c in cleanup:
        L.append('    ' + c)
    L.append('    return;')
    L.append('refused:')
    for c in cleanup:
        L.append('    ' + c)
    L.append('    ret->eax = %s;' % fail)
    L.append('}')
    return '\n'.join(L)


def gen_dispatch(ifaces, extras):
    o = [HEADER, '#include "d3d9w_dispatch.h"', '']
    specials = set()
    for ti in ifaces:
        for m in ti.methods:
            if m.special:
                specials.add(m.special)
    for s in sorted(specials):
        o.append('void d3d9w_sp_%s(unsigned int kind, void *This, const uint32_t *a, '
                 'struct ios_wow_native_ret *ret);' % s)
    o.append('')
    for ti in ifaces:
        for m in ti.methods:
            o.append(gen_method(m))
            o.append('')
    # what a refused call answers (D4): D3DERR_INVALIDCALL for an HRESULT, otherwise 0
    def refusal(m):
        return 'D3DERR_INVALIDCALL' if m.ret == 'hr' else '0'

    o.append('const struct d3d9w_method d3d9w_methods[D3D9W_NUM_KINDS][D3D9W_MAX_SLOTS] =')
    o.append('{')
    for ti in ifaces:
        o.append('    [D3D9W_KIND_%s] = {' % ti.short)
        for m in ti.methods:
            o.append('        { d_%s_%s, %d, "%s::%s", (uint32_t)%s },'
                     % (ti.short, m.name, m.nwords(), ti.short, m.name, refusal(m)))
        o.append('    },')
    o.append('};')
    o.append('')
    o.append('const struct d3d9w_method d3d9w_extras[D3D9W_MAX_SLOTS] =')
    o.append('{')
    for m in extras.methods:
        o.append('    { d3d9w_x_%s, %d, "%s", (uint32_t)%s },'
                 % (m.name, m.nwords(), m.name, refusal(m)))
    o.append('};')
    return '\n'.join(o) + '\n'


def gen_header_check(ifaces):
    o = [HEADER,
         '/* Compiled (not linked) for i686-w64-mingw32 and for aarch64 by the host tests (J1):',
         ' * the compiler checks the table\'s view of d3d9.h. Every method sits in its vtable slot,',
         ' * and the i386 argument bytes of its d3d9.h prototype (each argument rounded up to 4, as',
         ' * stdcall pushes it, plus This) equal the pop the generator gave it. */',
         '#define COBJMACROS',
         '#include <stddef.h>',
         '#include <windows.h>',
         '#include <d3d9.h>',
         '',
         '#define W(t) ((sizeof(t) + 3) & ~(size_t)3)',
         '']
    for ti in ifaces:
        for m in ti.methods:
            o.append('_Static_assert(offsetof(%sVtbl, %s) == %d * sizeof(void *), "%s::%s slot");'
                     % (ti.name, m.name, m.index, ti.name, m.name))
            o.append('#ifdef __i386__')
            sizes = ' + '.join(['4'] + ['W(%s)' % a.ctype for a in m.header.args])
            o.append('_Static_assert(%s == %d, "%s::%s pop");' % (sizes, m.pop(), ti.name, m.name))
            o.append('#endif')
    o.append('')
    o.append('/* Every value kind is 32 bits on both sides. */')
    for t in sorted(VALUE_TYPES):
        o.append('_Static_assert(sizeof(%s) == 4, "%s");' % (t, t))
    return '\n'.join(o) + '\n'


def guid_init(text):
    p = text.split('-')
    b = p[3] + p[4]
    return '{ 0x%s, 0x%s, 0x%s, { %s } }' % (
        p[0], p[1], p[2], ', '.join('0x%s' % b[i:i + 2] for i in range(0, 16, 2)))


# IUnknown's IID is COM's, not d3d9.h's.
IID_IUNKNOWN = '00000000-0000-0000-c000-000000000046'
# The IID each kind is probed with (the non-Ex one: a non-Ex object answers only that).
PROBE_IFACE = {'D3D9': 'IDirect3D9', 'Device': 'IDirect3DDevice9',
               'SwapChain': 'IDirect3DSwapChain9'}


def gen_iids(ifaces, header):
    o = [HEADER, '#ifndef D3D9W_IIDS_H', '#define D3D9W_IIDS_H', '',
         '/* The IIDs of d3d9.h (copied here so this library defines no IID_ symbol of its own),',
         ' * with the kinds a proxy of each may have. */', '',
         'static const struct { GUID iid; uint32_t mask; } d3d9w_iid_table[] =', '{',
         '    { %s, 0x%xu }, /* IUnknown */' % (guid_init(IID_IUNKNOWN), (1 << len(KINDS)) - 1)]
    for name in sorted(header):
        mask = None
        for k, types in KIND_HEADER_TYPES.items():
            if name in types:
                mask = kind_mask(k)
        if mask is None:
            continue
        o.append('    { %s, %s }, /* %s */' % (guid_init(header[name].iid), mask, name))
    o.append('};')
    o.append('')
    o.append('static const GUID d3d9w_kind_iid[D3D9W_NUM_KINDS] =')
    o.append('{')
    for ti in ifaces:
        name = PROBE_IFACE.get(ti.short, ti.name)
        o.append('    %s, /* %s */' % (guid_init(header[name].iid), name))
    o.append('};')
    o.append('')
    o.append('#endif')
    return '\n'.join(o) + '\n'


def outputs(ifaces, extras, header):
    return {
        'd3d9w_iids.h': gen_iids(ifaces, header),
        'd3d9w_ids.h': gen_ids(ifaces, extras),
        'd3d9w_stubs_i386.S': gen_stubs_asm(ifaces, extras),
        'd3d9w_stubs_i386.h': gen_stubs_h(ifaces, extras),
        'd3d9w_guest_i386.h': guest_protos(ifaces),
        'd3d9w_vtables_i386.c': gen_vtables(ifaces),
        'd3d9w_dispatch_gen.c': gen_dispatch(ifaces, extras),
        'd3d9w_header_check.c': gen_header_check(ifaces),
    }


def main(argv):
    if len(argv) < 2:
        print('usage: gen_d3d9w.py <wine-11.4>/include/d3d9.h [--check | --verify] [--table <file>]',
              file=sys.stderr)
        return 2
    header, _ = parse_d3d9_header(argv[1])
    table = argv[argv.index('--table') + 1] if '--table' in argv else TABLE
    try:
        ifaces = read_table(table)
        check(ifaces, header)
    except TableError as e:
        print('gen_d3d9w: ' + str(e), file=sys.stderr)
        return 1
    if '--verify' in argv:   # the table against the header only (the tests' broken tables)
        print('gen_d3d9w: the table matches d3d9.h')
        return 0
    extras = extras_methods()
    files = outputs(ifaces, extras, header)
    if '--check' in argv:
        stale = []
        for name, text in files.items():
            path = os.path.join(GEN, name)
            try:
                with open(path, newline='\n') as f:
                    if f.read() != text:
                        stale.append(name)
            except OSError:
                stale.append(name)
        if stale:
            print('gen_d3d9w: gen/ is stale: ' + ', '.join(stale), file=sys.stderr)
            return 1
        print('gen_d3d9w: table matches d3d9.h; gen/ is current (%d interfaces, %d methods)'
              % (len(ifaces), sum(len(t.methods) for t in ifaces)))
        return 0
    os.makedirs(GEN, exist_ok=True)
    for name, text in files.items():
        with open(os.path.join(GEN, name), 'w', newline='\n') as f:
            f.write(text)
    print('gen_d3d9w: wrote %d files to gen/ (%d interfaces, %d methods)'
          % (len(files), len(ifaces), sum(len(t.methods) for t in ifaces)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
