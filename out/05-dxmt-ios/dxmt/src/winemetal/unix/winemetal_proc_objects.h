// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 item 89: a program's Metal objects are released when its Wine process ends. In a
// reused session every program runs in the app's one process, and the Windows half of a program
// that is force-quit (or ends without releasing its device) never releases what winemetal's unix
// side handed it, so those objects stayed for the rest of the app run.
//
// The ledger counts, per pseudo-process (its PEB, `owner`), the references the Windows half
// holds on each object: +1 for each object a creating call hands it (the calls Metal.hpp wraps as
// owned), +1 for each retain, -1 for each release. Each record also holds one reference of its
// own, as spec 06 Part E's 32-bit table does, so the object's address can't be reused while it is
// recorded. When the program releases its last counted reference, the record goes and its own
// reference is given back right after: the object dies exactly when it would have.
//
// When the pseudo-process ends (ntdll's process-end hook, spec 22 C6), the ended process is taken
// out of the live list, its committed command buffers are waited for (at most a timeout), and its
// records are handed back in reverse creation order: each gives back the program's remaining
// references, never going below the references the tables themselves hold, then the record's
// own. A record of kind VIEW (a Metal view made for a window) has no reference count: it is
// handed to `release_view` once.
//
// Plain C with pthreads, no Apple or Wine types: objects and owners are opaque values, and retain,
// release and the retain count go through callbacks, so the host test drives it with fakes. One
// mutex guards everything; the callbacks other than `retain` are called outside it.

#ifndef DXMT_WINEMETAL_PROC_OBJECTS_H
#define DXMT_WINEMETAL_PROC_OBJECTS_H

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

enum wmp_kind {
  WMP_OBJECT = 0, /* an Objective-C object: retain / release */
  WMP_VIEW = 1,   /* a Metal view of a window: release_view, once */
};

struct wmp_record {
  uint64_t object; /* 0: free slot */
  uint64_t owner;  /* the pseudo-process (its PEB) */
  uint64_t count;  /* references the program holds (VIEW: 1) */
  uint64_t seq;    /* creation order, across all processes */
  uint8_t kind;
  uint8_t extra_own; /* set by the caller before give_back: other tables' own references */
};

struct wmp_ops {
  void (*retain)(uint64_t object);
  void (*release)(uint64_t object);
  uint64_t (*retain_count)(uint64_t object);
  void (*release_view)(uint64_t view);
};

/* A live pseudo-process: its owner value, its 32-bit window (0 for a 64-bit program), its
   command buffers committed and not yet completed, and the references to this struct (the live
   list, each in-flight command buffer, an end in progress). */
struct wmp_proc {
  uint64_t owner;
  uint64_t wow_base;
  uint32_t inflight;
  uint32_t refs;
  bool ended;
  struct wmp_proc *next;
};

struct wmp_state {
  pthread_mutex_t lock;
  pthread_cond_t cond; /* broadcast whenever an in-flight count drops */
  struct wmp_record *slots;
  uint32_t capacity; /* 0 or a power of two */
  uint32_t used;
  uint64_t next_seq;
  struct wmp_proc *procs; /* live ones */
  const struct wmp_ops *ops;
};

#define WMP_INITIAL_CAPACITY 1024u

#define WMP_STATE_INIT(ops_ptr) {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, 0, 0, 1, NULL, (ops_ptr)}

/* ---- the table (called with the lock held) ---- */

static inline uint32_t
wmp_home(uint32_t capacity, uint64_t object, uint64_t owner) {
  uint64_t h = (object >> 4) * 0x9E3779B97F4A7C15ull ^ (owner >> 4) * 0xC2B2AE3D27D4EB4Full;
  return (uint32_t)(h >> 32) & (capacity - 1);
}

static inline long
wmp_find(const struct wmp_state *s, uint64_t object, uint64_t owner) {
  if (!s->slots || !object)
    return -1;
  uint32_t mask = s->capacity - 1;
  for (uint32_t i = wmp_home(s->capacity, object, owner), n = 0; n < s->capacity; i = (i + 1) & mask, n++) {
    if (!s->slots[i].object)
      return -1;
    if (s->slots[i].object == object && s->slots[i].owner == owner)
      return (long)i;
  }
  return -1;
}

/* Linear-probing removal with backward shift (no tombstones). */
static inline void
wmp_remove_at(struct wmp_state *s, uint32_t hole) {
  uint32_t mask = s->capacity - 1;
  s->slots[hole].object = 0;
  s->used--;
  for (uint32_t i = (hole + 1) & mask; s->slots[i].object; i = (i + 1) & mask) {
    uint32_t home = wmp_home(s->capacity, s->slots[i].object, s->slots[i].owner);
    bool movable = hole <= i ? (home <= hole || home > i) : (home <= hole && home > i);
    if (movable) {
      s->slots[hole] = s->slots[i];
      s->slots[i].object = 0;
      hole = i;
    }
  }
}

