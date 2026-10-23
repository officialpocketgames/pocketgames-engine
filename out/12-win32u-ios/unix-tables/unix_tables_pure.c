// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 clarifications 8 and 9: see unix_tables_pure.h.

#include "unix_tables_pure.h"

int w32ios_unix_first_report( struct w32ios_unix_seen *seen, const void *peb, uint32_t pid )
{
    unsigned int i;

    for (i = 0; i < seen->count; i++)
        if (seen->peb[i] == peb && seen->pid[i] == pid) return 0;

    if (seen->count < W32IOS_UNIX_SEEN) i = seen->count++;
    else
    {
        i = seen->next;
        seen->next = (seen->next + 1) % W32IOS_UNIX_SEEN;
    }
    seen->peb[i] = peb;
    seen->pid[i] = pid;
    return 1;
}
