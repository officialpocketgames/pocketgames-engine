// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Included at the end of font.c (patches/B-10-font.patch), under WINE_IOS. Spec 12 Part B.
//
// Registry handles belong to the process that opened them: hkcu_key, wine_fonts_key and
// wine_fonts_cache_key are per pseudo-process. The session's first process opens its own in
// font_init (during init_user); every other process opens them on first use, once init_user
// has run, the same keys font_init opens.

HKEY *w32ios_font_key( int which )
{
    static const WCHAR fonts_keyW[] = {'S','o','f','t','w','a','r','e','\\','W','i','n','e','\\','F','o','n','t','s'};
    static const WCHAR cacheW[] = {'C','a','c','h','e'};
    struct w32ios_proc *proc = w32ios_proc_current();
    HKEY *slot = &proc->font_keys[which], root, key = 0;

    if (*slot || !w32ios_session_initialized()) return slot;

    switch (which)
    {
    case 0:
        key = open_hkcu();
        break;
    case 1:
        if ((root = *w32ios_font_key( 0 ))) key = reg_create_key( root, fonts_keyW, sizeof(fonts_keyW), 0, NULL );
        break;
    case 2:
        if ((root = *w32ios_font_key( 1 )))
            key = reg_create_key( root, cacheW, sizeof(cacheW), REG_OPTION_VOLATILE, NULL );
        break;
    }
    if (key && InterlockedCompareExchangePointer( (void **)slot, key, NULL )) NtClose( key );
    return slot;
}
