// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 06 Part E: the record table of Metal objects (textures and buffers)
// created by 32-bit programs, so that a finished program's objects can be
// released when Wine tears down its window.
//
// A record is {object, window base B, references the program holds}. The
// table also holds one reference of its own on every recorded object.
//
// Plain C (also compiles as C++), no locking: the caller wraps every call in
// one lock. Objects are opaque 64-bit values; retain, release and the retain
// count go through callbacks, so the table can be unit-tested with fakes.

#ifndef DXMT_IOS_WOW64_OBJECTS_H
#define DXMT_IOS_WOW64_OBJECTS_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

struct dxmt_wow_record {
  uint64_t object; /* 0: free slot */
  uint64_t base;
  uint64_t count; /* references the program holds */
};

struct dxmt_wow_objects {
  struct dxmt_wow_record *slots; /* `capacity` entries, open addressing */
  uint32_t capacity;             /* a power of two */
  uint32_t used;
  void (*retain)(uint64_t object);
  void (*release)(uint64_t object);
  uint64_t (*retain_count)(uint64_t object);
};

enum dxmt_wow_record_result {
  DXMT_WOW_RECORDED = 0,
  DXMT_WOW_TABLE_FULL = 1, /* not recorded; the caller logs it once */
};

/* `slots` must point to `capacity` zeroed records; capacity a power of two. */
static inline void
dxmt_wow_objects_init(
    struct dxmt_wow_objects *t, struct dxmt_wow_record *slots, uint32_t capacity, void (*retain)(uint64_t),
    void (*release)(uint64_t), uint64_t (*retain_count)(uint64_t)
) {
  t->slots = slots;
  t->capacity = capacity;
  t->used = 0;
  t->retain = retain;
  t->release = release;
  t->retain_count = retain_count;
}

static inline uint32_t
dxmt_wow_objects_home(const struct dxmt_wow_objects *t, uint64_t object) {
  uint64_t h = (object >> 4) * 0x9E3779B97F4A7C15ull;
  return (uint32_t)(h >> 32) & (t->capacity - 1);
}

/* Slot index of `object`, or -1. */
static inline long
dxmt_wow_objects_find(const struct dxmt_wow_objects *t, uint64_t object) {
  if (!t->slots || !object)
    return -1;
  uint32_t mask = t->capacity - 1;
  for (uint32_t i = dxmt_wow_objects_home(t, object), n = 0; n < t->capacity; i = (i + 1) & mask, n++) {
    if (t->slots[i].object == object)
      return (long)i;
    if (t->slots[i].object == 0)
      return -1;
  }
  return -1;
}

/* Linear-probing removal with backward shift, so no tombstones are needed. */
static inline void
dxmt_wow_objects_remove_at(struct dxmt_wow_objects *t, uint32_t hole) {
  uint32_t mask = t->capacity - 1;
  t->slots[hole].object = 0;
  t->used--;
  for (uint32_t i = (hole + 1) & mask; t->slots[i].object; i = (i + 1) & mask) {
    uint32_t home = dxmt_wow_objects_home(t, t->slots[i].object);
    /* Move the entry into the hole if the hole lies on its probe path
       (cyclically between its home slot and its current slot). */
    bool movable = hole <= i ? (home <= hole || home > i) : (home <= hole && home > i);
    if (movable) {
      t->slots[hole] = t->slots[i];
      t->slots[i].object = 0;
      hole = i;
    }
  }
}

/* A 32-bit program created `object` in the window at `base`; it holds one
   reference. The table takes one of its own. An address that is already
   recorded belongs to an object freed through a path the count didn't see:
   it gets a fresh record (and the stale record's reference is not released). */
static inline enum dxmt_wow_record_result
dxmt_wow_objects_created(struct dxmt_wow_objects *t, uint64_t object, uint64_t base) {
  long index = dxmt_wow_objects_find(t, object);
  if (index < 0) {
    /* Keep one slot free so probing always ends. */
    if (!t->slots || t->used + 1 >= t->capacity)
      return DXMT_WOW_TABLE_FULL;
    uint32_t mask = t->capacity - 1;
    uint32_t i = dxmt_wow_objects_home(t, object);
    while (t->slots[i].object)
      i = (i + 1) & mask;
    index = (long)i;
    t->slots[i].object = object;
    t->used++;
  }
  t->slots[index].base = base;
  t->slots[index].count = 1;
  t->retain(object);
  return DXMT_WOW_RECORDED;
}

/* The program retained `object` (a no-op for unrecorded objects). */
static inline void
dxmt_wow_objects_retained(struct dxmt_wow_objects *t, uint64_t object) {
  long index = dxmt_wow_objects_find(t, object);
  if (index >= 0)
    t->slots[index].count++;
}

/* The program is about to release `object`. Returns true when that was the
   program's last counted reference: the record is forgotten, and the caller
   gives back the table's own reference after the program's release. */
static inline bool
dxmt_wow_objects_released(struct dxmt_wow_objects *t, uint64_t object) {
  long index = dxmt_wow_objects_find(t, object);
  if (index < 0)
    return false;
  if (t->slots[index].count > 1) {
    t->slots[index].count--;
    return false;
  }
  dxmt_wow_objects_remove_at(t, (uint32_t)index);
  return true;
}

/* Teardown, step 1 (under the lock): removes every record of window `base`
   from the table and returns them in a malloc'd array (*out, free() it), with
   their number as the result. 0 and *out = NULL when there are none (or the
   array can't be allocated, in which case the records stay). */
static inline uint32_t
dxmt_wow_objects_take_window(struct dxmt_wow_objects *t, uint64_t base, struct dxmt_wow_record **out) {
  *out = NULL;
  if (!t->slots || t->used == 0)
    return 0;
  uint32_t n = 0;
  for (uint32_t i = 0; i < t->capacity; i++)
    if (t->slots[i].object && t->slots[i].base == base)
      n++;
  if (n == 0)
    return 0;
  struct dxmt_wow_record *records = (struct dxmt_wow_record *)malloc(sizeof(*records) * n);
  if (!records)
    return 0;
  uint32_t k = 0;
  for (uint32_t i = 0; i < t->capacity;) {
    if (t->slots[i].object && t->slots[i].base == base) {
      records[k++] = t->slots[i];
      /* the backward shift may move another entry into slot i: look again */
      dxmt_wow_objects_remove_at(t, i);
      continue;
    }
    i++;
  }
  *out = records;
  return k;
}

/* Teardown, step 2 (outside the lock): for each record, give back the
   program's remaining references one at a time, stopping as soon as only the
   table's own reference would be left, then give back the table's own. */
static inline void
dxmt_wow_objects_give_back(const struct dxmt_wow_objects *t, const struct dxmt_wow_record *records, uint32_t n) {
  for (uint32_t r = 0; r < n; r++) {
    uint64_t object = records[r].object;
    for (uint64_t left = records[r].count; left > 0 && t->retain_count(object) > 1; left--)
      t->release(object);
    t->release(object);
  }
}

#endif
