// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Included at the end of sysparams.c (patches/B-12-sysparams.patch and later Parts), under
// WINE_IOS. It sees sysparams.c's statics; the per-process macros of sysparams_ios.h are in
// effect, so per-process fields are always named pp_* here.

/* ================================================================ Part B */

static HKEY *w32ios_config_key( void )
{
    struct w32ios_proc *proc = w32ios_proc_current();
    HKEY key;

    if (!proc->pp_config_key && w32ios_session_initialized() &&
        (key = reg_create_ascii_key( NULL, config_keyA, 0, NULL )) &&
        InterlockedCompareExchangePointer( (void **)&proc->pp_config_key, key, NULL ))
        NtClose( key );
    return &proc->pp_config_key;
}

static HKEY *w32ios_volatile_base_key( void )
{
    struct w32ios_proc *proc = w32ios_proc_current();
    HKEY wine, key = 0;

    if (proc->pp_volatile_base_key || !w32ios_session_initialized()) return &proc->pp_volatile_base_key;

    /* HKCU\Software\Wine\Temporary System Parameters, as sysparams_init opens it */
    if ((wine = reg_create_ascii_key( hkcu_key, "Software\\Wine", 0, NULL )))
    {
        key = reg_create_ascii_key( wine, "Temporary System Parameters", REG_OPTION_VOLATILE, NULL );
        NtClose( wine );
    }
    if (key && InterlockedCompareExchangePointer( (void **)&proc->pp_volatile_base_key, key, NULL ))
        NtClose( key );
    return &proc->pp_volatile_base_key;
}

/* ================================================================ Part C: the virtual monitor */

/* No driver enumerates anything: gpus, sources and monitors stay empty, and the monitor list
 * holds upstream's virtual monitor (the path upstream uses for services), sized from
 * w32ios_display.c. */

static __thread int display_lock_depth;   /* the calling thread holds display_lock */
static struct w32ios_gpu registry_gpu;    /* C6, written under display_lock */
static LUID software_luid;                /* C7, when no registry GPU could be written */

static RECT w32ios_virtual_monitor_rect( void )
{
    struct w32ios_size size = w32ios_display_current();
    RECT rect;

    SetRect( &rect, 0, 0, size.w, size.h );
    return rect;
}

static void w32ios_display_lock_released( void )
{
    if (display_lock_depth) display_lock_depth--;
}

/* ---------------------------------------------------------------- C6: the registry GPU */

static void w32ios_delete_ascii_tree( HKEY root, const char *name )
{
    WCHAR nameW[MAX_PATH];
    UINT size;

    if (!root) return;
    size = asciiz_to_unicode( nameW, name ) - sizeof(WCHAR);
    reg_delete_tree( root, nameW, size );
}

/* The prefix is shared and saved: remove the adapter of the identity this launch doesn't use,
 * so a stale one can't sort first. */
static void w32ios_remove_registry_gpu( UINT16 vendor, UINT16 device )
{
    static const char *const interfaces[] = { guid_devinterface_display_adapterA, guid_display_device_arrivalA };
    char device_path[128], buffer[MAX_PATH], value_buffer[512], guid[39] = {0}, *ptr;
    KEY_VALUE_PARTIAL_INFORMATION *value = (void *)value_buffer;
    HKEY hkey, subkey;
    unsigned int i, pos;

    snprintf( device_path, sizeof(device_path), "PCI\\VEN_%04X&DEV_%04X&SUBSYS_00000000&REV_00", vendor, device );
    snprintf( buffer, sizeof(buffer), "%s\\00000000", device_path );
    if (!(hkey = reg_open_ascii_key( enum_key, buffer ))) return;
    if ((subkey = reg_open_ascii_key( hkey, "Device Parameters" )))
    {
        if (query_reg_ascii_value( subkey, "VideoID", value, sizeof(value_buffer) ) == sizeof(guid) * sizeof(WCHAR))
            for (i = 0; i < sizeof(guid); i++) guid[i] = ((const WCHAR *)value->Data)[i];
        NtClose( subkey );
    }
    NtClose( hkey );

    w32ios_delete_ascii_tree( enum_key, device_path );
    for (i = 0; i < ARRAY_SIZE(interfaces); i++)
    {
        pos = snprintf( buffer, sizeof(buffer), "DeviceClasses\\%s\\", interfaces[i] );
        snprintf( buffer + pos, sizeof(buffer) - pos, "##?#%s\\00000000#%s", device_path, interfaces[i] );
        for (ptr = buffer + pos; *ptr; ptr++) if (*ptr == '\\') *ptr = '#';
        w32ios_delete_ascii_tree( control_key, buffer );
    }
    if (guid[0])
    {
        snprintf( buffer, sizeof(buffer), "Video\\%s", guid );
        w32ios_delete_ascii_tree( control_key, buffer );
        if ((hkey = reg_open_ascii_key( NULL, directx_keyA )))
        {
            w32ios_delete_ascii_tree( hkey, guid );
            NtClose( hkey );
        }
    }
    w32ios_log( "removed the registry adapter %04X:%04X (not this launch's identity)", vendor, device );
}

