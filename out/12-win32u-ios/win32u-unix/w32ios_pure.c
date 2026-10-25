// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the MYIOSDECK author
//
// Pure logic of the iOS win32u (spec 12). See w32ios_pure.h. No Wine headers here: the contract
// header is included with the local type declarations it asks for (its own comment).

#include <string.h>
#include "w32ios_pure.h"

typedef void *HWND;
typedef void *HCURSOR;
typedef unsigned int UINT;
typedef int INT;
typedef uint32_t DWORD;
typedef int BOOL;
typedef struct { int32_t left, top, right, bottom; } RECT;
struct window_rects;
struct window_surface;
#include "wine_unix_contract.h"

_Static_assert( sizeof(struct pgios_gamepad) == 20, "struct pgios_gamepad is 20 bytes" );

/* ===================== Part B ===================== */

static uint32_t load_acquire( const uint32_t *p ) { return __atomic_load_n( p, __ATOMIC_ACQUIRE ); }

int w32ios_pp_find( const struct w32ios_pp_table *table, const void *peb, uint32_t pid )
{
    int i;

    for (i = 0; i < W32IOS_PP_SLOTS; i++)
    {
        const struct w32ios_pp_slot *slot = &table->slot[i];
        uint32_t seq = load_acquire( &slot->seq ), state;
        void *slot_peb;
        uint32_t slot_pid;

        if (seq & 1) continue;
        state = __atomic_load_n( &slot->state, __ATOMIC_RELAXED );
        slot_peb = __atomic_load_n( (void * const *)&slot->peb, __ATOMIC_RELAXED );
        slot_pid = __atomic_load_n( &slot->pid, __ATOMIC_RELAXED );
        __atomic_thread_fence( __ATOMIC_ACQUIRE );
        if (__atomic_load_n( &slot->seq, __ATOMIC_RELAXED ) != seq) continue;
        if (state == W32IOS_SLOT_LIVE && slot_peb == peb && slot_pid == pid) return i;
    }
    return -1;
}

