// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// The binary contract between the app-side Wine code (spec 02) and the iOS build of Wine's
// unix side (ntdll, win32u and wineserver compiled as static libraries into the same app
// binary). The unix side already exists and is built separately. Names, signatures, struct
// layouts and field names here must match exactly: the unix side is compiled against them.
//
// Section A: what the unix side PROVIDES (the app calls these).
// Section B: what the app MUST PROVIDE (the unix side calls these). Hooks marked "weak"
//            are optional on the unix side's end; the rest are hard link dependencies.
// Section C: shared data structures.
// Section D: environment variables the unix side reads.
//
// Types: the Wine handle types are pointer-sized. Spec 02 code must not include Wine's own
// headers (they clash with Apple's), so it declares what it needs locally:
//   typedef void *HWND; typedef void *HCURSOR; typedef unsigned int UINT; typedef int INT;
//   typedef uint32_t DWORD; typedef int32_t WINELONG;   /* Win32's 32-bit DWORD and LONG */
//   typedef struct { WINELONG left, top, right, bottom; } RECT;
//   struct window_rects; struct window_surface;   /* opaque, never dereferenced */
// BOOL is Objective-C's BOOL in .m files; on the unix side it is a 32-bit int. Hooks that
// return BOOL across the boundary return 0 or 1 only.

#ifndef MYIOSDECK_WINE_UNIX_CONTRACT_H
#define MYIOSDECK_WINE_UNIX_CONTRACT_H

#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <pthread.h>

/* Declared here at file scope so the prototypes in Section B name the same types that
 * Section C defines (a struct first named inside a parameter list is a different type). */
struct winios_gamepad;
struct winios_hidpad;
struct winios_hidpad_output;
struct winios_gamepad_vibration;

/* ================= Section A: provided by the unix side ================= */

/* ntdll: the Windows process main. argv[0] is "wine", argv[1] the Windows path of the
 * program, the rest its arguments. Runs the program on the calling thread. It either
 * returns, or the program's exit longjmps to wine_ios_exit_jmpbuf (Section B). */
extern void __wine_main(int argc, char *argv[]);

/* ntdll: send ntdll's own log lines to this file (append). */
extern void wine_log_set_file(const char *path);

/* ntdll (spec 22): boot another top-level program in the running session on the calling thread,
 * after the first program ended. server_fd: the client end of its own socket pair (the server end
 * was injected). i386: 1 for a 32-bit main image. Never returns on success: the program's exit
 * longjmps to wine_ios_exit_jmpbuf like the first program's. Returns a non-zero status when the
 * boot failed. */
extern int ios_session_start_program(int argc, char *argv[], int server_fd, int i386);

/* ntdll: set to 1 before __wine_main when the main program is a 32-bit (i386) PE, so the unix
 * side reserves the 32-bit address window before the first thread starts. */
extern int ios_main_image_i386;

/* ntdll: how many child processes started by the session's program are still alive and were
 * started at most max_age seconds ago (max_age < 0: any age). Crash reporters and helper
 * tools are not counted. Fills buf with their names, comma separated. */
extern int madeira_live_game_children(char *buf, int len, double max_age);

/* wineserver: its main(), renamed. Runs the server loop on the calling thread until stopped. */
extern int wineserver_main(int argc, char *argv[]);
/* wineserver: globals. Set foreground = 1 before wineserver_main. */
extern int foreground;
extern int debug_level;
/* wineserver: becomes 1 when the server has loaded the registry and enters its main loop.
 * Read with acquire ordering. The app resets it to 0 before starting the server thread. */
extern int wineserver_ready;
/* wineserver: hand it the server end of a connected socket pair. The server treats it as a
 * new client connection (the first process). */
extern void wineserver_inject_client_fd(int fd);
/* wineserver: log file for its C code, and where the NLS tables are (a directory). */
extern void wineserver_log_set_file(const char *path);
extern void wineserver_set_nls_dir(const char *path);
/* wineserver: a printf-style line to its log; and a line to the app UI log. */
extern void ws_log(const char *fmt, ...);
extern void wine_ui_log(const char *message);

