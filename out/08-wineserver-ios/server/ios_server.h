// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 08: declarations shared by the iOS parts of the in-app wineserver. Included at the end
// of object.h, so every server file sees it. The names in the first two blocks are fixed by
// interfaces/wine_unix_contract.h (Sections A and B) and by spec 08.

#ifndef MYIOSDECK_SERVER_IOS_SERVER_H
#define MYIOSDECK_SERVER_IOS_SERVER_H

#ifdef WINE_IOS

#include <stdio.h>
#include <sys/types.h>

/* ---- provided to the app (contract Section A, spec 08 Parts D and G) ---- */
extern int wineserver_ready;                         /* release store 1 before the main loop */
extern void wineserver_inject_client_fd( int fd );
extern void wineserver_log_set_file( const char *path );
extern void wineserver_set_nls_dir( const char *path );
extern void ws_log( const char *fmt, ... ) __attribute__((format(printf,1,2)));
extern void wine_ui_log( const char *message );
extern void wineserver_request_wake(void);
extern void wineserver_kill_programs( int exit_code );
extern int g_wineserver_force_quits;

/* ---- provided by the app (contract Section B) ---- */
extern volatile int g_wineserver_should_stop;

/* ---- ios_log.c ---- */
/* The stream that stands for stderr in server code (object.h maps stderr to it): every line
 * written to it goes to the log file with a timestamp. Before a log file is named it is the
 * process's own stderr. */
extern FILE *ws_log_stream(void);
extern void ws_perror( const char *str );

/* ---- ios_server.c ---- */
/* Part B3: the one wrapper for every kill() in the server. A signal to the app's own pid
 * (every pseudo-process has it) or to a process group does nothing and reports success. */
extern int ios_kill( pid_t pid, int sig );
/* Part B5: the directory the NLS tables are loaded from (NULL: none was set). */
extern const char *ios_nls_dir(void);
/* Part B1: set when the server must leave its main loop (stop request or a former exit path). */
extern void ios_begin_run(void);
extern int ios_should_leave_loop(void);
extern void ios_request_loop_exit( const char *why );
/* Part B6: call once start-up is complete, right before the main loop. */
extern void ios_set_ready(void);
/* Part B2: block all signals on the calling (server) thread. */
extern void ios_block_server_signals(void);
/* Part C1: the next queued client connection, -1 if none (any thread may queue, the server
 * thread takes) */
extern int ios_take_injected_fd(void);

/* Part D3: sleep up to nsec, or less if wineserver_request_wake() is called; and consume the
 * wake-ups counted so far (after waking, so a backlog can't keep the loop from sleeping). */
extern void ios_wait_for_wake( long long nsec );
extern void ios_drain_wakes(void);

/* Part F5: the screen size (the virtual monitor): from wineserver_set_screen_size(), else from
 * PG_SCREEN_W/H. Returns 0 if unknown. ios_take_screen_change() reports (once) that the
 * app changed it. */
extern void wineserver_set_screen_size( int width, int height );
extern int ios_get_screen_size( int *width, int *height );
extern int ios_take_screen_change(void);

/* winstation.c, Part F5: re-clip every desktop's cursor to the new screen (server thread) */
extern void ios_apply_screen_change(void);

/* Part G1: a pending force-quit request (taken once), and the per-thread alert. The alert
 * goes through a hook that spec 09's client side sets (0 when there is none). */
extern int ios_take_force_quit( int *exit_code );
extern void (*wineserver_thread_alert_hook)( unsigned int unix_tid );
extern int ios_alert_thread( int unix_tid );

/* process.c, Part G1: end every non-system process that still has running threads */
extern void ios_terminate_programs( int exit_code );

/* ---- fd.c ---- */
struct fd;
/* Part D6: always report POLLIN for this fd (its non-blocking read finds out) */
extern void set_fd_ios_always_read( struct fd *fd );

/* ---- request.c ---- */
/* Part C1: handle a new client connection like an accepted one on the master socket */
extern void ios_new_client_connection( int client );
/* Part C1: handle every queued injected connection (server thread, main loop step 2) */
extern void ios_accept_injected_clients(void);

/* Read an integer environment setting; def if unset or not a number. */
extern int ios_env_int( const char *name, int def );

#endif /* WINE_IOS */

#endif /* MYIOSDECK_SERVER_IOS_SERVER_H */
