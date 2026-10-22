/*
 * Ntdll Unix interface
 *
 * Copyright (C) 2020 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Changed for MYIOSDECK, 2026 (spec 17 Part D6): five unix calls appended after
 * unix_system_time_precise, with their argument blocks. Shared with ntdll's PE half (spec 18):
 * the enum and the structs must be identical on both sides. The change is marked "MYIOSDECK";
 * out/09-ntdll-processes/CHANGES.md lists it.
 */

#ifndef __NTDLL_UNIXLIB_H
#define __NTDLL_UNIXLIB_H

#include "wine/unixlib.h"

struct _DISPATCHER_CONTEXT;

struct wine_dbg_write_params
{
    const char  *str;
    unsigned int len;
};

struct wine_server_fd_to_handle_params
{
    int          fd;
    unsigned int access;
    unsigned int attributes;
    HANDLE      *handle;
};

struct wine_server_handle_to_fd_params
{
    HANDLE        handle;
    unsigned int  access;
    int          *unix_fd;
    unsigned int *options;
};

struct wine_spawnvp_params
{
    char       **argv;
    int          wait;
};

struct load_so_dll_params
{
    UNICODE_STRING              nt_name;
    void                      **module;
};

struct unwind_builtin_dll_params
{
    ULONG                       type;
    struct _DISPATCHER_CONTEXT *dispatch;
    CONTEXT                    *context;
};

/* MYIOSDECK (spec 17 D6): the argument blocks of calls 8-12 (64-bit layout only: the 32-bit
 * table answers them STATUS_NOT_SUPPORTED) */
struct ios_push_jit_aliases_params
{
    void (*callback)( unsigned long long pe_base, unsigned long long jit_base,
                      unsigned long long size );
};

struct ios_register_hold_release_params
{
    unsigned int size;
    unsigned int version;
    void        *peb;
    void        *callback;
};

struct ios_jit_alias_probe_params
{
    unsigned int       size;
    unsigned int       version;
    unsigned long long addr;
    unsigned long long rw;
    unsigned long long base;
    unsigned long long end;
    unsigned int       write_gen;
    unsigned int       written;
    unsigned long long highest;
    unsigned int       at_end;
    unsigned long long rw_base;
    unsigned long long rx_base;
    unsigned int       slot;
    unsigned int       dup_end;
};

struct ios_get_fex_arena_params
{
    ULONG64 base;
    ULONG64 end;
};

#define IOS_HOLD_RELEASE_VERSION 1
#define IOS_JIT_ALIAS_PROBE_VERSION 1

#ifdef _WIN64
_Static_assert( sizeof(struct ios_register_hold_release_params) == 24, "hold-release block" );
_Static_assert( sizeof(struct ios_jit_alias_probe_params) == 88, "alias-probe block" );
_Static_assert( sizeof(struct ios_get_fex_arena_params) == 16, "fex-arena block" );
#endif

enum ntdll_unix_funcs
{
    unix_load_so_dll,
    unix_unwind_builtin_dll,
    unix_wine_dbg_write,
    unix_wine_server_call,
    unix_wine_server_fd_to_handle,
    unix_wine_server_handle_to_fd,
    unix_wine_spawnvp,
    unix_system_time_precise,
    /* MYIOSDECK (spec 17 D6): appended, never inserted; the PE half uses these numbers */
    unix_ios_push_jit_aliases,        /* 8 */
    unix_ios_register_hold_release,   /* 9 */
    unix_ios_jit_alias_probe,         /* 10 */
    unix_ios_mono_bridge_ptr,         /* 11 */
    unix_ios_get_fex_arena,           /* 12 */
};

_Static_assert( unix_system_time_precise == 7 && unix_ios_get_fex_arena == 12, "unix call numbers" );

extern unixlib_handle_t __wine_unixlib_handle;

#endif /* __NTDLL_UNIXLIB_H */
