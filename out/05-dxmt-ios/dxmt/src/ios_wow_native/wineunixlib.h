/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (c) 2026 the MYIOSDECK author */
/*
 * Spec 33 B2: in the ios_wow_native build, winemetal's thunks (winemetal_thunks.c) call the unix
 * side directly through its 64-bit table, as src/nativemetal/wineunixlib.h does for the macOS
 * native build. On iOS that table is dxmt_winemetal_unix_call_funcs (spec 05 Part A), linked
 * into the same binary. The arguments go as they are: 64-bit structs, no WoW64 conversion (spec
 * 06 C), and encodeCommands chains unchanged.
 */
#ifndef __WINE_WINE_UNIXLIB_H
#define __WINE_WINE_UNIXLIB_H

typedef int NTSTATUS;
#define NTSTATUS_SUCCESS 0

typedef NTSTATUS (*dxmt_native_unix_entry)(void *args);

#ifdef __cplusplus
extern "C" {
#endif
extern const void *dxmt_winemetal_unix_call_funcs[];
/* Spec 05 item 89: the unix side counts what each program holds and releases it when the
   program's process ends, but the native frontend's objects of an ended process stay as they
   are (spec 33 clarification 13), so its calls are not counted. Weak: absent in a build without
   item 89 (and in the Linux compile check). */
extern void dxmt_ios_untracked_begin(void) __attribute__((weak));
extern void dxmt_ios_untracked_end(void) __attribute__((weak));
#ifdef __cplusplus
}
#endif

static inline NTSTATUS
dxmt_native_unix_call(unsigned int code, void *args) {
  if (dxmt_ios_untracked_begin)
    dxmt_ios_untracked_begin();
  NTSTATUS status = ((dxmt_native_unix_entry)dxmt_winemetal_unix_call_funcs[code])(args);
  if (dxmt_ios_untracked_end)
    dxmt_ios_untracked_end();
  return status;
}

#define WINE_UNIX_CALL(code, args) dxmt_native_unix_call((unsigned int)(code), (void *)(args))

#endif /* __WINE_WINE_UNIXLIB_H */
