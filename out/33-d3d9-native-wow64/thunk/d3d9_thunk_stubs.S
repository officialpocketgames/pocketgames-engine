// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 33 clarification 3: IDirect3DDevice9::GetNPatchMode's own stub. The app's gate returns
// results in EAX only, so the generated stub of this method returns the float's bits in EAX;
// this one calls it and hands the program the float in st(0), as a stdcall float return.
//
//   float __stdcall GetNPatchMode(IDirect3DDevice9 *This)

#if defined(_WIN32) || defined(__MINGW32__)
#define SYM(x) _##x
#else
#define SYM(x) x
#endif

    .text
    .globl SYM(d3d9t_Device_GetNPatchMode)
    .p2align 4
SYM(d3d9t_Device_GetNPatchMode):
    pushl 4(%esp)                               // This, the stub's one argument
    call SYM(d3d9w_s_Device_GetNPatchMode)      // stdcall: pops This; eax = the float's bits
    pushl %eax
    flds (%esp)                                 // st(0) = the float
    addl $4, %esp
    ret $4
