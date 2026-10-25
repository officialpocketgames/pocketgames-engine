/*
 * Server-side semaphore management
 *
 * Copyright (C) 1998 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Changed for MYIOSDECK, 2026 (spec 16 Part C4: with PG_FASTSYNC_SEM a semaphore's count
 * lives in a shared cell, out/16-sync-ios/include/ios_fastsync.h). The changes sit under
 * WINE_IOS; out/08-wineserver-ios/CHANGES.md lists them ("spec 16").
 */

#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <sys/types.h>

#include "ntstatus.h"
#include "windef.h"
#include "winternl.h"

#include "handle.h"
#include "thread.h"
#include "request.h"
#include "security.h"

#ifdef WINE_IOS
#include "ios_sync.h"
#endif

static const WCHAR semaphore_name[] = {'S','e','m','a','p','h','o','r','e'};

struct type_descr semaphore_type =
{
    { semaphore_name, sizeof(semaphore_name) },   /* name */
    SEMAPHORE_ALL_ACCESS,                         /* valid_access */
    {                                             /* mapping */
        STANDARD_RIGHTS_READ | SEMAPHORE_QUERY_STATE,
        STANDARD_RIGHTS_WRITE | SEMAPHORE_MODIFY_STATE,
        STANDARD_RIGHTS_EXECUTE | SYNCHRONIZE,
        SEMAPHORE_ALL_ACCESS
    },
};

struct semaphore_sync
{
    struct object       obj;                /* object header */
    unsigned int        count;              /* current count */
    unsigned int        max;                /* maximum possible count */
#ifdef WINE_IOS
    int                 ios_cell;           /* spec 16: its cell, -1: none (count above rules) */
    unsigned int        ios_claims;         /* tokens taken by signaled, not yet satisfied */
#endif
};

static void semaphore_sync_dump( struct object *obj, int verbose );
static int semaphore_sync_signaled( struct object *obj, struct wait_queue_entry *entry );
static void semaphore_sync_satisfied( struct object *obj, struct wait_queue_entry *entry );
#ifdef WINE_IOS
static int semaphore_sync_add_queue( struct object *obj, struct wait_queue_entry *entry );
static void semaphore_sync_remove_queue( struct object *obj, struct wait_queue_entry *entry );
static void semaphore_sync_destroy( struct object *obj );
#endif

static const struct object_ops semaphore_sync_ops =
{
    sizeof(struct semaphore_sync), /* size */
    &no_type,                      /* type */
    semaphore_sync_dump,           /* dump */
#ifdef WINE_IOS
    semaphore_sync_add_queue,      /* add_queue */
    semaphore_sync_remove_queue,   /* remove_queue */
#else
    add_queue,                     /* add_queue */
    remove_queue,                  /* remove_queue */
#endif
    semaphore_sync_signaled,       /* signaled */
    semaphore_sync_satisfied,      /* satisfied */
    no_signal,                     /* signal */
    no_get_fd,                     /* get_fd */
    default_get_sync,              /* get_sync */
    default_map_access,            /* map_access */
    default_get_sd,                /* get_sd */
    default_set_sd,                /* set_sd */
    default_get_full_name,         /* get_full_name */
    no_lookup_name,                /* lookup_name */
    directory_link_name,           /* link_name */
    default_unlink_name,           /* unlink_name */
    no_open_file,                  /* open_file */
    no_kernel_obj_list,            /* get_kernel_obj_list */
    no_close_handle,               /* close_handle */
#ifdef WINE_IOS
    semaphore_sync_destroy         /* destroy */
#else
    no_destroy                     /* destroy */
#endif
};

