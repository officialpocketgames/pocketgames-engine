// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 11 Part E (Part B2): ios_sig_exception, the table from a signal (and its si_code, the
// syndrome and the instruction at pc) to a Windows exception code, its parameters and the pc
// adjustment. Syndrome classes are Arm's ESR_EL1.EC values (0x20/0x21 instruction abort, 0x24/0x25
// data abort with WnR in bit 6, 0x3c BRK).

#include <string.h>

#include "ios_sig_pure.h"
#include "test_sig_check.h"

#define PC   0x140001000ull
#define X0   0x0000000000000007ull   /* FAST_FAIL_FATAL_APP_EXIT */

#define ESR(ec, iss) (((uint64_t)(ec) << 26) | (1ull << 25) | (iss))

static uint32_t brk( unsigned imm ) { return 0xd4200000u | (imm << 5); }

static void exc( int kind, int sub, uint64_t esr, uint64_t addr, uint32_t insn, int insn_ok,
                 struct ios_exc *e )
{
    memset( e, 0xcc, sizeof(*e) );  /* every field must be set */
    ios_sig_exception( kind, sub, esr, addr, PC, insn, insn_ok, X0, e );
}

static void test_access_violations(void)
{
    static const int kinds[] = { IOS_SIGK_SEGV, IOS_SIGK_BUS };
    struct ios_exc e;
    unsigned int k;

    for (k = 0; k < 2; k++)
    {
        /* read: no syndrome, or a data abort without WnR */
        exc( kinds[k], IOS_SUB_NONE, 0, 0x10, 0, 0, &e );
        CHECK_EQ( e.action, IOS_EXC_RAISE );
        CHECK_EQ( e.code, IOS_STATUS_ACCESS_VIOLATION );
        CHECK_EQ( e.flags, 0 );
        CHECK_EQ( e.nparams, 2 );
        CHECK_EQ( e.info[0], 0 );
        CHECK_EQ( e.info[1], 0x10 );
        CHECK_EQ( e.pc_adjust, 0 );
        CHECK_EQ( e.emul_reg, (uint64_t)-1 );
        exc( kinds[k], IOS_SUB_NONE, ESR(0x24, 0x07), 0x20, 0, 0, &e );
        CHECK_EQ( e.info[0], 0 );
        CHECK_EQ( e.info[1], 0x20 );

        /* write: a data abort (lower or same EL) with WnR */
        exc( kinds[k], IOS_SUB_NONE, ESR(0x24, 0x47), 0x30, 0, 0, &e );
        CHECK_EQ( e.info[0], 1 );
        CHECK_EQ( e.info[1], 0x30 );
        exc( kinds[k], IOS_SUB_NONE, ESR(0x25, 0x40), 0x30, 0, 0, &e );
        CHECK_EQ( e.info[0], 1 );
        /* WnR means nothing outside a data abort */
        exc( kinds[k], IOS_SUB_NONE, ESR(0x0e, 0x40), 0x30, 0, 0, &e );
        CHECK_EQ( e.info[0], 0 );

        /* execute: the address is the pc, or an instruction abort */
        exc( kinds[k], IOS_SUB_NONE, 0, PC, 0, 0, &e );
        CHECK_EQ( e.info[0], 8 );
        CHECK_EQ( e.info[1], PC );
        exc( kinds[k], IOS_SUB_NONE, ESR(0x20, 0x07), 0x50000, 0, 0, &e );
        CHECK_EQ( e.info[0], 8 );
        CHECK_EQ( e.info[1], 0x50000 );
        exc( kinds[k], IOS_SUB_NONE, ESR(0x21, 0x07), 0x50000, 0, 0, &e );
        CHECK_EQ( e.info[0], 8 );
        /* a jump to the pc with a write syndrome is still an execute */
        exc( kinds[k], IOS_SUB_NONE, ESR(0x24, 0x40), PC, 0, 0, &e );
        CHECK_EQ( e.info[0], 8 );
    }
}

