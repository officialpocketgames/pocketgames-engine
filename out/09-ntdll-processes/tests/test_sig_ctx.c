// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 11 Part E (Part A5): the CONTEXT <-> Darwin thread state conversions of ios_sig_pure.c,
// field by field and round trip, on plain structs. Also the mirrors' layouts against the
// documented sizes and offsets (Apple's ARM_THREAD_STATE64_COUNT and ARM_NEON_STATE64_COUNT,
// Microsoft's ARM64_NT_CONTEXT).

#include <stddef.h>
#include <string.h>

#include "ios_sig_pure.h"
#include "test_sig_check.h"

#define TEB 0x7ff6a5a50000ull

static void fill_ss( struct ios_darwin_ss *ss )
{
    int i;

    memset( ss, 0, sizeof(*ss) );
    for (i = 0; i < 29; i++) ss->x[i] = 0x0101010101010101ull * (uint64_t)(i + 1) + 0x10;
    ss->fp   = 0xf0f0f0f0f0f00029ull;
    ss->lr   = 0x1e1e1e1e1e1e0030ull;
    ss->sp   = 0x00000016fdf3c0a0ull;
    ss->pc   = 0x0000000140001234ull;
    ss->cpsr = 0x60001000;
    ss->pad  = 0x5a5a5a5a;
}

static void fill_ns( struct ios_darwin_ns *ns )
{
    int i, j;

    memset( ns, 0, sizeof(*ns) );
    for (i = 0; i < 32; i++)
        for (j = 0; j < 16; j++) ns->v[i][j] = (uint8_t)(i * 16 + j + 1);
    ns->fpsr = 0x0800009f;  /* QC and the cumulative flags */
    ns->fpcr = 0x03c00000;  /* AHP, DN, FZ, RMode = 2 */
    memset( ns->pad, 0x77, sizeof(ns->pad) );
}

/* the parts of the CONTEXT the conversions never touch */
static void fill_ctx_rest( struct ios_win_ctx *ctx )
{
    int i;

    memset( ctx, 0xa5, sizeof(*ctx) );
    ctx->ContextFlags = 0x0040000f;
    for (i = 0; i < 8; i++) { ctx->Bcr[i] = 0x100 + i; ctx->Bvr[i] = 0x200 + i; }
    for (i = 0; i < 2; i++) { ctx->Wcr[i] = 0x300 + i; ctx->Wvr[i] = 0x400 + i; }
}

static void check_ctx_rest( const struct ios_win_ctx *ctx )
{
    int i;

    CHECK_EQ( ctx->ContextFlags, 0x0040000f );
    for (i = 0; i < 8; i++) { CHECK_EQ( ctx->Bcr[i], 0x100 + i ); CHECK_EQ( ctx->Bvr[i], 0x200 + i ); }
    for (i = 0; i < 2; i++) { CHECK_EQ( ctx->Wcr[i], 0x300 + i ); CHECK_EQ( ctx->Wvr[i], 0x400 + i ); }
}

static void test_layouts(void)
{
    /* Apple: ARM_THREAD_STATE64_COUNT 68 and ARM_NEON_STATE64_COUNT 132 (32-bit words) */
    CHECK_EQ( sizeof(struct ios_darwin_ss), 68 * 4 );
    CHECK_EQ( offsetof(struct ios_darwin_ss, fp), 29 * 8 );
    CHECK_EQ( offsetof(struct ios_darwin_ss, pc), 32 * 8 );
    CHECK_EQ( offsetof(struct ios_darwin_ss, cpsr), 33 * 8 );
    CHECK_EQ( sizeof(struct ios_darwin_ns), 132 * 4 );
    CHECK_EQ( offsetof(struct ios_darwin_ns, fpsr), 512 );
    CHECK_EQ( offsetof(struct ios_darwin_ns, fpcr), 516 );

    /* Microsoft: ARM64_NT_CONTEXT */
    CHECK_EQ( sizeof(struct ios_win_ctx), 0x390 );
    CHECK_EQ( offsetof(struct ios_win_ctx, Cpsr), 0x4 );
    CHECK_EQ( offsetof(struct ios_win_ctx, X), 0x8 );
    CHECK_EQ( offsetof(struct ios_win_ctx, Fp), 0xf0 );
    CHECK_EQ( offsetof(struct ios_win_ctx, Lr), 0xf8 );
    CHECK_EQ( offsetof(struct ios_win_ctx, Sp), 0x100 );
    CHECK_EQ( offsetof(struct ios_win_ctx, Pc), 0x108 );
    CHECK_EQ( offsetof(struct ios_win_ctx, V), 0x110 );
    CHECK_EQ( offsetof(struct ios_win_ctx, Fpcr), 0x310 );
    CHECK_EQ( offsetof(struct ios_win_ctx, Fpsr), 0x314 );
    CHECK_EQ( offsetof(struct ios_win_ctx, Bcr), 0x318 );
    CHECK_EQ( offsetof(struct ios_win_ctx, Bvr), 0x338 );
    CHECK_EQ( offsetof(struct ios_win_ctx, Wcr), 0x378 );
    CHECK_EQ( offsetof(struct ios_win_ctx, Wvr), 0x380 );
}

