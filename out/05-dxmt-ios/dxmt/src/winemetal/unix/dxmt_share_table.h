// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 item 91: the app's one table of shared Metal objects. On iOS every Windows program runs
// in the app's one process on one MTLDevice, so a shared texture or fence needs no mach port and no
// bootstrap server: it is the Metal object itself (the MTLTexture, the MTLSharedEvent). The creator
// registers the object here and gets a key, which travels in the share name D3DKMT keeps for it;
// an opener looks the key up and gets the same object.
//
// - The table holds one retain of each object and counts its users: the creator and each opener.
//   When the last user releases its key, the table releases the object and frees the entry.
// - Keys are 32-bit (they ride in WMTTextureInfo's mach_port field), never 0, and increase, so a
//   key isn't reused until 2^32 more shares have been made.
// - A full table gives key 0: the caller makes its object as not shareable.
//
// Pure C with the retain and release as callbacks, so tests/test_share_table.c runs it against
// fake objects. Not thread-safe: the caller holds one lock around each call (winemetal_unix.c).

#ifndef DXMT_SHARE_TABLE_H
#define DXMT_SHARE_TABLE_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef DXMT_SHARE_TABLE_CAPACITY
#define DXMT_SHARE_TABLE_CAPACITY 256
#endif

enum dxmt_share_kind {
  DXMT_SHARE_TEXTURE = 1,
  DXMT_SHARE_EVENT = 2,
};

enum dxmt_share_open_result {
  DXMT_SHARE_FOUND = 0,
  DXMT_SHARE_MISSING = 1,    /* no entry has that key */
  DXMT_SHARE_WRONG_KIND = 2, /* the key is a texture's and a fence was asked for, or the reverse */
};

struct dxmt_share_entry {
  uint32_t key; /* 0: free */
  uint32_t kind;
  uint32_t users;
  uint64_t object;
};

struct dxmt_share_table {
  struct dxmt_share_entry entries[DXMT_SHARE_TABLE_CAPACITY];
  uint32_t capacity; /* at most DXMT_SHARE_TABLE_CAPACITY (the tests use less) */
  uint32_t used;
  uint32_t last_key;
  void (*retain)(uint64_t object);
  void (*release)(uint64_t object);
};

static inline void
dxmt_share_init(
    struct dxmt_share_table *table, uint32_t capacity, void (*retain)(uint64_t), void (*release)(uint64_t)
) {
  memset(table, 0, sizeof(*table));
  table->capacity = capacity < DXMT_SHARE_TABLE_CAPACITY ? capacity : DXMT_SHARE_TABLE_CAPACITY;
  table->retain = retain;
  table->release = release;
}

static inline struct dxmt_share_entry *
dxmt_share_find(struct dxmt_share_table *table, uint32_t key) {
  if (!key)
    return NULL;
  for (uint32_t i = 0; i < table->capacity; i++)
    if (table->entries[i].key == key)
      return &table->entries[i];
  return NULL;
}

/* Register `object` (the table retains it); its creator is the first user. The key, or 0 when
   the object is null or the table is full. */
static inline uint32_t
dxmt_share_add(struct dxmt_share_table *table, enum dxmt_share_kind kind, uint64_t object) {
  if (!object || table->used >= table->capacity)
    return 0;
  struct dxmt_share_entry *slot = NULL;
  for (uint32_t i = 0; i < table->capacity && !slot; i++)
    if (!table->entries[i].key)
      slot = &table->entries[i];
  if (!slot)
    return 0;
  /* the next key that is neither 0 nor in use (fewer than 2^32 are in use, so this ends) */
  uint32_t key = table->last_key;
  do
    key++;
  while (!key || dxmt_share_find(table, key));
  table->last_key = key;
  table->retain(object);
  slot->key = key;
  slot->kind = (uint32_t)kind;
  slot->users = 1;
  slot->object = object;
  table->used++;
  return key;
}

/* An opener: on FOUND, *object is the shared object, retained for the caller, and the entry has
   one more user (released with dxmt_share_release). Otherwise *object is 0 and nothing changed. */
static inline enum dxmt_share_open_result
dxmt_share_open(struct dxmt_share_table *table, enum dxmt_share_kind kind, uint32_t key, uint64_t *object) {
  *object = 0;
  struct dxmt_share_entry *entry = dxmt_share_find(table, key);
  if (!entry || entry->users == UINT32_MAX)
    return DXMT_SHARE_MISSING;
  if (entry->kind != (uint32_t)kind)
    return DXMT_SHARE_WRONG_KIND;
  entry->users++;
  table->retain(entry->object);
  *object = entry->object;
  return DXMT_SHARE_FOUND;
}

/* One user of `key` is gone. True when it was the last: the table released its retain and the
   entry is free. False for a key with users left, or one the table doesn't have. */
static inline bool
dxmt_share_release(struct dxmt_share_table *table, uint32_t key) {
  struct dxmt_share_entry *entry = dxmt_share_find(table, key);
  if (!entry)
    return false;
  if (--entry->users)
    return false;
  uint64_t object = entry->object;
  memset(entry, 0, sizeof(*entry));
  table->used--;
  table->release(object);
  return true;
}

/* The users of `key` (0: no such entry), for the tests and the log. */
static inline uint32_t
dxmt_share_users(struct dxmt_share_table *table, uint32_t key) {
  struct dxmt_share_entry *entry = dxmt_share_find(table, key);
  return entry ? entry->users : 0;
}

#endif /* DXMT_SHARE_TABLE_H */
