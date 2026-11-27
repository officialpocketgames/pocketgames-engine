// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 item 91: the app's share table (dxmt/src/winemetal/unix/dxmt_share_table.h), in C as
// winemetal_unix.c uses it, against fake objects whose retains are counted:
// - a create gives a key and the table's retain; an open gives the same object, +1, one more user;
// - the creator's release while an opener holds it keeps the entry; the last release frees it and
//   drops the table's retain;
// - a missing key, a key of the other kind, a released key are refused and change nothing;
// - a full table gives key 0 and retains nothing; a freed slot is used again;
// - keys are never 0 and not reused while in use, also where the counter wraps.
//   cc -std=c11 -I../dxmt/src/winemetal/unix test_share_table.c

#define DXMT_SHARE_TABLE_CAPACITY 8
#include "dxmt_share_table.h"

#include <stdio.h>

static unsigned g_checks, g_failures;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    if (!(cond)) {                                                                                                     \
      g_failures++;                                                                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                           \
    }                                                                                                                  \
  } while (0)

/* fake objects: handles 1..15, each with a retain count */
static int g_refs[16];

static void
fake_retain(uint64_t object) {
  g_refs[object]++;
}

static void
fake_release(uint64_t object) {
  g_refs[object]--;
}

static void
test_create_open_release(void) {
  struct dxmt_share_table table;
  dxmt_share_init(&table, DXMT_SHARE_TABLE_CAPACITY, fake_retain, fake_release);
  g_refs[1] = 1; /* the creator's own reference */

  uint32_t key = dxmt_share_add(&table, DXMT_SHARE_TEXTURE, 1);
  CHECK(key != 0);
  CHECK(g_refs[1] == 2); /* + the table's */
  CHECK(dxmt_share_users(&table, key) == 1);
  CHECK(table.used == 1);

  /* an open: the same object, retained for the opener, one more user */
  uint64_t object = 0;
  CHECK(dxmt_share_open(&table, DXMT_SHARE_TEXTURE, key, &object) == DXMT_SHARE_FOUND);
  CHECK(object == 1);
  CHECK(g_refs[1] == 3);
  CHECK(dxmt_share_users(&table, key) == 2);

  /* the creator goes first (its own reference too): the opener still has the entry */
  g_refs[1]--;
  CHECK(!dxmt_share_release(&table, key));
  CHECK(dxmt_share_users(&table, key) == 1);
  CHECK(g_refs[1] == 2);

  /* opened again after the creator's release */
  CHECK(dxmt_share_open(&table, DXMT_SHARE_TEXTURE, key, &object) == DXMT_SHARE_FOUND);
  CHECK(object == 1);
  CHECK(dxmt_share_users(&table, key) == 2);
  g_refs[1]--;
  CHECK(!dxmt_share_release(&table, key));

  /* the last user: the entry goes and with it the table's retain */
  g_refs[1]--;
  CHECK(dxmt_share_release(&table, key));
  CHECK(g_refs[1] == 0);
  CHECK(dxmt_share_find(&table, key) == NULL);
  CHECK(table.used == 0);

  /* a released key is refused, and so is releasing it again */
  CHECK(dxmt_share_open(&table, DXMT_SHARE_TEXTURE, key, &object) == DXMT_SHARE_MISSING);
  CHECK(object == 0);
  CHECK(!dxmt_share_release(&table, key));
  CHECK(g_refs[1] == 0);
}

