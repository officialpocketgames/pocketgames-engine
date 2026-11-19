// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 item 89: the per-process ledger of winemetal's unix side
// (dxmt/src/winemetal/unix/winemetal_proc_objects.h), with fake objects whose reference counts
// the test keeps:
//   - two processes' objects; ending one releases exactly its own, newest first, only after its
//     committed work completed; the other's stay; a repeated end is harmless;
//   - the end waits for work in flight, and gives up after the timeout;
//   - the normal path: a released object is never released again at the end; an object both
//     processes hold (the device) keeps the other's references;
//   - the floor: never below the tables' own references (extra_own for spec 06's 32-bit table);
//   - views go to release_view once; a view released normally doesn't;
//   - a PEB address reused by a later process starts afresh; the table grows past its first size.
// Only the device shows that ntdll calls the hook at a force quit, and the memory coming back
// (TESTS.md device check 43).

#define _GNU_SOURCE
#include "winemetal_proc_objects.h"

#include <stdio.h>
#include <time.h>
#include <unistd.h>

static int g_checks, g_failures;
#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    if (!(cond)) {                                                                                                     \
      g_failures++;                                                                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                           \
    }                                                                                                                  \
  } while (0)

/* ---- fake objects: object n (1..kObjects) has refs[n] references ---- */

#define kObjects 4096
static uint64_t refs[kObjects + 1];
static int over_released;
/* the order of releases that freed an object, and of view releases */
static uint64_t freed[kObjects + 1];
static unsigned freed_n;
static uint64_t views_released[16];
static unsigned views_n;
static _Atomic int completed_flag;
static _Atomic int release_before_completion;

static void
fake_retain(uint64_t o) {
  refs[o]++;
}

static void
fake_release(uint64_t o) {
  if (!completed_flag)
    release_before_completion = 1;
  if (refs[o] == 0) {
    over_released++;
    return;
  }
  if (--refs[o] == 0)
    freed[freed_n++] = o;
}

static uint64_t
fake_retain_count(uint64_t o) {
  return refs[o];
}

static void
fake_release_view(uint64_t v) {
  views_released[views_n++] = v;
}

static const struct wmp_ops ops = {fake_retain, fake_release, fake_retain_count, fake_release_view};

static void
reset(void) {
  memset(refs, 0, sizeof(refs));
  memset(freed, 0, sizeof(freed));
  freed_n = views_n = 0;
  over_released = 0;
  completed_flag = 1;
  release_before_completion = 0;
}

/* the program creates object o (+1 handed to it) */
static void
create(struct wmp_state *s, uint64_t o, uint64_t owner) {
  refs[o]++;
  wmp_add_ref(s, o, owner, WMP_OBJECT, 0);
}

/* the program releases object o, as dxmt_ios_proc_release does */
static void
program_release(struct wmp_state *s, uint64_t o, uint64_t owner) {
  enum wmp_release_result r = wmp_release_ref(s, o, owner);
  fake_release(o);
  if (r == WMP_LAST)
    fake_release(o);
}

static void
end_process(struct wmp_state *s, uint64_t owner, uint64_t timeout_ns, struct wmp_end *end) {
  wmp_take_ended(s, owner, timeout_ns, end);
  wmp_give_back(&ops, end->records, end->count);
}

static const uint64_t A = 0x7f0000001000ull, B = 0x7f0000009000ull;