static inline void
wmp_place(struct wmp_record *slots, uint32_t capacity, const struct wmp_record *r) {
  uint32_t mask = capacity - 1;
  uint32_t i = wmp_home(capacity, r->object, r->owner);
  while (slots[i].object)
    i = (i + 1) & mask;
  slots[i] = *r;
}

/* Room for one more record at most half full; false when memory can't be had. */
static inline bool
wmp_reserve(struct wmp_state *s) {
  if (s->slots && (uint64_t)(s->used + 1) * 2 <= s->capacity)
    return true;
  uint32_t capacity = s->capacity ? s->capacity * 2 : WMP_INITIAL_CAPACITY;
  if (capacity < s->capacity || capacity > (1u << 30))
    return false;
  struct wmp_record *slots = (struct wmp_record *)calloc(capacity, sizeof(*slots));
  if (!slots)
    return false;
  for (uint32_t i = 0; i < s->capacity; i++)
    if (s->slots[i].object)
      wmp_place(slots, capacity, &s->slots[i]);
  free(s->slots);
  s->slots = slots;
  s->capacity = capacity;
  return true;
}

/* The live process of `owner`, made when `create` and missing (NULL when it can't be). */
static inline struct wmp_proc *
wmp_proc_of(struct wmp_state *s, uint64_t owner, uint64_t wow_base, bool create) {
  for (struct wmp_proc *p = s->procs; p; p = p->next)
    if (p->owner == owner)
      return p;
  if (!create)
    return NULL;
  struct wmp_proc *p = (struct wmp_proc *)calloc(1, sizeof(*p));
  if (!p)
    return NULL;
  p->owner = owner;
  p->wow_base = wow_base;
  p->refs = 1; /* the live list's */
  p->next = s->procs;
  s->procs = p;
  return p;
}

/* Called with the lock held; frees the struct with its last reference. */
static inline void
wmp_proc_unref_locked(struct wmp_proc *p) {
  if (--p->refs == 0)
    free(p);
}

/* ---- what the unix calls do ---- */

enum wmp_add_result {
  WMP_ADDED = 0,   /* counted (a new record took its own reference) */
  WMP_NOT_ADDED = 1, /* no owner, a null object, or no memory: not counted, as before item 89 */
};

/* The program `owner` got one more reference to `object` (a creating call's +1, or a retain).
   `wow_base` is its 32-bit window, kept with its process. */
static inline enum wmp_add_result
wmp_add_ref(struct wmp_state *s, uint64_t object, uint64_t owner, enum wmp_kind kind, uint64_t wow_base) {
  if (!object || !owner)
    return WMP_NOT_ADDED;
  pthread_mutex_lock(&s->lock);
  long index = wmp_find(s, object, owner);
  if (index >= 0) {
    s->slots[index].count++;
    pthread_mutex_unlock(&s->lock);
    return WMP_ADDED;
  }
  if (!wmp_reserve(s) || !wmp_proc_of(s, owner, wow_base, true)) {
    pthread_mutex_unlock(&s->lock);
    return WMP_NOT_ADDED;
  }
  struct wmp_record r;
  memset(&r, 0, sizeof(r));
  r.object = object;
  r.owner = owner;
  r.count = 1;
  r.seq = s->next_seq++;
  r.kind = (uint8_t)kind;
  wmp_place(s->slots, s->capacity, &r);
  s->used++;
  if (kind == WMP_OBJECT)
    s->ops->retain(object); /* the record's own */
  pthread_mutex_unlock(&s->lock);
  return WMP_ADDED;
}

enum wmp_release_result {
  WMP_UNCOUNTED = 0, /* no record: just the program's release */
  WMP_KEPT = 1,      /* counted; the program still holds others */
  WMP_LAST = 2,      /* the program's last counted one: give back the record's own after it */
  WMP_LAST_VIEW = 3, /* a view's record is gone (it holds no reference of its own) */
};

/* The program `owner` is about to release one reference to `object`. The caller then does the
   program's release, and, for WMP_LAST, one more for the record's own. */
static inline enum wmp_release_result
wmp_release_ref(struct wmp_state *s, uint64_t object, uint64_t owner) {
  if (!object)
    return WMP_UNCOUNTED;
  pthread_mutex_lock(&s->lock);
  long index = wmp_find(s, object, owner);
  enum wmp_release_result result = WMP_UNCOUNTED;
  if (index >= 0) {
    struct wmp_record *r = &s->slots[index];
    if (r->count > 1) {
      r->count--;
      result = WMP_KEPT;
    } else {
      result = r->kind == WMP_VIEW ? WMP_LAST_VIEW : WMP_LAST;
      wmp_remove_at(s, (uint32_t)index);
    }
  }
  pthread_mutex_unlock(&s->lock);
  return result;
}

