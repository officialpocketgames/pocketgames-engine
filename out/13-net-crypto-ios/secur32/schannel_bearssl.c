/*
 * BearSSL-based implementation of the schannel (SSL/TLS) provider.
 *
 * Copyright 2005 Juan Lang
 * Copyright 2008 Henri Verbeet
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
 *
 * Changed for MYIOSDECK (2026, spec 13 clarifications 6, 10 and 13, Parts B and E): starts from
 * Wine 11.4's dlls/secur32/schannel_gnutls.c; GnuTLS replaced by BearSSL. The unix-call table and
 * its order, the SecBuffer walk, the alert-number mapping, the ALPN list parsing, the info layouts
 * and the 32-bit thunks (guest addresses converted with ios_g2h()) are upstream's; handshake
 * outcomes are logged (the first 32 of each kind). See out/13-net-crypto-ios/CHANGES.md.
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "bearssl/bearssl.h"

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "sspi.h"
#include "secur32_priv.h"

#include "wine/unixlib.h"
#include "wine/debug.h"
#include "ios_guest.h"
#include "ios_applecrypto.h"

WINE_DEFAULT_DEBUG_CHANNEL(secur32);

/* TLS record content types (RFC 5246 section 6.2.1) */
#define TLS_CONTENT_ALERT     21
#define TLS_CONTENT_HANDSHAKE 22

/* Clarification 10.7 names br_ssl_engine_fail(), which BearSSL declares only in its private
 * src/inner.h (it is an ordinary external function of libbearssl.a at the pinned commit). */
void br_ssl_engine_fail( br_ssl_engine_context *cc, int err );

/* ---------------------------------------------------------------- objects (clarification 10.2, 10.3) */

/* what schan_credentials->credentials points to */
struct bear_credentials
{
    br_x509_certificate chain;      /* the one certificate, DER; data NULL without one */
    br_rsa_private_key  sk;         /* its key (big-endian parts in key_buf) */
    unsigned char      *key_buf;
};

#define PEER_MAX_CERTS 16
#define PEER_MAX_BYTES 65536

/* the X.509 class of clarification 10.6: collects the peer chain, accepts it, and returns the
 * end-entity key for the handshake; verification stays on the Windows side */
struct peer_chain
{
    const br_x509_class *vtable;
    unsigned char *der[PEER_MAX_CERTS];
    size_t len[PEER_MAX_CERTS];
    size_t expected;
    unsigned int count;
    size_t total;
    BOOL too_big;
    br_x509_decoder_context decoder;
};

struct bear_session
{
    BOOL server;
    union
    {
        br_ssl_client_context client;
        br_ssl_server_context server;
    } u;
    br_x509_minimal_context minimal;    /* br_ssl_client_init_full wants one; ours replaces it */
    br_ssl_engine_context *eng;
    unsigned char *iobuf;
    struct peer_chain peer;
    struct bear_credentials *cred;
    char *target;
    char *alpn_buf;
    const char *alpn[16];
    unsigned int alpn_count;
    BOOL started;                       /* reset done (first handshake call) */
    BOOL handshake_done;                /* a handshake completed: tls-unique is valid */
    BOOL renegotiating;                 /* recv returned SEC_I_RENEGOTIATE */
};

static inline struct bear_session *session_from_handle( UINT64 handle )
{
    return (struct bear_session *)(ULONG_PTR)handle;
}

/* ---------------------------------------------------------------- logging (Part E) */

static void log_once( int *flag, const char *msg )
{
    if (!__atomic_exchange_n( flag, 1, __ATOMIC_RELAXED )) fprintf( stderr, "[secur32-ios] %s\n", msg );
}

/* MYIOSDECK (spec 13 Part E): handshake outcomes go to the log (stderr), the first 32 of each
 * kind per app run, so a busy client can't flood it. Counted app-wide, without locks. */
enum ios_handshake_kind
{
    IOS_HANDSHAKE_COMPLETE,
    IOS_HANDSHAKE_ALERT_RECEIVED,
    IOS_HANDSHAKE_ALERT_SENT,
    IOS_HANDSHAKE_FAILURE,
    IOS_HANDSHAKE_KINDS
};
#define IOS_HANDSHAKE_LOG_MAX 32
static unsigned int ios_handshake_logged[IOS_HANDSHAKE_KINDS];

/* TRUE while fewer than IOS_HANDSHAKE_LOG_MAX of this kind were logged; *last is set on the
 * final one */
static BOOL ios_handshake_log_slot( enum ios_handshake_kind kind, BOOL *last )
{
    unsigned int n = __atomic_load_n( &ios_handshake_logged[kind], __ATOMIC_RELAXED );

    do
    {
        if (n >= IOS_HANDSHAKE_LOG_MAX) return FALSE;
    }
    while (!__atomic_compare_exchange_n( &ios_handshake_logged[kind], &n, n + 1, TRUE,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED ));
    *last = (n + 1 == IOS_HANDSHAKE_LOG_MAX);
    return TRUE;
}

static void ios_log_handshake( struct bear_session *s, enum ios_handshake_kind kind, int value )
{
    static const char * const more = " (no more of this kind will be logged)";
    br_ssl_session_parameters sp;
    BOOL last;

    if (!ios_handshake_log_slot( kind, &last )) return;
    switch (kind)
    {
    case IOS_HANDSHAKE_COMPLETE:
        br_ssl_engine_get_session_parameters( s->eng, &sp );
        fprintf( stderr, "[secur32-ios] TLS handshake complete (%s), version %#x, suite %#06x%s\n",
                 s->server ? "server" : "client", sp.version, sp.cipher_suite, last ? more : "" );
        break;
    case IOS_HANDSHAKE_ALERT_RECEIVED:
        fprintf( stderr, "[secur32-ios] TLS fatal alert %d received%s\n", value, last ? more : "" );
        break;
    case IOS_HANDSHAKE_ALERT_SENT:
        fprintf( stderr, "[secur32-ios] TLS fatal alert %d sent%s\n", value, last ? more : "" );
        break;
    default:
        fprintf( stderr, "[secur32-ios] TLS failed, BearSSL error %d%s\n", value, last ? more : "" );
        break;
    }
}

/* the engine is closed: log why */
static void log_closed( struct bear_session *s )
{
    int err = br_ssl_engine_last_error( s->eng );

    if (err >= BR_ERR_RECV_FATAL_ALERT && err < BR_ERR_SEND_FATAL_ALERT)
        ios_log_handshake( s, IOS_HANDSHAKE_ALERT_RECEIVED, err - BR_ERR_RECV_FATAL_ALERT );
    else if (err >= BR_ERR_SEND_FATAL_ALERT)
        ios_log_handshake( s, IOS_HANDSHAKE_ALERT_SENT, err - BR_ERR_SEND_FATAL_ALERT );
    else if (err != BR_ERR_OK)
        ios_log_handshake( s, IOS_HANDSHAKE_FAILURE, err );
}

/* ---------------------------------------------------------------- the SecBuffer walk (upstream) */

struct schan_buffers
{
    SIZE_T offset;
    SIZE_T limit;
    const SecBufferDesc *desc;
    int current_buffer_idx;
};

static void init_schan_buffers(struct schan_buffers *s, const PSecBufferDesc desc)
{
    s->offset = 0;
    s->limit = ~0UL;
    s->desc = desc;
    s->current_buffer_idx = -1;
}

