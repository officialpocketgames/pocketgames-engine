// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// CI link check: do-nothing definitions of what the app itself provides to the unix side,
// interfaces/wine_unix_contract.h Section B (spec 02 implements them in the real app), and the
// app hooks of specs 10 Part C and 11. Compiled against the contract header, so a signature that
// drifts from it fails here.

#include <stddef.h>
#include <stdint.h>
#include <mach/mach.h>

/* the local declarations the contract asks its includer for */
typedef void *HWND;
typedef void *HCURSOR;
typedef unsigned int UINT;
typedef int INT;
typedef int BOOL;
typedef uint32_t DWORD;
typedef int32_t WINELONG;
typedef struct { WINELONG left, top, right, bottom; } RECT;
struct window_rects;
struct window_surface;

#include "wine_unix_contract.h"

_Thread_local jmp_buf wine_ios_exit_jmpbuf;
_Thread_local volatile int wine_ios_exit_code;
_Thread_local pthread_t wine_ios_main_thread;
_Thread_local int wine_ios_exit_initialized;

void wine_launched_process_did_exit(int status) { (void)status; }
void winios_phase(const char *name) { (void)name; }
void fatal_error(const char *err, ...) { (void)err; pthread_exit(NULL); }
volatile int g_wineserver_should_stop;
const char wine_build[] = "link-check";

/* IOKit's power-source query, which iOS lacks (CoreFoundation types, as opaque pointers) */
const void *IOPSCopyPowerSourcesInfo(void) { return NULL; }
const void *IOPSCopyPowerSourcesList(const void *blob) { (void)blob; return NULL; }
const void *IOPSGetPowerSourceDescription(const void *blob, const void *ps) { (void)blob; (void)ps; return NULL; }

BOOL winios_pCreateWindow(HWND hwnd) { (void)hwnd; return 0; }
void winios_pDestroyWindow(HWND hwnd) { (void)hwnd; }
UINT winios_pShowWindow(HWND hwnd, INT cmd, RECT *rect, UINT swp) { (void)hwnd; (void)cmd; (void)rect; (void)swp; return ~0u; }
void winios_pWindowPosChanged(HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                              const struct window_rects *new_rects, struct window_surface *surface)
{ (void)hwnd; (void)insert_after; (void)owner_hint; (void)swp_flags; (void)new_rects; (void)surface; }
BOOL winios_pProcessEvents(DWORD mask) { (void)mask; return 0; }
void winios_pSetCursor(HWND hwnd, HCURSOR cursor) { (void)hwnd; (void)cursor; }
void winios_pDestroyCursorIcon(HCURSOR cursor) { (void)cursor; }

int winios_surface_present(HWND hwnd, int dirty_x, int dirty_y, int dirty_w, int dirty_h,
                           int surf_w, int surf_h, int stride, const void *bits)
{ (void)hwnd; (void)dirty_x; (void)dirty_y; (void)dirty_w; (void)dirty_h; (void)surf_w; (void)surf_h; (void)stride; (void)bits; return 1; }
void winios_window_frame(HWND hwnd, int x, int y, int w, int h, int visible,
                         int cx, int cy, int cw, int ch)
{ (void)hwnd; (void)x; (void)y; (void)w; (void)h; (void)visible; (void)cx; (void)cy; (void)cw; (void)ch; }
void winios_cursor_set(unsigned int id, int w, int h, int hot_x, int hot_y, const void *bgra)
{ (void)id; (void)w; (void)h; (void)hot_x; (void)hot_y; (void)bgra; }
void winios_cursor_show(int show) { (void)show; }
void winios_dump_srcbits(const void *bits, int w, int h, int stride) { (void)bits; (void)w; (void)h; (void)stride; }

int  winios_direct_cursor_wanted(void) { return 0; }
void winios_direct_cursor_set(unsigned int id, int w, int h, int hot_x, int hot_y, const void *bgra)
{ (void)id; (void)w; (void)h; (void)hot_x; (void)hot_y; (void)bgra; }
void winios_direct_cursor_show(int show) { (void)show; }
void winios_direct_cursor_pos(int x, int y) { (void)x; (void)y; }

void winios_display_mode_changed(int w, int h) { (void)w; (void)h; }

/* --- the app's CPU modules and signed-image loader (Section B; specs 10 Part C and 11).
 * mid_fxi_mach_fault is given in the header only as a comment (its types come from
 * <mach/mach.h>), so it is defined with those types here --- */
_Bool mid_pe_dylib_instance(void *dlopen_handle, const char *path,
                            struct ios_nojit_pe_instance *out, char *err, size_t errlen)
{ (void)dlopen_handle; (void)path; (void)out; (void)err; (void)errlen; return 0; }
void *mid_fxi_win_host_table(int tsd_offset) { (void)tsd_offset; return NULL; }
void *mid_fxi_wow_host_table(int tsd_offset) { (void)tsd_offset; return NULL; }
int mid_fxi_mach_fault(mach_port_t thread, arm_thread_state64_t *state, uint64_t fault_addr)
{ (void)thread; (void)state; (void)fault_addr; return 0; }

int  winios_gamepad_get_state(int index, struct winios_gamepad *out) { (void)index; (void)out; return 0; }
void winios_gamepad_set_vibration(int index, uint16_t left, uint16_t right) { (void)index; (void)left; (void)right; }
int  winios_gamepad_rumble_caps(void) { return 0; }
int  winios_hidpad_get_state(struct winios_hidpad *out) { (void)out; return 0; }
void winios_hidpad_set_output(const struct winios_hidpad_output *output) { (void)output; }
