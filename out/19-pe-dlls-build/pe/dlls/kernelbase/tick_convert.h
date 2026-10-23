/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 the MYIOSDECK author */

/*
 * Spec 19 Part A1: the tick count from the performance counter, the arithmetic.
 *
 * A counter value at a frequency is converted to 100 ns units. At 10 MHz (Wine's frequency) or
 * with no frequency the counter already is in those units. Otherwise the whole seconds and the
 * remainder are converted separately, so no intermediate product can overflow: the result is
 * floor(counter * 10^7 / frequency), reduced modulo 2^64 when it doesn't fit. Pure code shared
 * by sync.c and the host tests.
 */

#ifndef MYIOSDECK_TICK_CONVERT_H
#define MYIOSDECK_TICK_CONVERT_H

#define TICK_UNITS_PER_SEC   10000000ull   /* 100 ns units per second */
#define TICK_UNITS_PER_MS    10000ull

/* floor(rem * TICK_UNITS_PER_SEC / freq) for rem < freq, without a wider type: binary long
 * multiplication that keeps the running product as quotient and remainder by freq */
static inline unsigned long long tick_scale_remainder( unsigned long long rem, unsigned long long freq )
{
    unsigned long long q = 0, r = 0;   /* q * freq + r == the product so far, r < freq */
    int bit;

    for (bit = 23; bit >= 0; bit--)    /* 10^7 < 2^24 */
    {
        /* double: q*2, r*2 (r < freq, so compare against freq - r instead of adding) */
        q <<= 1;
        if (r >= freq - r) { r -= freq - r; q++; }
        else r += r;
        if ((TICK_UNITS_PER_SEC >> bit) & 1)
        {
            /* add rem (< freq) */
            if (r >= freq - rem) { r -= freq - rem; q++; }
            else r += rem;
        }
    }
    return q;
}

/* the counter in 100 ns units */
static inline unsigned long long tick_counter_to_100ns( unsigned long long counter, unsigned long long freq )
{
    unsigned long long whole, rem;

    if (freq == TICK_UNITS_PER_SEC || !freq) return counter;
    whole = counter / freq;
    rem = counter % freq;
    /* rem * 10^7 fits in 64 bits for any frequency below 2^64 / 10^7 (about 1.8 THz) */
    if (freq <= ~0ull / TICK_UNITS_PER_SEC)
        return whole * TICK_UNITS_PER_SEC + rem * TICK_UNITS_PER_SEC / freq;
    return whole * TICK_UNITS_PER_SEC + tick_scale_remainder( rem, freq );
}

/* GetTickCount64 from the value in 100 ns units */
static inline unsigned long long tick_100ns_to_ms( unsigned long long t )
{
    return t / TICK_UNITS_PER_MS;
}

/* GetTickCount: the low 32 bits of the milliseconds */
static inline unsigned int tick_100ns_to_ms32( unsigned long long t )
{
    return (unsigned int)(tick_100ns_to_ms( t ) & 0xffffffffu);
}

#endif /* MYIOSDECK_TICK_CONVERT_H */
