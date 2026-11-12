// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 54: commits wait while the app isn't active
// (dxmt/src/winemetal/unix/app_active.h). Two threads hold a fake commit until the app becomes
// active again; the default is active; the held, resumed and lost lines come once per period.
// Only the device can check that iOS then stops refusing command buffers (TESTS.md).

#define _POSIX_C_SOURCE 200809L

#include "app_active.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_checks, g_failures;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    if (!(cond)) {                                                                                                     \
      g_failures++;                                                                                                    \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                         \
    }                                                                                                                  \
  } while (0)

#define CHECK_STR(a, b)                                                                                                \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    if (strcmp((a), (b)) != 0) {                                                                                       \
      g_failures++;                                                                                                    \
      fprintf(stderr, "%s:%d: got \"%s\", expected \"%s\"\n", __FILE__, __LINE__, (a), (b));                          \
    }                                                                                                                  \
  } while (0)

/* ---- the log, as the gate's log function sees it ---- */

#define MAX_LINES 16
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_lines[MAX_LINES][200];
static int g_line_count;

static void
capture(const char *line) {
  pthread_mutex_lock(&g_log_lock);
  if (g_line_count < MAX_LINES)
    snprintf(g_lines[g_line_count], sizeof(g_lines[0]), "%s", line);
  g_line_count++;
  pthread_mutex_unlock(&g_log_lock);
}

static int
line_count(void) {
  pthread_mutex_lock(&g_log_lock);
  int n = g_line_count;
  pthread_mutex_unlock(&g_log_lock);
  return n;
}

static void
clear_log(void) {
  pthread_mutex_lock(&g_log_lock);
  g_line_count = 0;
  pthread_mutex_unlock(&g_log_lock);
}

static void
pause_ms(int ms) {
  struct timespec ts = {0, (long)ms * 1000000L};
  nanosleep(&ts, NULL);
}

static uint64_t
held_now(struct dxmt_app_gate *gate) {
  pthread_mutex_lock(&gate->lock);
  uint64_t held = gate->held;
  pthread_mutex_unlock(&gate->lock);
  return held;
}

/* ---- a fake commit on its own thread ---- */

struct committer {
  struct dxmt_app_gate *gate;
  atomic_int committed; /* the fake commit ran */
  bool waited;          /* hold said it waited */
};

static void *
commit_thread(void *arg) {
  struct committer *c = arg;
  c->waited = dxmt_app_gate_hold(c->gate);
  atomic_store(&c->committed, 1);
  return NULL;
}

static void
test_default_active(void) {
  struct dxmt_app_gate gate = DXMT_APP_GATE_INIT(capture);
  clear_log();
  CHECK(dxmt_app_gate_active(&gate));
  CHECK(!dxmt_app_gate_hold(&gate)); /* returns at once */
  CHECK(!dxmt_app_gate_drop_present(&gate));
  dxmt_app_gate_set_active(&gate, 1, 5000000); /* already active: nothing */
  CHECK(line_count() == 0);
}

