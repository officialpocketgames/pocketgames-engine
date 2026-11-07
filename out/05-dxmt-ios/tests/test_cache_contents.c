/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Spec 05 clarification 12: host test of what a shader cache database holds and the line
 * logged when it opens (dxmt/src/winemetal/unix/cache_log.h), on small databases made here the
 * way the writer makes them (one table cache_<version>, key BLOB PRIMARY KEY, value BLOB): this
 * version's entries counted by the reader's query; a database with only an older version's
 * entries (starts empty); a new, empty one; one without this version's table, also with the
 * writer's failure; the read-only note; sizes rounded up to KB; the unavailable line.
 *
 *   cc -std=c11 -Isqlite3-api -I../dxmt/src/winemetal/unix test_cache_contents.c -l:libsqlite3.so.0
 *   ./a.out <directory for the fixtures>
 */

#include "cache_log.h"

#include <stdio.h>
#include <string.h>

static int g_checks, g_failures;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    if (!(cond)) {                                                                                                     \
      g_failures++;                                                                                                    \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                           \
    }                                                                                                                  \
  } while (0)

#define CHECK_STR(a, b)                                                                                                \
  do {                                                                                                                 \
    g_checks++;                                                                                                        \
    if (strcmp((a), (b)) != 0) {                                                                                       \
      g_failures++;                                                                                                    \
      printf("FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b));                                          \
    }                                                                                                                  \
  } while (0)

static const char *g_dir = ".";

/* A new database at <dir>/<name> made by `sql`. */
static int
make_db(const char *name, const char *sql, char *path, size_t size) {
  sqlite3 *db = NULL;
  char *err = NULL;
  int ok;
  snprintf(path, size, "%s/%s", g_dir, name);
  remove(path);
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) != SQLITE_OK) {
    printf("FAIL: can't create %s\n", path);
    sqlite3_close(db);
    return 0;
  }
  ok = sqlite3_exec(db, sql, NULL, NULL, &err) == SQLITE_OK;
  if (!ok)
    printf("FAIL: %s: %s\n", name, err ? err : "?");
  sqlite3_free(err);
  sqlite3_close(db);
  return ok;
}

/* What the reader sees: the database opened read-only, as CacheReader opens it. */
static struct dxmt_cache_contents
read_contents(const char *path, uint64_t version) {
  struct dxmt_cache_contents contents = {-2, 99};
  sqlite3 *db = NULL;
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK)
    dxmt_cache_read_contents(db, version, &contents);
  sqlite3_close(db);
  return contents;
}

#define TABLE(v) "CREATE TABLE cache_" #v " (key BLOB PRIMARY KEY, value BLOB NOT NULL);"

static void
test_this_version(void) {
  char path[1024], line[1200];
  CHECK(make_db(
      "cache_current.db",
      TABLE(18) "INSERT INTO cache_18 VALUES (x'01', x'aa'), (x'02', x'bb'), (x'03', x'cc');" TABLE(16)
          "INSERT INTO cache_16 VALUES (x'01', x'dd');",
      path, sizeof(path)
  ));
  struct dxmt_cache_contents c = read_contents(path, 18);
  CHECK(c.entries == 3);
  CHECK(c.other_version == 16); /* an older table doesn't hide this version's entries */
  dxmt_cache_open_line(line, sizeof(line), "/caches/dxmt/game.exe/shaders_310.db", &c, 5000, 18, NULL);
  CHECK_STR(line, "dxmt: shader cache /caches/dxmt/game.exe/shaders_310.db: open, 3 entries, 5 KB");
}

static void
test_older_version_only(void) {
  char path[1024], line[1200];
  /* an older build's entries, and the empty table this version's writer creates */
  CHECK(make_db(
      "cache_old.db",
      TABLE(16) "INSERT INTO cache_16 VALUES (x'01', x'aa'), (x'02', x'bb');" TABLE(15) TABLE(18), path,
      sizeof(path)
  ));
  struct dxmt_cache_contents c = read_contents(path, 18);
  CHECK(c.entries == 0);
  CHECK(c.other_version == 16);
  dxmt_cache_open_line(line, sizeof(line), "/c/s.db", &c, 8192, 18, NULL);
  CHECK_STR(line, "dxmt: shader cache /c/s.db: unavailable (it holds cache version 16, not 18; starts empty)");
}

static void
test_new_database(void) {
  char path[1024], line[1200];
  CHECK(make_db("cache_new.db", TABLE(18), path, sizeof(path)));
  struct dxmt_cache_contents c = read_contents(path, 18);
  CHECK(c.entries == 0);
  CHECK(c.other_version == 0);
  dxmt_cache_open_line(line, sizeof(line), "/c/s.db", &c, 0, 18, NULL);
  CHECK_STR(line, "dxmt: shader cache /c/s.db: open, 0 entries, 0 KB");
}

static void
test_no_table(void) {
  char path[1024], line[1200];
  CHECK(make_db("cache_notable.db", "CREATE TABLE other (x INTEGER);", path, sizeof(path)));
  struct dxmt_cache_contents c = read_contents(path, 18);
  CHECK(c.entries == -1);
  CHECK(c.other_version == 0);
  dxmt_cache_open_line(line, sizeof(line), "/c/s.db", &c, 4096, 18, NULL);
  CHECK_STR(line, "dxmt: shader cache /c/s.db: unavailable (no table for cache version 18)");
  dxmt_cache_open_line(line, sizeof(line), "/c/s.db", &c, 4096, 18, "can't create its table: disk full");
  CHECK_STR(line, "dxmt: shader cache /c/s.db: unavailable (no table for cache version 18; can't create its table: disk full)");
}

static void
test_text(void) {
  char line[1200];
  struct dxmt_cache_contents c = {7, 0};
  dxmt_cache_open_line(line, sizeof(line), "/c/s.db", &c, 2048, 18, "can't be opened for writing: readonly");
  CHECK_STR(line, "dxmt: shader cache /c/s.db: open, 7 entries, 2 KB, read-only (can't be opened for writing: readonly)");
  dxmt_cache_open_line(line, sizeof(line), "p", &c, 1, 18, NULL);
  CHECK_STR(line, "dxmt: shader cache p: open, 7 entries, 1 KB");
  dxmt_cache_open_line(line, sizeof(line), "p", &c, 1024, 18, NULL);
  CHECK_STR(line, "dxmt: shader cache p: open, 7 entries, 1 KB");
  dxmt_cache_open_line(line, sizeof(line), "p", &c, 1025, 18, NULL);
  CHECK_STR(line, "dxmt: shader cache p: open, 7 entries, 2 KB");
  dxmt_cache_unavailable_line(line, sizeof(line), "dxmt/game.exe/shaders_310.db", "no Caches directory");
  CHECK_STR(line, "dxmt: shader cache dxmt/game.exe/shaders_310.db: unavailable (no Caches directory)");
}

int
main(int argc, char **argv) {
  if (argc > 1)
    g_dir = argv[1];
  test_this_version();
  test_older_version_only();
  test_new_database();
  test_no_table();
  test_text();
  printf("test_cache_contents: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