/* display_lock held. One display adapter through upstream's writer (write_gpu_to_registry),
 * not added to the display topology (no source, no monitor, not in the gpus list). */
static void w32ios_write_registry_gpu( void )
{
    char value_buffer[512], buffer[MAX_PATH];
    KEY_VALUE_PARTIAL_INFORMATION *value = (void *)value_buffer;
    WCHAR bufferW[MAX_PATH];
    struct pci_id pci = {0};
    struct gpu gpu;
    const char *name;
    UINT16 vendor, device;
    ULONGLONG memory;
    HKEY hkey, subkey;
    DWORD len;
    unsigned int i;

    w32ios_gpu_identity( &vendor, &device, &name );
    if (!enum_key && !(enum_key = reg_create_ascii_key( NULL, enum_keyA, 0, NULL ))) return;
    if (!control_key && !(control_key = reg_create_ascii_key( NULL, control_keyA, 0, NULL ))) return;

    if (vendor == 0x10de) w32ios_remove_registry_gpu( 0x106b, 0x0001 );
    else w32ios_remove_registry_gpu( 0x10de, 0x2544 );

    memset( &gpu, 0, sizeof(gpu) );
    gpu.refcount = 1;
    pci.vendor = vendor;
    pci.device = device;
    RtlUTF8ToUnicodeN( gpu.name, sizeof(gpu.name) - sizeof(WCHAR), &len, name, strlen( name ) );
    snprintf( gpu.path, sizeof(gpu.path), "PCI\\VEN_%04X&DEV_%04X&SUBSYS_%08X&REV_%02X\\%08X",
              vendor, device, 0, 0, gpu.index );
    if (!(hkey = reg_create_ascii_key( enum_key, gpu.path, 0, NULL ))) return;

    /* keep the GUID and LUID of an earlier launch, as upstream's add_gpu does */
    if ((subkey = reg_create_ascii_key( hkey, "Device Parameters", 0, NULL )))
    {
        if (query_reg_ascii_value( subkey, "VideoID", value, sizeof(value_buffer) ) == sizeof(gpu.guid) * sizeof(WCHAR))
            for (i = 0; i < sizeof(gpu.guid); i++) gpu.guid[i] = ((const WCHAR *)value->Data)[i];
        NtClose( subkey );
    }
    if (!gpu.guid[0])
    {
        GUID guid;
        uuid_create( &guid );
        snprintf( gpu.guid, sizeof(gpu.guid), "{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}",
                  (unsigned int)guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2],
                  guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7] );
    }
    if (!w32ios_kmt_luid( &gpu.luid ))   /* Part D gives Metal's LUID */
    {
        if ((subkey = reg_open_ascii_key( hkey, devpropkey_gpu_luidA )))
        {
            if (query_reg_value( subkey, NULL, value, sizeof(value_buffer) ) == sizeof(LUID))
                memcpy( &gpu.luid, value->Data, sizeof(LUID) );
            NtClose( subkey );
        }
        if (!gpu.luid.LowPart && !gpu.luid.HighPart) NtAllocateLocallyUniqueId( &gpu.luid );
    }
    NtClose( hkey );

    memory = w32ios_kmt_memory();
    if (!write_gpu_to_registry( &gpu, &pci, memory ))
    {
        w32ios_log( "cannot write the registry GPU %04X:%04X", vendor, device );
        return;
    }

    /* the device key EnumDisplayDevices returns, a link to the Class key so it holds the same
     * values (as upstream's write_source_to_registry makes it) */
    snprintf( buffer, sizeof(buffer), "%s\\Video\\%s\\%04x", control_keyA, gpu.guid, 0 );
    hkey = reg_create_ascii_key( NULL, buffer, REG_OPTION_VOLATILE | REG_OPTION_CREATE_LINK, NULL );
    if (!hkey) hkey = reg_create_ascii_key( NULL, buffer, REG_OPTION_VOLATILE | REG_OPTION_OPEN_LINK, NULL );
    if (hkey)
    {
        snprintf( buffer, sizeof(buffer), "%s\\Class\\%s\\%04X", control_keyA, guid_devclass_displayA, gpu.index );
        len = asciiz_to_unicode( bufferW, buffer ) - sizeof(WCHAR);
        set_reg_value( hkey, symbolic_link_valueW, REG_LINK, bufferW, len );
        NtClose( hkey );
    }

    if (!registry_gpu.valid)
        w32ios_log( "registry GPU %04X:%04X \"%s\" %s, LUID %08x:%08x, %u MB, driver %s", vendor, device, name,
                    gpu.guid, (unsigned int)gpu.luid.HighPart, (unsigned int)gpu.luid.LowPart,
                    (unsigned int)(memory >> 20), w32ios_driver_version( vendor ) );
    registry_gpu.vendor = vendor;
    registry_gpu.device = device;
    snprintf( registry_gpu.name, sizeof(registry_gpu.name), "%s", name );
    snprintf( registry_gpu.path, sizeof(registry_gpu.path), "%s", gpu.path );
    memcpy( registry_gpu.guid, gpu.guid, sizeof(registry_gpu.guid) );
    registry_gpu.luid = gpu.luid;
    registry_gpu.memory = memory;
    registry_gpu.valid = TRUE;
}