int w32ios_pp_pick( const struct w32ios_pp_table *table, const void *peb, uint32_t pid,
                    int (*is_dead)( void *peb, uint32_t pid, void *ctx ), void *ctx, int *reuse )
{
    int i, free_slot = -1, same_peb = -1, dead = -1;
    uint32_t oldest = 0;

    *reuse = 0;
    for (i = 0; i < W32IOS_PP_SLOTS; i++)
    {
        const struct w32ios_pp_slot *slot = &table->slot[i];

        if (slot->state != W32IOS_SLOT_LIVE)
        {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        if (slot->peb == peb)
        {
            if (slot->pid == pid)
            {
                *reuse = 1;
                return i;
            }
            if (same_peb < 0) same_peb = i;
        }
    }
    if (free_slot >= 0) return free_slot;
    if (same_peb >= 0) return same_peb;
    if (!is_dead) return -1;

    /* every slot is live: take the oldest one whose process ended */
    for (i = 0; i < W32IOS_PP_SLOTS; i++)
    {
        const struct w32ios_pp_slot *slot = &table->slot[i];
        uint32_t age = table->clock - slot->stamp;

        if (dead >= 0 && age <= oldest) continue;
        if (!is_dead( slot->peb, slot->pid, ctx )) continue;
        dead = i;
        oldest = age;
    }
    return dead;
}

void w32ios_pp_begin( struct w32ios_pp_table *table, int index )
{
    struct w32ios_pp_slot *slot = &table->slot[index];

    __atomic_store_n( &slot->seq, slot->seq + 1, __ATOMIC_RELAXED );   /* odd: readers skip it */
    __atomic_thread_fence( __ATOMIC_RELEASE );
    __atomic_store_n( &slot->state, W32IOS_SLOT_FREE, __ATOMIC_RELAXED );
}

void w32ios_pp_publish( struct w32ios_pp_table *table, int index, void *peb, uint32_t pid )
{
    struct w32ios_pp_slot *slot = &table->slot[index];

    if (!(slot->seq & 1)) w32ios_pp_begin( table, index );
    __atomic_store_n( (void **)&slot->peb, peb, __ATOMIC_RELAXED );
    __atomic_store_n( &slot->pid, pid, __ATOMIC_RELAXED );
    slot->stamp = ++table->clock;
    __atomic_store_n( &slot->state, W32IOS_SLOT_LIVE, __ATOMIC_RELAXED );
    __atomic_store_n( &slot->seq, slot->seq + 1, __ATOMIC_RELEASE );   /* even: published */
}

int w32ios_is_session_root( int queried, uint64_t parent_pid )
{
    return queried && !parent_pid;
}

uint64_t w32ios_ceiling( int wow64, uint64_t highest_user_address )
{
    if (!wow64) return 0;
    return highest_user_address | 0x7fffffff;
}

w32ios_proc_pair *w32ios_procs_table( struct w32ios_client_procs *own, w32ios_proc_pair *global )
{
    if (own && __atomic_load_n( &own->set, __ATOMIC_ACQUIRE )) return own->procs;
    return global;
}

/* a table of pairs seen as 2 * count pointers */
int w32ios_procs_match( const void *table, int count, const void *func )
{
    void *const *flat = (void *const *)table;
    int i;

    if (!table || !func) return -1;
    for (i = 0; i < count; i++)   /* match either flavour: some programs confuse A and W */
        if (flat[2 * i] == func || flat[2 * i + 1] == func) return i;
    return -1;
}

int w32ios_procs_index( const void *table, int count, const void *entry )
{
    const char *base = table, *p = entry;

    if (!table || p < base || p >= base + (size_t)count * sizeof(w32ios_proc_pair)) return -1;
    if ((size_t)(p - base) % sizeof(w32ios_proc_pair)) return -1;
    return (int)((size_t)(p - base) / sizeof(w32ios_proc_pair));
}

enum w32ios_builtin_action w32ios_builtin_action( int done, uint32_t owner, uint32_t self )
{
    if (done) return W32IOS_BUILTIN_SKIP;
    if (owner == self) return W32IOS_BUILTIN_SKIP;   /* re-entered while registering */
    if (owner) return W32IOS_BUILTIN_WAIT;
    return W32IOS_BUILTIN_REGISTER;
}

uint64_t w32ios_winproc_handle( uint64_t value, uint64_t wow_base, int enabled )
{
    uint64_t offset;

    if (!enabled || !wow_base) return value;
    if (value <= 0xffffffffull || value < wow_base) return value;
    offset = value - wow_base;
    if (offset > 0xffffffffull) return value;
    if ((offset >> 16) != 0xffff) return value;
    return offset;
}

/* ===================== Part C ===================== */

const struct w32ios_size w32ios_std_modes[W32IOS_STD_MODE_COUNT] =
{
    {  640,  360 }, {  640,  400 }, {  640,  480 }, {  720,  480 }, {  720,  576 },
    {  800,  480 }, {  800,  600 }, {  848,  480 }, {  854,  480 }, {  960,  540 },
    {  960,  600 }, {  960,  720 }, { 1024,  576 }, { 1024,  600 }, { 1024,  640 },
    { 1024,  768 }, { 1120,  832 }, { 1152,  648 }, { 1152,  864 }, { 1176,  664 },
    { 1280,  720 }, { 1280,  768 }, { 1280,  800 }, { 1280,  960 }, { 1280, 1024 },
    { 1360,  768 }, { 1366,  768 }, { 1400, 1050 }, { 1440,  900 }, { 1600,  900 },
    { 1600, 1024 }, { 1600, 1200 }, { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 },
    { 2048, 1536 }, { 2560, 1440 },
};

static long parse_positive( const char *s )
{
    long v = 0;

    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s < '0' || *s > '9') return 0;
    while (*s >= '0' && *s <= '9')
    {
        v = v * 10 + (*s++ - '0');
        if (v > 100000) return 0;
    }
    while (*s == ' ' || *s == '\t') s++;
    return *s ? 0 : v;
}

struct w32ios_size w32ios_session_default( const char *w, const char *h )
{
    struct w32ios_size size = { 1024, 768 };
    long vw = parse_positive( w ), vh = parse_positive( h );

    if (vw > 0 && vh > 0)
    {
        size.w = (int)vw;
        size.h = (int)vh;
    }
    return size;
}