static int get_next_buffer(struct schan_buffers *s)
{
    if (s->current_buffer_idx == -1)
        return s->desc->cBuffers ? 0 : -1;
    if (s->current_buffer_idx == s->desc->cBuffers - 1)
        return -1;
    return s->current_buffer_idx + 1;
}

static char *get_buffer(struct schan_buffers *s, SIZE_T *count)
{
    SIZE_T max_count;
    PSecBuffer buffer;

    if (!s->desc)
    {
        TRACE("No desc\n");
        return NULL;
    }

    if (s->current_buffer_idx == -1)
    {
        /* Initial buffer */
        int buffer_idx = get_next_buffer(s);
        if (buffer_idx == -1)
        {
            TRACE("No next buffer\n");
            return NULL;
        }
        s->current_buffer_idx = buffer_idx;
    }

    buffer = &s->desc->pBuffers[s->current_buffer_idx];
    TRACE("Using buffer %d: cbBuffer %d, BufferType %#x, pvBuffer %p\n",
          s->current_buffer_idx, (unsigned)buffer->cbBuffer, (unsigned)buffer->BufferType, buffer->pvBuffer);

    max_count = buffer->cbBuffer - s->offset;
    if (s->limit != ~0UL && s->limit < max_count)
        max_count = s->limit;

    while (!max_count)
    {
        int buffer_idx;

        buffer_idx = get_next_buffer(s);
        if (buffer_idx == -1)
        {
            TRACE("No next buffer\n");
            return NULL;
        }
        s->current_buffer_idx = buffer_idx;
        s->offset = 0;
        buffer = &s->desc->pBuffers[buffer_idx];
        max_count = buffer->cbBuffer;
        if (s->limit != ~0UL && s->limit < max_count)
            max_count = s->limit;
    }

    if (*count > max_count)
        *count = max_count;
    if (s->limit != ~0UL)
        s->limit -= *count;

    return (char *)buffer->pvBuffer + s->offset;
}

/* upstream's push_adapter: bytes into the output buffers; FALSE when no room is left */
static BOOL push_bytes( struct schan_buffers *out, const unsigned char *data, size_t len, size_t *done )
{
    *done = 0;
    while (*done < len)
    {
        SIZE_T count = len - *done;
        char *b = get_buffer( out, &count );

        if (!b) return FALSE;
        memcpy( b, data + *done, count );
        out->offset += count;
        *done += count;
    }
    return TRUE;
}

/* the records the engine has to send, into the output buffers (clarification 10.7); FALSE when
 * out of room with bytes left, which stay in the engine for the next call */
static BOOL drain( struct bear_session *s, struct schan_buffers *out )
{
    unsigned char *rec;
    size_t len, done;
    BOOL all;

    while ((rec = br_ssl_engine_sendrec_buf( s->eng, &len )))
    {
        all = push_bytes( out, rec, len, &done );
        if (done) br_ssl_engine_sendrec_ack( s->eng, done );
        if (!all) return FALSE;
    }
    return TRUE;
}

/* upstream's pull_adapter: input into the engine, never more than it asks for, so the caller's
 * input_offset counts exactly what the engine consumed; FALSE when no input is left */
static BOOL feed( struct bear_session *s, struct schan_buffers *in )
{
    unsigned char *dst;
    size_t want;
    SIZE_T count;
    char *b;

    if (!(dst = br_ssl_engine_recvrec_buf( s->eng, &want ))) return FALSE;
    count = want;
    if (!(b = get_buffer( in, &count ))) return FALSE;
    memcpy( dst, b, count );
    in->offset += count;
    br_ssl_engine_recvrec_ack( s->eng, count );
    return TRUE;
}

/* ---------------------------------------------------------------- the peer chain (clarification 10.6) */

static void peer_free( struct peer_chain *pc )
{
    unsigned int i;

    for (i = 0; i < pc->count; i++) free( pc->der[i] );
    if (pc->count < PEER_MAX_CERTS) free( pc->der[pc->count] );   /* one being received */
    memset( pc->der, 0, sizeof(pc->der) );
    pc->count = 0;
    pc->total = 0;
    pc->too_big = FALSE;
}

static void peer_start_chain( const br_x509_class **ctx, const char *server_name )
{
    struct peer_chain *pc = (struct peer_chain *)ctx;
    peer_free( pc );
}

static void peer_start_cert( const br_x509_class **ctx, uint32_t length )
{
    struct peer_chain *pc = (struct peer_chain *)ctx;
    static int logged;

    if (pc->too_big) return;
    if (pc->count >= PEER_MAX_CERTS || pc->total + length > PEER_MAX_BYTES ||
        !(pc->der[pc->count] = malloc( length ? length : 1 )))
    {
        pc->too_big = TRUE;
        log_once( &logged, "peer certificate chain over 16 certificates or 64 KB: handshake refused" );
        return;
    }
    pc->len[pc->count] = 0;
    pc->expected = length;
    if (!pc->count) br_x509_decoder_init( &pc->decoder, NULL, NULL );
}

static void peer_append( const br_x509_class **ctx, const unsigned char *buf, size_t len )
{
    struct peer_chain *pc = (struct peer_chain *)ctx;
    size_t room;

    if (pc->too_big) return;
    room = pc->expected - pc->len[pc->count];
    if (len > room) len = room;
    memcpy( pc->der[pc->count] + pc->len[pc->count], buf, len );
    pc->len[pc->count] += len;
    if (!pc->count) br_x509_decoder_push( &pc->decoder, buf, len );
}

static void peer_end_cert( const br_x509_class **ctx )
{
    struct peer_chain *pc = (struct peer_chain *)ctx;

    if (pc->too_big) return;
    pc->total += pc->len[pc->count];
    pc->count++;
}

static unsigned peer_end_chain( const br_x509_class **ctx )
{
    struct peer_chain *pc = (struct peer_chain *)ctx;
    int err;

    if (pc->too_big) return BR_ERR_X509_LIMIT_EXCEEDED;
    if (!pc->count) return BR_ERR_X509_EMPTY_CHAIN;
    if ((err = br_x509_decoder_last_error( &pc->decoder ))) return err;
    return 0;
}

static const br_x509_pkey *peer_get_pkey( const br_x509_class *const *ctx, unsigned *usages )
{
    struct peer_chain *pc = (struct peer_chain *)ctx;

    if (usages) *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    return pc->count ? br_x509_decoder_get_pkey( &pc->decoder ) : NULL;
}

static const br_x509_class peer_chain_vtable =
{
    sizeof(struct peer_chain),
    peer_start_chain,
    peer_start_cert,
    peer_append,
    peer_end_cert,
    peer_end_chain,
    peer_get_pkey,
};

/* ---------------------------------------------------------------- suites (clarification 10.4) */

static const WCHAR emptyW[] = {0};
static const WCHAR shaW[] = {'S','H','A',0};
static const WCHAR sha1W[] = {'S','H','A','1',0};
static const WCHAR sha256W[] = {'S','H','A','2','5','6',0};
static const WCHAR sha384W[] = {'S','H','A','3','8','4',0};

struct suite_info
{
    uint16_t id;
    BOOL ecdhe;             /* else RSA key exchange */
    BOOL ecdsa;             /* certificate key: ECDSA, else RSA */
    BOOL gcm;               /* else CBC */
    ALG_ID cipher;
    DWORD cipher_strength;
    ALG_ID hash;
    DWORD hash_strength;
    const WCHAR *hash_full, *hash_short;
};

