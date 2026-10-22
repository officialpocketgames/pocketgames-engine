// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 11 Part E (clarification 1): the low-window instruction decoder and emulator of
// ios_sig_pure.c. For each supported A64 load/store encoding the fake memory records the access
// the decoder asked for (address, size, read/write), and the test compares the whole register
// file with the expected one (destination registers, write-back of the base, pc + 4, nothing
// else). Encodings outside the list, x18, unpredictable forms and refused memory leave the
// registers as they were. The encodings were written from Arm's A64 encoding tables and checked
// against clang 21's assembler (aarch64, +lse+rcpc).

#include <string.h>

#include "ios_sig_pure.h"
#include "test_sig_check.h"

#define BASE   0x7ffe0000ull   /* the window's guest address */
#define WSIZE  0x100
#define PC     0x140002000ull

struct fake_window
{
    uint8_t  buf[WSIZE];
    int      allow;        /* IOS_A64_READ / IOS_A64_WRITE the memory allows */
    int      calls;
    uint64_t addr, len;    /* the last request */
    int      access;
};

static struct fake_window win;

static void *fake_map( void *ctx, uint64_t addr, uint64_t len, int access )
{
    struct fake_window *w = ctx;

    w->calls++;
    w->addr = addr;
    w->len = len;
    w->access = access;
    if (addr < BASE || addr - BASE > WSIZE || len > WSIZE - (addr - BASE)) return NULL;
    if (access & ~w->allow) return NULL;
    return w->buf + (addr - BASE);
}

static const struct ios_a64_mem mem = { &win, fake_map };

static struct ios_a64_regs regs, want;

/* fresh registers and memory: x_n = 0x11111111_000000nn, v_n bytes 0xa0 + n, buf[i] = i */
static void setup(void)
{
    int i;

    memset( &regs, 0, sizeof(regs) );
    for (i = 0; i < 31; i++) regs.x[i] = 0x1111111100000000ull | (uint64_t)i;
    regs.sp = BASE + 0x80;
    regs.pc = PC;
    for (i = 0; i < 32; i++) memset( regs.v[i], 0xa0 + i, 16 );
    for (i = 0; i < WSIZE; i++) win.buf[i] = (uint8_t)i;
    win.allow = IOS_A64_READ | IOS_A64_WRITE;
    win.calls = 0;
    win.addr = win.len = 0;
    win.access = 0;
}

/* little-endian value of buf[off .. off + n) */
static uint64_t le( unsigned off, unsigned n )
{
    uint64_t v = 0;
    while (n--) v = (v << 8) | win.buf[off + n];
    return v;
}

static void put_le( unsigned off, unsigned n, uint64_t v )
{
    unsigned i;
    for (i = 0; i < n; i++) win.buf[off + i] = (uint8_t)(v >> (8 * i));
}

/* runs insn on regs after want = regs; the caller then edits want */
static int run( uint32_t insn )
{
    return ios_a64_emulate( insn, &regs, &mem );
}

#define EXPECT_ACCESS(a, n, acc) do { CHECK_EQ( win.calls, 1 ); CHECK_EQ( win.addr, (a) ); \
    CHECK_EQ( win.len, (n) ); CHECK_EQ( win.access, (acc) ); } while (0)

#define EXPECT_REGS() CHECK( !memcmp( &regs, &want, sizeof(regs) ) )

#define R  IOS_A64_READ
#define W  IOS_A64_WRITE

/* ---------- single-register integer forms ---------- */

