// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 10 Part C: images from signed dylibs (ios_dylib.c).

#ifndef MYIOSDECK_IOS_DYLIB_H
#define MYIOSDECK_IOS_DYLIB_H

#include <stdint.h>

/* Part C4: a fresh instance of a dylib's image, from the app (field names fixed by the spec) */
struct ios_nojit_pe_instance
{
    char *image, *data, *end;   /* like the three symbols */
    uint64_t *teb_offset;       /* store the TEB offset here */
    char *tramps;
    void *reserve;
    size_t reserve_size;
};

/* Part C7: one mapped image */
struct ios_dylib_image
{
    char          name[64];       /* the DLL's file name ("ntdll.dll") */
    char          path_tail[1024];/* the dylib's path */
    USHORT        machine;
    void         *handle;         /* the dlopen handle (first instance), else NULL */
    char         *image;          /* DOS header */
    char         *data;           /* first byte of data: below it headers and code, never written */
    char         *end;            /* end of the image */
    uint64_t     *teb_offset;
    int           has_view;       /* a view was registered over it */
    int           no_own_view;    /* clarifications 17 and 19: dyld's mapping is never registered
                                   * (unless the app gives no instance); every process gets an instance */
    char         *dyld_image;     /* clarification 17: an instance taken at first use in place of
                                   * dyld's mapping at this address (for the C8 line), else NULL */
    char         *header_end;     /* clarification 19: the end of the header range (set when the
                                   * view is registered): of the copy when header_copy, else of the
                                   * headers, never past data; NULL before registration */
    int           header_copy;    /* clarification 19: [image, header_end) is an anonymous copy that
                                   * may be made writable (for ImageBase); else signed, never written */
    int           header_logged;  /* clarification 19: the header base line was written */
};

extern int ios_teb_tls_slot_offset;

extern void ios_dylib_trace( const char *fmt, ... ) __attribute__((format(printf,1,2)));
extern const struct ios_dylib_image *ios_dylib_find( const void *addr );
extern const char *ios_dylib_next_start( const void *addr );
extern NTSTATUS ios_dylib_open( const UNICODE_STRING *nt_name, USHORT machine, struct ios_dylib_image **ret );
extern BOOL ios_dylib_exists( const UNICODE_STRING *nt_name, USHORT machine );   /* clarification 9 */
extern int ios_dylib_instances( const UNICODE_STRING *nt_name, USHORT machine, char **bases,
                                int max );   /* clarification 10 */

#endif /* MYIOSDECK_IOS_DYLIB_H */
