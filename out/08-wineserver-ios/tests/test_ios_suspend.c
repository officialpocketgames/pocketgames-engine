// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 08 Part E, clarification 5: host tests for the pure rules in server/ios_suspend.c.
//   - the hold: the first suspension takes it, nested ones only count, the last resume
//     releases it, a snapshot never releases one (including a snapshot whose own
//     thread_suspend fails), a failed hold is never "released";
//   - "inside a system call" and the context-write fingerprint match;
//   - writing native registers into a frame (never x18, restore_flags as upstream).
// The Mach calls are fakes: each fake thread has a Mach suspend count like the kernel's.

#include <stdio.h>
#include <string.h>

#include "ios_suspend.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { failures++; \
    fprintf( stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond ); } } while (0)

/* ---- fake Mach threads ---- */

#define NPORTS 8

static struct
{
    int count;          /* Mach suspend count */
    int fail_suspend;   /* thread_suspend fails (e.g. the thread is gone) */
    int suspends, resumes;
} fake[NPORTS];

static int fake_suspend( unsigned int port )
{
    if (port >= NPORTS || fake[port].fail_suspend) return 5; /* any Mach error */
    fake[port].count++;
    fake[port].suspends++;
    return 0;
}

static int fake_resume( unsigned int port )
{
    if (port >= NPORTS) return 5;
    fake[port].resumes++;
    if (fake[port].count == 0) return 5; /* KERN_FAILURE: not suspended */
    fake[port].count--;
    return 0;
}

static const struct ios_mach_ops ops = { fake_suspend, fake_resume };

static void reset_fakes(void)
{
    memset( fake, 0, sizeof(fake) );
}

/* ---- a model of the calling discipline in thread.c / process.c ----
 * upstream's counters decide when a thread becomes suspended or stops being suspended;
 * thread.c calls ios_suspend_begin / ios_suspend_end exactly at those points. */

struct model_thread
{
    unsigned int port;
    int suspend;        /* thread->suspend */
    int proc_suspend;   /* thread->process->suspend */
    int hold;           /* thread->ios_hold, 0 at creation */
};

static int real_suspend;

static int is_suspended( const struct model_thread *t ) { return t->suspend || t->proc_suspend; }

/* a snapshot: thread_suspend, read, thread_resume (E1) */
static void snapshot( struct model_thread *t )
{
    if (ios_freeze( &ops, t->port )) ios_thaw( &ops, t->port );
}

static void begin( struct model_thread *t )
{
    ios_hold_take( &ops, &t->hold, t->port, real_suspend );
    snapshot( t );
}

static void end( struct model_thread *t )
{
    ios_hold_release( &ops, &t->hold, t->port );
}

static void suspend_thread( struct model_thread *t )
{
    if (!is_suspended( t )) begin( t );
    t->suspend++;
}

static void resume_thread( struct model_thread *t )
{
    if (t->suspend > 0)
    {
        t->suspend--;
        if (!is_suspended( t )) end( t );
    }
}

static void suspend_process( struct model_thread *t )
{
    if (!t->proc_suspend++ && !t->suspend) begin( t );
}

static void resume_process( struct model_thread *t )
{
    if (!--t->proc_suspend && !t->suspend) end( t );
}

/* ---- the hold ---- */

static void test_default_mode_only_counts(void)
{
    struct model_thread t = { 1, 0, 0, 0 };

    reset_fakes();
    real_suspend = 0;
    suspend_thread( &t );
    CHECK( fake[1].count == 0 );         /* the thread keeps running */
    CHECK( fake[1].suspends == 1 );      /* the snapshot's own pair */
    CHECK( fake[1].resumes == 1 );
    suspend_thread( &t );
    suspend_thread( &t );
    CHECK( t.suspend == 3 );
    CHECK( fake[1].suspends == 1 );      /* nested suspensions take no new snapshot */
    snapshot( &t );                      /* GetThreadContext while suspended */
    CHECK( fake[1].count == 0 );
    resume_thread( &t );
    resume_thread( &t );
    resume_thread( &t );
    CHECK( t.suspend == 0 && t.hold == 0 );
    CHECK( fake[1].count == 0 );
    CHECK( fake[1].suspends == fake[1].resumes );
}