/* ---------------------------------------------------------------- C1/C2: the lock path */

static LONG monitors_pending;   /* clarification 10: the server hasn't got a new program's size */

void w32ios_monitor_resized( void )
{
    spi_loaded[SPI_SETWORKAREA_IDX] = FALSE;   /* recomputed from the monitor, as after a mode change */
    InterlockedExchange( &monitors_pending, 1 );
}

static BOOL w32ios_lock_display_devices( BOOL force )
{
    struct w32ios_proc *proc = w32ios_proc_current();
    struct w32ios_size size;
    UINT64 serial;

    pthread_mutex_lock( &display_lock );
    display_lock_depth++;

    if (list_empty( &monitors )) list_add_tail( &monitors, &virtual_monitor.entry );
    size = w32ios_display_current();
    SetRect( &virtual_monitor.rc_work, 0, 0, size.w, size.h );

    /* clarification 10: a new program's size goes to the server on the first display query that
     * can reach it (a thread connected to its desktop), before that query is answered */
    if (ReadNoFence( &monitors_pending ) && NtUserGetThreadInfo()->top_window &&
        InterlockedExchange( &monitors_pending, 0 ))
        force = TRUE;

    if (!proc->gpu_written)   /* C6: once per process */
    {
        proc->gpu_written = TRUE;
        w32ios_write_registry_gpu();
    }

    /* as upstream's virtual-monitor path: tell the server when forced or when its serial moved */
    serial = get_monitor_update_serial();
    if (force || monitor_update_serial < serial) set_winstation_monitors( TRUE );
    return TRUE;
}

/* C5: the session default goes to the server once, the first time a thread that already knows
 * its desktop window asks for SM_CXSCREEN with no lock held */
