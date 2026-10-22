// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 09 Part F: devices and machine identity.
//   F1: the prefix's dosdevices links for C: and Z: and their NT symbolic links.
//   F2: the hardware identity games read in the registry and in the SMBIOS table, one fixed
//       desktop described by the table below.

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "ntstatus.h"
#include "windef.h"
#include "winnt.h"
#include "winternl.h"
#include "unix_private.h"
#include "ios_pure.h"
#include "ios_hwident.h"

#ifdef WINE_IOS

/* spec 15's ntdll part (weak: skipped when it isn't linked) */
extern void ios_hidpad_publish(void) __attribute__((weak_import));

#ifndef SYMBOLIC_LINK_ALL_ACCESS  /* ddk/wdm.h */
#define SYMBOLIC_LINK_ALL_ACCESS (STANDARD_RIGHTS_REQUIRED | 0x1)
#endif

/* ---------- the identity: one plausible desktop, used everywhere ---------- */

const struct ios_hw_identity ios_hw_identity =
{
    .cpu_vendor        = "AuthenticAMD",
    .cpu_name          = "AMD Ryzen 7 5800X 8-Core Processor",
    .cpu_identifier    = "AMD64 Family 25 Model 33 Stepping 0",
    .cpu_smbios_family = 0x6b,   /* SMBIOS: AMD Zen processor family */

    .bios_vendor       = "American Megatrends International, LLC.",
    .bios_version      = "P3.40",
    .bios_date         = "06/22/2022",
    .bios_system_version = "ALASKA - 1072009",

    .sys_vendor        = "To Be Filled By O.E.M.",
    .sys_product       = "To Be Filled By O.E.M.",
    .sys_version       = "To Be Filled By O.E.M.",
    .sys_serial        = "To Be Filled By O.E.M.",
    .sys_sku           = "To Be Filled By O.E.M.",
    .sys_family        = "To Be Filled By O.E.M.",
    .sys_uuid          = { 0x3f, 0x8a, 0x12, 0x6c, 0x4e, 0x97, 0x4d, 0x21,
                           0x9b, 0x05, 0x7a, 0xc4, 0x61, 0xd8, 0x2e, 0x53 },

    .board_vendor      = "ASRock",
    .board_product     = "B550 Phantom Gaming 4",
    .board_version     = "",
    .board_serial      = "M80-E7016302714",
    .chassis_vendor    = "To Be Filled By O.E.M.",
    .chassis_type      = 3,      /* desktop */
    .chassis_serial    = "To Be Filled By O.E.M.",
};


/***********************************************************************
 *           ios_hw_cpu_mhz
 *
 * The clock to report: the counter frequency CNTFRQ_EL0 adjusted to a plausible desktop clock.
 */
unsigned int ios_hw_cpu_mhz(void)
{
    uint64_t freq = 0;

#ifdef __aarch64__
    __asm__ __volatile__( "mrs %0, cntfrq_el0" : "=r" (freq) );
#endif
    return ios_plausible_mhz( freq );
}


/* ---------- registry helpers ---------- */

/* opens or creates one level below root (root 0: name is a full NT path) */
static HANDLE create_key_level( HANDLE root, const char *name, ULONG options )
{
    WCHAR nameW[256];
    UNICODE_STRING str;
    OBJECT_ATTRIBUTES attr;
    HANDLE key = 0;
    size_t len = strlen( name );

    if (len >= ARRAY_SIZE(nameW)) return 0;
    ascii_to_unicode( nameW, name, len + 1 );
    init_unicode_string( &str, nameW );
    InitializeObjectAttributes( &attr, &str, OBJ_CASE_INSENSITIVE, root, NULL );
    if (NtCreateKey( &key, KEY_ALL_ACCESS, &attr, 0, NULL, options, NULL )) return 0;
    return key;
}

