// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Included at the end of class.c (patches/B-02-class.patch), under WINE_IOS. Spec 12 Part B.
//
// B2: window procedure handles stay session-wide (classes and windows store them), but the
// first NTUSER_NB_PROCS handles are user32's built-in procedures, which live at a different
// address in every process (and an x64 child's user32 isn't the session's ARM64 one). Turning
// one of those handles into a procedure, and matching a procedure against the built-ins, use
// the table of the calling thread's process; the global winproc_array keeps the first process's
// as the fallback for a process that never loads user32.
// B3: built-in classes once per pseudo-process, under a lock of their own.
// B7: procedure handles that a WoW64 thunk converted as guest addresses.

static w32ios_proc_pair *w32ios_builtin_table( void )
{
    return w32ios_procs_table( &w32ios_proc_current()->client_procs, (w32ios_proc_pair *)winproc_array );
}

static HINSTANCE w32ios_caller_user32_module( void )
{
    struct w32ios_proc *proc = w32ios_proc_current();

    if (__atomic_load_n( &proc->client_procs.set, __ATOMIC_ACQUIRE )) return proc->user32;
    return *w32ios_global_user32_module;
}

static WINDOWPROC *w32ios_find_winproc( WNDPROC func, BOOL ansi )
{
    WINDOWPROC *table = (WINDOWPROC *)w32ios_builtin_table();
    int index = w32ios_procs_match( table, NTUSER_NB_PROCS, func );
    UINT i;

    if (index >= 0) return &table[index];
    for (i = NTUSER_NB_PROCS; i < winproc_used; i++)
    {
        if (ansi ? winproc_array[i].procA == func : winproc_array[i].procW == func)
            return &winproc_array[i];
    }
    return NULL;
}

static BOOL w32ios_winproc_handles( void )
{
    static int enabled = -1;
    if (enabled < 0) enabled = w32ios_env_bool( "PG_WINPROC_HANDLE", TRUE );
    return enabled;
}

static WINDOWPROC *w32ios_get_winproc_ptr( WNDPROC handle )
{
    ULONG_PTR value = w32ios_winproc_handle( (ULONG_PTR)handle, ios_wow_base(), w32ios_winproc_handles() );
    UINT index = LOWORD( value );

    if (value >> 16 != WINPROC_HANDLE) return NULL;
    if (index >= MAX_WINPROCS) return WINPROC_PROC16;
    if (index >= winproc_used) return NULL;
    if (index < NTUSER_NB_PROCS) return &((WINDOWPROC *)w32ios_builtin_table())[index];
    return &winproc_array[index];
}

static WNDPROC w32ios_proc_to_handle( WINDOWPROC *proc )
{
    int index = w32ios_procs_index( w32ios_proc_current()->client_procs.procs, NTUSER_NB_PROCS, proc );

    if (index < 0) index = proc - winproc_array;
    return (WNDPROC)(ULONG_PTR)(index | (WINPROC_HANDLE << 16));
}

/* init_user for the session's first process, then remember that process (B4) */
static void w32ios_init_user( void )
{
    init_user();
    w32ios_set_session_process( ios_current_peb() );
}

static void w32ios_initialize_client_procs( const ntuser_client_func_ptr *client_procsA,
                                            const ntuser_client_func_ptr *client_procsW,
                                            HINSTANCE user_module, pthread_once_t *once )
{
    static LONG global_done;
    struct w32ios_proc *proc = w32ios_proc_current();
    UINT i;

    for (i = 0; i < NTUSER_NB_PROCS; i++)
    {
        proc->client_procs.procs[i][0] = client_procsA[i][0];
        proc->client_procs.procs[i][1] = client_procsW[i][0];
    }
    proc->user32 = user_module;
    __atomic_store_n( &proc->client_procs.set, 1, __ATOMIC_RELEASE );

    if (!InterlockedExchange( &global_done, 1 ))
    {
        for (i = 0; i < NTUSER_NB_PROCS; i++)
        {
            winproc_array[i].procA = client_procsA[i][0];
            winproc_array[i].procW = client_procsW[i][0];
        }
        *w32ios_global_user32_module = user_module;
    }
    w32ios_log_verbose( "process %04x: user32 at %p", (unsigned int)GetCurrentProcessId(), user_module );

    /* B6: a 32-bit process's ntdll wrote some PEB64 pointers as 32-bit values */
    ios_wow_fixup_peb64_ptrs();
    pthread_once( once, w32ios_init_user );
}

static void w32ios_register_builtin_classes( void )
{
    struct w32ios_proc *proc = w32ios_proc_current();
    LONG self = GetCurrentThreadId();

    if (w32ios_builtin_action( ReadAcquire( &proc->builtins_done ), ReadNoFence( &proc->builtins_owner ),
                               self ) == W32IOS_BUILTIN_SKIP)
        return;

    pthread_mutex_lock( &proc->builtins_lock );
    if (!proc->builtins_done)
    {
        proc->builtins_owner = self;
        register_builtins();
        proc->builtins_owner = 0;
        WriteRelease( &proc->builtins_done, 1 );
        w32ios_log_verbose( "process %04x: built-in classes registered", (unsigned int)GetCurrentProcessId() );
    }
    pthread_mutex_unlock( &proc->builtins_lock );
}