static uint64_t pixels( struct w32ios_size s ) { return (uint64_t)s.w * (uint64_t)s.h; }

static int same_size( struct w32ios_size a, struct w32ios_size b ) { return a.w == b.w && a.h == b.h; }

enum w32ios_launch_size w32ios_launch_size( int first, struct w32ios_size cur, struct w32ios_size want,
                                            int others )
{
    if (first) return W32IOS_LAUNCH_FIRST;
    if (same_size( cur, want )) return W32IOS_LAUNCH_SAME;
    return others ? W32IOS_LAUNCH_STAYS : W32IOS_LAUNCH_CHANGE;
}

static uint16_t lower16( uint16_t c )
{
    return (c >= 'A' && c <= 'Z') ? (uint16_t)(c - 'A' + 'a') : c;
}

/* case-insensitive (ASCII) search of an ASCII word in a counted UTF-16 string */
static int contains16( const uint16_t *s, size_t len, const char *word )
{
    size_t n = strlen( word ), i, j;

    for (i = 0; i + n <= len; i++)
    {
        for (j = 0; j < n; j++) if (lower16( s[i + j] ) != (uint16_t)word[j]) break;
        if (j == n) return 1;
    }
    return 0;
}

int w32ios_is_user_program( const uint16_t *path, size_t len, const uint16_t *sysroot, size_t root_len,
                            const uint16_t *name, size_t name_len )
{
    static const char *const helpers[] = { "crash", "handler", "report", "helper" };
    size_t i;

    for (i = 0; i < sizeof(helpers) / sizeof(helpers[0]); i++)
        if (name && contains16( name, name_len, helpers[i] )) return 0;

    if (!path || !len || !sysroot || !root_len) return 1;
    while (root_len && (sysroot[root_len - 1] == '\\' || sysroot[root_len - 1] == '/')) root_len--;
    if (!root_len || len <= root_len + 1) return 1;
    for (i = 0; i < root_len; i++)
    {
        uint16_t a = lower16( path[i] ), b = lower16( sysroot[i] );
        if (a == '/') a = '\\';
        if (b == '/') b = '\\';
        if (a != b) return 1;
    }
    return !(path[root_len] == '\\' || path[root_len] == '/');
}

int w32ios_build_modes( struct w32ios_size cur, struct w32ios_size def, int extended,
                        struct w32ios_size *out, int max )
{
    uint64_t cap = pixels( def ) * (extended ? 4 : 1);
    int i, count = 0;

    if (max <= 0) return 0;
    out[count++] = cur;
    for (i = 0; i < W32IOS_STD_MODE_COUNT && count < max; i++)
    {
        if (pixels( w32ios_std_modes[i] ) > cap) continue;
        if (same_size( w32ios_std_modes[i], cur )) continue;
        out[count++] = w32ios_std_modes[i];
    }
    return count;
}

enum w32ios_cds w32ios_mode_decision( int has_devmode, int has_w, int has_h, int w, int h,
                                      struct w32ios_size cur, struct w32ios_size def, int extended,
                                      struct w32ios_size *out )
{
    struct w32ios_size want, list[W32IOS_STD_MODE_COUNT + 1];
    uint64_t four = pixels( def ) * 4;
    int i, count;

    if (!has_devmode || (!has_w && !has_h))
    {
        *out = def;
        return W32IOS_CDS_RESTORE;
    }
    want.w = has_w ? w : cur.w;
    want.h = has_h ? h : cur.h;
    *out = want;
    if (want.w <= 0 || want.h <= 0) return W32IOS_CDS_BADMODE;

    if (same_size( want, def )) return W32IOS_CDS_ACCEPT;
    count = w32ios_build_modes( cur, def, extended, list, W32IOS_STD_MODE_COUNT + 1 );
    for (i = 0; i < count; i++) if (same_size( want, list[i] )) return W32IOS_CDS_ACCEPT;
    for (i = 0; i < W32IOS_STD_MODE_COUNT; i++)
        if (same_size( want, w32ios_std_modes[i] ) && pixels( want ) <= four) return W32IOS_CDS_ACCEPT;
    return W32IOS_CDS_BADMODE;
}

static uint16_t upper16( uint16_t c ) { return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 'a' + 'A') : c; }

