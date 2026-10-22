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
 * Changed for MYIOSDECK (spec 18 Part J): a 32-bit process's address a is host address B + a.
 * Every 32-bit value native code dereferences converts guest -> host, every host address written
 * back for 32-bit code converts host -> guest (wow64_window.h). With B = 0 nothing changes.
 */

#ifndef __WOW64_PRIVATE_H
#define __WOW64_PRIVATE_H

#include "../ntdll/ntsyscalls.h"
#include "struct32.h"
#include "wow64_window.h"

#define SYSCALL_ENTRY(id,name,_args) extern NTSTATUS WINAPI wow64_ ## name( UINT *args );
ALL_SYSCALLS32
#undef SYSCALL_ENTRY

extern void init_image_mapping( HMODULE module );
extern void init_file_redirects(void);
extern BOOL get_file_redirect( OBJECT_ATTRIBUTES *attr );

extern USHORT native_machine;
extern USHORT current_machine;
extern ULONG_PTR args_alignment;
extern ULONG_PTR highest_user_address;
extern ULONG_PTR default_zero_bits;
extern SYSTEM_DLL_INIT_BLOCK *pLdrSystemDllInitBlock;

extern void     (WINAPI *pBTCpuFlushInstructionCache2)( const void *, SIZE_T );
extern void     (WINAPI *pBTCpuFlushInstructionCacheHeavy)( const void *, SIZE_T );
extern NTSTATUS (WINAPI *pBTCpuNotifyMapViewOfSection)( void *, void *, void *, SIZE_T, ULONG, ULONG );
extern void     (WINAPI *pBTCpuNotifyMemoryAlloc)( void *, SIZE_T, ULONG, ULONG, BOOL, NTSTATUS );
extern void     (WINAPI *pBTCpuNotifyMemoryDirty)( void *, SIZE_T );
extern void     (WINAPI *pBTCpuNotifyMemoryFree)( void *, SIZE_T, ULONG, BOOL, NTSTATUS );
extern void     (WINAPI *pBTCpuNotifyMemoryProtect)( void *, SIZE_T, ULONG, BOOL, NTSTATUS );
extern void     (WINAPI *pBTCpuNotifyReadFile)( HANDLE, void *, SIZE_T, BOOL, NTSTATUS );
extern void     (WINAPI *pBTCpuNotifyUnmapViewOfSection)( void *, BOOL, NTSTATUS );
extern void     (WINAPI *pBTCpuUpdateProcessorInformation)( SYSTEM_CPU_INFORMATION * );
extern void     (WINAPI *pBTCpuProcessTerm)( HANDLE, BOOL, NTSTATUS );
extern void     (WINAPI *pBTCpuThreadTerm)( HANDLE, LONG );
extern void     (WINAPI *pBTCpuNotifyProcessExecuteFlagsChange)( ULONG );

/* MYIOSDECK (spec 18 Part J1): the base B of this process's guest window, 0 without one; set by
 * process_init before anything converts a pointer */
extern ULONG_PTR guest_base;

/* MYIOSDECK (spec 18 Part J3): B of the process a handle names (own B for the own process) */
extern ULONG_PTR wow_guest_base_for_process( HANDLE process );

/* MYIOSDECK (spec 18 Part J2): conversions with the own window */
static inline void *guest_to_host( ULONG value )
{
    return (void *)(ULONG_PTR)wow_guest_to_host( guest_base, value );
}

static inline ULONG host_to_guest( const void *ptr )
{
    return wow_host_to_guest( guest_base, (ULONG_PTR)ptr );
}

/* the same with another process's window */
static inline void *guest_to_host_base( ULONG_PTR base, ULONG value )
{
    return (void *)(ULONG_PTR)wow_guest_to_host( base, value );
}

static inline ULONG host_to_guest_base( ULONG_PTR base, const void *ptr )
{
    return wow_host_to_guest( base, (ULONG_PTR)ptr );
}