static void test_real_first_suspend_holds_nested_count_last_resume_releases(void)
{
    struct model_thread t = { 2, 0, 0, 0 };

    reset_fakes();
    real_suspend = 1;
    CHECK( t.hold == 0 );                /* initialised with the thread object */
    suspend_thread( &t );
    CHECK( t.hold == 1 );
    CHECK( fake[2].count == 1 );         /* one hold, the snapshot's pair already undone */
    suspend_thread( &t );
    suspend_thread( &t );
    CHECK( t.hold == 1 );
    CHECK( fake[2].count == 1 );         /* nested ones only count */
    resume_thread( &t );
    resume_thread( &t );
    CHECK( fake[2].count == 1 );         /* still suspended once */
    CHECK( t.hold == 1 );
    resume_thread( &t );
    CHECK( t.hold == 0 );
    CHECK( fake[2].count == 0 );         /* the last resume released exactly that hold */
    resume_thread( &t );                 /* a resume too many: no Mach call */
    CHECK( fake[2].count == 0 );
    CHECK( fake[2].suspends == fake[2].resumes );
}

static void test_snapshot_never_releases_the_hold(void)
{
    struct model_thread t = { 3, 0, 0, 0 };

    reset_fakes();
    real_suspend = 1;
    suspend_thread( &t );
    CHECK( fake[3].count == 1 );
    snapshot( &t );
    snapshot( &t );
    CHECK( fake[3].count == 1 );         /* each snapshot's resume undid only its own suspend */
    CHECK( t.hold == 1 );

    /* a snapshot whose thread_suspend fails must not resume: that would release the hold */
    fake[3].fail_suspend = 1;
    snapshot( &t );
    CHECK( fake[3].count == 1 );
    fake[3].fail_suspend = 0;

    resume_thread( &t );
    CHECK( fake[3].count == 0 );
}

static void test_failed_hold_is_not_released(void)
{
    struct model_thread t = { 4, 0, 0, 0 };
    int held = 0;

    reset_fakes();
    fake[4].fail_suspend = 1;
    CHECK( ios_hold_take( &ops, &held, 4, 1 ) == -1 );
    CHECK( held == 0 );
    ios_hold_release( &ops, &held, 4 );
    CHECK( fake[4].resumes == 0 );

    /* the same through the model: suspended, but nothing held and nothing resumed */
    real_suspend = 1;
    suspend_thread( &t );
    CHECK( t.suspend == 1 && t.hold == 0 );
    resume_thread( &t );
    CHECK( fake[4].resumes == 0 );
    CHECK( fake[4].count == 0 );
}

static void test_hold_take_and_release_are_0_or_1(void)
{
    int held = 0;

    reset_fakes();
    CHECK( ios_hold_take( &ops, &held, 5, 0 ) == 0 );    /* default mode: nothing to do */
    CHECK( held == 0 && fake[5].suspends == 0 );
    CHECK( ios_hold_take( &ops, &held, 5, 1 ) == 1 );
    CHECK( held == 1 && fake[5].count == 1 );
    CHECK( ios_hold_take( &ops, &held, 5, 1 ) == 0 );    /* never a second hold */
    CHECK( held == 1 && fake[5].count == 1 );
    ios_hold_release( &ops, &held, 5 );
    CHECK( held == 0 && fake[5].count == 0 );
    ios_hold_release( &ops, &held, 5 );                  /* e.g. cleanup after resume */
    CHECK( held == 0 && fake[5].resumes == 1 );
}