/* win32u (the iOS user driver's unix half): queue hardware input for the Windows side.
 * Must be called on a Wine thread (with a Windows thread context), so the app queues input
 * and the driver hook winios_pProcessEvents drains it there.
 *   mouse: x, y and flags exactly as the Win32 MOUSEINPUT (MOUSEEVENTF_*; with ABSOLUTE the
 *          coordinates are desktop pixels), mouse_data the wheel delta / XBUTTON, hwnd NULL.
 *   key:   a virtual-key code, flags KEYEVENTF_* (0x0002 = key up). */
extern void winios_drv_post_mouse(int x, int y, unsigned int flags, unsigned int mouse_data, HWND hwnd);
extern void winios_drv_post_key(unsigned short vk, unsigned int flags);

/* ================= Section B: provided by the app (spec 02) ================= */

/* --- process exit (hard dependencies; thread-local) ---
 * ntdll ends a Windows process by storing its exit code in wine_ios_exit_code and calling
 * longjmp(wine_ios_exit_jmpbuf, 1) when the exiting thread is the one that called
 * __wine_main (wine_ios_main_thread) and wine_ios_exit_initialized is 1. Each must be a
 * _Thread_local variable with exactly this name and type. */
extern _Thread_local jmp_buf wine_ios_exit_jmpbuf;
extern _Thread_local volatile int wine_ios_exit_code;
extern _Thread_local pthread_t wine_ios_main_thread;
extern _Thread_local int wine_ios_exit_initialized;

/* ntdll calls this (weak) when the session's initial program exits, with its exit status. */
void wine_launched_process_did_exit(int status);

/* ntdll calls this with a short milestone name during process start-up (hard dependency). */
void winios_phase(const char *name);

/* wineserver: its fatal error handler is replaced by the app's (hard dependency). It must log
 * the message and end only the calling thread, never the app. */
void fatal_error(const char *err, ...);

/* wineserver: checked by its event loop; set to 1 to make wineserver_main return. */
extern volatile int g_wineserver_should_stop;

/* ntdll: a version string (hard dependency). */
extern const char wine_build[];

/* Wine's power-source query uses IOKit's IOPowerSources API, which iOS lacks. Provide all three
 * returning NULL (CoreFoundation types). */
/* CFTypeRef IOPSCopyPowerSourcesInfo(void);
   CFArrayRef IOPSCopyPowerSourcesList(CFTypeRef blob);
   CFDictionaryRef IOPSGetPowerSourceDescription(CFTypeRef blob, CFTypeRef ps); */

/* --- user driver hooks (all weak), called by win32u on Wine threads --- */
BOOL winios_pCreateWindow(HWND hwnd);
void winios_pDestroyWindow(HWND hwnd);
/* Must return ~0u ("no override") unless it deliberately changes the SWP_* flags: win32u
 * uses any other value as the complete new flag set. */
UINT winios_pShowWindow(HWND hwnd, INT cmd, RECT *rect, UINT swp);
void winios_pWindowPosChanged(HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                              const struct window_rects *new_rects, struct window_surface *surface);
/* Called from the thread's message pump: deliver queued input. Return 1 if any was delivered. */
BOOL winios_pProcessEvents(DWORD mask);
void winios_pSetCursor(HWND hwnd, HCURSOR cursor);
void winios_pDestroyCursorIcon(HCURSOR cursor);

/* --- window images (weak), Wine threads ---
 * A top-level window's GDI surface was flushed. The whole surface is a 32-bit BGRX DIB of
 * surf_w x surf_h with `stride` bytes per row; `bits` is valid only during the call. The dirty
 * rectangle is informational. Return 1 when handled; return 0 only if the bits could not be
 * copied, and win32u then keeps the damage and flushes again later. Surfaces may be larger
 * than the window (sizes are rounded up); crop to the window's size when drawing. */
int winios_surface_present(HWND hwnd, int dirty_x, int dirty_y, int dirty_w, int dirty_h,
                           int surf_w, int surf_h, int stride, const void *bits);
/* A top-level window moved, resized, or was shown or hidden. (x, y, w, h): its visible
 * rectangle; (cx, cy, cw, ch): its client rectangle; both in desktop pixels. */