static void w32ios_screen_metric_queried( void )
{
    if (!NtUserGetThreadInfo()->top_window) return;
    if (user_lock_thread == GetCurrentThreadId() || display_lock_depth) return;
    if (!w32ios_display_take_report()) return;
    w32ios_log( "reporting the virtual monitor to the server" );
    update_display_cache( TRUE );
}

/* ---------------------------------------------------------------- C3: devices */

static int w32ios_device_of( const UNICODE_STRING *name )
{
    if (!name || !name->Length) return W32IOS_DEV_ADAPTER;
    return w32ios_match_device( name->Buffer, name->Length / sizeof(WCHAR) );
}

static RECT w32ios_get_display_rect( const WCHAR *display )
{
    UNICODE_STRING name;
    RECT rect = {0};

    RtlInitUnicodeString( &name, display );
    if (!name.Length || w32ios_device_of( &name ) != W32IOS_DEV_ADAPTER) return rect;
    return w32ios_virtual_monitor_rect();
}

static struct w32ios_gpu w32ios_get_registry_gpu( void )
{
    struct w32ios_gpu gpu;

    lock_display_devices( FALSE );   /* writes it for this process (C6) */
    gpu = registry_gpu;
    unlock_display_devices();
    if (!gpu.valid)
    {
        const char *name;
        w32ios_gpu_identity( &gpu.vendor, &gpu.device, &name );
        snprintf( gpu.name, sizeof(gpu.name), "%s", name );
        snprintf( gpu.path, sizeof(gpu.path), "PCI\\VEN_%04X&DEV_%04X&SUBSYS_00000000&REV_00\\00000000",
                  gpu.vendor, gpu.device );
        strcpy( gpu.guid, "{00000000-0000-0000-0000-000000000000}" );
    }
    return gpu;
}

