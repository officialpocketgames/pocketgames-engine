// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 17 Part J, Part E: the shared header overrides of out/09-ntdll-processes/include/.
// Compiled three ways by run-host-tests.sh against those headers and refs/wine-11.4/include:
//   -DWINE_IOS -DTEST_LINKED   ios_wow_base() defined here (base 0, then base B)
//   -DWINE_IOS                 ios_wow_base() not linked anywhere (the weak symbol is NULL)
//   (no WINE_IOS)              upstream's plain conversions

#include <stdint.h>
#include <stdio.h>

#include <windef.h>
#include <winternl.h>
#include "wine/unixlib.h"

#include "test_sig_check.h"

#if defined(WINE_IOS) && defined(TEST_LINKED)
static ULONG_PTR fake_base;
/* the same prototype as spec 09's ios_process.h: the two declarations must agree */
extern unsigned long ios_wow_base(void);
unsigned long ios_wow_base(void) { return fake_base; }
#endif

static void test_header_numbers(void)
{
    CHECK_EQ( ProcessWineIosWowGuestBase, 1010 );
    CHECK_EQ( ProcessWineMakeProcessSystem, 1000 );
    CHECK_EQ( ProcessWineGrantAdminToken, 1002 );
#ifdef _WIN64
    {
        /* declared with the other RtlWow64* functions of the 64-bit branch */
        NTSTATUS (WINAPI *fn)(HANDLE, ULONG *) = RtlWow64SuspendThread;
        (void)fn;
    }
#endif
}

static void test_base_zero(void)
{
    char x = 0;   /* only its address is used */

    CHECK_EQ( ios_wow_host_base(), 0 );
    CHECK( ios_wow_host_ptr( 0 ) == NULL );
    CHECK( ios_wow_guest_ptr32( NULL ) == 0 );
    CHECK_EQ( (uintptr_t)ios_wow_host_ptr( 0x12345678 ), 0x12345678 );
    CHECK_EQ( (uintptr_t)ios_wow_host_ptr( 0xfffff000 ), 0xfffff000 );
    CHECK_EQ( ios_wow_guest_ptr32( (void *)(uintptr_t)0x7ffe0000 ), 0x7ffe0000 );
    /* a 64-bit host pointer is truncated, as PtrToUlong does */
    CHECK_EQ( ios_wow_guest_ptr32( &x ), (ULONG)(uintptr_t)&x );
}

#if defined(WINE_IOS) && defined(TEST_LINKED)
static void test_base_b(void)
{
    const ULONG_PTR B = 0x300000000ul;

    fake_base = B;
    CHECK_EQ( ios_wow_host_base(), B );
    CHECK( ios_wow_host_ptr( 0 ) == NULL );            /* NULL stays NULL */
    CHECK( ios_wow_guest_ptr32( NULL ) == 0 );
    CHECK_EQ( (uintptr_t)ios_wow_host_ptr( 0x1000 ), B + 0x1000 );
    CHECK_EQ( (uintptr_t)ios_wow_host_ptr( 0xffffffff ), B + 0xffffffff );
    CHECK_EQ( ios_wow_guest_ptr32( (void *)(uintptr_t)(B + 0x401000) ), 0x401000 );
    CHECK_EQ( ios_wow_guest_ptr32( ios_wow_host_ptr( 0x7ffdf000 )), 0x7ffdf000 );
    /* truncated to 32 bits */
    CHECK_EQ( ios_wow_guest_ptr32( (void *)(uintptr_t)(B + 0x100000010ul) ), 0x10 );
    fake_base = 0;
    test_base_zero();
}
#endif

int main(void)
{
    test_header_numbers();
    test_base_zero();
#if defined(WINE_IOS) && defined(TEST_LINKED)
    test_base_b();
#endif
    return check_summary();
}