static const struct suite_info suites[] =
{
    { BR_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256, TRUE,  TRUE,  TRUE,  CALG_AES_128, 128, CALG_SHA_256, 0,   emptyW,  emptyW },
    { BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,   TRUE,  FALSE, TRUE,  CALG_AES_128, 128, CALG_SHA_256, 0,   emptyW,  emptyW },
    { BR_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384, TRUE,  TRUE,  TRUE,  CALG_AES_256, 256, CALG_SHA_384, 0,   emptyW,  emptyW },
    { BR_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,   TRUE,  FALSE, TRUE,  CALG_AES_256, 256, CALG_SHA_384, 0,   emptyW,  emptyW },
    { BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256, TRUE,  TRUE,  FALSE, CALG_AES_128, 128, CALG_SHA_256, 256, sha256W, sha256W },
    { BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA256,   TRUE,  FALSE, FALSE, CALG_AES_128, 128, CALG_SHA_256, 256, sha256W, sha256W },
    { BR_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA384, TRUE,  TRUE,  FALSE, CALG_AES_256, 256, CALG_SHA_384, 384, sha384W, sha384W },
    { BR_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384,   TRUE,  FALSE, FALSE, CALG_AES_256, 256, CALG_SHA_384, 384, sha384W, sha384W },
    { BR_TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA,    TRUE,  TRUE,  FALSE, CALG_AES_128, 128, CALG_SHA1,    160, sha1W,   shaW },
    { BR_TLS_ECDHE_RSA_WITH_AES_128_CBC_SHA,      TRUE,  FALSE, FALSE, CALG_AES_128, 128, CALG_SHA1,    160, sha1W,   shaW },
    { BR_TLS_ECDHE_ECDSA_WITH_AES_256_CBC_SHA,    TRUE,  TRUE,  FALSE, CALG_AES_256, 256, CALG_SHA1,    160, sha1W,   shaW },
    { BR_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA,      TRUE,  FALSE, FALSE, CALG_AES_256, 256, CALG_SHA1,    160, sha1W,   shaW },
    { BR_TLS_RSA_WITH_AES_128_GCM_SHA256,         FALSE, FALSE, TRUE,  CALG_AES_128, 128, CALG_SHA_256, 0,   emptyW,  emptyW },
    { BR_TLS_RSA_WITH_AES_256_GCM_SHA384,         FALSE, FALSE, TRUE,  CALG_AES_256, 256, CALG_SHA_384, 0,   emptyW,  emptyW },
    { BR_TLS_RSA_WITH_AES_128_CBC_SHA256,         FALSE, FALSE, FALSE, CALG_AES_128, 128, CALG_SHA_256, 256, sha256W, sha256W },
    { BR_TLS_RSA_WITH_AES_256_CBC_SHA256,         FALSE, FALSE, FALSE, CALG_AES_256, 256, CALG_SHA_256, 256, sha256W, sha256W },
    { BR_TLS_RSA_WITH_AES_128_CBC_SHA,            FALSE, FALSE, FALSE, CALG_AES_128, 128, CALG_SHA1,    160, sha1W,   shaW },
    { BR_TLS_RSA_WITH_AES_256_CBC_SHA,            FALSE, FALSE, FALSE, CALG_AES_256, 256, CALG_SHA1,    160, sha1W,   shaW },
};

/* the negotiated suite's row; NULL before a handshake completed */
static const struct suite_info *session_suite( struct bear_session *s )
{
    br_ssl_session_parameters sp;
    unsigned int i;

    br_ssl_engine_get_session_parameters( s->eng, &sp );
    for (i = 0; i < ARRAY_SIZE(suites); i++) if (suites[i].id == sp.cipher_suite) return &suites[i];
    return NULL;
}

/* ---------------------------------------------------------------- versions (clarification 10.5) */

static NTSTATUS schan_get_enabled_protocols( void *args )
{
    return SP_PROT_TLS1_0 | SP_PROT_TLS1_1 | SP_PROT_TLS1_2;
}

/* the credential's range for its side; FALSE when none is enabled */
static BOOL session_versions( const schan_credentials *cred, BOOL server, unsigned *lowest, unsigned *highest )
{
    static const DWORD client[] = { SP_PROT_TLS1_0_CLIENT, SP_PROT_TLS1_1_CLIENT, SP_PROT_TLS1_2_CLIENT };
    static const DWORD server_flags[] = { SP_PROT_TLS1_0_SERVER, SP_PROT_TLS1_1_SERVER, SP_PROT_TLS1_2_SERVER };
    static int gap_logged;
    const DWORD *flags = server ? server_flags : client;
    unsigned int i, count = 0;

    *lowest = *highest = 0;
    for (i = 0; i < 3; i++)
    {
        if (!(cred->enabled_protocols & flags[i])) continue;
        if (!*lowest) *lowest = BR_TLS10 + i;
        *highest = BR_TLS10 + i;
        count++;
    }
    if (!count) return FALSE;
    if (count != *highest - *lowest + 1)
        log_once( &gap_logged, "enabled TLS versions have a gap: using the whole range between them" );
    return TRUE;
}

/* ---------------------------------------------------------------- sessions */

static NTSTATUS schan_create_session( void *args )
{
    const struct create_session_params *params = args;
    schan_credentials *cred = params->cred;
    struct bear_credentials *bc = (struct bear_credentials *)(ULONG_PTR)cred->credentials;
    BOOL server = (cred->credential_use == SECPKG_CRED_INBOUND);
    unsigned char seed[32];
    unsigned int i, lowest, highest;
    uint16_t ids[ARRAY_SIZE(suites)];
    struct bear_session *s;

    *params->session = 0;

    if (!session_versions( cred, server, &lowest, &highest )) return STATUS_INTERNAL_ERROR;
    if (!(s = calloc( 1, sizeof(*s) ))) return STATUS_INTERNAL_ERROR;
    if (!(s->iobuf = malloc( BR_SSL_BUFSIZE_BIDI )) || ios_ac_random( seed, sizeof(seed) ))
    {
        free( s->iobuf );
        free( s );
        return STATUS_INTERNAL_ERROR;
    }
    s->server = server;
    s->cred = bc;

    if (server)
    {
        /* without a certificate the first handshake call fails (start_session), as upstream's
         * handshake did; the NULL key is only stored, never used */
        if (bc && bc->chain.data) br_ssl_server_init_full_rsa( &s->u.server, &bc->chain, 1, &bc->sk );
        else br_ssl_server_init_full_rsa( &s->u.server, NULL, 0, NULL );
        s->eng = &s->u.server.eng;
    }
    else
    {
        br_ssl_client_init_full( &s->u.client, &s->minimal, NULL, 0 );
        if (bc && bc->chain.data)
            br_ssl_client_set_single_rsa( &s->u.client, &bc->chain, 1, &bc->sk, br_rsa_pkcs1_sign_get_default() );
        s->eng = &s->u.client.eng;
    }
    for (i = 0; i < ARRAY_SIZE(suites); i++) ids[i] = suites[i].id;
    br_ssl_engine_set_suites( s->eng, ids, ARRAY_SIZE(ids) );
    br_ssl_engine_set_versions( s->eng, lowest, highest );
    s->peer.vtable = &peer_chain_vtable;
    br_ssl_engine_set_x509( s->eng, &s->peer.vtable );
    br_ssl_engine_set_buffer( s->eng, s->iobuf, BR_SSL_BUFSIZE_BIDI, 1 );
    br_ssl_engine_inject_entropy( s->eng, seed, sizeof(seed) );
    memset( seed, 0, sizeof(seed) );

    *params->session = (ULONG_PTR)s;
    return STATUS_SUCCESS;
}