static void test_from_darwin(void)
{
    struct ios_darwin_ss ss;
    struct ios_darwin_ns ns;
    struct ios_win_ctx ctx;
    int i;

    fill_ss( &ss );
    fill_ns( &ns );
    fill_ctx_rest( &ctx );
    ios_ctx_from_darwin( &ctx, &ss, &ns, TEB );

    for (i = 0; i < 29; i++) if (i != 18) CHECK_EQ( ctx.X[i], ss.x[i] );
    CHECK_EQ( ctx.X[18], TEB );  /* whatever Darwin had in x18 */
    CHECK_EQ( ctx.Fp, ss.fp );
    CHECK_EQ( ctx.Lr, ss.lr );
    CHECK_EQ( ctx.Sp, ss.sp );
    CHECK_EQ( ctx.Pc, ss.pc );
    CHECK_EQ( ctx.Cpsr, ss.cpsr );
    CHECK( !memcmp( ctx.V, ns.v, sizeof(ctx.V) ) );
    CHECK_EQ( ctx.Fpcr, 0x03c00000 );  /* not swapped with Fpsr */
    CHECK_EQ( ctx.Fpsr, 0x0800009f );
    check_ctx_rest( &ctx );

    /* without a NEON state the floating-point part stays as it was */
    fill_ctx_rest( &ctx );
    ios_ctx_from_darwin( &ctx, &ss, NULL, TEB );
    CHECK_EQ( ctx.Pc, ss.pc );
    CHECK_EQ( ctx.X[18], TEB );
    CHECK_EQ( ctx.V[0][0], 0xa5 );
    CHECK_EQ( ctx.V[31][15], 0xa5 );
    CHECK_EQ( ctx.Fpcr, 0xa5a5a5a5 );
    CHECK_EQ( ctx.Fpsr, 0xa5a5a5a5 );
    check_ctx_rest( &ctx );
}

static void test_to_darwin(void)
{
    struct ios_darwin_ss ss;
    struct ios_darwin_ns ns;
    struct ios_win_ctx ctx;
    int i, j;

    fill_ctx_rest( &ctx );
    for (i = 0; i < 29; i++) ctx.X[i] = 0xc0de000000000000ull + (uint64_t)i;
    ctx.Fp = 0x1001;
    ctx.Lr = 0x1002;
    ctx.Sp = 0x1003;
    ctx.Pc = 0x1004;
    ctx.Cpsr = 0x80000000;
    for (i = 0; i < 32; i++) for (j = 0; j < 16; j++) ctx.V[i][j] = (uint8_t)(0xff - i - j);
    ctx.Fpcr = 0x00400000;
    ctx.Fpsr = 0x00000001;

    memset( &ss, 0x3c, sizeof(ss) );
    memset( &ns, 0x3c, sizeof(ns) );
    ios_ctx_to_darwin( &ctx, &ss, &ns );

    for (i = 0; i < 29; i++) if (i != 18) CHECK_EQ( ss.x[i], ctx.X[i] );
    CHECK_EQ( ss.x[18], 0x3c3c3c3c3c3c3c3cull );  /* never written: the platform owns it */
    CHECK_EQ( ss.fp, 0x1001 );
    CHECK_EQ( ss.lr, 0x1002 );
    CHECK_EQ( ss.sp, 0x1003 );
    CHECK_EQ( ss.pc, 0x1004 );
    CHECK_EQ( ss.cpsr, 0x80000000 );
    CHECK_EQ( ss.pad, 0x3c3c3c3c );
    CHECK( !memcmp( ns.v, ctx.V, sizeof(ns.v) ) );
    CHECK_EQ( ns.fpcr, 0x00400000 );
    CHECK_EQ( ns.fpsr, 0x00000001 );
    CHECK_EQ( ns.pad[0], 0x3c );

    /* without a NEON state only the thread state is written */
    memset( &ss, 0x3c, sizeof(ss) );
    ios_ctx_to_darwin( &ctx, &ss, NULL );
    CHECK_EQ( ss.pc, 0x1004 );
    CHECK_EQ( ss.x[18], 0x3c3c3c3c3c3c3c3cull );
}

static void test_round_trips(void)
{
    struct ios_darwin_ss ss, ss2;
    struct ios_darwin_ns ns, ns2;
    struct ios_win_ctx ctx, ctx2;

    /* Darwin -> CONTEXT -> Darwin gives back the same state (x18 is the destination's own) */
    fill_ss( &ss );
    fill_ns( &ns );
    fill_ctx_rest( &ctx );
    ios_ctx_from_darwin( &ctx, &ss, &ns, TEB );
    ss2 = ss;
    ns2 = ns;
    memset( ss2.x, 0, sizeof(ss2.x) );
    ss2.x[18] = ss.x[18];
    ss2.fp = ss2.lr = ss2.sp = ss2.pc = 0;
    ss2.cpsr = 0;
    memset( ns2.v, 0, sizeof(ns2.v) );
    ns2.fpsr = ns2.fpcr = 0;
    ios_ctx_to_darwin( &ctx, &ss2, &ns2 );
    CHECK( !memcmp( &ss2, &ss, sizeof(ss) ) );
    CHECK( !memcmp( &ns2, &ns, sizeof(ns) ) );

    /* CONTEXT -> Darwin -> CONTEXT gives back the same context when X18 is the TEB */
    fill_ctx_rest( &ctx2 );
    ios_ctx_from_darwin( &ctx2, &ss2, &ns2, TEB );
    CHECK( !memcmp( &ctx2, &ctx, sizeof(ctx) ) );
}

int main(void)
{
    test_layouts();
    test_from_darwin();
    test_to_darwin();
    test_round_trips();
    return check_summary();
}
