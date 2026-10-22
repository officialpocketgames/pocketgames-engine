// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 16 Part G: pacing threads. G1, the adaptive yield: a thread that yields in a tight loop
// sleeps a little instead. G2, eco mode: the app's UI switches every Windows thread to a lower QoS
// class; a thread can only set its own class, so each one applies the current choice from a
// generation number when it next waits or yields.

#include <pthread.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#ifdef __APPLE__
# include <pthread/qos.h>
#endif

#include "ios_sync.h"

/* ---- G1 ---- */

#define YIELD_STREAK_GAP_NS  2000000u   /* 2 ms */

unsigned int ios_yield_step( struct ios_yield_state *st, uint64_t now_ns, unsigned int streak_limit,
                             unsigned int sleep_us )
{
    if (st->last_ns && now_ns - st->last_ns < YIELD_STREAK_GAP_NS) st->streak++;
    else st->streak = 0;
    st->last_ns = now_ns;
    if (sleep_us && st->streak > streak_limit) return sleep_us;
    return 0;
}

static _Thread_local struct ios_yield_state yield_state;

unsigned int ios_yield_decide(void)
{
    static int logged;
    const struct ios_sync_settings *s = ios_sync_settings();

    if (!__atomic_exchange_n( &logged, 1, __ATOMIC_RELAXED ))
        ios_sync_log( "yield: streak %u, sleep %u us", s->yield_streak, s->yield_sleep_us );
    return ios_yield_step( &yield_state, ios_fs_now_ns(), s->yield_streak, s->yield_sleep_us );
}

/* ---- G2 ---- */

static int eco_on;
static unsigned int eco_gen;   /* 0 until initialised, then 1, +1 on every switch */
static pthread_once_t eco_once = PTHREAD_ONCE_INIT;

static void eco_init(void)
{
    __atomic_store_n( &eco_on, ios_sync_settings()->eco, __ATOMIC_RELAXED );
    __atomic_store_n( &eco_gen, 1, __ATOMIC_RELEASE );
}

void madeira_set_eco( int on )
{
    struct timeval tv;
    struct tm tm;
    char stamp[32] = "";
    unsigned int gen;

    pthread_once( &eco_once, eco_init );
    __atomic_store_n( &eco_on, !!on, __ATOMIC_RELAXED );
    gen = __atomic_add_fetch( &eco_gen, 1, __ATOMIC_RELEASE );
    gettimeofday( &tv, NULL );
    if (localtime_r( &tv.tv_sec, &tm ))
        snprintf( stamp, sizeof(stamp), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
                  (int)(tv.tv_usec / 1000) );
    ios_sync_log( "eco: %s at %s (generation %u)", on ? "on" : "off", stamp, gen );
}

int madeira_get_eco(void)
{
    pthread_once( &eco_once, eco_init );
    return __atomic_load_n( &eco_on, __ATOMIC_RELAXED );
}

unsigned int ios_eco_generation(void)
{
    pthread_once( &eco_once, eco_init );
    return __atomic_load_n( &eco_gen, __ATOMIC_ACQUIRE );
}

int ios_eco_qos_class( int on, int eco_qos )
{
    if (!on) return IOS_QOS_USER_INTERACTIVE;   /* spec 09 Part E1 */
    switch (eco_qos)
    {
    case IOS_ECO_QOS_BACKGROUND: return IOS_QOS_BACKGROUND;
    case IOS_ECO_QOS_INITIATED:  return IOS_QOS_USER_INITIATED;
    default:                     return IOS_QOS_UTILITY;
    }
}

int ios_eco_must_apply( unsigned int *applied_gen, unsigned int current_gen, int on )
{
    unsigned int prev = *applied_gen;

    if (prev == current_gen) return 0;
    *applied_gen = current_gen;
    /* a thread starts at USER_INTERACTIVE: with eco never switched on there is nothing to do */
    return prev || on;
}

int ios_eco_last_class = -1;   /* host tests: the class the last apply chose */

static _Thread_local unsigned int eco_applied;

void ios_eco_apply(void)
{
    unsigned int gen = __atomic_load_n( &eco_gen, __ATOMIC_ACQUIRE );
    int on, cls;

    if (gen && gen == eco_applied) return;   /* the common case: one load */
    gen = ios_eco_generation();
    on = madeira_get_eco();
    if (!ios_eco_must_apply( &eco_applied, gen, on )) return;
    cls = ios_eco_qos_class( on, ios_sync_settings()->eco_qos );
    ios_eco_last_class = cls;
#ifdef __APPLE__
    {
        static const qos_class_t classes[] = { QOS_CLASS_USER_INTERACTIVE, QOS_CLASS_USER_INITIATED,
                                               QOS_CLASS_UTILITY, QOS_CLASS_BACKGROUND };
        pthread_set_qos_class_self_np( classes[cls], 0 );
    }
#endif
}
