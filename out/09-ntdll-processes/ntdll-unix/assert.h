// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 Part A "no exits", for assert(). build-ios.sh searches this folder first (-I$SRC), so
// "#include <assert.h>" in ntdll's unix files lands here: the system header, then an assert()
// that ends only the calling pseudo-process (ios_assert_fail, ios_exit.c) instead of calling
// libc's abort, which would end the whole app with no line in the Wine log. Outside WINE_IOS it
// is the system's assert. Like the system header it has no include guard: NDEBUG is honoured at
// each inclusion.

#include_next <assert.h>

#ifdef WINE_IOS
#undef assert
#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
extern void ios_assert_fail( const char *expr, const char *file, int line ) __attribute__((noreturn));
#define assert(expr) (__builtin_expect( !(expr), 0 ) ? ios_assert_fail( #expr, __FILE__, __LINE__ ) : (void)0)
#endif
#endif