static NTSTATUS schan_dispose_session( void *args )
{
    const struct session_params *params = args;
    struct bear_session *s = session_from_handle( params->session );

    if (!s) return STATUS_SUCCESS;
    peer_free( &s->peer );
    free( s->iobuf );
    free( s->target );
    free( s->alpn_buf );
    memset( s, 0, sizeof(*s) );
    free( s );
    return STATUS_SUCCESS;
}

static NTSTATUS schan_set_session_target( void *args )
{
    const struct set_session_target_params *params = args;
    struct bear_session *s = session_from_handle( params->session );

    free( s->target );
    s->target = params->target ? strdup( params->target ) : NULL;
    return STATUS_SUCCESS;
}

/* the engine reset of clarification 10.3, at the first handshake call */
static BOOL start_session( struct bear_session *s )
{
    static int no_cert_logged;

    if (s->alpn_count) br_ssl_engine_set_protocol_names( s->eng, s->alpn, s->alpn_count );
    s->started = TRUE;
    if (s->server && (!s->cred || !s->cred->chain.data))
    {
        log_once( &no_cert_logged, "server session without a certificate: handshake refused" );
        s->started = FALSE;
        return FALSE;
    }
    if (s->server) return br_ssl_server_reset( &s->u.server ) != 0;
    return br_ssl_client_reset( &s->u.client, s->target, 0 ) != 0;
}

/* ---------------------------------------------------------------- alerts */

/* upstream's map_alert_type / map_alert_number: the Windows numbers are the TLS ones; -1 when
 * unknown */
static int map_alert_type(unsigned int type)
{
    switch (type)
    {
    case TLS1_ALERT_WARNING: return 1;
    case TLS1_ALERT_FATAL:   return 2;
    default:
        FIXME( "unknown type %u\n", type );
        return -1;
    }
}

static int map_alert_number(unsigned int number)
{
    switch (number)
    {
    case TLS1_ALERT_CLOSE_NOTIFY:
    case TLS1_ALERT_UNEXPECTED_MESSAGE:
    case TLS1_ALERT_BAD_RECORD_MAC:
    case TLS1_ALERT_DECRYPTION_FAILED:
    case TLS1_ALERT_RECORD_OVERFLOW:
    case TLS1_ALERT_DECOMPRESSION_FAIL:
    case TLS1_ALERT_HANDSHAKE_FAILURE:
    case TLS1_ALERT_BAD_CERTIFICATE:
    case TLS1_ALERT_UNSUPPORTED_CERT:
    case TLS1_ALERT_CERTIFICATE_REVOKED:
    case TLS1_ALERT_CERTIFICATE_EXPIRED:
    case TLS1_ALERT_CERTIFICATE_UNKNOWN:
    case TLS1_ALERT_ILLEGAL_PARAMETER:
    case TLS1_ALERT_UNKNOWN_CA:
    case TLS1_ALERT_ACCESS_DENIED:
    case TLS1_ALERT_DECODE_ERROR:
    case TLS1_ALERT_DECRYPT_ERROR:
    case TLS1_ALERT_EXPORT_RESTRICTION:
    case TLS1_ALERT_PROTOCOL_VERSION:
    case TLS1_ALERT_INSUFFIENT_SECURITY:
    case TLS1_ALERT_INTERNAL_ERROR:
    case TLS1_ALERT_USER_CANCELED:
    case TLS1_ALERT_NO_RENEGOTIATION:
    case TLS1_ALERT_UNSUPPORTED_EXT:
    case TLS1_ALERT_UNKNOWN_PSK_IDENTITY:
    case TLS1_ALERT_NO_APP_PROTOCOL:
        return number;
    default:
        FIXME("unhandled alert %u\n", number);
        return -1;
    }
}

/* A fatal alert. br_ssl_engine_fail() only marks the engine failed and puts nothing on the wire,
 * so the 2-byte alert is made into a record by the engine's own record encryptor (BearSSL's code:
 * the current cipher state and sequence number), after the records already queued; then the
 * engine is failed with BR_ERR_SEND_FATAL_ALERT + number. QUESTIONS.md 13-6. */
static NTSTATUS send_fatal_alert( struct bear_session *s, struct schan_buffers *out, int number )
{
    unsigned char buf[512], *rec;
    size_t start = 5, end = sizeof(buf), len = 2, done;
    BOOL ok;

    if (br_ssl_engine_current_state( s->eng ) & BR_SSL_CLOSED) return SEC_E_INTERNAL_ERROR;
    if (!drain( s, out )) return SEC_I_CONTINUE_NEEDED;
    s->eng->out.vtable->max_plaintext( &s->eng->out.vtable, &start, &end );
    if (end < start + 2) return SEC_E_INTERNAL_ERROR;
    buf[start] = 2;                 /* fatal */
    buf[start + 1] = number;
    rec = s->eng->out.vtable->encrypt( &s->eng->out.vtable, TLS_CONTENT_ALERT,
                                       s->eng->version_out ? s->eng->version_out : BR_TLS10, buf + start, &len );
    ok = push_bytes( out, rec, len, &done );
    br_ssl_engine_fail( s->eng, BR_ERR_SEND_FATAL_ALERT + number );
    ios_log_handshake( s, IOS_HANDSHAKE_ALERT_SENT, number );
    return ok ? SEC_E_OK : SEC_E_INTERNAL_ERROR;
}

/* ApplyControlToken (clarification 10.7) */
static NTSTATUS control_token( struct bear_session *s, const struct handshake_params *params,
                               struct schan_buffers *out )
{
    static int warning_logged;
    int type, number;

    if (params->control_token == CONTROL_TOKEN_ALERT)
    {
        if ((type = map_alert_type( params->alert_type )) < 0 || (number = map_alert_number( params->alert_number )) < 0)
            return SEC_E_INTERNAL_ERROR;
        if (type == 2) return send_fatal_alert( s, out, number );
        if (number != TLS1_ALERT_CLOSE_NOTIFY)
        {
            /* BearSSL sends no warning alert other than close_notify */
            log_once( &warning_logged, "warning alerts other than close_notify are not sent" );
            return SEC_E_OK;
        }
    }
    /* CONTROL_TOKEN_SHUTDOWN, or a close_notify warning */
    br_ssl_engine_close( s->eng );
    return drain( s, out ) ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
}

/* ---------------------------------------------------------------- handshake, send, recv */

static NTSTATUS schan_handshake( void *args )
{
    const struct handshake_params *params = args;
    struct bear_session *s = session_from_handle(params->session);
    struct schan_buffers in, out;
    NTSTATUS status;
    unsigned st;

    init_schan_buffers(&in, params->input);
    in.limit = params->input_size;
    init_schan_buffers(&out, params->output);

    if (!s->started && !start_session( s ))
    {
        log_closed( s );
        status = SEC_E_INTERNAL_ERROR;
        goto done;
    }

    if (params->control_token)
    {
        status = control_token( s, params, &out );
        goto done;
    }

    for (;;)
    {
        st = br_ssl_engine_current_state( s->eng );
        if (st & BR_SSL_CLOSED)
        {
            log_closed( s );
            status = SEC_E_INTERNAL_ERROR;
            break;
        }
        if (st & BR_SSL_SENDREC)
        {
            if (drain( s, &out )) continue;
            TRACE("Continue...\n");
            status = SEC_I_CONTINUE_NEEDED;
            break;
        }
        if (st & (BR_SSL_SENDAPP | BR_SSL_RECVAPP))
        {
            TRACE("Handshake completed\n");
            if (!s->handshake_done || s->renegotiating) ios_log_handshake( s, IOS_HANDSHAKE_COMPLETE, 0 );
            s->handshake_done = TRUE;
            s->renegotiating = FALSE;
            status = SEC_E_OK;
            break;
        }
        if ((st & BR_SSL_RECVREC) && feed( s, &in )) continue;
        TRACE("Continue...\n");
        status = SEC_I_CONTINUE_NEEDED;
        break;
    }

done:
    *params->input_offset = in.offset;
    *params->output_buffer_idx = out.current_buffer_idx;
    *params->output_offset = out.offset;

    return status;
}

