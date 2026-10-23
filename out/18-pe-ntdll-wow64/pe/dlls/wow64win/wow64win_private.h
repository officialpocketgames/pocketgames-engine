/*
 * WoW64 private definitions
 *
 * Copyright 2021 Alexandre Julliard
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
 * Changed for MYIOSDECK (spec 18 Part K): the 32-bit guest window. Guest -> host keeps values
 * below 0x10000 (atoms, resource IDs) and NULL; anything else is B + value. Handles never convert.
 */

#ifndef __WOW64WIN_PRIVATE_H
#define __WOW64WIN_PRIVATE_H

#include "../win32u/win32syscalls.h"
#include "ntuser.h"
#include "wow64win_window.h"

/* MYIOSDECK (spec 18 Part K1): B, read on the first conversion (class 1010 on the own process,
 * cached; error -> 0); wow64win imports no data from wow64 */
static inline ULONG_PTR guest_window_base(void)
{
    static ULONG_PTR base;
    static LONG state;  /* 0: not read, 1: read */

    if (ReadAcquire( &state )) return base;
    if (NtQueryInformationProcess( GetCurrentProcess(), ProcessWineIosWowGuestBase,
                                   &base, sizeof(base), NULL ))
        base = 0;
    WriteRelease( &state, 1 );
    return base;
}

/* MYIOSDECK (spec 18 Part K2) */
static inline void *guest_to_host( ULONG value )
{
    return (void *)(ULONG_PTR)w64w_guest_to_host( guest_window_base(), value );
}

static inline ULONG host_to_guest( const void *ptr )
{
    return w64w_host_to_guest( guest_window_base(), (ULONG_PTR)ptr );
}

#define SYSCALL_ENTRY(id,name,_args) extern NTSTATUS WINAPI wow64_ ## name( UINT *args );
ALL_SYSCALLS32
#undef SYSCALL_ENTRY

extern ntuser_callback user_callbacks[];

struct object_attr64
{
    OBJECT_ATTRIBUTES   attr;
    UNICODE_STRING      str;
    SECURITY_DESCRIPTOR sd;
};

typedef struct
{
    ULONG Length;
    ULONG RootDirectory;
    ULONG ObjectName;
    ULONG Attributes;
    ULONG SecurityDescriptor;
    ULONG SecurityQualityOfService;
} OBJECT_ATTRIBUTES32;

static inline ULONG get_ulong( UINT **args ) { return *(*args)++; }
static inline HANDLE get_handle( UINT **args ) { return LongToHandle( *(*args)++ ); }
static inline void *get_ptr( UINT **args ) { return guest_to_host( *(*args)++ ); }

static inline void **addr_32to64( void **addr, ULONG *addr32 )
{
    if (!addr32) return NULL;
    *addr = guest_to_host( *addr32 );
    return addr;
}

static inline SIZE_T *size_32to64( SIZE_T *size, ULONG *size32 )
{
    if (!size32) return NULL;
    *size = *size32;
    return size;
}

static inline void put_addr( ULONG *addr32, void *addr )
{
    if (addr32) *addr32 = host_to_guest( addr );
}

static inline void put_size( ULONG *size32, SIZE_T size )
{
    if (size32) *size32 = min( size, MAXDWORD );
}

static inline UNICODE_STRING *unicode_str_32to64( UNICODE_STRING *str, const UNICODE_STRING32 *str32 )
{
    if (!str32) return NULL;
    str->Length = str32->Length;
    str->MaximumLength = str32->MaximumLength;
    str->Buffer = guest_to_host( str32->Buffer );
    return str;
}

static inline SECURITY_DESCRIPTOR *secdesc_32to64( SECURITY_DESCRIPTOR *out, const SECURITY_DESCRIPTOR *in )
{
    /* relative descr has the same layout for 32 and 64 */
    const SECURITY_DESCRIPTOR_RELATIVE *sd = (const SECURITY_DESCRIPTOR_RELATIVE *)in;

    if (!in) return NULL;
    out->Revision = sd->Revision;
    out->Sbz1     = sd->Sbz1;
    out->Control  = sd->Control & ~SE_SELF_RELATIVE;
    if (sd->Control & SE_SELF_RELATIVE)
    {
        if (sd->Owner) out->Owner = (PSID)((BYTE *)sd + sd->Owner);
        if (sd->Group) out->Group = (PSID)((BYTE *)sd + sd->Group);
        if ((sd->Control & SE_SACL_PRESENT) && sd->Sacl) out->Sacl = (PSID)((BYTE *)sd + sd->Sacl);
        if ((sd->Control & SE_DACL_PRESENT) && sd->Dacl) out->Dacl = (PSID)((BYTE *)sd + sd->Dacl);
    }
    else
    {
        out->Owner = guest_to_host( sd->Owner );
        out->Group = guest_to_host( sd->Group );
        if (sd->Control & SE_SACL_PRESENT) out->Sacl = guest_to_host( sd->Sacl );
        if (sd->Control & SE_DACL_PRESENT) out->Dacl = guest_to_host( sd->Dacl );
    }
    return out;
}

static inline OBJECT_ATTRIBUTES *objattr_32to64( struct object_attr64 *out, const OBJECT_ATTRIBUTES32 *in )
{
    memset( out, 0, sizeof(*out) );
    if (!in) return NULL;
    if (in->Length != sizeof(*in)) return &out->attr;

    out->attr.Length = sizeof(out->attr);
    out->attr.RootDirectory = LongToHandle( in->RootDirectory );
    out->attr.Attributes = in->Attributes;
    out->attr.ObjectName = unicode_str_32to64( &out->str, guest_to_host( in->ObjectName ));
    out->attr.SecurityQualityOfService = guest_to_host( in->SecurityQualityOfService );
    out->attr.SecurityDescriptor = secdesc_32to64( &out->sd, guest_to_host( in->SecurityDescriptor ));
    return &out->attr;
}

static inline void set_last_error32( DWORD err )
{
    TEB *teb = NtCurrentTeb();
    TEB32 *teb32 = (TEB32 *)((char *)teb + teb->WowTebOffset);
    teb32->LastErrorValue = err;
}

#endif /* __WOW64WIN_PRIVATE_H */
