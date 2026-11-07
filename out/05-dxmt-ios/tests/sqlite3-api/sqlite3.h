/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 the MYIOSDECK author
 *
 * Test-only: declarations of the part of SQLite's public C API (sqlite.org/c3ref) that
 * dxmt/src/winemetal/unix/cache_log.h and tests/test_cache_contents.c use, so the host test
 * links the system's libsqlite3.so.0 on a host without SQLite's development headers. The app
 * builds cache.c against the iOS SDK's own sqlite3.h. SQLite itself is in the public domain.
 */
#ifndef DXMT_TEST_SQLITE3_H
#define DXMT_TEST_SQLITE3_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef long long int sqlite3_int64;

#define SQLITE_OK 0
#define SQLITE_ROW 100
#define SQLITE_DONE 101

#define SQLITE_OPEN_READONLY 0x00000001
#define SQLITE_OPEN_READWRITE 0x00000002
#define SQLITE_OPEN_CREATE 0x00000004

int sqlite3_open_v2(const char *filename, sqlite3 **ppDb, int flags, const char *zVfs);
int sqlite3_close(sqlite3 *db);
int sqlite3_exec(sqlite3 *db, const char *sql, int (*callback)(void *, int, char **, char **), void *arg, char **errmsg);
int sqlite3_prepare_v2(sqlite3 *db, const char *zSql, int nByte, sqlite3_stmt **ppStmt, const char **pzTail);
int sqlite3_step(sqlite3_stmt *stmt);
sqlite3_int64 sqlite3_column_int64(sqlite3_stmt *stmt, int iCol);
const unsigned char *sqlite3_column_text(sqlite3_stmt *stmt, int iCol);
int sqlite3_finalize(sqlite3_stmt *stmt);
const char *sqlite3_errmsg(sqlite3 *db);
void sqlite3_free(void *p);

#ifdef __cplusplus
}
#endif

#endif