static void test_thread_and_process_suspension_share_one_hold(void)
{
    struct model_thread t = { 6, 0, 0, 0 };

    reset_fakes();
    real_suspend = 1;
    suspend_thread( &t );
    suspend_process( &t );               /* already suspended: no second hold */
    CHECK( fake[6].count == 1 );
    resume_thread( &t );                 /* still suspended by its process */
    CHECK( t.hold == 1 && fake[6].count == 1 );
    resume_process( &t );
    CHECK( t.hold == 0 && fake[6].count == 0 );

    suspend_process( &t );               /* the other order */
    suspend_thread( &t );
    CHECK( fake[6].count == 1 );
    resume_process( &t );
    CHECK( fake[6].count == 1 );
    resume_thread( &t );
    CHECK( t.hold == 0 && fake[6].count == 0 );
}

/* ---- inside a system call, and the fingerprint ---- */

static void test_in_syscall(void)
{
    const uint64_t kstack = 0x100000000ull, frame = kstack + 0x100000 - 0x330;

    CHECK( ios_in_syscall( frame - 0x40, kstack, frame ) );
    CHECK( ios_in_syscall( kstack, kstack, frame ) );
    CHECK( !ios_in_syscall( frame, kstack, frame ) );        /* storing or reloading the frame */
    CHECK( !ios_in_syscall( frame + 0x10, kstack, frame ) ); /* above it: Windows code */
    CHECK( !ios_in_syscall( kstack - 8, kstack, frame ) );
    CHECK( !ios_in_syscall( frame - 0x40, 0, frame ) );      /* TEB not set up */
    CHECK( !ios_in_syscall( frame - 0x40, kstack, 0 ) );
    CHECK( !ios_in_syscall( 0x50, frame, kstack ) );         /* nonsense bounds */
}

static void make_frame( struct ios_syscall_frame *f )
{
    int i;

    memset( f, 0, sizeof(*f) );
    for (i = 0; i < 29; i++) f->x[i] = 0x1000 + i;
    f->fp = 0x7000f000;
    f->lr = 0x18000aa00;
    f->sp = 0x7000ef80;
    f->pc = 0x18000ab40;
    f->cpsr = 0x60000000;
    f->syscall_id = 0x12;
    f->prev_frame = 0;
}

static void test_fingerprint_match(void)
{
    struct ios_syscall_frame f, g;
    struct ios_frame_mark mark;
    const uint64_t addr = 0x1000ff000ull;

    make_frame( &f );
    ios_frame_mark_set( &mark, addr, &f );
    CHECK( mark.frame == addr );

    g = f;
    CHECK( ios_frame_mark_match( &mark, addr, &g ) );

    /* volatile registers and the call's bookkeeping change during a call: still the same call */
    g.x[0] = 0xdead; g.x[9] = 0xbeef; g.syscall_id = 0x44; g.v[3].low = 1;
    CHECK( ios_frame_mark_match( &mark, addr, &g ) );

    g = f; CHECK( !ios_frame_mark_match( &mark, addr + 0x330, &g ) );  /* another frame */
    g = f; g.pc += 4; CHECK( !ios_frame_mark_match( &mark, addr, &g ) );
    g = f; g.sp -= 16; CHECK( !ios_frame_mark_match( &mark, addr, &g ) );
    g = f; g.fp += 16; CHECK( !ios_frame_mark_match( &mark, addr, &g ) );
    g = f; g.lr += 8; CHECK( !ios_frame_mark_match( &mark, addr, &g ) );

    /* not inside a system call at capture: nothing ever matches, even a zero frame */
    ios_frame_mark_clear( &mark );
    memset( &g, 0, sizeof(g) );
    CHECK( !ios_frame_mark_match( &mark, 0, &g ) );
    CHECK( !ios_frame_mark_match( &mark, addr, &f ) );
}

/* ---- native registers into a frame ---- */

static void make_regs( struct ios_arm64_regs *r )
{
    int i;

    memset( r, 0, sizeof(*r) );
    for (i = 0; i < 31; i++) r->x[i] = 0x5000 + i;
    r->sp = 0x6000e000;
    r->pc = 0x14000c000;
    r->pstate = 0x20000000;
    for (i = 0; i < 32; i++) { r->q[i].low = 0x700 + i; r->q[i].high = 0x800 + i; }
    r->fpcr = 0x03000000;
    r->fpsr = 0x10;
}

