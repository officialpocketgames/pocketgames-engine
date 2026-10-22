// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 16 Part E: an in-process ntsync "driver". Upstream Wine 11.4 can run its synchronization on
// Linux's ntsync driver: the server creates the objects, hands each client a descriptor, and every
// wait and signal is one ioctl. iOS has no such driver, but the server is a thread of this app, so
// both sides call this library instead. Semantics follow the kernel's ntsync documentation.
//
// One mutex guards all object state (E6). Descriptors are 0x70000000 | index into a table of
// object pointers that grows by doubling and is read without the lock (the table is published
// before the capacity, readers load the capacity first, old tables are never freed, and an
// object's memory stays with its slot for the life of the app). Waiters sleep on their own
// Mach semaphore, in a record on the heap kept in thread-specific data.

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
# include <mach/mach.h>
# include <mach/semaphore.h>
#else
# include <semaphore.h>
#endif

#include "ios_sync.h"
#include "linux/ntsync.h"

enum obj_type { OBJ_FREE, OBJ_SEM, OBJ_MUTEX, OBJ_EVENT };

struct obj;
struct waiter;

struct entry   /* a waiter queued on an object, in arrival order */
{
    struct entry  *prev, *next;
    struct waiter *waiter;
    struct obj    *obj;
};

/* fields read without the lock (type, count, owner, signaled) are 32-bit and accessed with
 * relaxed atomics; everything else only under the lock */
struct obj
{
    uint32_t     type;
    uint32_t     refs;
    uint32_t     count;      /* semaphore count, mutex recursion count */
    uint32_t     max;        /* semaphore */
    uint32_t     owner;      /* mutex */
    uint32_t     abandoned;  /* mutex */
    uint32_t     manual;     /* event */
    uint32_t     signaled;   /* event */
    uint32_t     index;      /* its slot */
    uint32_t     next_free;
    struct entry waiters;    /* list head */
};

struct waiter
{
#ifdef __APPLE__
    semaphore_t   sem;
#else
    sem_t         sem;
#endif
    int           active;    /* holds references on objs / alert */
    int           queued;    /* its entries are on the objects' lists */
    int           completed; /* set by a waker, under the lock */
    int           all;
    uint32_t      count;
    uint32_t      owner;
    uint32_t      index;     /* result */
    int           error;     /* result: 0 or EOWNERDEAD */
    struct obj   *objs[NTSYNC_MAX_WAIT_COUNT];
    struct obj   *alert;
    struct entry  entries[NTSYNC_MAX_WAIT_COUNT + 1];
};

#define RD(f)     __atomic_load_n( &(f), __ATOMIC_RELAXED )
#define WR(f, v)  __atomic_store_n( &(f), (v), __ATOMIC_RELAXED )

#define INITIAL_CAPACITY  4096
#define INDEX_LIMIT       0x0fffffffu
#define MAX_POSTS         256

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct obj **table;
static uint32_t capacity;
static uint32_t bump = 1;        /* index 0 is never used: an alert of 0 means none */
static uint32_t free_head;       /* 0: empty */

struct post
{
    uint32_t pid, token;
    int      fd;
};
static struct post posts[MAX_POSTS];

static pthread_key_t waiter_key;
static pthread_once_t waiter_once = PTHREAD_ONCE_INIT;