static int equal_ascii_nocase( const uint16_t *name, size_t len, const char *ascii )
{
    size_t i, n = strlen( ascii );

    if (len != n) return 0;
    for (i = 0; i < n; i++) if (upper16( name[i] ) != (uint16_t)upper16( (uint8_t)ascii[i] )) return 0;
    return 1;
}

int w32ios_match_device( const uint16_t *name, size_t len )
{
    if (!name || !len) return W32IOS_DEV_ADAPTER;
    while (len && !name[len - 1]) len--;   /* tolerate a counted NUL */
    if (!len) return W32IOS_DEV_ADAPTER;
    if (equal_ascii_nocase( name, len, "\\\\.\\DISPLAY1" )) return W32IOS_DEV_ADAPTER;
    if (equal_ascii_nocase( name, len, "\\\\.\\DISPLAY1\\Monitor0" )) return W32IOS_DEV_MONITOR;
    return W32IOS_DEV_NONE;
}

const char *w32ios_driver_version( uint16_t vendor )
{
    switch (vendor)
    {
    case 0x10de: return "32.0.15.8157";      /* NVIDIA: reads as 581.57 */
    case 0x1002: return "35.0.21025.1024";   /* AMD */
    case 0x8086: return "35.0.101.6314";     /* Intel */
    default:     return "35.0.10.1000";
    }
}

uint64_t w32ios_pack_version( const char *version )
{
    uint64_t parts[4] = { 0, 0, 0, 0 };
    int i = 0;

    if (!version) return 0;
    while (*version && i < 4)
    {
        if (*version >= '0' && *version <= '9')
            parts[i] = parts[i] * 10 + (uint64_t)(*version - '0');
        else if (*version == '.')
            i++;
        else
            break;
        version++;
    }
    return ((parts[0] & 0xffff) << 48) | ((parts[1] & 0xffff) << 32) |
           ((parts[2] & 0xffff) << 16) | (parts[3] & 0xffff);
}

/* ===================== Part D ===================== */

uint64_t w32ios_luid_from_registry_id( uint64_t id )
{
    return __builtin_bswap64( id );
}

uint64_t w32ios_dedicated_memory( const char *vram_mb, uint64_t fallback )
{
    uint64_t mb = 0;
    const char *p = vram_mb;

    if (!p) return fallback;
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return fallback;
    while (*p >= '0' && *p <= '9')
    {
        mb = mb * 10 + (uint64_t)(*p++ - '0');
        if (mb > (1ull << 30)) return fallback;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p || mb < 256) return fallback;
    return mb << 20;
}

/* ===================== Part E ===================== */

static int mask_bit( const uint8_t *mask, int stride, int x, int y )
{
    return (mask[y * stride + x / 8] >> (7 - (x % 8))) & 1;
}

static int mask_stride( int w ) { return ((w + 31) / 32) * 4; }

int w32ios_cursor_has_alpha( const uint8_t *color, int w, int h )
{
    int i;

    if (!color) return 0;
    for (i = 0; i < w * h; i++) if (color[i * 4 + 3]) return 1;
    return 0;
}

void w32ios_cursor_color( const uint8_t *color, const uint8_t *and_mask, int w, int h, uint8_t *out )
{
    int x, y, alpha = w32ios_cursor_has_alpha( color, w, h ), stride = mask_stride( w );

    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            const uint8_t *src = color + (y * w + x) * 4;
            uint8_t *dst = out + (y * w + x) * 4;

            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            if (alpha) dst[3] = src[3];
            else if (and_mask) dst[3] = mask_bit( and_mask, stride, x, y ) ? 0 : 255;
            else dst[3] = 255;
            if (!dst[3]) dst[0] = dst[1] = dst[2] = 0;
        }
    }
}

void w32ios_cursor_mono( const uint8_t *mask, int w, int h, uint8_t *out )
{
    int x, y, stride = mask_stride( w );

    for (y = 0; y < h; y++)
    {
        for (x = 0; x < w; x++)
        {
            int and_bit = mask_bit( mask, stride, x, y );
            int xor_bit = mask_bit( mask, stride, x, y + h );
            uint8_t *dst = out + (y * w + x) * 4;

            if (xor_bit) dst[0] = dst[1] = dst[2] = dst[3] = 255;          /* white */
            else if (and_bit) dst[0] = dst[1] = dst[2] = dst[3] = 0;       /* transparent */
            else { dst[0] = dst[1] = dst[2] = 0; dst[3] = 255; }           /* black */
        }
    }
}