static void test_apply_native(void)
{
    struct ios_syscall_frame f, orig;
    struct ios_arm64_regs r;
    int i;

    make_frame( &orig );
    make_regs( &r );

    f = orig;
    ios_frame_apply_native( &f, &r, 0 );
    CHECK( !memcmp( &f, &orig, sizeof(f) ) );

    f = orig;
    ios_frame_apply_native( &f, &r, IOS_REGS_INTEGER );
    for (i = 0; i < 29; i++)
    {
        if (i == 18) CHECK( f.x[18] == orig.x[18] );   /* never x18 */
        else CHECK( f.x[i] == r.x[i] );
    }
    CHECK( f.fp == orig.fp && f.lr == orig.lr && f.sp == orig.sp && f.pc == orig.pc );
    CHECK( f.restore_flags == 0 );                     /* x0..x17 stay the call's results */

    f = orig;
    ios_frame_apply_native( &f, &r, IOS_REGS_CONTROL );
    CHECK( f.fp == r.x[29] && f.lr == r.x[30] && f.sp == r.sp && f.pc == r.pc && f.cpsr == r.pstate );
    CHECK( f.x[0] == orig.x[0] && f.x[18] == orig.x[18] );
    CHECK( f.restore_flags == IOS_FRAME_RESTORE_CONTROL );

    f = orig;
    ios_frame_apply_native( &f, &r, IOS_REGS_FLOATING_POINT );
    for (i = 0; i < 32; i++) CHECK( f.v[i].low == r.q[i].low && f.v[i].high == r.q[i].high );
    CHECK( f.fpcr == r.fpcr && f.fpsr == r.fpsr );
    CHECK( f.restore_flags == IOS_FRAME_RESTORE_FLOATING_POINT );
    CHECK( f.pc == orig.pc );

    f = orig;
    ios_frame_apply_native( &f, &r, IOS_REGS_CONTROL | IOS_REGS_INTEGER | IOS_REGS_FLOATING_POINT );
    CHECK( f.x[18] == orig.x[18] );
    CHECK( f.restore_flags == (IOS_FRAME_RESTORE_CONTROL | IOS_FRAME_RESTORE_FLOATING_POINT) );
    /* the dispatcher's bookkeeping is never touched */
    CHECK( f.prev_frame == orig.prev_frame && f.syscall_cfa == orig.syscall_cfa );
    CHECK( f.syscall_id == orig.syscall_id && f.align == orig.align );
}

/* the byte ranges written back to a frame skip x18 and the bookkeeping fields */
static void test_frame_write_ranges(void)
{
    CHECK( IOS_FRAME_WRITE1_END == offsetof(struct ios_syscall_frame, x[18]) );
    CHECK( IOS_FRAME_WRITE2_START == offsetof(struct ios_syscall_frame, x[19]) );
    CHECK( IOS_FRAME_WRITE2_END == offsetof(struct ios_syscall_frame, prev_frame) );
    CHECK( IOS_FRAME_WRITE3_START == offsetof(struct ios_syscall_frame, fpcr) );
    CHECK( IOS_FRAME_WRITE3_END == sizeof(struct ios_syscall_frame) );
}

int main(void)
{
    test_default_mode_only_counts();
    test_real_first_suspend_holds_nested_count_last_resume_releases();
    test_snapshot_never_releases_the_hold();
    test_failed_hold_is_not_released();
    test_hold_take_and_release_are_0_or_1();
    test_thread_and_process_suspension_share_one_hold();
    test_in_syscall();
    test_fingerprint_match();
    test_apply_native();
    test_frame_write_ranges();

    if (failures)
    {
        fprintf( stderr, "test_ios_suspend: %d failure(s)\n", failures );
        return 1;
    }
    printf( "test_ios_suspend: all passed\n" );
    return 0;
}
