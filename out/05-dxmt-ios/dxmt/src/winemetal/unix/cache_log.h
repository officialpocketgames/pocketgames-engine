/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 05 clarification 12: what a shader cache database holds, for the line logged when it
 * opens (cache.c, once per database per process). Plain C over SQLite's public API, so the
 * host tests run it on a small database (tests/test_cache_contents.c).
 *
 * A database keeps one table per cache version, cache_<version> (key BLOB, value BLOB), made
 * by the writer. Entries are the rows of this version's table; a database whose only entries
 * are in another version's table is reported as not usable for this version (it starts empty).
 */
#ifndef DXMT_CACHE_LOG_H
#define DXMT_CACHE_LOG_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "sqlite3.h"

struct dxmt_cache_contents {
  int64_t entries;        /* rows of cache_<version>; -1 when there is no such table */
  uint64_t other_version; /* the highest version of another cache_<n> table, 0 if none */
};

static inline void
dxmt_cache_read_contents(sqlite3 *db, uint64_t version, struct dxmt_cache_contents *out) {
  char sql[64];
  sqlite3_stmt *stmt = NULL;

  out->entries = -1;
  out->other_version = 0;

  snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM cache_%" PRIu64 ";", version);
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW)
    out->entries = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt); /* a no-op on NULL */

  stmt = NULL;
  if (sqlite3_prepare_v2(
          db, "SELECT name FROM sqlite_master WHERE type = 'table' AND name GLOB 'cache_[0-9]*';", -1, &stmt, NULL
      ) == SQLITE_OK) {
    while (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *name = (const char *)sqlite3_column_text(stmt, 0);
      if (!name)
        continue;
      uint64_t n = strtoull(name + 6, NULL, 10); /* after "cache_" */
      if (n != version && n > out->other_version)
        out->other_version = n;
    }
  }
  sqlite3_finalize(stmt);
}

/* The line for a database the reader opened, without the newline. `bytes` is the size of the
   database file and its write-ahead log; `not_writable` says why the writer couldn't open it,
   or is NULL. */
static inline void
dxmt_cache_open_line(
    char *buf, size_t size, const char *path, const struct dxmt_cache_contents *contents, uint64_t bytes,
    uint64_t version, const char *not_writable
) {
  if (contents->entries < 0)
    snprintf(
        buf, size, "dxmt: shader cache %s: unavailable (no table for cache version %" PRIu64 "%s%s)", path, version,
        not_writable ? "; " : "", not_writable ? not_writable : ""
    );
  else if (contents->entries == 0 && contents->other_version)
    snprintf(
        buf, size, "dxmt: shader cache %s: unavailable (it holds cache version %" PRIu64 ", not %" PRIu64 "; starts empty)",
        path, contents->other_version, version
    );
  else
    snprintf(
        buf, size, "dxmt: shader cache %s: open, %" PRId64 " entries, %" PRIu64 " KB%s%s%s", path, contents->entries,
        (bytes + 1023) / 1024, not_writable ? ", read-only (" : "", not_writable ? not_writable : "",
        not_writable ? ")" : ""
    );
}

/* The line for a database that couldn't be opened (or a path that couldn't be resolved). */
static inline void
dxmt_cache_unavailable_line(char *buf, size_t size, const char *path, const char *reason) {
  snprintf(buf, size, "dxmt: shader cache %s: unavailable (%s)", path, reason);
}

#endif
