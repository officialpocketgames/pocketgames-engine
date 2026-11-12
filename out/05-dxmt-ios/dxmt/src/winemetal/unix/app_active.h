// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 05 clarification 54: no GPU work while the app isn't active. iOS discards command
// buffers committed from the background, uploads included, so commits wait here until the app
// is active again, and presents are dropped meanwhile. The app switches it with
// pg_dxmt_set_app_active (ios_support.m); it starts active.
//
// Plain C with POSIX threads, no Apple calls: the host tests use it as it is, with their own
// clock values and log function. Also compiles as C++.

#ifndef DXMT_APP_ACTIVE_H
#define DXMT_APP_ACTIVE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

typedef void (*dxmt_app_gate_log_fn)(const char *line);

/* All-zero apart from the lock, the condition and the log function: active, no period yet. */
struct dxmt_app_gate {
  pthread_mutex_t lock;
  pthread_cond_t resumed;
  dxmt_app_gate_log_fn log;
  bool inactive;              /* false: active (the default) */
  uint64_t period;            /* inactive periods begun so far */
  uint64_t inactive_since_ns; /* when the current or last period began */
  uint64_t held;              /* commits held in the current or last period */
  uint64_t dropped;           /* presents dropped in it */
  bool held_logged;           /* the held line was logged in it */
  uint64_t lost_logged;       /* period + 1 whose lost line was logged, 0 for none */
};

#define DXMT_APP_GATE_INIT(log_fn) {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, (log_fn), false, 0, 0, 0, 0, false, 0}

static inline void
dxmt_app_gate_say(const struct dxmt_app_gate *gate, const char *line) {
  if (gate->log)
    gate->log(line);
}

/* The app resigned (0) or became (non-zero) active, at `now_ns` on a monotonic clock. Becoming
   active wakes every held commit and logs the resumed line; repeating a state does nothing. */
static inline void
dxmt_app_gate_set_active(struct dxmt_app_gate *gate, int active, uint64_t now_ns) {
  char line[160];
  bool say = false;
  pthread_mutex_lock(&gate->lock);
  if (!active && !gate->inactive) {
    gate->inactive = true;
    gate->period++;
    gate->inactive_since_ns = now_ns;
    gate->held = 0;
    gate->dropped = 0;
    gate->held_logged = false;
  } else if (active && gate->inactive) {
    gate->inactive = false;
    uint64_t ms = now_ns > gate->inactive_since_ns ? (now_ns - gate->inactive_since_ns) / 1000000u : 0;
    snprintf(line, sizeof(line), "winemetal: resumed after %llu ms, %llu command buffers held, %llu presents dropped",
             (unsigned long long)ms, (unsigned long long)gate->held, (unsigned long long)gate->dropped);
    say = true;
    pthread_cond_broadcast(&gate->resumed);
  }
  pthread_mutex_unlock(&gate->lock);
  if (say)
    dxmt_app_gate_say(gate, line);
}

/* Before a commit: returns at once while active (false); otherwise counts the commit, logs the
   held line once per period and blocks until the app is active again (true). */
static inline bool
dxmt_app_gate_hold(struct dxmt_app_gate *gate) {
  pthread_mutex_lock(&gate->lock);
  if (!gate->inactive) {
    pthread_mutex_unlock(&gate->lock);
    return false;
  }
  gate->held++;
  if (!gate->held_logged) {
    gate->held_logged = true;
    pthread_mutex_unlock(&gate->lock);
    dxmt_app_gate_say(gate, "winemetal: GPU work held while the app is inactive");
    pthread_mutex_lock(&gate->lock);
  }
  while (gate->inactive)
    pthread_cond_wait(&gate->resumed, &gate->lock);
  pthread_mutex_unlock(&gate->lock);
  return true;
}

/* Before asking the layer for a drawable: true (and counted) when the frame is to be dropped. */
static inline bool
dxmt_app_gate_drop_present(struct dxmt_app_gate *gate) {
  pthread_mutex_lock(&gate->lock);
  bool drop = gate->inactive;
  if (drop)
    gate->dropped++;
  pthread_mutex_unlock(&gate->lock);
  return drop;
}

/* A command buffer was refused for background execution anyway (at the edge of a change):
   logs "winemetal: GPU work lost in the background: <what>" once per period (the current or
   last one; period 0 before any). Returns whether it logged. */
static inline bool
dxmt_app_gate_lost(struct dxmt_app_gate *gate, const char *what) {
  pthread_mutex_lock(&gate->lock);
  bool say = gate->lost_logged != gate->period + 1;
  if (say)
    gate->lost_logged = gate->period + 1;
  pthread_mutex_unlock(&gate->lock);
  if (say) {
    char line[200];
    snprintf(line, sizeof(line), "winemetal: GPU work lost in the background: %s", what ? what : "?");
    dxmt_app_gate_say(gate, line);
  }
  return say;
}

static inline bool
dxmt_app_gate_active(struct dxmt_app_gate *gate) {
  pthread_mutex_lock(&gate->lock);
  bool active = !gate->inactive;
  pthread_mutex_unlock(&gate->lock);
  return active;
}

#endif
