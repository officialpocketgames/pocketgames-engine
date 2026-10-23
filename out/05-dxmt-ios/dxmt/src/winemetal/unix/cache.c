/* Changed for MYIOSDECK (spec 05 H3). Upstream: DXMT a3a6e8f, MIT (see LICENSE). */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include "sqlite3.h"
#define WINEMETAL_API
#include "../winemetal_thunks.h"

@interface CacheReader : NSObject
- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version;
- (dispatch_data_t)get:(NSData *)key;
@end

@interface CacheWriter : NSObject
- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version;
- (void)set:(NSData *)key value:(dispatch_data_t)value;
@end

@interface CacheReader () {
  sqlite3 *_db;
  sqlite3_stmt *_stmt;
}
@end

#if defined(DXMT_IOS)
/* Spec 05 H3: on iOS a relative cache path resolves under the app's
   NSCachesDirectory. The directory (the parent of a file path) is created with
   intermediate directories; nil if it can't be resolved or created. */
static inline NSString *
resolve_cache_dir(NSString *path, bool path_is_file) {
  if (!path)
    return nil;
  if (![path hasPrefix:@"/"]) {
    NSArray *dirs = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    if (dirs.count == 0)
      return nil;
    path = [dirs[0] stringByAppendingPathComponent:path];
  }
  NSString *dir = path_is_file ? [path stringByDeletingLastPathComponent] : path;
  NSError *error = nil;
  if (![[NSFileManager defaultManager] createDirectoryAtPath:dir
                                 withIntermediateDirectories:YES
                                                  attributes:nil
                                                       error:&error])
    return nil;
  return path;
}
#else
static inline NSString *
resolve_cache_dir(NSString *path, bool path_is_file) {
  if (![path hasPrefix:@"/"]) {
    char buf[PATH_MAX];
    size_t len = confstr(_CS_DARWIN_USER_CACHE_DIR, buf, sizeof(buf));

    if (!len) {
      return nil;
    }

    NSString *base = [NSString stringWithUTF8String:buf];
    path = [base stringByAppendingPathComponent:path];
  }
  [[NSFileManager defaultManager] createDirectoryAtPath:path_is_file ? [path stringByDeletingLastPathComponent] : path
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];
  return path;
}
#endif

@implementation CacheReader

- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version {
  if ((self = [super init])) {
    NSString *dbPath = resolve_cache_dir(path, true);
    if (!dbPath) {
      NSLog(@"[CacheReader] Failed to resolve cache path");
      return nil;
    }
    NSString *tableName = [NSString stringWithFormat:@"cache_%llu", version];
    if (sqlite3_open_v2([dbPath fileSystemRepresentation], &_db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL) !=
        SQLITE_OK) {
      NSLog(@"[CacheReader] Failed to open DB: %s", sqlite3_errmsg(_db));
      sqlite3_close(_db);
      return nil;
    }

    NSString *sqlGet = [NSString stringWithFormat:@"SELECT value FROM %@ WHERE key = ?;", tableName];
    if (sqlite3_prepare_v2(_db, sqlGet.UTF8String, -1, &_stmt, NULL) != SQLITE_OK) {
      NSLog(@"[CacheReader] Failed to prepare SELECT: %s", sqlite3_errmsg(_db));
      sqlite3_close(_db);
      return nil;
    }
  }
  return self;
}