struct object_attr64
{
    OBJECT_ATTRIBUTES   attr;
    UNICODE_STRING      str;
    SECURITY_DESCRIPTOR sd;
};

/* cf. GetSystemWow64Directory2 */
static inline const WCHAR *get_machine_wow64_dir( USHORT machine )
{
    switch (machine)
    {
    case IMAGE_FILE_MACHINE_TARGET_HOST: return L"\\??\\C:\\windows\\system32";
    case IMAGE_FILE_MACHINE_I386:        return L"\\??\\C:\\windows\\syswow64";
    case IMAGE_FILE_MACHINE_ARMNT:       return L"\\??\\C:\\windows\\sysarm32";
    default: return NULL;
    }
}

static inline TEB32 *NtCurrentTeb32(void)
{
    return (TEB32 *)((char *)NtCurrentTeb() + NtCurrentTeb()->WowTebOffset);
}

static inline ULONG get_ulong( UINT **args ) { return *(*args)++; }
static inline HANDLE get_handle( UINT **args ) { return LongToHandle( *(*args)++ ); }
static inline void *get_ptr( UINT **args ) { return guest_to_host( *(*args)++ ); }
/* MYIOSDECK: a pointer-typed argument that is a guest number (start routine, its parameter, an
 * opaque value), passed on unconverted */
static inline void *get_guest_value( UINT **args ) { return ULongToPtr( *(*args)++ ); }

static inline ULONG64 get_ulong64( UINT **args )
{
    ULONG64 ret;

    *args = (UINT *)(((ULONG_PTR)*args + args_alignment - 1) & ~(args_alignment - 1));
    ret = *(ULONG64 *)*args;
    *args += 2;
    return ret;
}

static inline ULONG_PTR get_zero_bits( ULONG_PTR zero_bits )
{
    return zero_bits ? zero_bits : default_zero_bits;
}

static inline void **addr_32to64( void **addr, ULONG *addr32 )
{
    if (!addr32) return NULL;
    *addr = guest_to_host( *addr32 );
    return addr;
}

/* MYIOSDECK (spec 18 Part J12): an in/out address in another process's window */
static inline void **addr_32to64_base( void **addr, ULONG *addr32, ULONG_PTR base )
{
    if (!addr32) return NULL;
    *addr = guest_to_host_base( base, *addr32 );
    return addr;
}

static inline SIZE_T *size_32to64( SIZE_T *size, ULONG *size32 )
{
    if (!size32) return NULL;
    *size = *size32;
    return size;
}

static inline void *apc_32to64( ULONG func )
{
    return func ? Wow64ApcRoutine : NULL;
}

static inline void *apc_param_32to64( ULONG func, ULONG context )
{
    if (!func) return ULongToPtr( context );
    return (void *)(ULONG_PTR)(((ULONG64)func << 32) | context);
}

static inline IO_STATUS_BLOCK *iosb_32to64( IO_STATUS_BLOCK *io, IO_STATUS_BLOCK32 *io32 )
{
    if (!io32) return NULL;
    io->Pointer = io32;
    return io;
}

static inline UNICODE_STRING *unicode_str_32to64( UNICODE_STRING *str, const UNICODE_STRING32 *str32 )
{
    if (!str32) return NULL;
    str->Length = str32->Length;
    str->MaximumLength = str32->MaximumLength;
    str->Buffer = guest_to_host( str32->Buffer );
    return str;
}