static NTSTATUS schan_send( void *args )
{
    const struct send_params *params = args;
    struct bear_session *s = session_from_handle(params->session);
    struct schan_buffers out;
    unsigned char *buf;
    size_t len, total = 0;

    init_schan_buffers(&out, params->output);

    for (;;)
    {
        /* records already made go first */
        if (!drain( s, &out )) return SEC_I_CONTINUE_NEEDED;
        if (total == params->length) break;
        if (!(buf = br_ssl_engine_sendapp_buf( s->eng, &len )))
        {
            log_closed( s );
            return SEC_E_INTERNAL_ERROR;
        }
        if (len > params->length - total) len = params->length - total;
        memcpy( buf, (const char *)params->buffer + total, len );
        br_ssl_engine_sendapp_ack( s->eng, len );
        total += len;
        TRACE( "sent %lu now %lu/%u\n", (unsigned long)len, (unsigned long)total, (unsigned)params->length );
        if (total == params->length) br_ssl_engine_flush( s->eng, 0 );
    }

    *params->output_buffer_idx = out.current_buffer_idx;
    *params->output_offset = out.offset;
    return SEC_E_OK;
}

static NTSTATUS schan_recv( void *args )
{
    const struct recv_params *params = args;
    struct bear_session *s = session_from_handle(params->session);
    size_t data_size = *params->length;
    size_t received = 0, len;
    struct schan_buffers in;
    SECURITY_STATUS status = SEC_E_OK;
    unsigned char *buf;
    unsigned st;

    init_schan_buffers(&in, params->input);
    in.limit = params->input_size;

    for (;;)
    {
        st = br_ssl_engine_current_state( s->eng );
        if ((st & BR_SSL_RECVAPP) && received < data_size)
        {
            buf = br_ssl_engine_recvapp_buf( s->eng, &len );
            if (len > data_size - received) len = data_size - received;
            memcpy( (char *)params->buffer + received, buf, len );
            br_ssl_engine_recvapp_ack( s->eng, len );
            received += len;
            continue;
        }
        if (st & BR_SSL_CLOSED)
        {
            if (br_ssl_engine_last_error( s->eng ) == BR_ERR_OK) break;   /* closed by close_notify */
            log_closed( s );
            return SEC_E_INTERNAL_ERROR;
        }
        if (received == data_size || !(st & BR_SSL_RECVREC) || !feed( s, &in )) break;

        st = br_ssl_engine_current_state( s->eng );
        if (!received && (st & BR_SSL_SENDREC) && !(st & BR_SSL_CLOSED))
        {
            if (s->eng->record_type_in == TLS_CONTENT_HANDSHAKE)
            {
                /* the peer asked for a new handshake: the next handshake call sends our answer */
                TRACE("Rehandshake requested\n");
                s->renegotiating = TRUE;
                status = SEC_I_RENEGOTIATE;
                break;
            }
            if (s->eng->record_type_in == TLS_CONTENT_ALERT) break;   /* close_notify: ours is queued */
        }
    }

    *params->length = received;
    return status;
}

/* ---------------------------------------------------------------- information */

static NTSTATUS schan_get_session_cipher_block_size( void *args )
{
    return 16;
}

static NTSTATUS schan_get_max_message_size( void *args )
{
    return 16384;
}

static DWORD get_protocol( struct bear_session *s )
{
    unsigned version = br_ssl_engine_get_version( s->eng );

    switch (version)
    {
    case BR_TLS10: return s->server ? SP_PROT_TLS1_0_SERVER : SP_PROT_TLS1_0_CLIENT;
    case BR_TLS11: return s->server ? SP_PROT_TLS1_1_SERVER : SP_PROT_TLS1_1_CLIENT;
    case BR_TLS12: return s->server ? SP_PROT_TLS1_2_SERVER : SP_PROT_TLS1_2_CLIENT;
    default:
        FIXME("unknown protocol %#x\n", version);
        return 0;
    }
}

static NTSTATUS schan_get_connection_info( void *args )
{
    const struct get_connection_info_params *params = args;
    struct bear_session *s = session_from_handle(params->session);
    SecPkgContext_ConnectionInfo *info = params->info;
    const struct suite_info *suite = session_suite( s );

    info->dwProtocol = get_protocol( s );
    info->aiCipher = suite ? suite->cipher : 0;
    info->dwCipherStrength = suite ? suite->cipher_strength : 0;
    info->aiHash = suite ? suite->hash : 0;
    info->dwHashStrength = suite ? suite->hash_strength : 0;
    info->aiExch = !suite ? 0 : suite->ecdhe ? CALG_ECDH_EPHEM : CALG_RSA_KEYX;
    /* FIXME: info->dwExchStrength? */
    info->dwExchStrength = 0;
    return SEC_E_OK;
}

static const WCHAR *get_exchange_str( const struct suite_info *suite, BOOL full )
{
    static const WCHAR ecdhW[] = {'E','C','D','H',0};
    static const WCHAR ecdheW[] = {'E','C','D','H','E',0};
    static const WCHAR rsaW[] = {'R','S','A',0};
    static const WCHAR unknownW[] = {'<','u','n','k','n','o','w','n','>',0};

    if (!suite) return unknownW;
    if (suite->ecdhe) return full ? ecdheW : ecdhW;
    return rsaW;
}

static const WCHAR *get_certificate_str( const struct suite_info *suite )
{
    static const WCHAR rsaW[] = {'R','S','A',0};
    static const WCHAR ecdsaW[] = {'E','C','D','S','A',0};
    static const WCHAR unknownW[] = {'<','u','n','k','n','o','w','n','>',0};

    if (!suite) return unknownW;
    return suite->ecdsa ? ecdsaW : rsaW;
}