static int release_semaphore( struct semaphore_sync *sem, unsigned int count,
                              unsigned int *prev )
{
#ifdef WINE_IOS
    if (sem->ios_cell >= 0)
    {
        unsigned int previous;

        switch (ios_fs_sem_release( sem->ios_cell, count, &previous ))
        {
        case IOS_FS_SEM_LIMIT:
            if (prev) *prev = previous;
            set_error( STATUS_SEMAPHORE_LIMIT_EXCEEDED );
            return 0;
        case IOS_FS_SEM_OK:
            if (prev) *prev = previous;
            sem->count = previous + count;   /* the field follows, as a fallback */
            /* clients add tokens without telling the server, so "the count was non-zero, nobody
             * waits" and a limit of count would leave queued threads asleep on tokens that are
             * there; each satisfied wait takes a real token from the cell (spec 16 C4) */
            wake_up( &sem->obj, 0 );
            return 1;
        }
        /* DISABLED: upstream's arithmetic on the field */
    }
#endif
    if (prev) *prev = sem->count;
    if (sem->count + count < sem->count || sem->count + count > sem->max)
    {
        set_error( STATUS_SEMAPHORE_LIMIT_EXCEEDED );
        return 0;
    }
    else if (sem->count)
    {
        /* there cannot be any thread to wake up if the count is != 0 */
        sem->count += count;
    }
    else
    {
        sem->count = count;
        wake_up( &sem->obj, count );
    }
    return 1;
}

#ifdef WINE_IOS
static unsigned int semaphore_sync_count( struct semaphore_sync *sem )
{
    int count;

    if (sem->ios_cell >= 0 && (count = ios_fs_sem_count( sem->ios_cell )) >= 0) return count;
    return sem->count;
}

static int semaphore_sync_add_queue( struct object *obj, struct wait_queue_entry *entry )
{
    struct semaphore_sync *sem = (struct semaphore_sync *)obj;
    assert( obj->ops == &semaphore_sync_ops );
    if (sem->ios_cell >= 0) ios_fs_srv_queue( sem->ios_cell, 1 );
    return add_queue( obj, entry );
}

static void semaphore_sync_remove_queue( struct object *obj, struct wait_queue_entry *entry )
{
    struct semaphore_sync *sem = (struct semaphore_sync *)obj;
    int cell = sem->ios_cell;   /* read first: removal can release the object */

    assert( obj->ops == &semaphore_sync_ops );
    remove_queue( obj, entry );
    if (cell >= 0) ios_fs_srv_queue( cell, -1 );
}

static void semaphore_sync_destroy( struct object *obj )
{
    struct semaphore_sync *sem = (struct semaphore_sync *)obj;
    assert( obj->ops == &semaphore_sync_ops );
    if (sem->ios_cell >= 0) ios_fs_srv_free( sem->ios_cell );
    sem->ios_cell = -1;
}

/* spec 16 C5: give back a claim a failed wait-all pass took (other objects: nothing) */
void ios_semaphore_sync_unclaim( struct object *obj )
{
    struct semaphore_sync *sem = (struct semaphore_sync *)obj;

    if (obj->ops != &semaphore_sync_ops || !sem->ios_claims) return;
    sem->ios_claims--;
    if (sem->ios_cell < 0 || ios_fs_sem_give_back( sem->ios_cell ) < 0) sem->count++;
}
#endif

static void semaphore_sync_dump( struct object *obj, int verbose )
{
    struct semaphore_sync *sem = (struct semaphore_sync *)obj;
    assert( obj->ops == &semaphore_sync_ops );
#ifdef WINE_IOS
    fprintf( stderr, "Semaphore count=%d max=%d cell=%d\n", semaphore_sync_count( sem ), sem->max, sem->ios_cell );
#else
    fprintf( stderr, "Semaphore count=%d max=%d\n", sem->count, sem->max );
#endif
}

static int semaphore_sync_signaled( struct object *obj, struct wait_queue_entry *entry )
{
    struct semaphore_sync *sem = (struct semaphore_sync *)obj;
    assert( obj->ops == &semaphore_sync_ops );
#ifdef WINE_IOS
    if (sem->ios_cell >= 0)
    {
        /* take the token now; satisfied consumes the claim, a failed wait-all gives it back */
        int ret = ios_fs_sem_take( sem->ios_cell );
        if (ret > 0) sem->ios_claims++;
        if (ret >= 0) return ret;
    }
#endif
    return (sem->count > 0);
}