/* \Registry\Machine\<path>, creating the missing levels one by one */
static HANDLE create_machine_key( const char *path )
{
    char part[128];
    const char *p = path, *end;
    HANDLE key, root = create_key_level( 0, "\\Registry\\Machine", 0 );

    while (root && *p)
    {
        if (!(end = strchr( p, '\\' ))) end = p + strlen( p );
        if (end - p >= sizeof(part)) break;
        memcpy( part, p, end - p );
        part[end - p] = 0;
        /* HARDWARE is volatile on Windows: rebuilt at every boot */
        key = create_key_level( root, part, REG_OPTION_VOLATILE );
        NtClose( root );
        root = key;
        p = *end ? end + 1 : end;
    }
    return root;
}

static void set_value( HANDLE key, const char *name, ULONG type, const void *data, ULONG size )
{
    WCHAR nameW[64];
    UNICODE_STRING str;
    size_t len = strlen( name );

    if (len >= ARRAY_SIZE(nameW)) return;
    ascii_to_unicode( nameW, name, len + 1 );
    init_unicode_string( &str, nameW );
    NtSetValueKey( key, &str, 0, type, data, size );
}

static void set_sz( HANDLE key, const char *name, const char *value )
{
    WCHAR data[128];
    size_t len = strlen( value );

    if (len >= ARRAY_SIZE(data)) len = ARRAY_SIZE(data) - 1;
    ascii_to_unicode( data, value, len );
    data[len] = 0;
    set_value( key, name, REG_SZ, data, (len + 1) * sizeof(WCHAR) );
}

static void set_multi_sz( HANDLE key, const char *name, const char *value )
{
    WCHAR data[128];
    size_t len = strlen( value );

    if (len >= ARRAY_SIZE(data) - 1) len = ARRAY_SIZE(data) - 2;
    ascii_to_unicode( data, value, len );
    data[len] = data[len + 1] = 0;
    set_value( key, name, REG_MULTI_SZ, data, (len + 2) * sizeof(WCHAR) );
}

static void set_dword( HANDLE key, const char *name, DWORD value )
{
    set_value( key, name, REG_DWORD, &value, sizeof(value) );
}


/***********************************************************************
 *           ios_write_hardware_registry
 *
 * Part F2: HARDWARE\DESCRIPTION\System (BIOS strings) and ...\CentralProcessor\N for each core,
 * as a desktop has them. Called by the session's first program after wineboot has run, so these
 * values are the ones that stay.
 */
void ios_write_hardware_registry(void)
{
    const struct ios_hw_identity *id = &ios_hw_identity;
    unsigned int i, mhz = ios_hw_cpu_mhz();
    unsigned int count = ios_cur_peb()->NumberOfProcessors;
    char path[128];
    HANDLE key;

    if (!count) count = sysconf( _SC_NPROCESSORS_ONLN ) > 0 ? sysconf( _SC_NPROCESSORS_ONLN ) : 1;

    if ((key = create_machine_key( "HARDWARE\\DESCRIPTION\\System" )))
    {
        set_sz( key, "Identifier", "AT/AT COMPATIBLE" );
        set_multi_sz( key, "SystemBiosVersion", id->bios_system_version );
        set_sz( key, "SystemBiosDate", id->bios_date );
        NtClose( key );
    }

    if ((key = create_machine_key( "HARDWARE\\DESCRIPTION\\System\\BIOS" )))
    {
        set_sz( key, "BIOSVendor", id->bios_vendor );
        set_sz( key, "BIOSVersion", id->bios_version );
        set_sz( key, "BIOSReleaseDate", id->bios_date );
        set_sz( key, "SystemManufacturer", id->sys_vendor );
        set_sz( key, "SystemProductName", id->sys_product );
        set_sz( key, "SystemFamily", id->sys_family );
        set_sz( key, "SystemVersion", id->sys_version );
        set_sz( key, "SystemSKU", id->sys_sku );
        set_sz( key, "BaseBoardManufacturer", id->board_vendor );
        set_sz( key, "BaseBoardProduct", id->board_product );
        set_sz( key, "BaseBoardVersion", id->board_version );
        NtClose( key );
    }

    for (i = 0; i < count; i++)
    {
        snprintf( path, sizeof(path), "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\%u", i );
        if (!(key = create_machine_key( path ))) continue;
        set_sz( key, "Identifier", id->cpu_identifier );
        set_sz( key, "VendorIdentifier", id->cpu_vendor );
        set_sz( key, "ProcessorNameString", id->cpu_name );
        set_dword( key, "~MHz", mhz );
        NtClose( key );
    }
    if (!ios_quiet) ios_log( "hardware identity: %s, %u cores at %u MHz\n", id->cpu_name, count, mhz );
}