static inline void cpu_yield(void)
{
#if defined(__aarch64__)
    __asm__ __volatile__( "yield" ::: "memory" );
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

static int is_ours( int fd )
{
    return ((uint32_t)fd & 0xf0000000u) == IOS_USYNC_FD_TAG;
}

/* ---- the lock (E6) ---- */

static void lock_enter( sigset_t *old )
{
    unsigned int i, spins = ios_sync_settings()->lock_spin;
    sigset_t all;

    /* a Windows thread can be ended from a signal handler: never with the lock held */
    sigfillset( &all );
    pthread_sigmask( SIG_BLOCK, &all, old );
    for (i = 0; i < spins; i++)
    {
        if (!pthread_mutex_trylock( &lock )) return;
        cpu_yield();
    }
    pthread_mutex_lock( &lock );
}

static void lock_leave( const sigset_t *old )
{
    pthread_mutex_unlock( &lock );
    pthread_sigmask( SIG_SETMASK, old, NULL );
}

/* ---- the table (E2) ---- */

static struct obj *lookup( int fd )   /* with or without the lock */
{
    uint32_t idx = (uint32_t)fd & INDEX_LIMIT, cap;
    struct obj **tbl, *obj;

    if (!is_ours( fd ) || fd == IOS_USYNC_DEVICE) return NULL;
    cap = __atomic_load_n( &capacity, __ATOMIC_ACQUIRE );
    tbl = __atomic_load_n( &table, __ATOMIC_ACQUIRE );
    if (!idx || idx >= cap) return NULL;
    obj = __atomic_load_n( &tbl[idx], __ATOMIC_ACQUIRE );
    if (!obj || RD( obj->type ) == OBJ_FREE) return NULL;
    return obj;
}

static int grow( uint32_t need )   /* locked */
{
    uint32_t cap = capacity ? capacity : INITIAL_CAPACITY / 2, i;
    struct obj **tbl;

    while (cap <= need) cap *= 2;
    if (!(tbl = calloc( cap, sizeof(*tbl) ))) return 0;
    for (i = 0; i < capacity; i++) tbl[i] = table[i];
    /* the new table before the new capacity; the old one is never freed (readers) */
    __atomic_store_n( &table, tbl, __ATOMIC_RELEASE );
    __atomic_store_n( &capacity, cap, __ATOMIC_RELEASE );
    return 1;
}

static struct obj *alloc_obj( uint32_t type, uint32_t *ret_idx )   /* locked */
{
    struct obj *obj;
    uint32_t idx;

    if (free_head)
    {
        idx = free_head;
        obj = table[idx];
        free_head = obj->next_free;
    }
    else
    {
        if (bump >= INDEX_LIMIT) return NULL;
        if (bump >= capacity && !grow( bump )) return NULL;
        if (!(obj = calloc( 1, sizeof(*obj) ))) return NULL;
        idx = bump++;
        obj->index = idx;
        __atomic_store_n( &table[idx], obj, __ATOMIC_RELEASE );
    }
    obj->refs = 1;   /* the creator's */
    obj->count = obj->max = obj->owner = obj->abandoned = obj->manual = obj->signaled = 0;
    obj->waiters.prev = obj->waiters.next = &obj->waiters;
    WR( obj->type, type );
    *ret_idx = idx;
    return obj;
}

static void unref( struct obj *obj )   /* locked */
{
    if (--obj->refs) return;
    /* nothing waits on it (every wait holds a reference): the slot can be reused */
    WR( obj->type, OBJ_FREE );
    obj->next_free = free_head;
    free_head = obj->index;
}

/* ---- readiness ---- */

static int ready_for( const struct obj *obj, uint32_t owner )   /* locked */
{
    switch (obj->type)
    {
    case OBJ_SEM:   return obj->count > 0;
    case OBJ_MUTEX: return !obj->owner || (obj->owner == owner && obj->count < UINT32_MAX);
    case OBJ_EVENT: return obj->signaled;
    }
    return 0;
}

static int looks_ready( struct obj *obj, uint32_t owner )   /* without the lock: a hint */
{
    switch (RD( obj->type ))
    {
    case OBJ_SEM:   return RD( obj->count ) > 0;
    case OBJ_MUTEX: { uint32_t o = RD( obj->owner ); return !o || o == owner; }
    case OBJ_EVENT: return RD( obj->signaled ) != 0;
    }
    return 1;   /* freed: let the locked path report it */
}

/* take the object for a waiter; 1 when it was an abandoned mutex */
static int acquire( struct obj *obj, uint32_t owner )   /* locked */
{
    int abandoned = 0;

    switch (obj->type)
    {
    case OBJ_SEM:
        WR( obj->count, obj->count - 1 );
        break;
    case OBJ_MUTEX:
        if (obj->abandoned)
        {
            abandoned = 1;   /* reported once */
            obj->abandoned = 0;
        }
        WR( obj->owner, owner );
        WR( obj->count, obj->count + 1 );
        break;
    case OBJ_EVENT:
        if (!obj->manual) WR( obj->signaled, 0 );
        break;
    }
    return abandoned;
}

/* ---- waiters ---- */

static void dequeue( struct waiter *w )   /* locked */
{
    uint32_t i, n = w->count + (w->alert ? 1 : 0);

    if (!w->queued) return;
    for (i = 0; i < n; i++)
    {
        struct entry *e = &w->entries[i];
        e->prev->next = e->next;
        e->next->prev = e->prev;
        e->prev = e->next = e;
    }
    w->queued = 0;
}

static void release_refs( struct waiter *w )   /* locked */
{
    uint32_t i;

    if (!w->active) return;
    for (i = 0; i < w->count; i++) unref( w->objs[i] );
    if (w->alert) unref( w->alert );
    w->active = 0;
}

/* complete the wait if it can be, taking what it waits for; 1 when completed */
static int try_complete( struct waiter *w )   /* locked */
{
    uint32_t i;
    int abandoned = 0;

    if (!w->all)
    {
        /* the first ready object in array order */
        for (i = 0; i < w->count; i++)
        {
            if (!ready_for( w->objs[i], w->owner )) continue;
            w->index = i;
            w->error = acquire( w->objs[i], w->owner ) ? EOWNERDEAD : 0;
            return 1;
        }
    }
    else
    {
        /* all of them atomically, or none */
        for (i = 0; i < w->count; i++) if (!ready_for( w->objs[i], w->owner )) break;
        if (i == w->count)
        {
            for (i = 0; i < w->count; i++) abandoned |= acquire( w->objs[i], w->owner );
            w->index = 0;
            w->error = abandoned ? EOWNERDEAD : 0;
            return 1;
        }
    }
    if (w->alert && w->alert->signaled)
    {
        if (!w->alert->manual) WR( w->alert->signaled, 0 );
        w->index = w->count;
        w->error = 0;
        return 1;
    }
    return 0;
}

static void signal_waiter( struct waiter *w )   /* locked: the record can't go away meanwhile */
{
#ifdef __APPLE__
    semaphore_signal( w->sem );
#else
    sem_post( &w->sem );
#endif
}

static int obj_open( const struct obj *obj )   /* could some waiter take it now? */
{
    switch (obj->type)
    {
    case OBJ_SEM:   return obj->count > 0;
    case OBJ_MUTEX: return !obj->owner;   /* owners are threads: the owner itself isn't queued */
    case OBJ_EVENT: return obj->signaled;
    }
    return 0;
}

/* E1: the object may have become ready: complete its waiters in arrival order while it stays so */
static void wake_object( struct obj *obj )   /* locked */
{
    struct entry *e = obj->waiters.next;

    while (e != &obj->waiters && obj_open( obj ))
    {
        struct waiter *w = e->waiter;

        if (try_complete( w ))
        {
            dequeue( w );
            __atomic_store_n( &w->completed, 1, __ATOMIC_RELEASE );
            signal_waiter( w );
            e = obj->waiters.next;   /* the list changed */
        }
        else e = e->next;
    }
}

static void waiter_destroy( void *ptr )
{
    struct waiter *w = ptr;
    sigset_t old;

    /* the thread ends, maybe while queued: no waker may touch the record after this */
    lock_enter( &old );
    dequeue( w );
    release_refs( w );
    lock_leave( &old );
#ifdef __APPLE__
    semaphore_destroy( mach_task_self(), w->sem );
#else
    sem_destroy( &w->sem );
#endif
    free( w );
}

static void make_key(void)
{
    pthread_key_create( &waiter_key, waiter_destroy );
}

static struct waiter *get_waiter(void)
{
    struct waiter *w;

    pthread_once( &waiter_once, make_key );
    if ((w = pthread_getspecific( waiter_key ))) return w;
    if (!(w = calloc( 1, sizeof(*w) ))) return NULL;
#ifdef __APPLE__
    if (semaphore_create( mach_task_self(), &w->sem, SYNC_POLICY_FIFO, 0 ) != KERN_SUCCESS)
#else
    if (sem_init( &w->sem, 0, 0 ))
#endif
    {
        free( w );
        return NULL;
    }
    pthread_setspecific( waiter_key, w );
    return w;
}

/* ---- time ---- */

static uint64_t clock_now( int realtime )
{
    struct timespec ts;
    clock_gettime( realtime ? CLOCK_REALTIME : CLOCK_MONOTONIC, &ts );
    return (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec;
}

/* sleep on the waiter's semaphore for at most ns (UINT64_MAX: no limit) */
static void sleep_for( struct waiter *w, uint64_t ns )
{
#ifdef __APPLE__
    if (ns == UINT64_MAX) semaphore_wait( w->sem );
    else
    {
        mach_timespec_t ts;
        uint64_t sec = ns / 1000000000u;
        ts.tv_sec = sec > 0x7fffffff ? 0x7fffffff : (unsigned int)sec;
        ts.tv_nsec = (clock_res_t)(ns % 1000000000u);
        semaphore_timedwait( w->sem, ts );
    }
#else
    if (ns == UINT64_MAX) sem_wait( &w->sem );
    else
    {
        struct timespec ts;
        uint64_t when;
        clock_gettime( CLOCK_REALTIME, &ts );
        when = (uint64_t)ts.tv_sec * 1000000000u + ts.tv_nsec + ns;
        ts.tv_sec = when / 1000000000u;
        ts.tv_nsec = when % 1000000000u;
        sem_timedwait( &w->sem, &ts );
    }
#endif
}

/* ---- E1: the operations ---- */

static int fail( int err )
{
    errno = err;
    return -1;
}

static int create( uint32_t type, void *arg )
{
    struct obj *obj;
    uint32_t idx, a = 0, b = 0;
    sigset_t old;

    if (!arg) return fail( EFAULT );
    switch (type)
    {
    case OBJ_SEM:
    {
        struct ntsync_sem_args *args = arg;
        if (args->count > args->max) return fail( EINVAL );
        a = args->count; b = args->max;
        break;
    }
    case OBJ_MUTEX:
    {
        struct ntsync_mutex_args *args = arg;
        if (!args->owner != !args->count) return fail( EINVAL );
        a = args->owner; b = args->count;
        break;
    }
    case OBJ_EVENT:
    {
        struct ntsync_event_args *args = arg;
        a = !!args->manual; b = !!args->signaled;
        break;
    }
    }

    lock_enter( &old );
    if (!(obj = alloc_obj( type, &idx )))
    {
        lock_leave( &old );
        return fail( ENOMEM );
    }
    switch (type)
    {
    case OBJ_SEM:   WR( obj->count, a ); obj->max = b; break;
    case OBJ_MUTEX: WR( obj->owner, a ); WR( obj->count, b ); break;
    case OBJ_EVENT: obj->manual = a; WR( obj->signaled, b ); break;
    }
    lock_leave( &old );
    return (int)(IOS_USYNC_FD_TAG | idx);
}

static int object_op( struct obj *obj, unsigned long request, void *arg )   /* locked */
{
    switch (request)
    {
    case NTSYNC_IOC_SEM_RELEASE:
    {
        __u32 *count = arg, prev;
        if (obj->type != OBJ_SEM) return fail( EINVAL );
        if ((uint64_t)obj->count + *count > obj->max) return fail( EOVERFLOW );
        prev = obj->count;
        WR( obj->count, obj->count + *count );
        *count = prev;
        wake_object( obj );
        return 0;
    }
    case NTSYNC_IOC_SEM_READ:
    {
        struct ntsync_sem_args *args = arg;
        if (obj->type != OBJ_SEM) return fail( EINVAL );
        args->count = obj->count;
        args->max = obj->max;
        return 0;
    }
    case NTSYNC_IOC_MUTEX_UNLOCK:
    {
        struct ntsync_mutex_args *args = arg;
        if (obj->type != OBJ_MUTEX) return fail( EINVAL );
        if (!args->owner) return fail( EINVAL );
        if (obj->owner != args->owner) return fail( EPERM );
        args->count = obj->count;
        WR( obj->count, obj->count - 1 );
        if (!obj->count)
        {
            WR( obj->owner, 0 );
            wake_object( obj );
        }
        return 0;
    }
    case NTSYNC_IOC_MUTEX_KILL:
    {
        __u32 *owner = arg;
        if (obj->type != OBJ_MUTEX) return fail( EINVAL );
        if (!*owner) return fail( EINVAL );
        if (obj->owner != *owner) return fail( EPERM );
        obj->abandoned = 1;
        WR( obj->owner, 0 );
        WR( obj->count, 0 );
        wake_object( obj );
        return 0;
    }
    case NTSYNC_IOC_MUTEX_READ:
    {
        struct ntsync_mutex_args *args = arg;
        if (obj->type != OBJ_MUTEX) return fail( EINVAL );
        if (obj->abandoned)
        {
            args->owner = args->count = 0;
            return fail( EOWNERDEAD );
        }
        args->owner = obj->owner;
        args->count = obj->count;
        return 0;
    }
    case NTSYNC_IOC_EVENT_SET:
    case NTSYNC_IOC_EVENT_RESET:
    case NTSYNC_IOC_EVENT_PULSE:
    {
        __u32 *prev = arg, old;
        if (obj->type != OBJ_EVENT) return fail( EINVAL );
        old = obj->signaled;
        WR( obj->signaled, request != NTSYNC_IOC_EVENT_RESET );
        if (request != NTSYNC_IOC_EVENT_RESET) wake_object( obj );
        /* a pulse releases only the threads waiting now */
        if (request == NTSYNC_IOC_EVENT_PULSE) WR( obj->signaled, 0 );
        if (prev) *prev = old;
        return 0;
    }
    case NTSYNC_IOC_EVENT_READ:
    {
        struct ntsync_event_args *args = arg;
        if (obj->type != OBJ_EVENT) return fail( EINVAL );
        args->manual = obj->manual;
        args->signaled = obj->signaled;
        return 0;
    }
    }
    return fail( ENOTTY );
}

/* without the lock: 0 only when the arguments are valid and nothing looks ready */
static int maybe_ready( const int *fds, uint32_t n, int all, uint32_t owner, int alert )
{
    struct obj *obj;
    uint32_t i, j;

    if (!owner) return 1;
    for (i = 0; i < n; i++)
    {
        if (!(obj = lookup( fds[i] ))) return 1;
        if (looks_ready( obj, owner )) return 1;
        if (all && fds[i] == alert) return 1;
        if (all) for (j = 0; j < i; j++) if (fds[j] == fds[i]) return 1;
    }
    if (alert)
    {
        if (!(obj = lookup( alert )) || RD( obj->type ) != OBJ_EVENT) return 1;
        if (looks_ready( obj, 0 )) return 1;
    }
    return 0;
}

static int wait_objs( struct ntsync_wait_args *args, int all )
{
    int fds[NTSYNC_MAX_WAIT_COUNT];
    struct waiter *w;
    uint32_t i, j, n;
    uint64_t now;
    int infinite, realtime, ret = 0;
    sigset_t old;

    if (!args) return fail( EFAULT );
    if (args->pad || (args->flags & ~NTSYNC_WAIT_REALTIME)) return fail( EINVAL );
    if (args->count > NTSYNC_MAX_WAIT_COUNT) return fail( EINVAL );
    n = args->count;
    if (n) memcpy( fds, (const void *)(uintptr_t)args->objs, n * sizeof(int) );
    infinite = args->timeout == ~(__u64)0;
    realtime = !!(args->flags & NTSYNC_WAIT_REALTIME);

    /* E6: polls without the lock. The caller holds a reference on each object; the words are
     * 32-bit; a stale read only sends the call to the locked path, and so does anything the
     * locked path would refuse. */
    if (!infinite && clock_now( realtime ) >= args->timeout)
    {
        if (!maybe_ready( fds, n, all, args->owner, (int)args->alert )) return fail( ETIMEDOUT );
    }
    else
    {
        /* optional: spin on the words without the lock before sleeping */
        unsigned int spin_us = ios_sync_settings()->spin_us;
        if (spin_us)
        {
            uint64_t until = ios_fs_now_ns() + spin_us * 1000ull;
            while (!maybe_ready( fds, n, all, args->owner, (int)args->alert ) && ios_fs_now_ns() < until)
                cpu_yield();
        }
    }

    if (!(w = get_waiter())) return fail( ENOMEM );

    lock_enter( &old );
    /* a previous wait of this thread left by a longjmp or a handler: clean it up first */
    dequeue( w );
    release_refs( w );

    for (i = 0; i < n; i++)
    {
        if (!(w->objs[i] = lookup( fds[i] ))) { ret = EBADF; break; }
        if (all) for (j = 0; j < i; j++) if (w->objs[j] == w->objs[i]) ret = EINVAL;
        if (ret) break;
    }
    w->alert = NULL;
    if (!ret && args->alert)
    {
        if (!(w->alert = lookup( (int)args->alert ))) ret = EBADF;
        else if (w->alert->type != OBJ_EVENT) ret = EINVAL;
    }
    if (!ret && all && w->alert)
        for (i = 0; i < n; i++) if (w->objs[i] == w->alert) ret = EINVAL;
    if (!ret && !args->owner) ret = EINVAL;
    if (ret)
    {
        lock_leave( &old );
        return fail( ret );
    }

    /* E3: a wait holds a reference on each object, so a close meanwhile can't free it */
    for (i = 0; i < n; i++) w->objs[i]->refs++;
    if (w->alert) w->alert->refs++;
    w->active = 1;
    w->count = n;
    w->all = all;
    w->owner = args->owner;
    w->completed = 0;

    if (try_complete( w )) goto done;
    if (!infinite && clock_now( realtime ) >= args->timeout)
    {
        ret = ETIMEDOUT;   /* a poll that finds nothing returns without queueing */
        goto done;
    }

    /* queue on every object, in arrival order */
    for (i = 0; i < n + (w->alert ? 1 : 0); i++)
    {
        struct entry *e = &w->entries[i];
        struct obj *obj = i < n ? w->objs[i] : w->alert;
        e->waiter = w;
        e->obj = obj;
        e->prev = obj->waiters.prev;
        e->next = &obj->waiters;
        obj->waiters.prev->next = e;
        obj->waiters.prev = e;
    }
    w->queued = 1;

    for (;;)
    {
        uint64_t left = UINT64_MAX;

        if (!infinite)
        {
            now = clock_now( realtime );
            left = now < args->timeout ? args->timeout - now : 0;
        }
        lock_leave( &old );
        if (left) sleep_for( w, left );
        lock_enter( &old );

        /* signalled, aborted, timed out, or a stale signal of an earlier wait: look again */
        if (__atomic_load_n( &w->completed, __ATOMIC_ACQUIRE )) break;
        if (try_complete( w ))
        {
            dequeue( w );
            break;
        }
        if (!infinite && clock_now( realtime ) >= args->timeout)
        {
            dequeue( w );
            ret = ETIMEDOUT;
            break;
        }
    }

done:
    release_refs( w );
    lock_leave( &old );
    if (ret) return fail( ret );
    args->index = w->index;
    if (w->error) return fail( w->error );
    return 0;
}

/* ---- the entry points ---- */

int ios_usync_ioctl( int fd, unsigned long request, void *arg )
{
    struct obj *obj;
    sigset_t old;
    int ret;

    if (fd == IOS_USYNC_DEVICE)
    {
        switch (request)
        {
        case NTSYNC_IOC_CREATE_SEM:   return create( OBJ_SEM, arg );
        case NTSYNC_IOC_CREATE_MUTEX: return create( OBJ_MUTEX, arg );
        case NTSYNC_IOC_CREATE_EVENT: return create( OBJ_EVENT, arg );
        case NTSYNC_IOC_WAIT_ANY:     return wait_objs( arg, 0 );
        case NTSYNC_IOC_WAIT_ALL:     return wait_objs( arg, 1 );
        }
        return fail( ENOTTY );
    }
    if (!is_ours( fd )) return ioctl( fd, request, arg );
    if (!arg) return fail( EFAULT );

    lock_enter( &old );
    if (!(obj = lookup( fd ))) ret = fail( EBADF );
    else ret = object_op( obj, request, arg );
    if (ret < 0)
    {
        int err = errno;
        lock_leave( &old );
        errno = err;
        return ret;
    }
    lock_leave( &old );
    return ret;
}

int ios_usync_close( int fd )
{
    struct obj *obj;
    sigset_t old;

    if (fd == IOS_USYNC_DEVICE) return 0;   /* closing the device does nothing */
    if (!is_ours( fd )) return close( fd );

    lock_enter( &old );
    if (!(obj = lookup( fd )))
    {
        lock_leave( &old );
        return fail( EBADF );
    }
    unref( obj );
    lock_leave( &old );
    return 0;
}

int ios_usync_post( unsigned int pid, unsigned int token, int fd )
{
    struct obj *obj, *prev;
    sigset_t old;
    int i, slot = -1;

    lock_enter( &old );
    if (!(obj = lookup( fd )))
    {
        lock_leave( &old );
        return fail( EBADF );
    }
    for (i = 0; i < MAX_POSTS; i++)
    {
        if (posts[i].fd && posts[i].pid == pid && posts[i].token == token)
        {
            /* a second post for the same pair replaces the first */
            if ((prev = lookup( posts[i].fd ))) unref( prev );
            slot = i;
            break;
        }
        if (!posts[i].fd && slot < 0) slot = i;
    }
    if (slot < 0)
    {
        lock_leave( &old );
        return fail( ENOSPC );
    }
    obj->refs++;   /* the hand-off's reference, the client's from now on */
    posts[slot].pid = pid;
    posts[slot].token = token;
    posts[slot].fd = fd;
    lock_leave( &old );
    return 0;
}

int ios_usync_take( unsigned int pid, unsigned int token )
{
    sigset_t old;
    int i, fd = -1;

    lock_enter( &old );
    for (i = 0; i < MAX_POSTS; i++)
    {
        if (!posts[i].fd || posts[i].pid != pid || posts[i].token != token) continue;
        fd = posts[i].fd;
        posts[i].fd = 0;
        break;
    }
    lock_leave( &old );
    return fd;
}
