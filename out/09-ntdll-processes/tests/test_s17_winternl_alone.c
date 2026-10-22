// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 17 Part J, Part E2: the winternl.h override compiles on its own, outside a unix library
// (as the wineserver includes it: server/user.c includes nothing before windef.h and
// winternl.h, and there __ms_va_list is va_list). Deliberately no <stdarg.h> here.

#include <windef.h>
#include <winternl.h>

int main(void)
{
    return ProcessWineIosWowGuestBase == 1010 ? 0 : 1;
}