static void test_illegal(void)
{
    struct ios_exc e;

    /* mrs xN, CurrentEL is emulated: xN = 0 (EL0), pc + 4 */
    exc( IOS_SIGK_ILL, IOS_SUB_NONE, 0, PC, 0xd5384245, 1, &e );
    CHECK_EQ( e.action, IOS_EXC_EMULATED );
    CHECK_EQ( e.emul_reg, 5 );
    CHECK_EQ( e.pc_adjust, 4 );
    exc( IOS_SIGK_ILL, IOS_SUB_NONE, 0, PC, 0xd538425f, 1, &e );  /* into xzr */
    CHECK_EQ( e.action, IOS_EXC_EMULATED );
    CHECK_EQ( e.emul_reg, 31 );

    /* the same encoding unread is an ordinary illegal instruction */
    exc( IOS_SIGK_ILL, IOS_SUB_NONE, 0, PC, 0xd5384245, 0, &e );
    CHECK_EQ( e.action, IOS_EXC_RAISE );
    CHECK_EQ( e.code, IOS_STATUS_ILLEGAL_INSTRUCTION );
    CHECK_EQ( e.nparams, 0 );
    CHECK_EQ( e.pc_adjust, 0 );

    /* another system register (mrs x0, MIDR_EL1), and an undefined encoding */
    exc( IOS_SIGK_ILL, IOS_SUB_NONE, 0, PC, 0xd5380000, 1, &e );
    CHECK_EQ( e.action, IOS_EXC_RAISE );
    CHECK_EQ( e.code, IOS_STATUS_ILLEGAL_INSTRUCTION );
    exc( IOS_SIGK_ILL, IOS_SUB_NONE, 0, PC, 0x00000000, 1, &e );
    CHECK_EQ( e.code, IOS_STATUS_ILLEGAL_INSTRUCTION );
    CHECK_EQ( e.pc_adjust, 0 );
}

static void test_breakpoints(void)
{
    static const unsigned others[] = { 0, 1, 0xf002, 0xf005, 0xffff };
    struct ios_exc e;
    unsigned int i;

    /* __debugbreak: the pc stays on the brk */
    exc( IOS_SIGK_TRAP, IOS_TRAP_BRK, ESR(0x3c, 0xf000), PC, brk( 0xf000 ), 1, &e );
    CHECK_EQ( e.action, IOS_EXC_RAISE );
    CHECK_EQ( e.code, IOS_STATUS_BREAKPOINT );
    CHECK_EQ( e.flags, 0 );
    CHECK_EQ( e.nparams, 1 );
    CHECK_EQ( e.info[0], 0 );
    CHECK_EQ( e.pc_adjust, 0 );

    /* brk #0xf001: an assertion */
    exc( IOS_SIGK_TRAP, IOS_TRAP_BRK, 0, PC, brk( 0xf001 ), 1, &e );
    CHECK_EQ( e.action, IOS_EXC_RAISE );
    CHECK_EQ( e.code, IOS_STATUS_ASSERTION_FAILURE );
    CHECK_EQ( e.pc_adjust, 0 );

    /* brk #0xf003: __fastfail, the code from x0, not continuable */
    exc( IOS_SIGK_TRAP, IOS_TRAP_BRK, 0, PC, brk( 0xf003 ), 1, &e );
    CHECK_EQ( e.action, IOS_EXC_FASTFAIL );
    CHECK_EQ( e.code, IOS_STATUS_STACK_BUFFER_OVERRUN );
    CHECK_EQ( e.flags, IOS_EXCEPTION_NONCONTINUABLE );
    CHECK_EQ( e.nparams, 1 );
    CHECK_EQ( e.info[0], X0 );
    CHECK_EQ( e.pc_adjust, 0 );

    /* brk #0xf004: the compiler's integer divide-by-zero check */
    exc( IOS_SIGK_TRAP, IOS_TRAP_BRK, 0, PC, brk( 0xf004 ), 1, &e );
    CHECK_EQ( e.action, IOS_EXC_RAISE );
    CHECK_EQ( e.code, IOS_STATUS_INT_DIVIDE_BY_ZERO );
    CHECK_EQ( e.pc_adjust, 0 );

    /* any other immediate is a breakpoint */
    for (i = 0; i < sizeof(others) / sizeof(others[0]); i++)
    {
        exc( IOS_SIGK_TRAP, IOS_TRAP_BRK, 0, PC, brk( others[i] ), 1, &e );
        CHECK_EQ( e.action, IOS_EXC_RAISE );
        CHECK_EQ( e.code, IOS_STATUS_BREAKPOINT );
        CHECK_EQ( e.nparams, 1 );
        CHECK_EQ( e.pc_adjust, 0 );
    }

    /* a trap whose instruction can't be read, or isn't a brk, is a breakpoint too */
    exc( IOS_SIGK_TRAP, IOS_TRAP_BRK, 0, PC, 0, 0, &e );
    CHECK_EQ( e.code, IOS_STATUS_BREAKPOINT );
    CHECK_EQ( e.nparams, 1 );
    exc( IOS_SIGK_TRAP, IOS_TRAP_BRK, 0, PC, 0xd503201f, 1, &e );  /* nop */
    CHECK_EQ( e.code, IOS_STATUS_BREAKPOINT );
    /* hlt and svc aren't brk */
    exc( IOS_SIGK_TRAP, IOS_TRAP_BRK, 0, PC, 0xd45e0000, 1, &e );  /* hlt #0xf000 */
    CHECK_EQ( e.action, IOS_EXC_RAISE );
    CHECK_EQ( e.code, IOS_STATUS_BREAKPOINT );
    exc( IOS_SIGK_TRAP, IOS_TRAP_BRK, 0, PC, 0xd41e0061, 1, &e );  /* svc #0xf003 */
    CHECK_EQ( e.action, IOS_EXC_RAISE );
    CHECK_EQ( e.code, IOS_STATUS_BREAKPOINT );

    /* a single step */
    exc( IOS_SIGK_TRAP, IOS_TRAP_TRACE, ESR(0x32, 0x22), PC, 0xd503201f, 1, &e );
    CHECK_EQ( e.action, IOS_EXC_RAISE );
    CHECK_EQ( e.code, IOS_STATUS_SINGLE_STEP );
    CHECK_EQ( e.nparams, 0 );
    CHECK_EQ( e.pc_adjust, 0 );
    exc( IOS_SIGK_TRAP, IOS_TRAP_TRACE, 0, PC, 0, 0, &e );
    CHECK_EQ( e.code, IOS_STATUS_SINGLE_STEP );
    /* a step that stops in front of a brk is still a step: the brk hasn't run yet */
    exc( IOS_SIGK_TRAP, IOS_TRAP_TRACE, ESR(0x32, 0x22), PC, brk( 0xf000 ), 1, &e );
    CHECK_EQ( e.action, IOS_EXC_RAISE );
    CHECK_EQ( e.code, IOS_STATUS_SINGLE_STEP );
    exc( IOS_SIGK_TRAP, IOS_TRAP_TRACE, ESR(0x32, 0x22), PC, brk( 0xf003 ), 1, &e );
    CHECK_EQ( e.action, IOS_EXC_RAISE );
    CHECK_EQ( e.code, IOS_STATUS_SINGLE_STEP );
    CHECK_EQ( e.nparams, 0 );
    /* a BRK syndrome is a brk whatever si_code says */
    exc( IOS_SIGK_TRAP, IOS_TRAP_TRACE, ESR(0x3c, 0xf000), PC, brk( 0xf000 ), 1, &e );
    CHECK_EQ( e.code, IOS_STATUS_BREAKPOINT );
}