static NTSTATUS schan_get_cipher_info( void *args )
{
    static const WCHAR tlsW[] = {'T','L','S','_',0};
    static const WCHAR underscoreW[] = {'_',0};
    static const WCHAR widthW[] = {'_','W','I','T','H','_',0};
    static const WCHAR aesW[] = {'A','E','S',0};
    static const WCHAR cbcW[] = {'C','B','C',0};
    static const WCHAR gcmW[] = {'G','C','M',0};
    static const WCHAR unknownW[] = {'<','u','n','k','n','o','w','n','>',0};
    const struct get_cipher_info_params *params = args;
    struct bear_session *s = session_from_handle( params->session );
    const struct suite_info *suite = session_suite( s );
    SecPkgContext_CipherInfo *info = params->info;
    char buf[11];
    WCHAR *ptr;
    const WCHAR *hash;
    int len;

    info->dwProtocol = br_ssl_engine_get_version( s->eng );
    info->dwCipherSuite = 0; /* FIXME */
    info->dwBaseCipherSuite = 0; /* FIXME */
    wcscpy( info->szCipher, suite ? aesW : unknownW );
    info->dwCipherLen = suite ? suite->cipher_strength : 0;
    info->dwCipherBlockLen = 16;
    wcscpy( info->szHash, suite ? suite->hash_full : unknownW );
    info->dwHashLen = suite ? suite->hash_strength : 0;
    wcscpy( info->szExchange, get_exchange_str( suite, FALSE ) );
    info->dwMinExchangeLen = 0;
    info->dwMaxExchangeLen = 65536;
    wcscpy( info->szCertificate, get_certificate_str( suite ) );
    info->dwKeyType = 0; /* FIXME */

    wcscpy( info->szCipherSuite, tlsW );
    wcscat( info->szCipherSuite, get_exchange_str( suite, TRUE ) );
    wcscat( info->szCipherSuite, underscoreW );
    wcscat( info->szCipherSuite, info->szCertificate );
    wcscat( info->szCipherSuite, widthW );
    wcscat( info->szCipherSuite, info->szCipher );
    wcscat( info->szCipherSuite, underscoreW );
    len = snprintf( buf, sizeof(buf), "%u", (unsigned int)info->dwCipherLen ) + 1;
    ptr = info->szCipherSuite + wcslen( info->szCipherSuite );
    ntdll_umbstowcs( buf, len, ptr, len );
    wcscat( info->szCipherSuite, underscoreW );
    wcscat( info->szCipherSuite, !suite ? unknownW : suite->gcm ? gcmW : cbcW );
    wcscat( info->szCipherSuite, underscoreW );
    hash = suite ? suite->hash_short : unknownW;
    if (hash[0]) wcscat( info->szCipherSuite, hash );
    else wcscat( info->szCipherSuite, sha384W ); /* FIXME */
    return SEC_E_OK;
}

/* RFC 5929 tls-unique: the client's Finished verify_data of the last (full) handshake */
static NTSTATUS schan_get_unique_channel_binding( void *args )
{
    const struct get_unique_channel_binding_params *params = args;
    struct bear_session *s = session_from_handle(params->session);
    const ULONG size = 12;
    SECURITY_STATUS ret;

    if (!s->handshake_done) return SEC_E_INTERNAL_ERROR;
    if (params->buffer && *params->bufsize >= size)
    {
        memcpy( params->buffer, s->eng->saved_finished, size );
        ret = SEC_E_OK;
    }
    else ret = SEC_E_BUFFER_TOO_SMALL;

    *params->bufsize = size;
    return ret;
}

static NTSTATUS schan_get_key_signature_algorithm( void *args )
{
    const struct session_params *params = args;
    struct bear_session *s = session_from_handle(params->session);
    const struct suite_info *suite = session_suite( s );

    TRACE("(%p)\n", s);

    if (!suite) return 0;
    return suite->ecdsa ? CALG_ECDSA : CALG_RSA_SIGN;
}

static NTSTATUS schan_get_session_peer_certificate( void *args )
{
    const struct get_session_peer_certificate_params *params = args;
    struct bear_session *s = session_from_handle(params->session);
    unsigned int i, size, count = s->peer.count;
    BYTE *ptr;
    ULONG *sizes;

    if (!count) return SEC_E_INTERNAL_ERROR;

    size = count * sizeof(*sizes);
    for (i = 0; i < count; i++) size += s->peer.len[i];

    if (!params->buffer || *params->bufsize < size)
    {
        *params->bufsize = size;
        return SEC_E_BUFFER_TOO_SMALL;
    }
    sizes = (ULONG *)params->buffer;
    ptr = params->buffer + count * sizeof(*sizes);
    for (i = 0; i < count; i++)
    {
        sizes[i] = s->peer.len[i];
        memcpy(ptr, s->peer.der[i], s->peer.len[i]);
        ptr += s->peer.len[i];
    }

    *params->bufsize = size;
    *params->retcount = count;
    return SEC_E_OK;
}

/* ---------------------------------------------------------------- ALPN */

struct alpn_item
{
    const unsigned char *data;
    unsigned int size;
};

static unsigned int parse_alpn_protocol_list(unsigned char *buffer, unsigned int buflen, struct alpn_item *list)
{
    unsigned int len, offset = 0, count = 0;

    while (buflen)
    {
        len = buffer[offset++];
        buflen--;
        if (!len || len > buflen) return 0;
        if (list)
        {
            list[count].data = &buffer[offset];
            list[count].size = len;
        }
        buflen -= len;
        offset += len;
        count++;
    }

    return count;
}

static NTSTATUS schan_set_application_protocols( void *args )
{
    const struct set_application_protocols_params *params = args;
    struct bear_session *s = session_from_handle(params->session);
    unsigned int extension_len, extension, count = 0, offset = 0, i;
    unsigned short list_len;
    struct alpn_item *protocols;
    char *p;

    if (sizeof(extension_len) > params->buflen) return STATUS_INVALID_PARAMETER;
    extension_len = *(unsigned int *)&params->buffer[offset];
    offset += sizeof(extension_len);

    if (offset + sizeof(extension) > params->buflen) return STATUS_INVALID_PARAMETER;
    extension = *(unsigned int *)&params->buffer[offset];
    if (extension != SecApplicationProtocolNegotiationExt_ALPN)
    {
        FIXME("extension %u not supported\n", extension);
        return STATUS_NOT_SUPPORTED;
    }
    offset += sizeof(extension);

    if (offset + sizeof(list_len) > params->buflen) return STATUS_INVALID_PARAMETER;
    list_len = *(unsigned short *)&params->buffer[offset];
    offset += sizeof(list_len);

    if (offset + list_len > params->buflen) return STATUS_INVALID_PARAMETER;
    count = parse_alpn_protocol_list(&params->buffer[offset], list_len, NULL);
    if (!count || !(protocols = malloc(count * sizeof(*protocols)))) return STATUS_NO_MEMORY;

    parse_alpn_protocol_list(&params->buffer[offset], list_len, protocols);
    if (count > ARRAY_SIZE(s->alpn)) count = ARRAY_SIZE(s->alpn);

    /* BearSSL keeps the names as NUL-terminated strings: copies owned by the session */
    free( s->alpn_buf );
    s->alpn_count = 0;
    if ((s->alpn_buf = p = malloc( list_len + count )))
    {
        for (i = 0; i < count; i++)
        {
            memcpy( p, protocols[i].data, protocols[i].size );
            p[protocols[i].size] = 0;
            s->alpn[i] = p;
            p += protocols[i].size + 1;
        }
        s->alpn_count = count;
        if (s->started) br_ssl_engine_set_protocol_names( s->eng, s->alpn, s->alpn_count );
    }

    free(protocols);
    return STATUS_SUCCESS;
}

static NTSTATUS schan_get_application_protocol( void *args )
{
    const struct get_application_protocol_params *params = args;
    struct bear_session *s = session_from_handle(params->session);
    SecPkgContext_ApplicationProtocol *protocol = params->protocol;
    const char *selected;
    size_t size;

    memset(protocol, 0, sizeof(*protocol));
    if (!(selected = br_ssl_engine_get_selected_protocol( s->eng ))) return SEC_E_OK;

    if ((size = strlen( selected )) <= sizeof(protocol->ProtocolId))
    {
        protocol->ProtoNegoStatus = SecApplicationProtocolNegotiationStatus_Success;
        protocol->ProtoNegoExt    = SecApplicationProtocolNegotiationExt_ALPN;
        protocol->ProtocolIdSize  = size;
        memcpy(protocol->ProtocolId, selected, size);
        TRACE("returning %s\n", wine_dbgstr_an(selected, size));
    }
    return SEC_E_OK;
}

