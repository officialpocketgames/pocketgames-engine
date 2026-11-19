/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 33 E3 and H: the thunk's decisions that need no Windows call (thunk/d3d9_thunk_pure.h),
 * on the Linux host. The rest of the thunk is i386 guest code: the host tests compile and link
 * it with llvm-mingw, and TESTS.md lists what only the device shows. */

#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <d3d9.h>
#include "d3d9w_proxy.h"
#include "d3d9_thunk_pure.h"

static int failures, checks;

#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main(void)
{
    int copy;

    /* Part H: PG_D3D9_NATIVE is exactly "1" */
    CHECK(d3d9t_env_wants_native("1", 1));
    CHECK(!d3d9t_env_wants_native("", 0));
    CHECK(!d3d9t_env_wants_native("0", 1));
    CHECK(!d3d9t_env_wants_native("10", 2));
    CHECK(!d3d9t_env_wants_native("yes", 3));
    CHECK(strstr(d3d9t_nogate_reason(D3D9W_NOGATE_NO_CPU_GATE), "no gate") != NULL);
    CHECK(d3d9t_nogate_reason(D3D9W_NOGATE_VERSION) != d3d9t_nogate_reason(D3D9W_NOGATE_REFUSED));

    /* SetCursorProperties' checks */
    CHECK(d3d9t_validate_cursor(D3DFMT_A8R8G8B8, 32, 32, 0, 0, 1920, 1080) == D3D_OK);
    CHECK(d3d9t_validate_cursor(D3DFMT_A8R8G8B8, 32, 32, 31, 31, 1920, 1080) == D3D_OK);
    CHECK(d3d9t_validate_cursor(D3DFMT_A8R8G8B8, 32, 32, 32, 0, 1920, 1080) == D3DERR_INVALIDCALL);
    CHECK(d3d9t_validate_cursor(D3DFMT_X8R8G8B8, 32, 32, 0, 0, 1920, 1080) == D3DERR_INVALIDCALL);
    CHECK(d3d9t_validate_cursor(D3DFMT_A8R8G8B8, 24, 32, 0, 0, 1920, 1080) == D3DERR_INVALIDCALL);
    CHECK(d3d9t_validate_cursor(D3DFMT_A8R8G8B8, 64, 64, 0, 0, 32, 32) == D3DERR_INVALIDCALL);
    CHECK(d3d9t_validate_cursor(D3DFMT_A8R8G8B8, 0, 0, 5, 5, 0, 0) == D3D_OK);   /* zero extents pass */

    /* the x87 control word a device sets (clarification 12): masked, single, nearest */
    CHECK(d3d9t_fpu_control(0x037f) == 0x007f);   /* the x87 default: extended precision */
    CHECK(d3d9t_fpu_control(0x0c72) == 0x007f);   /* truncate, two exceptions unmasked */
    CHECK(d3d9t_fpu_control(0x1f7f) == 0x107f);   /* bit 12 (infinity control) kept */
    CHECK(d3d9t_fpu_control(0x007f) == 0x007f);

    /* GetDC's formats */
    CHECK(d3d9t_getdc_format_ok(D3DFMT_X8R8G8B8));
    CHECK(d3d9t_getdc_format_ok(D3DFMT_R5G6B5));
    CHECK(!d3d9t_getdc_format_ok(D3DFMT_DXT1));
    CHECK(!d3d9t_getdc_format_ok(D3DFMT_A16B16G16R16F));

    /* GetPrivateData of an IUnknown entry */
    CHECK(d3d9t_private_iface_size(0, 0, &copy) == D3D_OK && !copy);
    CHECK(d3d9t_private_iface_size(1, 3, &copy) == D3DERR_MOREDATA && !copy);
    CHECK(d3d9t_private_iface_size(1, 4, &copy) == D3D_OK && copy);

    printf("test_thunk_pure: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