- (dispatch_data_t)get:(NSData *)key {
  sqlite3_reset(_stmt);
  sqlite3_clear_bindings(_stmt);
  sqlite3_bind_blob64(_stmt, 1, key.bytes, key.length, SQLITE_STATIC);

  dispatch_data_t result = nil;
  if (sqlite3_step(_stmt) == SQLITE_ROW) {
    const void *bytes = sqlite3_column_blob(_stmt, 0);
    int len = sqlite3_column_bytes(_stmt, 0);
    result = dispatch_data_create(bytes, len, nil, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  }
  sqlite3_reset(_stmt);
  return result;
}

- (void)dealloc {
  if (_stmt)
    sqlite3_finalize(_stmt);
  if (_db)
    sqlite3_close(_db);
  [super dealloc];
}

@end

@interface CacheWriter () {
  sqlite3 *_db;
  sqlite3_stmt *_stmt;
}
@end

@implementation CacheWriter

- (instancetype)initWithPath:(NSString *)path version:(uint64_t)version {
  if ((self = [super init])) {
    NSString *dbPath = resolve_cache_dir(path, true);
    if (!dbPath) {
      NSLog(@"[CacheReader] Failed to resolve cache path");
      return nil;
    }

    NSString *lockPath = [dbPath stringByAppendingString:@"-lock"];
    int fd = open([lockPath fileSystemRepresentation], O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
      NSLog(@"[CacheWriter] Failed to open file for locking %@", lockPath);
      return nil;
    }
    flock(fd, LOCK_EX);

    if (sqlite3_open_v2(
            [dbPath fileSystemRepresentation], &_db, //
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL
        ) != SQLITE_OK) {
      NSLog(@"[CacheWriter] Failed to open DB: %s", sqlite3_errmsg(_db));
      flock(fd, LOCK_UN);
      close(fd);
      return nil;
    }

    sqlite3_exec(_db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(_db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);

    NSString *tableName = [NSString stringWithFormat:@"cache_%llu", version];
    NSString *sqlCreate = [NSString stringWithFormat:@"CREATE TABLE IF NOT EXISTS %@ ("
                                                      "key BLOB PRIMARY KEY, "
                                                      "value BLOB NOT NULL);",
                                                     tableName];

    char *errMsg = NULL;
    if (sqlite3_exec(_db, sqlCreate.UTF8String, NULL, NULL, &errMsg) != SQLITE_OK) {
      NSLog(@"[CacheWriter] Failed to create table: %s", errMsg);
      sqlite3_free(errMsg);
    }

    flock(fd, LOCK_UN);
    close(fd);

    NSString *sqlSet = [NSString stringWithFormat:@"INSERT OR REPLACE INTO %@ (key, value) VALUES (?, ?);", tableName];
    if (sqlite3_prepare_v2(_db, sqlSet.UTF8String, -1, &_stmt, NULL) != SQLITE_OK) {
      NSLog(@"[CacheWriter] Failed to prepare INSERT: %s", sqlite3_errmsg(_db));
      sqlite3_close(_db);
      return nil;
    }
  }
  return self;
}

- (void)set:(NSData *)key value:(dispatch_data_t)value {
  sqlite3_reset(_stmt);
  sqlite3_clear_bindings(_stmt);
  sqlite3_bind_blob64(_stmt, 1, key.bytes, key.length, SQLITE_STATIC);

  const void *bytes = NULL;
  size_t length = 0;
  dispatch_data_t flat = dispatch_data_create_map(value, &bytes, &length);
  sqlite3_bind_blob64(_stmt, 2, bytes, length, SQLITE_STATIC);

  if (sqlite3_step(_stmt) != SQLITE_DONE) {
    NSLog(@"[CacheWriter] Failed to insert: %s", sqlite3_errmsg(_db));
  }

  dispatch_release(flat);
  sqlite3_reset(_stmt);
}

- (void)dealloc {
  if (_stmt)
    sqlite3_finalize(_stmt);
  if (_db)
    sqlite3_close(_db);
  [super dealloc];
}

@end

int
_CacheReader_alloc_init(void *obj) {
  struct unixcall_cache_alloc_init *params = obj;
  NSString *path = [[NSString alloc] initWithCString:params->path.ptr encoding:NSUTF8StringEncoding];
  params->ret_cache = (obj_handle_t)[[CacheReader alloc] initWithPath:path version:params->version];
  [path release];
  return 0;
}

int
_CacheReader_get(void *obj) {
  struct unixcall_cache_get *params = obj;
  NSData *key =
      [[NSData alloc] initWithBytesNoCopy:(void *)params->key.ptr length:params->key_length freeWhenDone:false];
  CacheReader *reader = (CacheReader *)params->cache;
  params->ret_data = (obj_handle_t)[reader get:key];
  [key release];
  return 0;
}

int
_CacheWriter_alloc_init(void *obj) {
  struct unixcall_cache_alloc_init *params = obj;
  NSString *path = [[NSString alloc] initWithCString:params->path.ptr encoding:NSUTF8StringEncoding];
  params->ret_cache = (obj_handle_t)[[CacheWriter alloc] initWithPath:path version:params->version];
  [path release];
  return 0;
}

int
_CacheWriter_set(void *obj) {
  struct unixcall_cache_set *params = obj;
  NSData *key =
      [[NSData alloc] initWithBytesNoCopy:(void *)params->key.ptr length:params->key_length freeWhenDone:false];
  CacheWriter *writer = (CacheWriter *)params->cache;
  [writer set:key value:(dispatch_data_t)params->value_data];
  [key release];
  return 0;
}

#if defined(DXMT_IOS) && !defined(DXMT_NO_PRIVATE_API)

/* Spec 05 H3: the private setter is looked up at run time on iOS (it may not
   be exported there), and Metal's cache path is left alone, not set to nil,
   when the path can't be resolved or created. */
int
_WMTSetMetalShaderCachePath(void *obj) {
  struct unixcall_setmetalcachepath *params = obj;
  void (*set_path)(NSString *) = (void (*)(NSString *))dlsym(RTLD_DEFAULT, "MTLSetShaderCachePath");
  NSString *(*get_path)(void) = (NSString * (*)(void)) dlsym(RTLD_DEFAULT, "MTLGetShaderCachePath");
  params->ret_success = 0;
  if (!set_path)
    return 0;
  NSString *path = [[NSString alloc] initWithCString:params->path.ptr encoding:NSUTF8StringEncoding];
  NSString *resolved_path = resolve_cache_dir(path, false);
  if (resolved_path) {
    set_path(resolved_path);
    params->ret_success = get_path ? [get_path() isEqualToString:resolved_path] : 1;
  }
  [path release];
  return 0;
}

#elif !defined(DXMT_NO_PRIVATE_API)

extern void MTLSetShaderCachePath(NSString* path);
extern NSString* MTLGetShaderCachePath();

int
_WMTSetMetalShaderCachePath(void *obj) {
  struct unixcall_setmetalcachepath *params = obj;
  NSString *path = [[NSString alloc] initWithCString:params->path.ptr encoding:NSUTF8StringEncoding];
  NSString *resolved_path = resolve_cache_dir(path, false);
  MTLSetShaderCachePath(resolved_path);
  params->ret_success = [MTLGetShaderCachePath() isEqualToString:resolved_path];
  [path release];
  return 0;
};

#else

int
WMTSetMetalShaderCachePath(void *obj) {
  struct unixcall_setmetalcachepath *params = obj;
  params->ret_success = 0;
  return 0;
}

#endif