static NTSTATUS w32ios_enum_display_devices( UNICODE_STRING *device, DWORD index, DISPLAY_DEVICEW *info,
                                             DWORD flags )
{
    struct w32ios_gpu gpu;
    char buffer[MAX_PATH], *tmp;
    BOOL monitor;

    TRACE( "%s %u %p %#x\n", debugstr_us( device ), index, info, flags );

    if (!info || !info->cb) return STATUS_UNSUCCESSFUL;
    if (!device || !device->Length) monitor = FALSE;                         /* the adapter */
    else if (w32ios_device_of( device ) == W32IOS_DEV_ADAPTER) monitor = TRUE;  /* its monitor */
    else return STATUS_UNSUCCESSFUL;
    if (index) return STATUS_UNSUCCESSFUL;

    gpu = w32ios_get_registry_gpu();

    if (info->cb >= offsetof(DISPLAY_DEVICEW, DeviceName) + sizeof(info->DeviceName))
        asciiz_to_unicode( info->DeviceName, monitor ? "\\\\.\\DISPLAY1\\Monitor0" : "\\\\.\\DISPLAY1" );
    if (info->cb >= offsetof(DISPLAY_DEVICEW, DeviceString) + sizeof(info->DeviceString))
    {
        if (monitor) asciiz_to_unicode( info->DeviceString, "Generic Non-PnP Monitor" );
        else
        {
            DWORD len;
            RtlUTF8ToUnicodeN( info->DeviceString, sizeof(info->DeviceString) - sizeof(WCHAR), &len,
                               gpu.name, strlen( gpu.name ) );
            info->DeviceString[len / sizeof(WCHAR)] = 0;
        }
    }
    if (info->cb >= offsetof(DISPLAY_DEVICEW, StateFlags) + sizeof(info->StateFlags))
    {
        if (monitor) info->StateFlags = DISPLAY_DEVICE_ATTACHED | DISPLAY_DEVICE_ACTIVE;
        else info->StateFlags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE |
                                DISPLAY_DEVICE_VGA_COMPATIBLE;
    }
    if (info->cb >= offsetof(DISPLAY_DEVICEW, DeviceID) + sizeof(info->DeviceID))
    {
        if (monitor && (flags & EDD_GET_DEVICE_INTERFACE_NAME))
        {
            snprintf( buffer, sizeof(buffer), "\\\\?\\DISPLAY\\Default_Monitor\\%04X&%04X#%s", 0, 0,
                      guid_devinterface_monitorA );
            for (tmp = buffer + 4; *tmp; tmp++) if (*tmp == '\\') *tmp = '#';
        }
        else if (monitor)
            snprintf( buffer, sizeof(buffer), "MONITOR\\Default_Monitor\\%s\\%04X", guid_devclass_monitorA, 0 );
        else if (flags & EDD_GET_DEVICE_INTERFACE_NAME)
            buffer[0] = 0;
        else
        {
            snprintf( buffer, sizeof(buffer), "%s", gpu.path );
            if ((tmp = strrchr( buffer, '\\' ))) *tmp = 0;
        }
        asciiz_to_unicode( info->DeviceID, buffer );
    }
    if (info->cb >= offsetof(DISPLAY_DEVICEW, DeviceKey) + sizeof(info->DeviceKey))
    {
        if (monitor) snprintf( buffer, sizeof(buffer), "%s\\Class\\%s\\%04X", control_keyA, guid_devclass_monitorA, 0 );
        else snprintf( buffer, sizeof(buffer), "%s\\Video\\%s\\%04x", control_keyA, gpu.guid, 0 );
        asciiz_to_unicode( info->DeviceKey, buffer );
    }
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- C4/C5: modes */

static BOOL w32ios_enum_display_settings( UNICODE_STRING *device, DWORD index, DEVMODEW *devmode,
                                          DWORD flags )
{
    static const WCHAR driverW[] = {'W','i','n','e',' ','D','i','s','p','l','a','y',' ','D','r','i','v','e','r',0};
    struct w32ios_size modes[W32IOS_STD_MODE_COUNT + 1], size;
    int count;

    TRACE( "device %s, index %#x, devmode %p, flags %#x\n", debugstr_us(device), index, devmode, flags );

    if (w32ios_device_of( device ) == W32IOS_DEV_NONE) return FALSE;

    lstrcpynW( devmode->dmDeviceName, driverW, ARRAY_SIZE(devmode->dmDeviceName) );
    devmode->dmSpecVersion = DM_SPECVERSION;
    devmode->dmDriverVersion = DM_SPECVERSION;
    devmode->dmSize = offsetof(DEVMODEW, dmICMMethod);
    devmode->dmDriverExtra = 0;

    if (index == ENUM_CURRENT_SETTINGS) size = w32ios_display_current();
    else if (index == ENUM_REGISTRY_SETTINGS) size = w32ios_display_default();
    else if (index == WINE_ENUM_PHYSICAL_SETTINGS) return FALSE;
    else
    {
        count = w32ios_build_modes( w32ios_display_current(), w32ios_display_default(),
                                    w32ios_display_extended_modes(), modes, ARRAY_SIZE(modes) );
        if (index >= (DWORD)count)
        {
            RtlSetLastWin32Error( ERROR_NO_MORE_FILES );
            return FALSE;
        }
        size = modes[index];
    }
    w32ios_display_fill_devmode( devmode, size );
    return TRUE;
}

/* after a change, with no display lock held */
static void w32ios_display_changed( struct w32ios_size size )
{
    LPARAM lparam = MAKELPARAM( size.w, size.h );

    update_display_cache( TRUE );                    /* set_winstation_monitors */
    spi_loaded[SPI_SETWORKAREA_IDX] = FALSE;         /* recomputed from the monitor */
    w32ios_app_display_mode_changed( size.w, size.h );

    NtUserClipCursor( NULL );
    send_notify_message( get_desktop_window(), WM_DISPLAYCHANGE, 32, lparam, FALSE );
    send_message_timeout( HWND_BROADCAST, WM_DISPLAYCHANGE, 32, lparam, SMTO_ABORTIFHUNG, 2000, FALSE );
    NtUserPostMessage( NtUserGetForegroundWindow(), WM_WINE_CLIPCURSOR, SET_CURSOR_FSCLIP, 0 );
}

static LONG w32ios_change_display_settings( UNICODE_STRING *devname, DEVMODEW *devmode, HWND hwnd,
                                            DWORD flags, void *lparam )
{
    static const char *const names[] = { "restore", "accept", "bad mode" };
    struct w32ios_size cur = w32ios_display_current(), def = w32ios_display_default(), want;
    DWORD fields = devmode ? devmode->dmFields : 0;
    enum w32ios_cds decision;

    if (w32ios_device_of( devname ) == W32IOS_DEV_NONE) return DISP_CHANGE_BADPARAM;

    decision = w32ios_mode_decision( devmode != NULL, !!(fields & DM_PELSWIDTH), !!(fields & DM_PELSHEIGHT),
                                     devmode ? (int)devmode->dmPelsWidth : 0,
                                     devmode ? (int)devmode->dmPelsHeight : 0,
                                     cur, def, w32ios_display_extended_modes(), &want );
    w32ios_log( "ChangeDisplaySettings %dx%d (fields %#x, flags %#x): %s", want.w, want.h,
                (unsigned int)fields, (unsigned int)flags, names[decision] );

    if (decision == W32IOS_CDS_BADMODE) return DISP_CHANGE_BADMODE;
    if (flags & CDS_TEST) return DISP_CHANGE_SUCCESSFUL;
    if (!w32ios_display_mode_set_enabled()) return DISP_CHANGE_SUCCESSFUL;   /* valid, size kept */
    if (want.w == cur.w && want.h == cur.h) return DISP_CHANGE_SUCCESSFUL;

    w32ios_display_set_current( want );
    w32ios_display_changed( want );
    return DISP_CHANGE_SUCCESSFUL;
}

/* ---------------------------------------------------------------- C7: opening the adapter */

static NTSTATUS w32ios_open_adapter_from_gdi_name( D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME *desc )
{
    D3DKMT_OPENADAPTERFROMLUID luid_desc = {0};
    UNICODE_STRING name;
    NTSTATUS status;

    RtlInitUnicodeString( &name, desc->DeviceName );
    if (w32ios_device_of( &name ) == W32IOS_DEV_NONE) return STATUS_UNSUCCESSFUL;

    lock_display_devices( FALSE );
    if (registry_gpu.valid) luid_desc.AdapterLuid = registry_gpu.luid;
    else
    {
        if (!software_luid.LowPart && !software_luid.HighPart)
        {
            NtAllocateLocallyUniqueId( &software_luid );
            w32ios_log( "no registry GPU: adapter LUID %08x:%08x is a software identity",
                        (unsigned int)software_luid.HighPart, (unsigned int)software_luid.LowPart );
        }
        luid_desc.AdapterLuid = software_luid;
    }
    unlock_display_devices();

    if ((status = NtGdiDdDDIOpenAdapterFromLuid( &luid_desc ))) return status;
    desc->hAdapter = luid_desc.hAdapter;
    desc->AdapterLuid = luid_desc.AdapterLuid;
    desc->VidPnSourceId = 1;
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- C8: non-client metrics */

static void w32ios_fix_nonclient_metrics( NONCLIENTMETRICSW *ncm )
{
    static LONG logged;

    if (ncm->iCaptionHeight > 0 && ncm->iCaptionHeight <= 200) return;
    if (!InterlockedExchange( &logged, 1 ))
        w32ios_log( "unusable non-client metrics (caption height %d): using Windows' defaults", ncm->iCaptionHeight );
    ncm->iBorderWidth = 1;
    ncm->iScrollWidth = 17;
    ncm->iScrollHeight = 17;
    ncm->iCaptionWidth = 22;
    ncm->iCaptionHeight = 22;
    ncm->iSmCaptionWidth = 22;
    ncm->iSmCaptionHeight = 16;
    ncm->iMenuWidth = 18;
    ncm->iMenuHeight = 19;
    if (ncm->cbSize == sizeof(NONCLIENTMETRICSW)) ncm->iPaddedBorderWidth = 4;
}
