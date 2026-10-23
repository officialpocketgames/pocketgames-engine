// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author

/*
 * Spec 18 Part H7: QueryPerformanceCounter from the generic timer, the arithmetic.
 *
 * Counts of CNTVCT_EL0 at CNTFRQ_EL0 are converted to 100 ns units and shifted by an offset to
 * the system call's counter (same 10 MHz rate, different origin, and it keeps counting while the
 * device sleeps). The offset is measured when there is none, and again once at least one second
 * of counter time passed since the last measurement; a new offset is adopted only when it is the
 * first or larger than the current one by more than 1 ms, so it never moves backwards.
 * Pure code shared by time.c and the host tests.
 */

#ifndef MYIOSDECK_QPC_ARITH_H
#define MYIOSDECK_QPC_ARITH_H

#include <stdint.h>

#define QPC_UNITS_PER_SEC   10000000ull  /* 100 ns units */
#define QPC_ADOPT_MIN_STEP  10000        /* 1 ms in 100 ns units */

/* whole seconds times 10^7 plus the remainder scaled: no overflow for freq below ~1.8e12 */
static inline uint64_t qpc_ticks_to_100ns( uint64_t ticks, uint64_t freq )
{
    return (ticks / freq) * QPC_UNITS_PER_SEC + (ticks % freq) * QPC_UNITS_PER_SEC / freq;
}

/* system value minus the converted midpoint of the two counter reads around it */
static inline int64_t qpc_measured_offset( uint64_t before, int64_t system, uint64_t after, uint64_t freq )
{
    uint64_t mid = before + (after - before) / 2;
    return system - (int64_t)qpc_ticks_to_100ns( mid, freq );
}

/* is a measurement due? have: an offset exists; last: counter at the last measurement */
static inline int qpc_measure_due( int have, uint64_t last, uint64_t now, uint64_t freq )
{
    return !have || now - last >= freq;
}

/* should candidate replace current? */
static inline int qpc_adopt_offset( int have, int64_t current, int64_t candidate )
{
    return !have || candidate > current + QPC_ADOPT_MIN_STEP;
}

#endif /* MYIOSDECK_QPC_ARITH_H */
