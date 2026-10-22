// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 12 clarifications 8 and 9: stand-in unix call tables for libraries iOS doesn't have.

#ifndef MYIOSDECK_UNIX_TABLES_IOS_H
#define MYIOSDECK_UNIX_TABLES_IOS_H

#define W32IOS_UNIX_LIBS 8   /* > the largest W32IOS_TABLE_ID of tables.txt's libraries */

/* One per library: its log line's head and its id (a per-thread cache slot). */
struct w32ios_unix_report
{
    unsigned int id;
    const char  *head;   /* e.g. "[opengl32] no OpenGL on iOS" */
};

/* Every entry but the setup entries ends here with its index: returns STATUS_NOT_SUPPORTED, and
 * the first call of a pseudo-process for this library logs
 *     <head>: <index> returns STATUS_NOT_SUPPORTED
 * (unix_tables_log.c). */
NTSTATUS w32ios_unix_unsupported( const struct w32ios_unix_report *report, unsigned int index );

#endif
