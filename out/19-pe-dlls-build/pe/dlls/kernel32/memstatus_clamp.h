/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 the MYIOSDECK author */

/*
 * Spec 19 Part A3: when the 32-bit GlobalMemoryStatus clamps its values to 2 GB. Pure code
 * shared by heap.c and the host tests.
 */

#ifndef MYIOSDECK_MEMSTATUS_CLAMP_H
#define MYIOSDECK_MEMSTATUS_CLAMP_H

#define MEMSTATUS_2GB 0x80000000ull

/* Clamp only for an image without IMAGE_FILE_LARGE_ADDRESS_AWARE whose address space, as
 * GlobalMemoryStatusEx reported it, is at most 2 GB as well. On Windows the two agree; here
 * such a program may be given 4 GB, and the honest answer is what it can actually use. */
static inline int memstatus_clamp_to_2gb( int large_address_aware, unsigned long long total_virtual )
{
    return !large_address_aware && total_virtual <= MEMSTATUS_2GB;
}

#endif /* MYIOSDECK_MEMSTATUS_CLAMP_H */