static void test_ldst_integer(void)
{
    /* ldr x1, [x2, #8]: unsigned immediate, scaled */
    setup(); regs.x[2] = BASE + 0x10; want = regs;
    CHECK_EQ( run( 0xf9400441 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x18, 8, R );
    want.x[1] = le( 0x18, 8 ); want.pc += 4;
    EXPECT_REGS();

    /* str w3, [x4, #-4]!: pre-index, base written back */
    setup(); regs.x[4] = BASE + 0x24; regs.x[3] = 0xdeadbeefcafef00dull; want = regs;
    CHECK_EQ( run( 0xb81fcc83 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x20, 4, W );
    CHECK_EQ( le( 0x20, 4 ), 0xcafef00d );
    CHECK_EQ( win.buf[0x24], 0x24 );
    want.x[4] = BASE + 0x20; want.pc += 4;
    EXPECT_REGS();

    /* ldrb w5, [x6], #1: post-index, zero-extended into the whole register */
    setup(); regs.x[6] = BASE + 0x30; want = regs;
    CHECK_EQ( run( 0x384014c5 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x30, 1, R );
    want.x[5] = 0x30; want.x[6] = BASE + 0x31; want.pc += 4;
    EXPECT_REGS();

    /* ldrsh x7, [x8, x9, lsl #1]: register offset, scaled, sign-extended to 64 bits */
    setup(); regs.x[8] = BASE; regs.x[9] = 0x20; put_le( 0x40, 2, 0x9234 ); want = regs;
    CHECK_EQ( run( 0x78a97907 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x40, 2, R );
    want.x[7] = 0xffffffffffff9234ull; want.pc += 4;
    EXPECT_REGS();

    /* ldrsw x1, [x2, w3, sxtw]: only w3 counts, sign-extended (-16) */
    setup(); regs.x[2] = BASE + 0x60; regs.x[3] = 0x12345678fffffff0ull; put_le( 0x50, 4, 0x80525150 );
    want = regs;
    CHECK_EQ( run( 0xb8a3c841 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x50, 4, R );
    want.x[1] = 0xffffffff80525150ull; want.pc += 4;
    EXPECT_REGS();

    /* ldr x1, [x2, w3, uxtw #3]: w3 zero-extended, scaled by 8 */
    setup(); regs.x[2] = BASE; regs.x[3] = 0xffffffff00000004ull; want = regs;
    CHECK_EQ( run( 0xf8635841 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x20, 8, R );
    want.x[1] = le( 0x20, 8 ); want.pc += 4;
    EXPECT_REGS();

    /* ldrh w1, [x2, x3]: unscaled register offset, unaligned */
    setup(); regs.x[2] = BASE + 0x40; regs.x[3] = 3; want = regs;
    CHECK_EQ( run( 0x78636841 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x43, 2, R );
    want.x[1] = 0x4443; want.pc += 4;
    EXPECT_REGS();

    /* ldr x1, [x2, x3, sxtx]: a negative 64-bit offset */
    setup(); regs.x[2] = BASE + 0x18; regs.x[3] = (uint64_t)-8; want = regs;
    CHECK_EQ( run( 0xf863e841 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x10, 8, R );
    want.x[1] = le( 0x10, 8 ); want.pc += 4;
    EXPECT_REGS();

    /* ldur x1, [x2, #-3]: unscaled immediate */
    setup(); regs.x[2] = BASE + 0x13; want = regs;
    CHECK_EQ( run( 0xf85fd041 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x10, 8, R );
    want.x[1] = le( 0x10, 8 ); want.pc += 4;
    EXPECT_REGS();

    /* sttr w1, [x2, #4]: unprivileged, as at EL0 */
    setup(); regs.x[2] = BASE + 0x8; want = regs;
    CHECK_EQ( run( 0xb8004841 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0xc, 4, W );
    CHECK_EQ( le( 0xc, 4 ), 0x00000001 );
    want.pc += 4;
    EXPECT_REGS();

    /* ldr x1, [x2], #8: post-index */
    setup(); regs.x[2] = BASE + 0x8; want = regs;
    CHECK_EQ( run( 0xf8408441 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x8, 8, R );
    want.x[1] = le( 0x8, 8 ); want.x[2] = BASE + 0x10; want.pc += 4;
    EXPECT_REGS();

    /* str xzr, [x2]: register 31 stores zero */
    setup(); regs.x[2] = BASE + 0x8; want = regs;
    CHECK_EQ( run( 0xf900005f ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x8, 8, W );
    CHECK_EQ( le( 0x8, 8 ), 0 );
    want.pc += 4;
    EXPECT_REGS();

    /* ldr xzr, [x2]: the value is discarded */
    setup(); regs.x[2] = BASE + 0x8; want = regs;
    CHECK_EQ( run( 0xf940005f ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x8, 8, R );
    want.pc += 4;
    EXPECT_REGS();

    /* ldr x1, [sp, #16]: register 31 as a base is sp */
    setup(); want = regs;
    CHECK_EQ( run( 0xf9400be1 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x90, 8, R );
    want.x[1] = le( 0x90, 8 ); want.pc += 4;
    EXPECT_REGS();
}

/* ---------- FP/SIMD single registers ---------- */

static void test_ldst_simd(void)
{
    int i;

    /* ldr q0, [x1, #16] */
    setup(); regs.x[1] = BASE; want = regs;
    CHECK_EQ( run( 0x3dc00420 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x10, 16, R );
    memcpy( want.v[0], win.buf + 0x10, 16 ); want.pc += 4;
    EXPECT_REGS();

    /* ldr s3, [x4]: the rest of v3 is cleared */
    setup(); regs.x[4] = BASE + 0x8; want = regs;
    CHECK_EQ( run( 0xbd400083 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x8, 4, R );
    memset( want.v[3], 0, 16 ); memcpy( want.v[3], win.buf + 0x8, 4 ); want.pc += 4;
    EXPECT_REGS();

    /* str d2, [sp, #8] */
    setup(); want = regs;
    CHECK_EQ( run( 0xfd0007e2 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x88, 8, W );
    for (i = 0; i < 8; i++) CHECK_EQ( win.buf[0x88 + i], 0xa2 );
    CHECK_EQ( win.buf[0x90], 0x90 );
    want.pc += 4;
    EXPECT_REGS();

    /* str b0, [x1, #3] */
    setup(); regs.x[1] = BASE; want = regs;
    CHECK_EQ( run( 0x3d000c20 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x3, 1, W );
    CHECK_EQ( win.buf[3], 0xa0 );
    CHECK_EQ( win.buf[4], 0x04 );
    want.pc += 4;
    EXPECT_REGS();

    /* stur q1, [x2, #-16] */
    setup(); regs.x[2] = BASE + 0x20; want = regs;
    CHECK_EQ( run( 0x3c9f0041 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x10, 16, W );
    for (i = 0; i < 16; i++) CHECK_EQ( win.buf[0x10 + i], 0xa1 );
    want.pc += 4;
    EXPECT_REGS();

    /* ldr q18, [x1]: v18 is an ordinary register (only x18 is the platform's) */
    setup(); regs.x[1] = BASE + 0x40; want = regs;
    CHECK_EQ( run( 0x3dc00032 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x40, 16, R );
    memcpy( want.v[18], win.buf + 0x40, 16 ); want.pc += 4;
    EXPECT_REGS();
}

/* ---------- pairs ---------- */

static void test_pairs(void)
{
    /* ldp x1, x2, [x3, #16] */
    setup(); regs.x[3] = BASE; want = regs;
    CHECK_EQ( run( 0xa9410861 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x10, 16, R );
    want.x[1] = le( 0x10, 8 ); want.x[2] = le( 0x18, 8 ); want.pc += 4;
    EXPECT_REGS();

    /* stp w1, w2, [sp, #-8]! */
    setup(); want = regs;
    CHECK_EQ( run( 0x29bf0be1 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x78, 8, W );
    CHECK_EQ( le( 0x78, 4 ), 1 );
    CHECK_EQ( le( 0x7c, 4 ), 2 );
    want.sp = BASE + 0x78; want.pc += 4;
    EXPECT_REGS();

    /* ldpsw x1, x2, [x3], #8: both sign-extended, post-index */
    setup(); regs.x[3] = BASE + 0x40; put_le( 0x40, 4, 0x80000001 ); put_le( 0x44, 4, 0x7fffffff );
    want = regs;
    CHECK_EQ( run( 0x68c10861 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x40, 8, R );
    want.x[1] = 0xffffffff80000001ull; want.x[2] = 0x7fffffff; want.x[3] = BASE + 0x48; want.pc += 4;
    EXPECT_REGS();

    /* ldp q0, q1, [x2] */
    setup(); regs.x[2] = BASE + 0x20; want = regs;
    CHECK_EQ( run( 0xad400440 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x20, 32, R );
    memcpy( want.v[0], win.buf + 0x20, 16 ); memcpy( want.v[1], win.buf + 0x30, 16 ); want.pc += 4;
    EXPECT_REGS();

    /* ldnp d1, d2, [x3, #16] */
    setup(); regs.x[3] = BASE; want = regs;
    CHECK_EQ( run( 0x6c410861 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x10, 16, R );
    memset( want.v[1], 0, 16 ); memcpy( want.v[1], win.buf + 0x10, 8 );
    memset( want.v[2], 0, 16 ); memcpy( want.v[2], win.buf + 0x18, 8 );
    want.pc += 4;
    EXPECT_REGS();
}

/* ---------- acquire/release ---------- */

static void test_acquire_release(void)
{
    /* ldar w1, [x2] and ldlar w1, [x2] */
    setup(); regs.x[2] = BASE + 0x20; want = regs;
    CHECK_EQ( run( 0x88dffc41 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x20, 4, R );
    want.x[1] = le( 0x20, 4 ); want.pc += 4;
    EXPECT_REGS();
    setup(); regs.x[2] = BASE + 0x20; want = regs;
    CHECK_EQ( run( 0x88df7c41 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x20, 4, R );
    want.x[1] = le( 0x20, 4 ); want.pc += 4;
    EXPECT_REGS();

    /* stlr x1, [x2] */
    setup(); regs.x[2] = BASE + 0x20; regs.x[1] = 0x0123456789abcdefull; want = regs;
    CHECK_EQ( run( 0xc89ffc41 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x20, 8, W );
    CHECK_EQ( le( 0x20, 8 ), 0x0123456789abcdefull );
    want.pc += 4;
    EXPECT_REGS();

    /* ldapr x1, [x2] */
    setup(); regs.x[2] = BASE + 0x28; want = regs;
    CHECK_EQ( run( 0xf8bfc041 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x28, 8, R );
    want.x[1] = le( 0x28, 8 ); want.pc += 4;
    EXPECT_REGS();

    /* stlur w1, [x2, #-1] */
    setup(); regs.x[2] = BASE + 0x21; regs.x[1] = 0xaabbccdd; want = regs;
    CHECK_EQ( run( 0x991ff041 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x20, 4, W );
    CHECK_EQ( le( 0x20, 4 ), 0xaabbccdd );
    want.pc += 4;
    EXPECT_REGS();

    /* ldapursh w1, [x2, #2]: sign-extended to 32 bits only */
    setup(); regs.x[2] = BASE + 0x20; put_le( 0x22, 2, 0x8001 ); want = regs;
    CHECK_EQ( run( 0x59c02041 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x22, 2, R );
    want.x[1] = 0xffff8001; want.pc += 4;
    EXPECT_REGS();
}

/* ---------- atomics ---------- */

static void test_atomics(void)
{
    uint64_t old;

    /* ldadd x1, x2, [x3]: x2 = old, memory = old + x1 */
    setup(); regs.x[3] = BASE + 0x40; regs.x[1] = 5; old = le( 0x40, 8 ); want = regs;
    CHECK_EQ( run( 0xf8210062 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x40, 8, R | W );
    CHECK_EQ( le( 0x40, 8 ), old + 5 );
    want.x[2] = old; want.pc += 4;
    EXPECT_REGS();

    /* stadd x1, [x3] (ldadd into xzr) */
    setup(); regs.x[3] = BASE + 0x40; regs.x[1] = 7; old = le( 0x40, 8 ); want = regs;
    CHECK_EQ( run( 0xf821007f ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x40, 8, R | W );
    CHECK_EQ( le( 0x40, 8 ), old + 7 );
    want.pc += 4;
    EXPECT_REGS();

    /* swpal w1, w2, [x3] */
    setup(); regs.x[3] = BASE + 0x44; regs.x[1] = 0x11112222deadbeefull; old = le( 0x44, 4 ); want = regs;
    CHECK_EQ( run( 0xb8e18062 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x44, 4, R | W );
    CHECK_EQ( le( 0x44, 4 ), 0xdeadbeef );
    CHECK_EQ( le( 0x48, 4 ), 0x4b4a4948 );
    want.x[2] = old; want.pc += 4;
    EXPECT_REGS();

    /* ldsmaxh w1, w2, [x3]: a signed 16-bit compare */
    setup(); regs.x[3] = BASE + 0x40; regs.x[1] = 5; put_le( 0x40, 2, 0x8000 ); want = regs;
    CHECK_EQ( run( 0x78214062 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x40, 2, R | W );
    CHECK_EQ( le( 0x40, 2 ), 5 );
    want.x[2] = 0x8000; want.pc += 4;
    EXPECT_REGS();
    setup(); regs.x[3] = BASE + 0x40; regs.x[1] = 0xfff0; put_le( 0x40, 2, 7 ); want = regs;
    CHECK_EQ( run( 0x78214062 ), IOS_A64_DONE );
    CHECK_EQ( le( 0x40, 2 ), 7 );
    want.x[2] = 7; want.pc += 4;
    EXPECT_REGS();

    /* cas x1, x2, [x3]: equal, memory = x2 and x1 = old (unchanged) */
    setup(); regs.x[3] = BASE + 0x40; old = le( 0x40, 8 ); regs.x[1] = old; regs.x[2] = 0x5555; want = regs;
    CHECK_EQ( run( 0xc8a17c62 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x40, 8, R | W );
    CHECK_EQ( le( 0x40, 8 ), 0x5555 );
    want.pc += 4;
    EXPECT_REGS();
    /* not equal: memory unchanged, x1 = what was there */
    setup(); regs.x[3] = BASE + 0x40; old = le( 0x40, 8 ); regs.x[1] = 1; regs.x[2] = 0x5555; want = regs;
    CHECK_EQ( run( 0xc8a17c62 ), IOS_A64_DONE );
    CHECK_EQ( le( 0x40, 8 ), old );
    want.x[1] = old; want.pc += 4;
    EXPECT_REGS();

    /* casalb w1, w2, [x3]: one byte, the rest of w1 cleared */
    setup(); regs.x[3] = BASE + 0x45; regs.x[1] = 0x1111111100000045ull; regs.x[2] = 0x99; want = regs;
    CHECK_EQ( run( 0x08e1fc62 ), IOS_A64_DONE );
    EXPECT_ACCESS( BASE + 0x45, 1, R | W );
    CHECK_EQ( win.buf[0x45], 0x99 );
    CHECK_EQ( win.buf[0x46], 0x46 );
    want.x[1] = 0x45; want.pc += 4;
    EXPECT_REGS();

    /* a misaligned atomic is refused before memory is asked for */
    setup(); regs.x[3] = BASE + 0x41; regs.x[1] = 5; want = regs;
    CHECK_EQ( run( 0xf8210062 ), IOS_A64_NO_ACCESS );
    CHECK_EQ( win.calls, 0 );
    CHECK_EQ( le( 0x40, 8 ), 0x4746454443424140ull );
    EXPECT_REGS();
}

/* ---------- refusals ---------- */

static void test_unsupported(void)
{
    static const struct { uint32_t insn; const char *what; } list[] =
    {
        { 0xf9400241, "ldr x1, [x18]" },
        { 0xf9400032, "ldr x18, [x1]" },
        { 0xf8726841, "ldr x1, [x2, x18]" },
        { 0xf8210242, "ldadd x1, x2, [x18]" },
        { 0xf8320062, "ldadd x18, x2, [x3]" },
        { 0xa9404a41, "ldp x1, x18, [x18]" },
        { 0xf8408421, "ldr x1, [x1], #8 (write-back base also loaded)" },
        { 0xf8008c21, "str x1, [x1, #8]! (write-back base also stored)" },
        { 0xa9400461, "ldp x1, x1, [x3] (one register twice)" },
        { 0xa8c10422, "ldp x2, x1, [x1], #16 (write-back base also loaded)" },
        { 0xc85f7c41, "ldxr x1, [x2] (exclusive)" },
        { 0x58000041, "ldr x1, <literal>" },
        { 0xf9800020, "prfm pldl1keep, [x1]" },
        { 0xb9c00041, "ldrsw to a W register (unallocated)" },
        { 0x7dc00041, "ldr with opc 11 and size 01 (unallocated)" },
        { 0xf8630841, "ldr x1, [x2, w3, uxtb] (reserved extend)" },
        { 0x3c400841, "ldtr into a SIMD register (unallocated)" },
        { 0xf8a1c062, "o3 = 1 with opc 100 and Rs != 31" },
        { 0x4c407020, "ld1 {v0.16b}, [x1]" },
        { 0xd503201f, "nop" },
        { 0x8b020020, "add x0, x1, x2" },
        { 0xd4200000, "brk #0" },
    };
    unsigned int i;

    for (i = 0; i < sizeof(list) / sizeof(list[0]); i++)
    {
        int res;

        setup(); want = regs;
        res = run( list[i].insn );
        if (res != IOS_A64_UNSUPPORTED) printf( "  (%08x: %s)\n", list[i].insn, list[i].what );
        CHECK_EQ( res, IOS_A64_UNSUPPORTED );
        CHECK_EQ( win.calls, 0 );
        EXPECT_REGS();
        CHECK_EQ( ios_a64_base_reg( list[i].insn ), (uint64_t)-1 );
        CHECK_EQ( ios_a64_is_simd( list[i].insn ), 0 );
    }
}

static void test_no_access(void)
{
    /* the address outside the window: asked, refused, nothing changed */
    setup(); regs.x[2] = BASE - 0x10; want = regs;
    CHECK_EQ( run( 0xf9400441 ), IOS_A64_NO_ACCESS );
    EXPECT_ACCESS( BASE - 0x8, 8, R );
    EXPECT_REGS();

    /* a span that leaves the window at the end */
    setup(); regs.x[2] = BASE + WSIZE - 0xc; want = regs;
    CHECK_EQ( run( 0xf9400441 ), IOS_A64_NO_ACCESS );
    EXPECT_ACCESS( BASE + WSIZE - 0x4, 8, R );
    EXPECT_REGS();

    /* a store to memory that only allows reads; a pre-index base isn't written back */
    setup(); win.allow = IOS_A64_READ; regs.x[4] = BASE + 0x24; want = regs;
    CHECK_EQ( run( 0xb81fcc83 ), IOS_A64_NO_ACCESS );
    EXPECT_ACCESS( BASE + 0x20, 4, W );
    CHECK_EQ( le( 0x20, 4 ), 0x23222120 );
    EXPECT_REGS();

    /* an atomic needs both */
    setup(); win.allow = IOS_A64_READ; regs.x[3] = BASE + 0x40; want = regs;
    CHECK_EQ( run( 0xf8210062 ), IOS_A64_NO_ACCESS );
    EXPECT_ACCESS( BASE + 0x40, 8, R | W );
    EXPECT_REGS();

    /* an address that wraps around */
    setup(); regs.x[2] = (uint64_t)-4; want = regs;
    CHECK_EQ( run( 0xf9400041 ), IOS_A64_NO_ACCESS );
    CHECK_EQ( win.calls, 0 );
    EXPECT_REGS();
}

static void test_base_reg(void)
{
    CHECK_EQ( ios_a64_base_reg( 0xf9400441 ), 2 );    /* ldr x1, [x2, #8] */
    CHECK_EQ( ios_a64_base_reg( 0xfd0007e2 ), 31 );   /* str d2, [sp, #8] */
    CHECK_EQ( ios_a64_base_reg( 0x29bf0be1 ), 31 );   /* stp w1, w2, [sp, #-8]! */
    CHECK_EQ( ios_a64_base_reg( 0x78a97907 ), 8 );    /* ldrsh x7, [x8, x9, lsl #1] */
    CHECK_EQ( ios_a64_base_reg( 0xf8210062 ), 3 );    /* ldadd x1, x2, [x3] */
    CHECK_EQ( ios_a64_base_reg( 0xc8a17c62 ), 3 );    /* cas x1, x2, [x3] */
    CHECK_EQ( ios_a64_base_reg( 0x88dffc41 ), 2 );    /* ldar w1, [x2] */

    CHECK_EQ( ios_a64_is_simd( 0x3dc00420 ), 1 );     /* ldr q0, [x1, #16] */
    CHECK_EQ( ios_a64_is_simd( 0xad400440 ), 1 );     /* ldp q0, q1, [x2] */
    CHECK_EQ( ios_a64_is_simd( 0x3c9f0041 ), 1 );     /* stur q1, [x2, #-16] */
    CHECK_EQ( ios_a64_is_simd( 0xf9400441 ), 0 );
    CHECK_EQ( ios_a64_is_simd( 0xa9410861 ), 0 );     /* ldp x1, x2, [x3, #16] */
    CHECK_EQ( ios_a64_is_simd( 0xf8210062 ), 0 );
}

int main(void)
{
    test_ldst_integer();
    test_ldst_simd();
    test_pairs();
    test_acquire_release();
    test_atomics();
    test_unsupported();
    test_no_access();
    test_base_reg();
    return check_summary();
}
