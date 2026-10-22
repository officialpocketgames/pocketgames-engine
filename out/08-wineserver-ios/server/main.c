/*
 * Server main function
 *
 * Copyright (C) 1998 Alexandre Julliard
 *
 * Changed for MYIOSDECK (in-app wineserver on iOS, spec 08), 2026; see CHANGES.md.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef HAVE_SYS_RESOURCE_H
# include <sys/resource.h>
#endif
#ifdef HAVE_SYS_SYSCTL_H
# include <sys/sysctl.h>
#endif

#include "object.h"
#include "file.h"
#include "thread.h"
#include "request.h"
#include "unicode.h"

/* command-line options */
int debug_level = 0;
int foreground = 0;
timeout_t master_socket_timeout = 3 * -TICKS_PER_SEC;  /* master socket timeout, default is 3 seconds */
const char *server_argv0;

/* parse-line args */

static void usage( FILE *fh )
{
    fprintf(fh, "Usage: %s [options]\n\n", server_argv0);
    fprintf(fh, "Options:\n");
    fprintf(fh, "   -d[n], --debug[=n]       set debug level to n or +1 if n not specified\n");
    fprintf(fh, "   -f,    --foreground      remain in the foreground for debugging\n");
    fprintf(fh, "   -h,    --help            display this help message\n");
    fprintf(fh, "   -k[n], --kill[=n]        kill the current wineserver, optionally with signal n\n");
    fprintf(fh, "   -p[n], --persistent[=n]  make server persistent, optionally for n seconds\n");
    fprintf(fh, "   -v,    --version         display version information and exit\n");
    fprintf(fh, "   -w,    --wait            wait until the current wineserver terminates\n");
    fprintf(fh, "\n");
}

#ifdef WINE_IOS
/* spec 08 Part B1: the server is a thread of the app, so no option may end the process.
 * Options that only make sense for a separate server process are logged and ignored. */
static void option_callback( int optc, char *optarg )
{
    switch (optc)
    {
    case 'd':
        if (optarg && isdigit(*optarg))
            debug_level = atoi( optarg );
        else
            debug_level++;
        break;
    case 'f':
        foreground = 1;
        break;
    case 'h':
        usage( stderr );
        break;
    case 'v':
        fprintf( stderr, "%s\n", PACKAGE_STRING );
        break;
    case 'p':  /* the server is always persistent here */
        break;
    case 'k':
    case 'w':
        fprintf( stderr, "wineserver: option -%c ignored inside the app\n", optc );
        break;
    }
}
#else
static void option_callback( int optc, char *optarg )
{
    int ret;

    switch (optc)
    {
    case 'd':
        if (optarg && isdigit(*optarg))
            debug_level = atoi( optarg );
        else
            debug_level++;
        break;
    case 'f':
        foreground = 1;
        break;
    case 'h':
        usage(stdout);
        exit(0);
        break;
    case 'k':
        if (optarg && isdigit(*optarg))
            ret = kill_lock_owner( atoi( optarg ) );
        else
            ret = kill_lock_owner(-1);
        exit( !ret );
    case 'p':
        if (optarg && isdigit(*optarg))
            master_socket_timeout = (timeout_t)atoi( optarg ) * -TICKS_PER_SEC;
        else
            master_socket_timeout = TIMEOUT_INFINITE;
        break;
    case 'v':
        fprintf( stderr, "%s\n", PACKAGE_STRING );
        exit(0);
    case 'w':
        wait_for_lock();
        exit(0);
    }
}
#endif

/* command-line option parsing */
/* partly based on the GLibc getopt() implementation */

static struct long_option
{
    const char *name;
    int has_arg;
    int val;
} long_options[] =
{
    {"debug",       2, 'd'},
    {"foreground",  0, 'f'},
    {"help",        0, 'h'},
    {"kill",        2, 'k'},
    {"persistent",  2, 'p'},
    {"version",     0, 'v'},
    {"wait",        0, 'w'},
    { NULL }
};