/* no DTLS session can exist (clarification 10.11) */
static NTSTATUS schan_set_dtls_mtu( void *args )
{
    return SEC_E_OK;
}

static NTSTATUS schan_set_dtls_timeouts( void *args )
{
    return SEC_E_OK;
}

/* ---------------------------------------------------------------- credentials (clarification 10.2) */

/* little-endian src of len bytes -> big-endian at dst */
static void reverse_copy( unsigned char *dst, const BYTE *src, ULONG len )
{
    ULONG i;
    for (i = 0; i < len; i++) dst[i] = src[len - 1 - i];
}

/* upstream's get_x509_key layout: BLOBHEADER, RSAPUBKEY, modulus, p, q, dp, dq, qinv, d (all
 * little-endian); into a br_rsa_private_key */
static BOOL get_rsa_key( struct bear_credentials *bc, ULONG key_size, const BYTE *key_blob )
{
    const RSAPUBKEY *rsakey;
    const BYTE *ptr;
    ULONG half;

    if (key_size < sizeof(BLOBHEADER) + sizeof(RSAPUBKEY)) return FALSE;
    rsakey = (const RSAPUBKEY *)(key_blob + sizeof(BLOBHEADER));
    TRACE("RSA key bitlen %u pubexp %u\n", (unsigned)rsakey->bitlen, (unsigned)rsakey->pubexp);
    if (!rsakey->bitlen || rsakey->bitlen % 16) return FALSE;
    half = rsakey->bitlen / 16;
    if (key_size < sizeof(BLOBHEADER) + sizeof(RSAPUBKEY) + rsakey->bitlen / 8 * 2 + half * 5) return FALSE;
    if (!(bc->key_buf = malloc( half * 5 ))) return FALSE;

    ptr = (const BYTE *)(rsakey + 1) + rsakey->bitlen / 8;      /* after the modulus */
    bc->sk.n_bitlen = rsakey->bitlen;
    bc->sk.p = bc->key_buf;              reverse_copy( bc->sk.p, ptr, half );  ptr += half;
    bc->sk.q = bc->key_buf + half;       reverse_copy( bc->sk.q, ptr, half );  ptr += half;
    bc->sk.dp = bc->key_buf + 2 * half;  reverse_copy( bc->sk.dp, ptr, half ); ptr += half;
    bc->sk.dq = bc->key_buf + 3 * half;  reverse_copy( bc->sk.dq, ptr, half ); ptr += half;
    bc->sk.iq = bc->key_buf + 4 * half;  reverse_copy( bc->sk.iq, ptr, half );
    bc->sk.plen = bc->sk.qlen = bc->sk.dplen = bc->sk.dqlen = bc->sk.iqlen = half;
    return TRUE;
}

static void free_bear_credentials( struct bear_credentials *bc )
{
    if (!bc) return;
    free( bc->chain.data );
    if (bc->key_buf) memset( bc->key_buf, 0, bc->sk.plen * 5 );
    free( bc->key_buf );
    free( bc );
}

static NTSTATUS schan_allocate_certificate_credentials( void *args )
{
    const struct allocate_certificate_credentials_params *params = args;
    struct bear_credentials *bc;

    if (!(bc = calloc( 1, sizeof(*bc) ))) return STATUS_INTERNAL_ERROR;

    if (!params->cert_blob)
    {
        params->c->credentials = (ULONG_PTR)bc;
        return STATUS_SUCCESS;
    }

    if (params->cert_encoding != X509_ASN_ENCODING)
    {
        FIXME("encoding type %u not supported\n", (unsigned)params->cert_encoding);
        free_bear_credentials( bc );
        return STATUS_INTERNAL_ERROR;
    }
    if (!params->cert_size || !(bc->chain.data = malloc( params->cert_size )) ||
        !get_rsa_key( bc, params->key_size, params->key_blob ))
    {
        free_bear_credentials( bc );
        return STATUS_INTERNAL_ERROR;
    }
    memcpy( bc->chain.data, params->cert_blob, params->cert_size );
    bc->chain.data_len = params->cert_size;

    params->c->credentials = (ULONG_PTR)bc;
    return STATUS_SUCCESS;
}

static NTSTATUS schan_free_certificate_credentials( void *args )
{
    const struct free_certificate_credentials_params *params = args;
    free_bear_credentials( (struct bear_credentials *)(ULONG_PTR)params->c->credentials );
    return STATUS_SUCCESS;
}

/* ---------------------------------------------------------------- attach (clarification 13) */

static int attach_logged;

/* nothing app-wide to set up for BearSSL; every attach succeeds */
static NTSTATUS process_attach( void *args )
{
    log_once( &attach_logged, "secur32: BearSSL 7bea48e (TLS 1.0-1.2)" );
    return STATUS_SUCCESS;
}

static NTSTATUS process_detach( void *args )
{
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    process_attach,
    process_detach,
    schan_allocate_certificate_credentials,
    schan_create_session,
    schan_dispose_session,
    schan_free_certificate_credentials,
    schan_get_application_protocol,
    schan_get_cipher_info,
    schan_get_connection_info,
    schan_get_enabled_protocols,
    schan_get_key_signature_algorithm,
    schan_get_max_message_size,
    schan_get_session_cipher_block_size,
    schan_get_session_peer_certificate,
    schan_get_unique_channel_binding,
    schan_handshake,
    schan_recv,
    schan_send,
    schan_set_application_protocols,
    schan_set_dtls_mtu,
    schan_set_session_target,
    schan_set_dtls_timeouts,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == unix_funcs_count);

#ifdef _WIN64

/* MYIOSDECK (spec 13 Part B): every PTR32 below, and every pointer inside the SecBufferDesc32
 * and SecBuffer32 they point to, is a guest address of the calling 32-bit program; ios_g2h()
 * turns it into a host pointer (B + value, 0 stays NULL). Session and credential handles are
 * not addresses and pass unchanged. */

typedef ULONG PTR32;

typedef struct SecBufferDesc32
{
    ULONG ulVersion;
    ULONG cBuffers;
    PTR32 pBuffers;
} SecBufferDesc32;

typedef struct SecBuffer32
{
    ULONG cbBuffer;
    ULONG BufferType;
    PTR32 pvBuffer;
} SecBuffer32;

static NTSTATUS wow64_schan_allocate_certificate_credentials( void *args )
{
    struct
    {
        PTR32 c;
        ULONG cert_encoding;
        ULONG cert_size;
        PTR32 cert_blob;
        ULONG key_size;
        PTR32 key_blob;
    } const *params32 = args;
    struct allocate_certificate_credentials_params params =
    {
        ios_g2h(params32->c),
        params32->cert_encoding,
        params32->cert_size,
        ios_g2h(params32->cert_blob),
        params32->key_size,
        ios_g2h(params32->key_blob),
    };
    return schan_allocate_certificate_credentials(&params);
}

static NTSTATUS wow64_schan_create_session( void *args )
{
    struct
    {
        PTR32 cred;
        PTR32 session;
    } const *params32 = args;
    struct create_session_params params =
    {
        ios_g2h(params32->cred),
        ios_g2h(params32->session),
    };
    return schan_create_session(&params);
}