void winios_window_frame(HWND hwnd, int x, int y, int w, int h, int visible,
                         int cx, int cy, int cw, int ch);
/* The program's cursor image changed (straight-alpha BGRA, w*h*4 bytes, valid during the
 * call), or it was shown or hidden. Used by desktop sessions only; spec 02 may ignore them. */
void winios_cursor_set(unsigned int id, int w, int h, int hot_x, int hot_y, const void *bgra);
void winios_cursor_show(int show);
/* win32u's DIB driver calls this (hard dependency) with the source bits of every PutImage
 * (w x h pixels, `stride` bytes per row, valid only during the call) for an optional diagnostic
 * dump. A no-op is enough. */
void winios_dump_srcbits(const void *bits, int w, int h, int stride);

/* --- the program's cursor for hardware-mouse users (weak). If wanted() returns 0 the driver
 * never calls the other three. Spec 02 returns 0 and makes the others no-ops. --- */
int  winios_direct_cursor_wanted(void);
void winios_direct_cursor_set(unsigned int id, int w, int h, int hot_x, int hot_y, const void *bgra);
void winios_direct_cursor_show(int show);
void winios_direct_cursor_pos(int x, int y);

/* --- display (weak) --- win32u calls this when a program changes the display mode. */
void winios_display_mode_changed(int w, int h);

/* --- controllers (hard dependencies), any thread, see Section C --- */
int  winios_gamepad_get_state(int index, struct winios_gamepad *out);
void winios_gamepad_set_vibration(int index, uint16_t left, uint16_t right);
int  winios_gamepad_rumble_caps(void);
int  winios_hidpad_get_state(struct winios_hidpad *out);
void winios_hidpad_set_output(const struct winios_hidpad_output *output);

/* --- the app's interpreters and signed-image loader (hard dependencies) ---
 * Defined by the app, called by ntdll's unix side. Exact rules: spec 10 Part C (images from
 * signed dylibs) and spec 11 Part C (the Mach exception port).
 *   mid_pe_dylib_instance: a fresh instance of the PE image a signed dylib carries; struct
 *     ios_nojit_pe_instance is declared by spec 10 Part C. Returns 0 and fills err on failure.
 *   mid_fxi_win_host_table / mid_fxi_wow_host_table: the interpreter's host table for the x64
 *     (ARM64EC) and the i386 (WoW64) CPU module, given the TEB's TSD slot offset.
 *   mid_fxi_mach_fault: offered every EXC_BAD_ACCESS of a registered Windows thread first;
 *     non-zero = handled, state updated (spec 11 Part C2). Its types come from <mach/mach.h>:
 *     int mid_fxi_mach_fault(mach_port_t thread, arm_thread_state64_t *state, uint64_t fault_addr); */
struct ios_nojit_pe_instance;
_Bool mid_pe_dylib_instance(void *dlopen_handle, const char *path,
                            struct ios_nojit_pe_instance *out, char *err, size_t errlen);
void *mid_fxi_win_host_table(int tsd_offset);
void *mid_fxi_wow_host_table(int tsd_offset);

/* ================= Section C: shared structures ================= */

#define WINIOS_GAMEPAD_MAX 4

/* One XInput slot. 20 bytes, no padding. `packet` is owned by the store: it increases by one
 * every time a stored value actually changes, and the unix side uses it as XInput's
 * dwPacketNumber. Buttons are XINPUT_GAMEPAD_* bits; sticks full signed range, +y up. */
struct winios_gamepad {
    uint32_t packet;
    uint16_t buttons;
    uint8_t left_trigger, right_trigger;
    int16_t lx, ly, rx, ry;
    uint8_t connected;
    uint8_t reserved[3];
};

/* Extra button bits of the HID gamepad, above the XINPUT_GAMEPAD_* bits in the low 16. */
#define WINIOS_HIDPAD_TOUCHPAD 0x00010000u
#define WINIOS_HIDPAD_MUTE     0x00020000u
#define WINIOS_HIDPAD_L2       0x00040000u   /* trigger past its digital threshold */
#define WINIOS_HIDPAD_R2       0x00080000u
#define WINIOS_HIDPAD_BATTERY_UNKNOWN 0xff