static void semaphore_sync_satisfied( struct object *obj, struct wait_queue_entry *entry )
{
    struct semaphore_sync *sem = (struct semaphore_sync *)obj;
    assert( obj->ops == &semaphore_sync_ops );
#ifdef WINE_IOS
    if (sem->ios_claims)
    {
        /* the token is already out of the cell (and out of the count a disable folded) */
        sem->ios_claims--;
        if (sem->ios_cell >= 0 && ios_fs_sem_count( sem->ios_cell ) >= 0 && sem->count) sem->count--;
        return;
    }
    if (sem->ios_cell >= 0 && ios_fs_sem_take( sem->ios_cell ) >= 0)   /* not expected */
    {
        if (sem->count) sem->count--;   /* the fallback follows */
        return;
    }
#endif
    assert( sem->count );
    sem->count--;
}

static struct object *create_semaphore_sync( unsigned int initial, unsigned int max )
{
    struct semaphore_sync *sem;

    if (get_inproc_device_fd() >= 0) return (struct object *)create_inproc_semaphore_sync( initial, max );

    if (!(sem = alloc_object( &semaphore_sync_ops ))) return NULL;
    sem->count = initial;
    sem->max   = max;
#ifdef WINE_IOS
    sem->ios_claims = 0;
    sem->ios_cell = ios_fs_srv_sem_alloc( initial, max );   /* only with PG_FASTSYNC_SEM */
#endif
    return &sem->obj;
}

struct semaphore
{
    struct object          obj;    /* object header */
    struct object         *sync;   /* semaphore sync object */
};

static void semaphore_dump( struct object *obj, int verbose );
static struct object *semaphore_get_sync( struct object *obj );
static int semaphore_signal( struct object *obj, unsigned int access, int signal );
static void semaphore_destroy( struct object *obj );

static const struct object_ops semaphore_ops =
{
    sizeof(struct semaphore),      /* size */
    &semaphore_type,               /* type */
    semaphore_dump,                /* dump */
    NULL,                          /* add_queue */
    NULL,                          /* remove_queue */
    NULL,                          /* signaled */
    NULL,                          /* satisfied */
    semaphore_signal,              /* signal */
    no_get_fd,                     /* get_fd */
    semaphore_get_sync,            /* get_sync */
    default_map_access,            /* map_access */
    default_get_sd,                /* get_sd */
    default_set_sd,                /* set_sd */
    default_get_full_name,         /* get_full_name */
    no_lookup_name,                /* lookup_name */
    directory_link_name,           /* link_name */
    default_unlink_name,           /* unlink_name */
    no_open_file,                  /* open_file */
    no_kernel_obj_list,            /* get_kernel_obj_list */
    no_close_handle,               /* close_handle */
    semaphore_destroy,             /* destroy */
};

static struct semaphore *create_semaphore( struct object *root, const struct unicode_str *name,
                                           unsigned int attr, unsigned int initial, unsigned int max,
                                           const struct security_descriptor *sd )
{
    struct semaphore *sem;

    if (!max || (initial > max))
    {
        set_error( STATUS_INVALID_PARAMETER );
        return NULL;
    }
    if ((sem = create_named_object( root, &semaphore_ops, name, attr, sd )))
    {
        if (get_error() != STATUS_OBJECT_NAME_EXISTS)
        {
            /* initialize it if it didn't already exist */
            sem->sync = NULL;

            if (!(sem->sync = create_semaphore_sync( initial, max )))
            {
                release_object( sem );
                return NULL;
            }
        }
    }
    return sem;
}

#ifdef WINE_IOS
static struct semaphore_sync *get_semaphore_sync( struct object *obj )
{
    struct semaphore *sem = (struct semaphore *)obj;

    if (obj->ops != &semaphore_ops || sem->sync->ops != &semaphore_sync_ops) return NULL;
    return (struct semaphore_sync *)sem->sync;
}

/* spec 16 C7: the learn reply for a cell-backed semaphore (the client reads kind and max from
 * the cell); 0 when it has none */
int ios_semaphore_fastsync_type( struct object *obj, int *type )
{
    struct semaphore_sync *sync = get_semaphore_sync( obj );

    if (!sync || sync->ios_cell < 0 || ios_fs_sem_count( sync->ios_cell ) < 0) return 0;
    *type = (int)(IOS_FASTSYNC_REPLY_CELL | (unsigned int)sync->ios_cell);
    return 1;
}