static inline CLIENT_ID *client_id_32to64( CLIENT_ID *id, const CLIENT_ID32 *id32 )
{
    if (!id32) return NULL;
    id->UniqueProcess = LongToHandle( id32->UniqueProcess );
    id->UniqueThread = LongToHandle( id32->UniqueThread );
    return id;
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
        out->Owner = sd->Owner ? (PSID)((BYTE *)sd + sd->Owner) : NULL;
        out->Group = sd->Group ? (PSID)((BYTE *)sd + sd->Group) : NULL;
        out->Sacl = ((sd->Control & SE_SACL_PRESENT) && sd->Sacl) ? (PSID)((BYTE *)sd + sd->Sacl) : NULL;
        out->Dacl = ((sd->Control & SE_DACL_PRESENT) && sd->Dacl) ? (PSID)((BYTE *)sd + sd->Dacl) : NULL;
    }
    else  /* absolute: guest pointers; self-relative offsets above don't convert */
    {
        out->Owner = guest_to_host( sd->Owner );
        out->Group = guest_to_host( sd->Group );
        out->Sacl = (sd->Control & SE_SACL_PRESENT) ? guest_to_host( sd->Sacl ) : NULL;
        out->Dacl = (sd->Control & SE_DACL_PRESENT) ? guest_to_host( sd->Dacl ) : NULL;
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

static inline OBJECT_ATTRIBUTES *objattr_32to64_redirect( struct object_attr64 *out,
                                                          const OBJECT_ATTRIBUTES32 *in )
{
    OBJECT_ATTRIBUTES *attr = objattr_32to64( out, in );

    if (attr) get_file_redirect( attr );
    return attr;
}

static inline TOKEN_USER *token_user_32to64( TOKEN_USER *out, const TOKEN_USER32 *in )
{
    out->User.Sid = guest_to_host( in->User.Sid );
    out->User.Attributes = in->User.Attributes;
    return out;
}

static inline TOKEN_OWNER *token_owner_32to64( TOKEN_OWNER *out, const TOKEN_OWNER32 *in )
{
    out->Owner = guest_to_host( in->Owner );
    return out;
}

static inline TOKEN_PRIMARY_GROUP *token_primary_group_32to64( TOKEN_PRIMARY_GROUP *out, const TOKEN_PRIMARY_GROUP32 *in )
{
    out->PrimaryGroup = guest_to_host( in->PrimaryGroup );
    return out;
}

static inline TOKEN_DEFAULT_DACL *token_default_dacl_32to64( TOKEN_DEFAULT_DACL *out, const TOKEN_DEFAULT_DACL32 *in )
{
    out->DefaultDacl = guest_to_host( in->DefaultDacl );
    return out;
}

static inline void put_handle( ULONG *handle32, HANDLE handle )
{
    *handle32 = HandleToULong( handle );
}

static inline void put_addr( ULONG *addr32, void *addr )
{
    if (addr32) *addr32 = host_to_guest( addr );
}

/* MYIOSDECK (spec 18 Part J12): an address in another process's window back to guest */
static inline void put_addr_base( ULONG *addr32, void *addr, ULONG_PTR base )
{
    if (addr32) *addr32 = host_to_guest_base( base, addr );
}

static inline void put_size( ULONG *size32, SIZE_T size )
{
    if (size32) *size32 = min( size, MAXDWORD );
}

static inline void put_client_id( CLIENT_ID32 *id32, const CLIENT_ID *id )
{
    if (!id32) return;
    id32->UniqueProcess = HandleToLong( id->UniqueProcess );
    id32->UniqueThread = HandleToLong( id->UniqueThread );
}

static inline void put_iosb( IO_STATUS_BLOCK32 *io32, const IO_STATUS_BLOCK *io )
{
    /* sync I/O modifies the 64-bit iosb right away, so in that case we update the 32-bit one */
    /* async I/O leaves the 64-bit one untouched and updates the 32-bit one directly later on */
    if (io32 && io->Pointer != io32)
    {
        io32->Status = io->Status;
        io32->Information = io->Information;
    }
}

extern void put_section_image_info( SECTION_IMAGE_INFORMATION32 *info32,
                                    const SECTION_IMAGE_INFORMATION *info );
extern void put_vm_counters( VM_COUNTERS_EX32 *info32, const VM_COUNTERS_EX *info,
                             ULONG size );

#endif /* __WOW64_PRIVATE_H */