static void test_fpe(void)
{
    static const struct { int sub; uint32_t code; } table[] =
    {
        { IOS_FPE_INTDIV, IOS_STATUS_INT_DIVIDE_BY_ZERO },
        { IOS_FPE_INTOVF, IOS_STATUS_INT_OVERFLOW },
        { IOS_FPE_FLTDIV, IOS_STATUS_FLT_DIVIDE_BY_ZERO },
        { IOS_FPE_FLTOVF, IOS_STATUS_FLT_OVERFLOW },
        { IOS_FPE_FLTUND, IOS_STATUS_FLT_UNDERFLOW },
        { IOS_FPE_FLTRES, IOS_STATUS_FLT_INEXACT_RESULT },
        { IOS_FPE_FLTINV, IOS_STATUS_FLT_INVALID_OPERATION },
        { IOS_FPE_FLTSUB, IOS_STATUS_ARRAY_BOUNDS_EXCEEDED },
        { IOS_SUB_NONE,   IOS_STATUS_FLT_INVALID_OPERATION },  /* unknown si_code */
    };
    struct ios_exc e;
    unsigned int i;

    for (i = 0; i < sizeof(table) / sizeof(table[0]); i++)
    {
        exc( IOS_SIGK_FPE, table[i].sub, 0, PC, 0, 0, &e );
        CHECK_EQ( e.action, IOS_EXC_RAISE );
        CHECK_EQ( e.code, table[i].code );
        CHECK_EQ( e.flags, 0 );
        CHECK_EQ( e.nparams, 0 );
        CHECK_EQ( e.pc_adjust, 0 );
    }
}

int main(void)
{
    test_access_violations();
    test_illegal();
    test_breakpoints();
    test_fpe();
    return check_summary();
}