/* spec 16 C4 / C7 (DISABLE_SEMAPHORE): out of the fast path for good, the count folded into
 * the field, parked clients woken; 1 when obj is a semaphore */
int ios_semaphore_fastsync_disable( struct object *obj )
{
    struct semaphore_sync *sync = get_semaphore_sync( obj );
    int count;

    if (!sync) return 0;
    if (sync->ios_cell >= 0 && (count = ios_fs_sem_disable( sync->ios_cell )) >= 0) sync->count = count;
    return 1;
}
#endif

static void semaphore_dump( struct object *obj, int verbose )
{
    struct semaphore *sem = (struct semaphore *)obj;
    assert( obj->ops == &semaphore_ops );
    sem->sync->ops->dump( sem->sync, verbose );
}

static struct object *semaphore_get_sync( struct object *obj )
{
    struct semaphore *sem = (struct semaphore *)obj;
    assert( obj->ops == &semaphore_ops );
    return grab_object( sem->sync );
}

static int semaphore_signal( struct object *obj, unsigned int access, int signal )
{
    struct semaphore *sem = (struct semaphore *)obj;
    assert( obj->ops == &semaphore_ops );

    assert( sem->sync->ops == &semaphore_sync_ops ); /* never called with inproc syncs */
    assert( signal == -1 ); /* always called from signal_object */

    if (!(access & SEMAPHORE_MODIFY_STATE))
    {
        set_error( STATUS_ACCESS_DENIED );
        return 0;
    }
    return release_semaphore( (struct semaphore_sync *)sem->sync, 1, NULL );
}

static void semaphore_destroy( struct object *obj )
{
    struct semaphore *sem = (struct semaphore *)obj;
    assert( obj->ops == &semaphore_ops );
    if (sem->sync) release_object( sem->sync );
}

/* create a semaphore */
DECL_HANDLER(create_semaphore)
{
    struct semaphore *sem;
    struct unicode_str name;
    struct object *root;
    const struct security_descriptor *sd;
    const struct object_attributes *objattr = get_req_object_attributes( &sd, &name, &root );

    if (!objattr) return;

    if ((sem = create_semaphore( root, &name, objattr->attributes, req->initial, req->max, sd )))
    {
        if (get_error() == STATUS_OBJECT_NAME_EXISTS)
            reply->handle = alloc_handle( current->process, sem, req->access, objattr->attributes );
        else
            reply->handle = alloc_handle_no_access_check( current->process, sem,
                                                          req->access, objattr->attributes );
        release_object( sem );
    }

    if (root) release_object( root );
}

/* open a handle to a semaphore */
DECL_HANDLER(open_semaphore)
{
    struct unicode_str name = get_req_unicode_str();

    reply->handle = open_object( current->process, req->rootdir, req->access,
                                 &semaphore_ops, &name, req->attributes );
}

/* release a semaphore */
DECL_HANDLER(release_semaphore)
{
    struct semaphore *sem;

    if ((sem = (struct semaphore *)get_handle_obj( current->process, req->handle,
                                                   SEMAPHORE_MODIFY_STATE, &semaphore_ops )))
    {
        struct semaphore_sync *sync = (struct semaphore_sync *)sem->sync;
        assert( sem->sync->ops == &semaphore_sync_ops ); /* never called with inproc syncs */

        /* spec 16 D4: a release of 0 is a client's "re-run your queue" request */
        release_semaphore( sync, req->count, &reply->prev_count );
        release_object( sem );
    }
}

/* query details about the semaphore */
DECL_HANDLER(query_semaphore)
{
    struct semaphore *sem;

    if ((sem = (struct semaphore *)get_handle_obj( current->process, req->handle,
                                                   SEMAPHORE_QUERY_STATE, &semaphore_ops )))
    {
        struct semaphore_sync *sync = (struct semaphore_sync *)sem->sync;
        assert( sem->sync->ops == &semaphore_sync_ops ); /* never called with inproc syncs */

#ifdef WINE_IOS
        reply->current = semaphore_sync_count( sync );
#else
        reply->current = sync->count;
#endif
        reply->max = sync->max;
        release_object( sem );
    }
}