/* A command buffer of `owner` is about to be committed: returns its process with one more
   in-flight buffer and reference (hand it to wmp_completed when the buffer completes), or NULL
   when the owner has nothing recorded (nothing to wait for at its end). */
static inline struct wmp_proc *
wmp_will_commit(struct wmp_state *s, uint64_t owner) {
  if (!owner)
    return NULL;
  pthread_mutex_lock(&s->lock);
  struct wmp_proc *p = wmp_proc_of(s, owner, 0, false);
  if (p) {
    p->inflight++;
    p->refs++;
  }
  pthread_mutex_unlock(&s->lock);
  return p;
}

static inline void
wmp_completed(struct wmp_state *s, struct wmp_proc *p) {
  if (!p)
    return;
  pthread_mutex_lock(&s->lock);
  p->inflight--;
  wmp_proc_unref_locked(p);
  pthread_cond_broadcast(&s->cond);
  pthread_mutex_unlock(&s->lock);
}

/* ---- the end of a pseudo-process ---- */

struct wmp_end {
  struct wmp_record *records; /* malloc'd, newest first; free() it */
  uint32_t count;
  uint64_t wow_base;
  bool known;   /* the owner had a live process */
  bool drained; /* its command buffers completed before the timeout */
  uint32_t left_inflight;
};

static inline int
wmp_newest_first(const void *a, const void *b) {
  const struct wmp_record *x = (const struct wmp_record *)a, *y = (const struct wmp_record *)b;
  return x->seq < y->seq ? 1 : (x->seq > y->seq ? -1 : 0);
}

static inline void
wmp_deadline(struct timespec *ts, uint64_t timeout_ns) {
  clock_gettime(CLOCK_REALTIME, ts);
  uint64_t ns = (uint64_t)ts->tv_nsec + timeout_ns % 1000000000ull;
  ts->tv_sec += (time_t)(timeout_ns / 1000000000ull + ns / 1000000000ull);
  ts->tv_nsec = (long)(ns % 1000000000ull);
}

/* `owner` ended: takes its process out of the live list (a later process with the same PEB
   address starts afresh), waits until its committed command buffers have completed or
   `timeout_ns` passed, then takes its records out of the table, newest first. Nothing is
   released here: the caller looks at the records (bytes, views, the 32-bit table), then calls
   wmp_give_back. A second end of the same owner finds nothing. */
static inline void
wmp_take_ended(struct wmp_state *s, uint64_t owner, uint64_t timeout_ns, struct wmp_end *out) {
  memset(out, 0, sizeof(*out));
  out->drained = true;
  pthread_mutex_lock(&s->lock);
  struct wmp_proc **link = &s->procs, *p = NULL;
  for (; *link; link = &(*link)->next)
    if ((*link)->owner == owner) {
      p = *link;
      *link = p->next;
      break;
    }
  if (p) {
    out->known = true;
    out->wow_base = p->wow_base;
    p->ended = true;
    if (p->inflight) {
      struct timespec deadline;
      wmp_deadline(&deadline, timeout_ns);
      while (p->inflight) {
        if (pthread_cond_timedwait(&s->cond, &s->lock, &deadline) == ETIMEDOUT)
          break;
      }
    }
    out->drained = p->inflight == 0;
    out->left_inflight = p->inflight;
    wmp_proc_unref_locked(p); /* the live list's; in-flight handlers hold their own */
  }
  uint32_t n = 0;
  for (uint32_t i = 0; s->slots && i < s->capacity; i++)
    if (s->slots[i].object && s->slots[i].owner == owner)
      n++;
  if (n) {
    out->records = (struct wmp_record *)malloc(sizeof(*out->records) * n);
    if (out->records) {
      for (uint32_t i = 0; i < s->capacity;) {
        if (s->slots[i].object && s->slots[i].owner == owner) {
          out->records[out->count++] = s->slots[i];
          wmp_remove_at(s, i); /* the shift may move another record into slot i: look again */
          continue;
        }
        i++;
      }
    }
  }
  pthread_mutex_unlock(&s->lock);
  if (out->count > 1)
    qsort(out->records, out->count, sizeof(*out->records), wmp_newest_first);
}

/* Hands the taken records back, in their order: a view to release_view; an object gets the
   program's remaining references released one at a time while more than the tables' own
   (1 + extra_own) would be left, then the record's own. Returns how many records it handled. */
static inline uint32_t
wmp_give_back(const struct wmp_ops *ops, const struct wmp_record *records, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    const struct wmp_record *r = &records[i];
    if (r->kind == WMP_VIEW) {
      ops->release_view(r->object);
      continue;
    }
    uint64_t floor = 1u + r->extra_own;
    for (uint64_t left = r->count; left > 0 && ops->retain_count(r->object) > floor; left--)
      ops->release(r->object);
    ops->release(r->object); /* the record's own */
  }
  return n;
}

#endif