static void
test_two_processes(void) {
  reset();
  struct wmp_state s = WMP_STATE_INIT(&ops);
  /* A: a queue (1), buffers 2 and 3, a texture 4 retained twice more; B: 10, 11 */
  create(&s, 1, A);
  create(&s, 10, B);
  create(&s, 2, A);
  create(&s, 3, A);
  create(&s, 11, B);
  create(&s, 4, A);
  fake_retain(4);
  wmp_add_ref(&s, 4, A, WMP_OBJECT, 0);
  fake_retain(4);
  wmp_add_ref(&s, 4, A, WMP_OBJECT, 0);
  CHECK(refs[4] == 4); /* the program's 3 and the record's own */
  program_release(&s, 3, A); /* released normally: gone at once */
  CHECK(refs[3] == 0 && freed_n == 1 && freed[0] == 3);

  struct wmp_end end;
  end_process(&s, A, 1000000, &end);
  CHECK(end.known && end.drained && end.count == 3);
  /* newest first: 4, 2, 1; nothing of B; 3 not again */
  CHECK(freed_n == 4 && freed[1] == 4 && freed[2] == 2 && freed[3] == 1);
  CHECK(refs[1] == 0 && refs[2] == 0 && refs[4] == 0);
  CHECK(refs[10] == 2 && refs[11] == 2);
  CHECK(over_released == 0);
  free(end.records);

  /* a repeated end of A finds nothing */
  end_process(&s, A, 1000000, &end);
  CHECK(!end.known && end.count == 0 && freed_n == 4);
  free(end.records);

  /* B goes on: its objects work normally, and its end takes the rest */
  program_release(&s, 10, B);
  CHECK(refs[10] == 0);
  end_process(&s, B, 1000000, &end);
  CHECK(end.count == 1 && refs[11] == 0 && over_released == 0);
  free(end.records);
  free(s.slots);
}

static void
test_shared_device(void) {
  reset();
  struct wmp_state s = WMP_STATE_INIT(&ops);
  /* the device (1) is one object; the app holds 1, A retains it twice, B once */
  refs[1] = 1;
  fake_retain(1);
  wmp_add_ref(&s, 1, A, WMP_OBJECT, 0);
  fake_retain(1);
  wmp_add_ref(&s, 1, A, WMP_OBJECT, 0);
  fake_retain(1);
  wmp_add_ref(&s, 1, B, WMP_OBJECT, 0);
  CHECK(refs[1] == 1 + 2 + 1 + 2); /* app, A's two, B's one, two records' own */
  struct wmp_end end;
  end_process(&s, A, 1000000, &end);
  CHECK(refs[1] == 1 + 1 + 1); /* A's two and its record's own gone */
  free(end.records);
  program_release(&s, 1, B);
  CHECK(refs[1] == 1 && over_released == 0);
  free(s.slots);
}

struct completer {
  struct wmp_state *s;
  struct wmp_proc *p;
  unsigned delay_ms;
};

static void *
complete_later(void *arg) {
  struct completer *c = arg;
  usleep(c->delay_ms * 1000);
  completed_flag = 1;
  wmp_completed(c->s, c->p);
  return NULL;
}

static uint64_t
now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void
test_waits_for_work(void) {
  reset();
  struct wmp_state s = WMP_STATE_INIT(&ops);
  create(&s, 1, A);
  create(&s, 2, A);
  CHECK(wmp_will_commit(&s, B) == NULL); /* B has nothing recorded */
  struct wmp_proc *p = wmp_will_commit(&s, A);
  CHECK(p != NULL && p->inflight == 1);
  completed_flag = 0;
  struct completer c = {&s, p, 100};
  pthread_t thread;
  pthread_create(&thread, NULL, complete_later, &c);
  uint64_t t0 = now_ns();
  struct wmp_end end;
  end_process(&s, A, 2000000000ull, &end);
  uint64_t waited = now_ns() - t0;
  pthread_join(thread, NULL);
  CHECK(end.drained && end.count == 2);
  CHECK(!release_before_completion); /* nothing released while its work was in flight */
  CHECK(waited >= 90000000ull && waited < 1500000000ull);
  CHECK(refs[1] == 0 && refs[2] == 0);
  free(end.records);

  /* work that never completes: the end gives up after the timeout and releases anyway */
  reset();
  create(&s, 5, B);
  struct wmp_proc *q = wmp_will_commit(&s, B);
  CHECK(q != NULL);
  t0 = now_ns();
  end_process(&s, B, 150000000ull, &end);
  waited = now_ns() - t0;
  CHECK(!end.drained && end.left_inflight == 1 && end.count == 1);
  CHECK(waited >= 140000000ull && waited < 1500000000ull);
  CHECK(refs[5] == 0);
  free(end.records);
  /* the late completion only drops its own reference to the ended process */
  wmp_completed(&s, q);
  CHECK(s.procs == NULL);
  free(s.slots);
}

