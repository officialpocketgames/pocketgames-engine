// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 17 Parts B, C and D: the rules of ntdll's smaller unix-side changes, as plain functions
// of their inputs (no Wine headers, no globals of ntdll), so the host tests can check them.
// The glue that feeds them the calling thread's TEB, PEB and guest window is in ios_s17.c and
// in the modified upstream files (unix_private.h, file.c, debug.c, system.c, syscall.c).

#ifndef MYIOSDECK_IOS_S17_PURE_H
#define MYIOSDECK_IOS_S17_PURE_H

#include <stddef.h>
#include <stdint.h>

/* ---- switches (each read once per app run) ---- */

/* an environment switch that is on unless its value is exactly "0" (unset: on) */
int ios_s17_switch_value( const char *value );

/* ---- Part B: which pseudo-process is asking ---- */

/* B1: TRUE for a thread whose TEB has a 32-bit TEB, else the process-wide answer;
 * by_teb = 0 (PG_WOW64_BY_TEB=0) uses only the process-wide answer */
int ios_s17_is_wow64( int by_teb, int teb_has_wow_teb, int process_wide );

/* B2 (64-bit build): TRUE exactly when the caller's pseudo-process has a guest window;
 * owner = 0 (PG_IO_STATUS_OWNER=0) gives upstream's answer */
int ios_s17_in_wow64_call( int owner, unsigned long wow_base, int upstream );

/* B3: the bit of ptr's page in an EcCodeBitMap; a NULL map is "not ARM64EC code" */
int ios_s17_ec_bit( const uint64_t *map, uintptr_t ptr, unsigned long page_size );

/* B4: the ceiling ntdll's own section views use: the guest ceiling only for a pseudo-process
 * with a guest window, else 0; per_process = 0 (PG_SECTION_PROCESS_LIMIT=0) keeps the old
 * behaviour (always the guest ceiling) */
unsigned long ios_s17_section_limit( int per_process, unsigned long wow_base,
                                     unsigned long wow_limit );

/* ---- Part C1: a volume serial for 32-bit programs ---- */

#define IOS_S17_SERIAL_FILE ".pocketgames-volume-serial"

/* 32-bit FNV-1a over the stored value (4 bytes, least significant first) and the drive letter
 * byte; 0 or 0xffffffff becomes the stored value; no stored value (0): 0 */
uint32_t ios_s17_volume_serial( uint32_t stored, unsigned char letter );

/* the stored value from the file's first bytes (at most 15 are read), hex; 0 if none */
uint32_t ios_s17_parse_serial( const char *buf, size_t len );

/* the stored value of a prefix: read <config_dir>/.pocketgames-volume-serial, or draw one with
 * rnd() (never 0 or 0xffffffff) and create the file (O_CREAT | O_EXCL, 0600, "%08x\n"); a
 * creator that loses the race takes the winner's value. 0: no config dir, or no value. */
typedef uint32_t (*ios_s17_random_fn)(void);
uint32_t ios_s17_load_serial( const char *config_dir, ios_s17_random_fn rnd );

/* ---- Part C2: failed opens ---- */

#define IOS_S17_FAIL_BUDGET_ANY     64
#define IOS_S17_FAIL_BUDGET_WRITE   192
#define IOS_S17_FAIL_BUDGET_PROGRAM 256   /* C2 addendum */

/* the budgets, by kind */
#define IOS_S17_BUDGET_ANY     0   /* read-only failures under C:\windows\, or without a name */
#define IOS_S17_BUDGET_WRITE   1   /* opens that write or create */
#define IOS_S17_BUDGET_PROGRAM 2   /* read-only failures anywhere else (C2 addendum) */

struct ios_s17_budgets
{
    int any;     /* lines used for read-only failures under C:\windows\ (or without a name) */
    int write;   /* lines used for opens that write or create */
    int program; /* lines used for read-only failures elsewhere */
};

/* an open that writes or creates: access includes FILE_WRITE_DATA, FILE_APPEND_DATA or
 * GENERIC_WRITE, or a disposition other than FILE_OPEN */
int ios_s17_open_is_write( uint32_t access, uint32_t disposition );

/* C2 addendum: a name under C:\windows\ (or C:\windows itself), case-insensitive, with or without
 * the \??\ or \\?\ prefix */
int ios_s17_is_windows_path( const uint16_t *name, size_t len );

/* C2 addendum: the budget a failure takes its line from: writes and creates the write budget,
 * read-only failures under C:\windows\ or without a name the shared one, the others their own */
int ios_s17_budget_kind( int is_write, const uint16_t *name, size_t len );

/* takes one line from the budget of that kind (thread-safe); 0 when it is used up */
int ios_s17_budget_take( struct ios_s17_budgets *budgets, int kind );

/* printable ASCII copies (other characters become '?'), always NUL-terminated */
void ios_s17_ascii_w( char *dst, size_t cap, const uint16_t *src, size_t len );
void ios_s17_ascii_a( char *dst, size_t cap, const char *src );

/* ---- Part D3: the CPU count ---- */

/* the settings file (spec 02 Part G): the value of key in text, trimmed; 1 if found */
int ios_s17_cfg_lookup( const char *text, size_t len, const char *key, char *out, size_t cap );
/* the same on <docs_dir>/pocketgames.cfg; 0 when there is no file or no key */
int ios_s17_cfg_file_get( const char *docs_dir, const char *key, char *out, size_t cap );

/* a whole decimal integer (surrounding blanks allowed); 1 if value holds one */
int ios_s17_parse_int( const char *value, long *out );

/* the number Windows code sees: PG_CPU_COUNT if a positive integer, else the settings
 * file's cpu-count (absent or 0: none); applied only if 1 to 63. 0: no override. */
int ios_s17_cpu_override( const char *env_value, const char *cfg_value );

/* ---- Part D5: naming a system call ---- */

/* names[num] when names is registered and num is below limit, else NULL */
const char *ios_s17_syscall_name( const char *const *names, unsigned long limit, unsigned int num );

#endif /* MYIOSDECK_IOS_S17_PURE_H */
