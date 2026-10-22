// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

/*
 * Spec 18 Part D3: is an IAT slot the memory operand of one of the image's own x64 export stubs?
 *
 * A mixed ARM64EC image may re-export an import through an x64 stub `jmp qword ptr [rip+disp32]`
 * (FF 25 disp32) that reads the import's IAT slot. x64 callers of that re-export read the slot,
 * so the loader must keep the x64-callable address there instead of the native one.
 *
 * Pure code: no Windows headers, reads only inside the mapped image, writes nothing.
 * loader.c includes this file in the ARM64EC build; the host tests include it as well.
 * Layouts used (PE/COFF and ARM64EC documentation):
 *   export directory: 40 bytes, NumberOfFunctions at +20, AddressOfFunctions at +28;
 *   code-range map entry: 8 bytes, { StartOffset (low 2 bits = type, 2 = x64), Length }.
 */

#include <stddef.h>
#include <stdint.h>

#define EC_EXPORT_DIR_MIN_SIZE  40
#define EC_CODE_RANGE_X64       2

static inline uint32_t ec_iat_read32( const unsigned char *p )
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* [rva, rva + len) lies inside an image of image_size bytes */
static inline int ec_iat_range_ok( uint64_t image_size, uint64_t rva, uint64_t len )
{
    return rva <= image_size && len <= image_size - rva;
}

/* the 6 bytes of a stub at stub_rva lie entirely inside one x64 code range */
static int ec_iat_stub_in_x64_range( const unsigned char *map, uint32_t count, uint32_t stub_rva )
{
    uint32_t i;

    for (i = 0; i < count; i++)
    {
        uint32_t start = ec_iat_read32( map + 8 * (size_t)i );
        uint32_t length = ec_iat_read32( map + 8 * (size_t)i + 4 );

        if ((start & 3) != EC_CODE_RANGE_X64) continue;
        start &= ~3u;
        if (stub_rva < start) continue;
        if ((uint64_t)stub_rva + 6 <= (uint64_t)start + length) return 1;
    }
    return 0;
}

static int arm64ec_iat_slot_is_x64_export( const void *image, uint64_t image_size, uint32_t slot_rva,
                                           uint32_t export_rva, uint32_t export_size,
                                           uint32_t code_map_rva, uint32_t code_map_count )
{
    const unsigned char *base = image, *exports, *functions, *map;
    uint32_t i, count, functions_rva;

    if (!base) return 0;
    if (slot_rva & 7) return 0;
    if (!ec_iat_range_ok( image_size, slot_rva, 8 )) return 0;
    if (export_size < EC_EXPORT_DIR_MIN_SIZE) return 0;
    if (!ec_iat_range_ok( image_size, export_rva, export_size )) return 0;
    if (!code_map_count) return 0;
    if (!ec_iat_range_ok( image_size, code_map_rva, (uint64_t)code_map_count * 8 )) return 0;

    exports = base + export_rva;
    count = ec_iat_read32( exports + 20 );
    functions_rva = ec_iat_read32( exports + 28 );
    if (!ec_iat_range_ok( image_size, functions_rva, (uint64_t)count * 4 )) return 0;
    functions = base + functions_rva;
    map = base + code_map_rva;

    for (i = 0; i < count; i++)
    {
        uint32_t entry = ec_iat_read32( functions + 4 * (size_t)i );
        const unsigned char *stub;
        int64_t target;

        if (!entry) continue;
        /* a forwarder string, not code */
        if (entry >= export_rva && (uint64_t)entry < (uint64_t)export_rva + export_size) continue;
        if (!ec_iat_range_ok( image_size, entry, 6 )) continue;
        stub = base + entry;
        if (stub[0] != 0xff || stub[1] != 0x25) continue;
        target = (int64_t)entry + 6 + (int32_t)ec_iat_read32( stub + 2 );
        if (target != (int64_t)slot_rva) continue;
        if (ec_iat_stub_in_x64_range( map, code_map_count, entry )) return 1;
    }
    return 0;
}