static NTSTATUS wow64_schan_free_certificate_credentials( void *args )
{
    struct
    {
        PTR32 c;
    } const *params32 = args;
    struct free_certificate_credentials_params params =
    {
        ios_g2h(params32->c),
    };
    return schan_free_certificate_credentials(&params);
}

static NTSTATUS wow64_schan_get_application_protocol( void *args )
{
    struct
    {
        schan_session session;
        PTR32 protocol;
    } const *params32 = args;
    struct get_application_protocol_params params =
    {
        params32->session,
        ios_g2h(params32->protocol),
    };
    return schan_get_application_protocol(&params);
}

static NTSTATUS wow64_schan_get_connection_info( void *args )
{
    struct
    {
        schan_session session;
        PTR32 info;
    } const *params32 = args;
    struct get_connection_info_params params =
    {
        params32->session,
        ios_g2h(params32->info),
    };
    return schan_get_connection_info(&params);
}

static NTSTATUS wow64_schan_get_cipher_info( void *args )
{
    struct
    {
        schan_session session;
        PTR32 info;
    } const *params32 = args;
    struct get_cipher_info_params params =
    {
        params32->session,
        ios_g2h(params32->info),
    };
    return schan_get_cipher_info(&params);
}

static NTSTATUS wow64_schan_get_session_peer_certificate( void *args )
{
    struct
    {
        schan_session session;
        PTR32 buffer;
        PTR32 bufsize;
        PTR32 retcount;
    } const *params32 = args;
    struct get_session_peer_certificate_params params =
    {
        params32->session,
        ios_g2h(params32->buffer),
        ios_g2h(params32->bufsize),
        ios_g2h(params32->retcount),
    };
    return schan_get_session_peer_certificate(&params);
}

static NTSTATUS wow64_schan_get_unique_channel_binding( void *args )
{
    struct
    {
        schan_session session;
        PTR32 buffer;
        PTR32 bufsize;
    } const *params32 = args;
    struct get_unique_channel_binding_params params =
    {
        params32->session,
        ios_g2h(params32->buffer),
        ios_g2h(params32->bufsize),
    };
    return schan_get_unique_channel_binding(&params);
}

static void secbufferdesc_32to64(const SecBufferDesc32 *desc32, SecBufferDesc *desc)
{
    unsigned int i;

    desc->ulVersion = desc32->ulVersion;
    desc->cBuffers = desc32->cBuffers;
    for (i = 0; i < desc->cBuffers; ++i)
    {
        SecBuffer32 *buffer32 = ios_g2h(desc32->pBuffers + i * sizeof(*buffer32));
        desc->pBuffers[i].cbBuffer = buffer32->cbBuffer;
        desc->pBuffers[i].BufferType = buffer32->BufferType;
        desc->pBuffers[i].pvBuffer = ios_g2h(buffer32->pvBuffer);
    }
}

static NTSTATUS wow64_schan_handshake( void *args )
{
    SecBuffer input_buffers[3];
    SecBufferDesc input = { 0, 0, input_buffers };
    SecBuffer output_buffers[3];
    SecBufferDesc output = { 0, 0, output_buffers };

    struct
    {
        schan_session session;
        PTR32 input;
        ULONG input_size;
        PTR32 output;
        PTR32 input_offset;
        PTR32 output_buffer_idx;
        PTR32 output_offset;
        enum control_token control_token;
        unsigned int alert_type;
        unsigned int alert_number;
    } const *params32 = args;
    struct handshake_params params =
    {
        params32->session,
        params32->input ? &input : NULL,
        params32->input_size,
        params32->output ? &output : NULL,
        ios_g2h(params32->input_offset),
        ios_g2h(params32->output_buffer_idx),
        ios_g2h(params32->output_offset),
        params32->control_token,
        params32->alert_type,
        params32->alert_number,
    };
    if (params32->input)
    {
        SecBufferDesc32 *desc32 = ios_g2h(params32->input);
        assert(desc32->cBuffers <= ARRAY_SIZE(input_buffers));
        secbufferdesc_32to64(desc32, &input);
    }
    if (params32->output)
    {
        SecBufferDesc32 *desc32 = ios_g2h(params32->output);
        assert(desc32->cBuffers <= ARRAY_SIZE(output_buffers));
        secbufferdesc_32to64(desc32, &output);
    }
    return schan_handshake(&params);
}

static NTSTATUS wow64_schan_recv( void *args )
{
    SecBuffer buffers[3];
    SecBufferDesc input = { 0, 0, buffers };

    struct
    {
        schan_session session;
        PTR32 input;
        ULONG input_size;
        PTR32 buffer;
        PTR32 length;
    } const *params32 = args;
    struct recv_params params =
    {
        params32->session,
        params32->input ? &input : NULL,
        params32->input_size,
        ios_g2h(params32->buffer),
        ios_g2h(params32->length),
    };
    if (params32->input)
    {
        SecBufferDesc32 *desc32 = ios_g2h(params32->input);
        assert(desc32->cBuffers <= ARRAY_SIZE(buffers));
        secbufferdesc_32to64(desc32, &input);
    }
    return schan_recv(&params);
}

static NTSTATUS wow64_schan_send( void *args )
{
    SecBuffer buffers[3];
    SecBufferDesc output = { 0, 0, buffers };

    struct
    {
        schan_session session;
        PTR32 output;
        PTR32 buffer;
        ULONG length;
        PTR32 output_buffer_idx;
        PTR32 output_offset;
    } const *params32 = args;
    struct send_params params =
    {
        params32->session,
        params32->output ? &output : NULL,
        ios_g2h(params32->buffer),
        params32->length,
        ios_g2h(params32->output_buffer_idx),
        ios_g2h(params32->output_offset),
    };
    if (params32->output)
    {
        SecBufferDesc32 *desc32 = ios_g2h(params32->output);
        assert(desc32->cBuffers <= ARRAY_SIZE(buffers));
        secbufferdesc_32to64(desc32, &output);
    }
    return schan_send(&params);
}

static NTSTATUS wow64_schan_set_application_protocols( void *args )
{
    struct
    {
        schan_session session;
        PTR32 buffer;
        unsigned int buflen;
    } const *params32 = args;
    struct set_application_protocols_params params =
    {
        params32->session,
        ios_g2h(params32->buffer),
        params32->buflen,
    };
    return schan_set_application_protocols(&params);
}

static NTSTATUS wow64_schan_set_session_target( void *args )
{
    struct
    {
        schan_session session;
        PTR32 target;
    } const *params32 = args;
    struct set_session_target_params params =
    {
        params32->session,
        ios_g2h(params32->target),
    };
    return schan_set_session_target(&params);
}

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    process_attach,
    process_detach,
    wow64_schan_allocate_certificate_credentials,
    wow64_schan_create_session,
    schan_dispose_session,
    wow64_schan_free_certificate_credentials,
    wow64_schan_get_application_protocol,
    wow64_schan_get_cipher_info,
    wow64_schan_get_connection_info,
    schan_get_enabled_protocols,
    schan_get_key_signature_algorithm,
    schan_get_max_message_size,
    schan_get_session_cipher_block_size,
    wow64_schan_get_session_peer_certificate,
    wow64_schan_get_unique_channel_binding,
    wow64_schan_handshake,
    wow64_schan_recv,
    wow64_schan_send,
    wow64_schan_set_application_protocols,
    schan_set_dtls_mtu,
    wow64_schan_set_session_target,
    schan_set_dtls_timeouts,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == unix_funcs_count);

#endif /* _WIN64 */