/* ---------- Part F1 ---------- */

/* the NT symbolic link \DosDevices\<drive> -> target, permanent (it outlives the handle and the
 * program that made it); an existing one is kept */
static void create_drive_link( const char *drive, const char *target )
{
    WCHAR nameW[32], targetW[64];
    UNICODE_STRING name, dest;
    OBJECT_ATTRIBUTES attr;
    HANDLE handle;
    NTSTATUS status;
    char buf[32];

    snprintf( buf, sizeof(buf), "\\DosDevices\\%s", drive );
    ascii_to_unicode( nameW, buf, strlen( buf ) + 1 );
    ascii_to_unicode( targetW, target, strlen( target ) + 1 );
    init_unicode_string( &name, nameW );
    init_unicode_string( &dest, targetW );
    InitializeObjectAttributes( &attr, &name, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT, 0, NULL );
    status = NtCreateSymbolicLinkObject( &handle, SYMBOLIC_LINK_ALL_ACCESS, &attr, &dest );
    if (!status) NtClose( handle );
    else if (status != STATUS_OBJECT_NAME_COLLISION && status != STATUS_OBJECT_NAME_EXISTS)
        ios_log( "can't create %s: %x\n", buf, (int)status );
}

/* a dosdevices link in the prefix, made only when missing */
static void create_unix_link( const char *dir, const char *name, const char *target )
{
    char *path = NULL;
    struct stat st;

    if (asprintf( &path, "%s/%s", dir, name ) == -1) return;
    if (lstat( path, &st ) == -1 && symlink( target, path ) == -1)
        ios_log( "can't create %s: %s\n", path, strerror( errno ));
    free( path );
}

/***********************************************************************
 *           ios_create_dos_devices
 *
 * Part F1, at the session's first boot: the prefix's dosdevices links for C: (its drive_c) and
 * Z: (/), and the NT symbolic links \DosDevices\C: and \DosDevices\Z:. The NT names have
 * upper-case drive letters: GetLogicalDrives derives the drive bit from them. There is no mount
 * manager service. Absolute paths only: the working directory is never changed.
 */
void ios_create_dos_devices(void)
{
    char *dosdevices = NULL, *drive_c = NULL;

    if (asprintf( &dosdevices, "%s/dosdevices", config_dir ) == -1) return;
    if (asprintf( &drive_c, "%s/drive_c", config_dir ) == -1)
    {
        free( dosdevices );
        return;
    }
    if (mkdir( dosdevices, 0777 ) == -1 && errno != EEXIST)
        ios_log( "can't create %s: %s\n", dosdevices, strerror( errno ));
    if (mkdir( drive_c, 0777 ) == -1 && errno != EEXIST)
        ios_log( "can't create %s: %s\n", drive_c, strerror( errno ));
    create_unix_link( dosdevices, "c:", "../drive_c" );
    create_unix_link( dosdevices, "z:", "/" );
    free( dosdevices );
    free( drive_c );

    create_drive_link( "C:", "\\Device\\HarddiskVolume1" );
    create_drive_link( "Z:", "\\Device\\HarddiskVolume2" );

    /* spec 09 clarification 1: the HID pad's registry entries (spec 15), when linked */
    if (ios_hidpad_publish) ios_hidpad_publish();
}

#endif /* WINE_IOS */