static void
test_wait_and_resume(void) {
  struct dxmt_app_gate gate = DXMT_APP_GATE_INIT(capture);
  clear_log();
  dxmt_app_gate_set_active(&gate, 0, 1000000000ull); /* resigns at t = 1 s */
  dxmt_app_gate_set_active(&gate, 0, 1100000000ull); /* again: the same period */
  CHECK(!dxmt_app_gate_active(&gate));
  CHECK(gate.period == 1);

  struct committer a = {&gate, 0, false}, b = {&gate, 0, false};
  pthread_t ta, tb;
  CHECK(pthread_create(&ta, NULL, commit_thread, &a) == 0);
  CHECK(pthread_create(&tb, NULL, commit_thread, &b) == 0);
  for (int i = 0; i < 2000 && held_now(&gate) < 2; i++)
    pause_ms(1);
  CHECK(held_now(&gate) == 2);
  pause_ms(20); /* both stay held */
  CHECK(!atomic_load(&a.committed));
  CHECK(!atomic_load(&b.committed));

  /* presents are dropped meanwhile */
  CHECK(dxmt_app_gate_drop_present(&gate));
  CHECK(dxmt_app_gate_drop_present(&gate));
  CHECK(dxmt_app_gate_drop_present(&gate));

  /* one held line for the period, however many commits wait */
  CHECK(line_count() == 1);
  CHECK_STR(g_lines[0], "winemetal: GPU work held while the app is inactive");

  dxmt_app_gate_set_active(&gate, 1, 1812400000ull); /* back at t = 1.8124 s */
  pthread_join(ta, NULL);
  pthread_join(tb, NULL);
  CHECK(atomic_load(&a.committed) && atomic_load(&b.committed));
  CHECK(a.waited && b.waited);
  CHECK(line_count() == 2);
  CHECK_STR(g_lines[1], "winemetal: resumed after 812 ms, 2 command buffers held, 3 presents dropped");
  CHECK(!dxmt_app_gate_hold(&gate)); /* active again: no wait */
  CHECK(!dxmt_app_gate_drop_present(&gate));

  /* a second period logs its own held line, and counts from zero */
  dxmt_app_gate_set_active(&gate, 0, 2000000000ull);
  struct committer c = {&gate, 0, false};
  pthread_t tc;
  CHECK(pthread_create(&tc, NULL, commit_thread, &c) == 0);
  for (int i = 0; i < 2000 && held_now(&gate) < 1; i++)
    pause_ms(1);
  dxmt_app_gate_set_active(&gate, 1, 2050000000ull);
  pthread_join(tc, NULL);
  CHECK(c.waited);
  CHECK(line_count() == 4);
  CHECK_STR(g_lines[2], "winemetal: GPU work held while the app is inactive");
  CHECK_STR(g_lines[3], "winemetal: resumed after 50 ms, 1 command buffers held, 0 presents dropped");
}

static void
test_lost_once_per_period(void) {
  struct dxmt_app_gate gate = DXMT_APP_GATE_INIT(capture);
  clear_log();
  /* before any inactive period: once */
  CHECK(dxmt_app_gate_lost(&gate, "command buffer #12"));
  CHECK(!dxmt_app_gate_lost(&gate, "command buffer #13"));
  CHECK(line_count() == 1);
  CHECK_STR(g_lines[0], "winemetal: GPU work lost in the background: command buffer #12");

  /* a period: once, also for refusals that complete after the app is back (the edge) */
  dxmt_app_gate_set_active(&gate, 0, 0);
  CHECK(dxmt_app_gate_lost(&gate, "upload"));
  dxmt_app_gate_set_active(&gate, 1, 3000000);
  CHECK(!dxmt_app_gate_lost(&gate, "upload"));
  CHECK(line_count() == 3); /* lost, resumed */
  CHECK_STR(g_lines[1], "winemetal: GPU work lost in the background: upload");
  CHECK_STR(g_lines[2], "winemetal: resumed after 3 ms, 0 command buffers held, 0 presents dropped");

  /* the next period: once more */
  dxmt_app_gate_set_active(&gate, 0, 4000000);
  CHECK(dxmt_app_gate_lost(&gate, NULL));
  CHECK(line_count() == 4);
  CHECK_STR(g_lines[3], "winemetal: GPU work lost in the background: ?");
  dxmt_app_gate_set_active(&gate, 1, 4000000);

  /* no log function: nothing is written, and nothing breaks */
  struct dxmt_app_gate quiet = DXMT_APP_GATE_INIT(NULL);
  dxmt_app_gate_set_active(&quiet, 0, 0);
  CHECK(dxmt_app_gate_lost(&quiet, "x"));
  dxmt_app_gate_set_active(&quiet, 1, 0);
  CHECK(line_count() == 5); /* only the first gate's resumed line was added */
}

int
main(void) {
  test_default_active();
  test_wait_and_resume();
  test_lost_once_per_period();
  printf("test_app_active: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