static void
test_refusals(void) {
  struct dxmt_share_table table;
  dxmt_share_init(&table, DXMT_SHARE_TABLE_CAPACITY, fake_retain, fake_release);
  g_refs[2] = 1;
  g_refs[3] = 1;
  uint32_t texture = dxmt_share_add(&table, DXMT_SHARE_TEXTURE, 2);
  uint32_t event = dxmt_share_add(&table, DXMT_SHARE_EVENT, 3);
  CHECK(texture && event && texture != event);

  uint64_t object = 99;
  CHECK(dxmt_share_open(&table, DXMT_SHARE_TEXTURE, 0, &object) == DXMT_SHARE_MISSING);
  CHECK(object == 0);
  CHECK(dxmt_share_open(&table, DXMT_SHARE_TEXTURE, 0xdeadbeef, &object) == DXMT_SHARE_MISSING);
  /* the other kind: a fence key opened as a texture, and the reverse */
  CHECK(dxmt_share_open(&table, DXMT_SHARE_TEXTURE, event, &object) == DXMT_SHARE_WRONG_KIND);
  CHECK(object == 0);
  CHECK(dxmt_share_open(&table, DXMT_SHARE_EVENT, texture, &object) == DXMT_SHARE_WRONG_KIND);
  CHECK(dxmt_share_users(&table, texture) == 1 && dxmt_share_users(&table, event) == 1);
  CHECK(g_refs[2] == 2 && g_refs[3] == 2);
  /* a fence opens as a fence */
  CHECK(dxmt_share_open(&table, DXMT_SHARE_EVENT, event, &object) == DXMT_SHARE_FOUND);
  CHECK(object == 3);
  CHECK(dxmt_share_users(&table, event) == 2);
  g_refs[3]--; /* the opener lets its reference go */
  /* a null object isn't registered */
  CHECK(dxmt_share_add(&table, DXMT_SHARE_TEXTURE, 0) == 0);
  CHECK(table.used == 2);

  CHECK(!dxmt_share_release(&table, event));
  CHECK(dxmt_share_release(&table, event));
  CHECK(dxmt_share_release(&table, texture));
  CHECK(g_refs[2] == 1 && g_refs[3] == 1); /* the creators' own references are left */
  CHECK(table.used == 0);
}

static void
test_full_table(void) {
  struct dxmt_share_table table;
  dxmt_share_init(&table, 3, fake_retain, fake_release);
  uint32_t keys[3];
  for (int i = 0; i < 3; i++) {
    g_refs[4 + i] = 1;
    keys[i] = dxmt_share_add(&table, DXMT_SHARE_TEXTURE, (uint64_t)(4 + i));
    CHECK(keys[i] != 0);
  }
  /* full: key 0, nothing retained (the caller makes its texture as not shareable) */
  g_refs[7] = 1;
  CHECK(dxmt_share_add(&table, DXMT_SHARE_TEXTURE, 7) == 0);
  CHECK(g_refs[7] == 1);
  CHECK(table.used == 3);
  /* a freed slot takes the next one, under a new key */
  CHECK(dxmt_share_release(&table, keys[1]));
  uint32_t again = dxmt_share_add(&table, DXMT_SHARE_TEXTURE, 7);
  CHECK(again != 0 && again != keys[0] && again != keys[1] && again != keys[2]);
  CHECK(g_refs[7] == 2);
  CHECK(dxmt_share_release(&table, keys[0]));
  CHECK(dxmt_share_release(&table, keys[2]));
  CHECK(dxmt_share_release(&table, again));
  CHECK(table.used == 0);
  /* the capacity is never more than the array */
  dxmt_share_init(&table, 1000, fake_retain, fake_release);
  CHECK(table.capacity == DXMT_SHARE_TABLE_CAPACITY);
}

static void
test_keys(void) {
  struct dxmt_share_table table;
  dxmt_share_init(&table, DXMT_SHARE_TABLE_CAPACITY, fake_retain, fake_release);
  /* near the end of the counter: 0 is skipped, and a key still in use isn't given again */
  g_refs[8] = g_refs[9] = g_refs[10] = 1;
  table.last_key = 0;
  uint32_t first = dxmt_share_add(&table, DXMT_SHARE_TEXTURE, 8); /* key 1 */
  CHECK(first == 1);
  table.last_key = UINT32_MAX - 1;
  uint32_t high = dxmt_share_add(&table, DXMT_SHARE_TEXTURE, 9);
  CHECK(high == UINT32_MAX);
  uint32_t wrapped = dxmt_share_add(&table, DXMT_SHARE_EVENT, 10);
  CHECK(wrapped == 2); /* not 0, not 1 (in use) */
  uint64_t object = 0;
  CHECK(dxmt_share_open(&table, DXMT_SHARE_TEXTURE, UINT32_MAX, &object) == DXMT_SHARE_FOUND && object == 9);
  g_refs[9]--; /* the opener lets its reference go */
  CHECK(dxmt_share_release(&table, UINT32_MAX) == false);
  CHECK(dxmt_share_release(&table, UINT32_MAX));
  CHECK(dxmt_share_release(&table, first));
  CHECK(dxmt_share_release(&table, wrapped));
  CHECK(g_refs[8] == 1 && g_refs[9] == 1 && g_refs[10] == 1);
}

int
main(void) {
  test_create_open_release();
  test_refusals();
  test_full_table();
  test_keys();
  printf("test_share_table: %u checks, %u failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