static void parse_options( int argc, char **argv, const char *short_opts,
                           const struct long_option *long_opts, void (*callback)( int, char* ) )
{
    const char *flag;
    char *start, *end;
    int i;

    for (i = 1; i < argc; i++)
    {
        if (argv[i][0] != '-' || !argv[i][1])  /* not an option */
            continue;
        if (!strcmp( argv[i], "--" ))
            break;
        start = argv[i] + 1 + (argv[i][1] == '-');

        if (argv[i][1] == '-')
        {
            /* handle long option */
            const struct long_option *opt, *found = NULL;
            int count = 0;

            if (!(end = strchr( start, '=' ))) end = start + strlen(start);
            for (opt = long_opts; opt && opt->name; opt++)
            {
                if (strncmp( opt->name, start, end - start )) continue;
                if (!opt->name[end - start])  /* exact match */
                {
                    found = opt;
                    count = 1;
                    break;
                }
                if (!found)
                {
                    found = opt;
                    count++;
                }
                else if (found->has_arg != opt->has_arg || found->val != opt->val)
                {
                    count++;
                }
            }

            if (count > 1) goto error;

            if (found)
            {
                if (*end)
                {
                    if (!found->has_arg) goto error;
                    end++;  /* skip '=' */
                }
                else if (found->has_arg == 1)
                {
                    if (i == argc - 1) goto error;
                    end = argv[++i];
                }
                else end = NULL;

                callback( found->val, end );
                continue;
            }
            goto error;
        }

        /* handle short option */
        for ( ; *start; start++)
        {
            if (!(flag = strchr( short_opts, *start ))) goto error;
            if (flag[1] == ':')
            {
                end = start + 1;
                if (!*end) end = NULL;
                if (flag[2] != ':' && !end)
                {
                    if (i == argc - 1) goto error;
                    end = argv[++i];
                }
                callback( *start, end );
                break;
            }
            callback( *start, NULL );
        }
    }
    return;

error:
    usage( stderr );
#ifndef WINE_IOS
    exit(1);
#endif
}

#ifndef WINE_IOS
static void sigterm_handler( int signum )
{
    exit(1);  /* make sure atexit functions get called */
}
#endif

static void init_limits(void)
{
#ifdef RLIMIT_NOFILE
    struct rlimit rlimit;

    if (!getrlimit( RLIMIT_NOFILE, &rlimit ))
    {
        rlimit.rlim_cur = rlimit.rlim_max;
        if (!setrlimit( RLIMIT_NOFILE, &rlimit )) return;
#ifdef __APPLE__
        {
            /* macOS before Big Sur fails if rlim_max is larger than maxfilesperproc */
            unsigned int nlimit = 0;
            size_t size = sizeof(nlimit);
            sysctlbyname("kern.maxfilesperproc", &nlimit, &size, NULL, 0);
            rlimit.rlim_cur = max( nlimit, OPEN_MAX );
            setrlimit( RLIMIT_NOFILE, &rlimit );
        }
#endif
    }
#endif
}

#ifdef WINE_IOS
/* spec 15 Part B (spec 08 clarification 6): the HID pad's server part, if linked */
extern void ios_hidpad_server_init(void) __attribute__((weak));

/* The app's server thread calls this (spec 02 Part B). It returns when the app sets
 * g_wineserver_should_stop. The server's state lives on in the app after it returns: a
 * later call in the same app run picks it up again instead of starting a second server,
 * which can't exist (spec 08 Part B4; QUESTIONS.md, spec 08 question 1). */
int wineserver_main( int argc, char *argv[] )
{
    static int started;

    ios_block_server_signals();  /* Part B2 (again at the start of the main loop) */
    ios_begin_run();
    server_argv0 = argv[0];
    parse_options( argc, argv, "d::fhk::p::vw", long_options, option_callback );
    foreground = 1;
    master_socket_timeout = TIMEOUT_INFINITE;  /* Part B1: never stop for lack of clients */

    /* Part B2: no handlers for SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGABRT (upstream's end the
     * process); the server thread has every signal blocked instead */
    signal( SIGPIPE, SIG_IGN );

    if (!started)
    {
        init_limits();
        sock_init();
        open_master_socket();

        ws_log( "starting (pid=%ld)", (long)getpid() );
        set_current_time();
        init_signals();
        init_memory();
        init_directories( load_intl_file() );
        init_threading();
        init_registry();  /* changes the app's working directory; nothing runs on it yet */
        /* clarification 6: spec 15's HID pad, once, after the namespace and the registry;
         * weak, so skipped when spec 15's server part isn't linked */
        if (ios_hidpad_server_init) ios_hidpad_server_init();
        started = 1;
    }
    else ws_log( "continuing with the state of the previous run" );

    ios_set_ready();  /* Part B6: after init_registry and the rest of start-up */
    main_loop();

    __atomic_store_n( &wineserver_ready, 0, __ATOMIC_RELEASE );
    flush_registry();
    ws_log( "stopped" );
    return 0;
}
#else
int main( int argc, char *argv[] )
{
    setvbuf( stderr, NULL, _IOLBF, 0 );
    server_argv0 = argv[0];
    parse_options( argc, argv, "d::fhk::p::vw", long_options, option_callback );

    /* setup temporary handlers before the real signal initialization is done */
    signal( SIGPIPE, SIG_IGN );
    signal( SIGHUP, sigterm_handler );
    signal( SIGINT, sigterm_handler );
    signal( SIGQUIT, sigterm_handler );
    signal( SIGTERM, sigterm_handler );
    signal( SIGABRT, sigterm_handler );
    init_limits();

    sock_init();
    open_master_socket();

    if (debug_level) fprintf( stderr, "wineserver: starting (pid=%ld)\n", (long) getpid() );
    set_current_time();
    init_signals();
    init_memory();
    init_directories( load_intl_file() );
    init_threading();
    init_registry();
    main_loop();
    return 0;
}
#endif