static void
test_floor(void) {
  reset();
  struct wmp_state s = WMP_STATE_INIT(&ops);
  /* a 32-bit texture: the program's 1, this record's own, the 32-bit table's own */
  create(&s, 7, A);
  refs[7]++; /* the 32-bit table's own */
  /* an inflated count (a retain the program never had) can't take the other table's reference */
  wmp_add_ref(&s, 7, A, WMP_OBJECT, 0);
  struct wmp_end end;
  wmp_take_ended(&s, A, 1000000, &end);
  CHECK(end.count == 1);
  end.records[0].extra_own = 1;
  wmp_give_back(&ops, end.records, end.count);
  CHECK(refs[7] == 1); /* only the 32-bit table's own left; the caller gives it back */
  fake_release(7);
  CHECK(refs[7] == 0 && over_released == 0);
  free(end.records);
  free(s.slots);
}

static void
test_views(void) {
  reset();
  struct wmp_state s = WMP_STATE_INIT(&ops);
  wmp_add_ref(&s, 100, A, WMP_VIEW, 0);
  wmp_add_ref(&s, 101, A, WMP_VIEW, 0);
  CHECK(refs[100] == 0); /* a view holds no reference */
  CHECK(wmp_release_ref(&s, 101, A) == WMP_LAST_VIEW); /* released normally */
  CHECK(wmp_release_ref(&s, 101, A) == WMP_UNCOUNTED);
  struct wmp_end end;
  end_process(&s, A, 1000000, &end);
  CHECK(views_n == 1 && views_released[0] == 100);
  CHECK(over_released == 0);
  free(end.records);
  free(s.slots);
}

static void
test_reuse_and_growth(void) {
  reset();
  struct wmp_state s = WMP_STATE_INIT(&ops);
  for (uint64_t o = 1; o <= 3000; o++)
    create(&s, o, A);
  CHECK(s.capacity >= 4096 && s.used == 3000);
  for (uint64_t o = 1; o <= 3000; o += 2)
    program_release(&s, o, A);
  struct wmp_end end;
  end_process(&s, A, 1000000, &end);
  CHECK(end.count == 1500);
  bool all_freed = true, newest_first = true;
  for (uint64_t o = 1; o <= 3000; o++)
    all_freed &= refs[o] == 0;
  for (uint32_t i = 1; i < end.count; i++)
    newest_first &= end.records[i - 1].seq > end.records[i].seq;
  CHECK(all_freed && newest_first && s.used == 0 && over_released == 0);
  free(end.records);
  /* a later process whose PEB lands at A's address starts afresh */
  create(&s, 3001, A);
  end_process(&s, A, 1000000, &end);
  CHECK(end.known && end.count == 1 && refs[3001] == 0);
  free(end.records);
  free(s.slots);
}

static void
test_ignored(void) {
  reset();
  struct wmp_state s = WMP_STATE_INIT(&ops);
  CHECK(wmp_add_ref(&s, 0, A, WMP_OBJECT, 0) == WMP_NOT_ADDED); /* a null object */
  CHECK(wmp_add_ref(&s, 1, 0, WMP_OBJECT, 0) == WMP_NOT_ADDED); /* no pseudo-process */
  CHECK(refs[1] == 0 && s.used == 0);
  CHECK(wmp_release_ref(&s, 1, A) == WMP_UNCOUNTED);
  struct wmp_end end;
  end_process(&s, A, 1000000, &end); /* an owner never seen */
  CHECK(!end.known && end.count == 0);
  free(end.records);
  free(s.slots);
}

int
main(void) {
  test_two_processes();
  test_shared_device();
  test_waits_for_work();
  test_floor();
  test_views();
  test_reuse_and_growth();
  test_ignored();
  printf("test_proc_objects: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