int w32ios_key_extended( unsigned int vk, unsigned int scan_ex, int nav_e0 )
{
    unsigned int prefix = (scan_ex >> 8) & 0xff;

    if (prefix == 0xe0 || prefix == 0xe1) return 1;
    if (!nav_e0) return 0;
    switch (vk)
    {
    case 0x21: /* VK_PRIOR */
    case 0x22: /* VK_NEXT */
    case 0x23: /* VK_END */
    case 0x24: /* VK_HOME */
    case 0x25: /* VK_LEFT */
    case 0x26: /* VK_UP */
    case 0x27: /* VK_RIGHT */
    case 0x28: /* VK_DOWN */
    case 0x2d: /* VK_INSERT */
    case 0x2e: /* VK_DELETE */
        return 1;
    }
    return 0;
}

int w32ios_log_event( unsigned int count )
{
    return count < 8 || !(count % 256);
}

unsigned int w32ios_diag_event( struct w32ios_diag *d, uint64_t now_ms )
{
    if (!d->total) d->window_start = now_ms;
    d->total++;
    d->window++;
    return d->total;
}

unsigned int w32ios_diag_due( struct w32ios_diag *d, uint64_t now_ms )
{
    unsigned int n;

    if (!d->window || now_ms < d->window_start || now_ms - d->window_start < W32IOS_DIAG_WINDOW_MS) return 0;
    n = d->window;
    d->window = 0;
    d->window_start = now_ms;
    return n;
}

int w32ios_rawinput_slot( unsigned int handle, unsigned int vendor, unsigned int product )
{
    if (vendor != 0x045e || product != 0x028e) return -1;
    if (handle < 0x101 || handle > 0x104) return -1;
    return (int)(handle - 0x101);
}

int64_t w32ios_slice_deadline( int64_t now, int has_deadline, int64_t deadline, int64_t slice,
                               int *final )
{
    int64_t end = now + slice;

    if (has_deadline && deadline <= end)
    {
        *final = 1;
        return deadline;
    }
    *final = 0;
    return end;
}

int w32ios_poll_skip_request( int pure, uint32_t now_ms, uint32_t last_ms )
{
    if (!pure) return 0;
    return (uint32_t)(now_ms - last_ms) < 1000;
}

int w32ios_covers_screen( int l, int t, int r, int b, int screen_w, int screen_h )
{
    return l <= 0 && t <= 0 && r >= screen_w && b >= screen_h;
}

int w32ios_dialog_candidate( int l, int t, int r, int b, int screen_w, int screen_h )
{
    if (r - l < 32 || b - t < 32) return 0;
    return !w32ios_covers_screen( l, t, r, b, screen_w, screen_h );
}

/* ===================== Part F ===================== */