/* Player 1 as a virtual HID gamepad (served by the wineserver). 48 bytes, no padding; the
 * reader compares whole structs bytewise, so unused fields must be zero. */
struct winios_hidpad {
    uint32_t packet;              /* owned by the store, as above */
    uint32_t buttons;
    int16_t lx, ly, rx, ry;
    uint8_t left_trigger, right_trigger;
    uint8_t connected;
    uint8_t battery;              /* 0-100, or WINIOS_HIDPAD_BATTERY_UNKNOWN */
    uint8_t charging;             /* 0 discharging, 1 charging, 2 full */
    uint8_t motion;               /* 1: gyro/accel are live */
    uint8_t touch[2];             /* touchpad points 0/1 down */
    uint8_t reserved[2];
    uint16_t touch_x[2];          /* 0-1919 */
    uint16_t touch_y[2];          /* 0-1079 */
    int16_t gyro[3];
    int16_t accel[3];
    uint16_t reserved2;
};

/* What a game last sent to the HID gamepad (rumble, light bar, LEDs, trigger effects).
 * 44 bytes, no padding. `serial` is owned by the store and increases on every change. */
struct winios_hidpad_output {
    uint32_t serial;
    uint8_t rumble_valid, rumble_left, rumble_right;
    uint8_t lightbar_valid, red, green, blue;
    uint8_t player_leds_valid, player_leds;
    uint8_t mute_led_valid, mute_led;
    uint8_t trigger_valid[2];
    uint8_t trigger[2][11];
    uint8_t power_valid;
    uint8_t power_reduction;
    uint8_t reserved[3];
};

/* XInput rumble per slot. `serial` is 0 until first set, then increases on every change
 * (skipping 0 on wrap). */
struct winios_gamepad_vibration {
    uint32_t serial;
    uint16_t left;    /* XINPUT_VIBRATION.wLeftMotorSpeed */
    uint16_t right;   /* wRightMotorSpeed */
};

/* ================= Section D: environment the unix side reads =================
 * The app bridge sets these before __wine_main (names are fixed by the unix side):
 *   WINEPREFIX, HOME            the prefix directory
 *   WINELOADERNOEXEC=1          never re-exec a loader
 *   WINEDLLPATH                 the app bundle directory (holds the DLL farms)
 *   WINESERVERSOCKET            decimal fd of the client end of the server socket pair
 *   WINEDEBUG                   Wine's debug channel string
 *   MADEIRA_WIN32U=1            use the iOS win32u paths
 *   MADEIRA_QUIET=1             turn off the unix side's heavy diagnostics
 *   MADEIRA_INITIAL_CWD         the program's working folder as a Windows path ending in '\'
 *   MADEIRA_DOCS_DIR            the app's Documents directory
 *   MADEIRA_CA_BUNDLE           path of a PEM file of trusted root certificates
 *   MADEIRA_JIT_WRITE_OFFSET    "0x..." RX->RW offset of the JIT pool (JIT builds only)
 *   FEX_MADEIRA_HOSTPROBE       CPU feature list for the x86 translator (format in spec 02)
 *   SteamAppId, SteamGameId, SteamAppPath   a Steam game's identity (spec 02 says when)
 * Set by the app's Swift/C code before boot and only passed through (spec 02 must not clear
 * them unless it says so): WINE_IOS_NOJIT, WINE_IOS_JIT_RX/RW/SIZE, WINE_IOS_EXE_WINDOW,
 * MYIOSDECK_PE_DIR, MYIOSDECK_PE_DIR_A64, MADEIRA_SCREEN_W/H, MADEIRA_HIDPAD,
 * MADEIRA_HIDPAD_NAME, MADEIRA_DINPUT_PAD, MADEIRA_DINPUT_TRACE, MADEIRA_DOCK*,
 * MADEIRA_STEAM_HOST_*, MADEIRA_GAME_WINDOWS, MYIOSDECK_SESSION_REUSE ("1": keep the session for
 * another program, spec 22; read by the bridge at a program's end, the server and the audio
 * driver). */

#endif