uint32_t w32ios_gamepad_answer( const struct w32ios_pad_source *src, uint32_t arg1, void *buffer )
{
    int index = arg1 & 0xff, op = (arg1 >> 8) & 0xff;
    struct pgios_gamepad pad;

    if (!buffer || index > 3) return 0;

    switch (op)
    {
    case W32IOS_PAD_STATE:
    {
        struct w32ios_xinput_state state;

        memset( &pad, 0, sizeof(pad) );
        if (!src->get_state( index, &pad ) || !pad.connected) return 0;
        state.dwPacketNumber = pad.packet;
        state.Gamepad.wButtons = pad.buttons;
        state.Gamepad.bLeftTrigger = pad.left_trigger;
        state.Gamepad.bRightTrigger = pad.right_trigger;
        state.Gamepad.sThumbLX = pad.lx;
        state.Gamepad.sThumbLY = pad.ly;
        state.Gamepad.sThumbRX = pad.rx;
        state.Gamepad.sThumbRY = pad.ry;
        memcpy( buffer, &state, sizeof(state) );
        return 1;
    }
    case W32IOS_PAD_CAPS:
    {
        struct w32ios_xinput_capabilities caps;

        memset( &pad, 0, sizeof(pad) );
        if (!src->get_state( index, &pad ) || !pad.connected) return 0;
        memset( &caps, 0, sizeof(caps) );
        caps.Type = 0x01;      /* XINPUT_DEVTYPE_GAMEPAD */
        caps.SubType = 0x01;   /* XINPUT_DEVSUBTYPE_GAMEPAD */
        caps.Flags = 0;
        caps.Gamepad.wButtons = 0xf3ff;
        caps.Gamepad.bLeftTrigger = 0xff;
        caps.Gamepad.bRightTrigger = 0xff;
        caps.Gamepad.sThumbLX = (int16_t)0xffc0;
        caps.Gamepad.sThumbLY = (int16_t)0xffc0;
        caps.Gamepad.sThumbRX = (int16_t)0xffc0;
        caps.Gamepad.sThumbRY = (int16_t)0xffc0;
        if (src->rumble_caps())
        {
            caps.Vibration.wLeftMotorSpeed = 0xff;
            caps.Vibration.wRightMotorSpeed = 0xff;
        }
        memcpy( buffer, &caps, sizeof(caps) );
        return 1;
    }
    case W32IOS_PAD_VIBRATION:
    {
        struct w32ios_xinput_vibration vib;

        memcpy( &vib, buffer, sizeof(vib) );
        src->set_vibration( index, vib.wLeftMotorSpeed, vib.wRightMotorSpeed );
        return 1;
    }
    }
    return 0;
}

/* ===================== helpers ===================== */

static int lower( int c ) { return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c; }

static int equal_nocase( const char *a, const char *b )
{
    while (*a && *b && lower( (unsigned char)*a ) == lower( (unsigned char)*b )) { a++; b++; }
    return !*a && !*b;
}

int w32ios_parse_bool( const char *value, int def )
{
    if (!value || !*value) return def;
    if (equal_nocase( value, "1" ) || equal_nocase( value, "on" ) || equal_nocase( value, "true" ) ||
        equal_nocase( value, "yes" )) return 1;
    if (equal_nocase( value, "0" ) || equal_nocase( value, "off" ) || equal_nocase( value, "false" ) ||
        equal_nocase( value, "no" )) return 0;
    return def;
}

static int is_space( char c ) { return c == ' ' || c == '\t' || c == '\r'; }

int w32ios_cfg_find( const char *text, size_t len, const char *key, char *out, size_t cap )
{
    size_t pos = 0, keylen = strlen( key );
    int found = 0;

    if (len >= 3 && (uint8_t)text[0] == 0xef && (uint8_t)text[1] == 0xbb && (uint8_t)text[2] == 0xbf)
        pos = 3;   /* UTF-8 byte order mark */

    while (pos < len)
    {
        size_t start = pos, end, eq, ks, ke, vs, ve;

        while (pos < len && text[pos] != '\n') pos++;
        end = pos;
        if (pos < len) pos++;   /* the newline */

        ks = start;
        while (ks < end && is_space( text[ks] )) ks++;
        if (ks == end || text[ks] == '#') continue;
        for (eq = ks; eq < end && text[eq] != '='; eq++) ;
        if (eq == end) continue;   /* no '=' */
        ke = eq;
        while (ke > ks && is_space( text[ke - 1] )) ke--;
        if (ke - ks != keylen || memcmp( text + ks, key, keylen )) continue;

        vs = eq + 1;
        while (vs < end && is_space( text[vs] )) vs++;
        ve = end;
        while (ve > vs && is_space( text[ve - 1] )) ve--;
        if (cap)
        {
            size_t n = ve - vs < cap - 1 ? ve - vs : cap - 1;
            memcpy( out, text + vs, n );
            out[n] = 0;
        }
        found = 1;   /* keep going: the last occurrence wins */
    }
    return found;
}

/* ===================== spec 22 Part E ===================== */

int w32ios_client_object_ok( uint32_t owner_pid, uint32_t caller_pid, int is_window,
                             uint32_t object_handle, uint32_t full_handle )
{
    if (!owner_pid || owner_pid != caller_pid) return 0;
    if (is_window && object_handle != full_handle) return 0;
    return 1;
}
