// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (c) 2026 the MYIOSDECK author
//
// Spec 13 clarification 15, Schannel: secur32/schannel_bearssl.c (included below) as an in-process
// client and server talking only through the unix-call entries, with SecBufferDesc arrays built
// the way Wine 11.4's dlls/secur32/schannel.c builds them (one TOKEN buffer per side for the
// handshake, { STREAM_HEADER, DATA, STREAM_TRAILER } for send, one record per recv). The server's
// certificate and key are tests/fixtures/fixtures.h (tests/fixtures/make-fixtures.sh); random
// bytes come from the fake of include/ios_applecrypto.h.

#include "secur32/schannel_bearssl.c"

#include "fake_applecrypto.h"
#include "fixtures/fixtures.h"
#include "ios_test.h"

/* ---- stubs for what ntdll's unix side provides ---- */
unsigned char __cdecl __wine_dbg_get_channel_flags( struct __wine_debug_channel *channel ) { return 0; }
const char * __cdecl __wine_dbg_strdup( const char *str ) { return str; }
int __cdecl __wine_dbg_output( const char *str ) { return fputs( str, stderr ); }
int __cdecl __wine_dbg_header( enum __wine_debug_class cls, struct __wine_debug_channel *channel,
                               const char *function ) { return 0; }
DWORD ntdll_umbstowcs( const char *src, DWORD srclen, WCHAR *dst, DWORD dstlen )
{
    DWORD i;
    for (i = 0; i < srclen && i < dstlen; i++) dst[i] = (unsigned char)src[i];
    return i;
}
static unsigned long fake_base;
unsigned long ios_wow_base( void ) { return fake_base; }
ULONG ios_wow_guest_addr( const void *host ) { return (ULONG)((ULONG_PTR)host - fake_base); }

#define CALL(f, p) __wine_unix_call_funcs[unix_##f]( p )
#define CALL32(f, p) __wine_unix_call_wow64_funcs[unix_##f]( p )

/* hex string -> bytes */
static size_t unhex( const char *s, unsigned char *out, size_t cap )
{
    size_t n = strlen( s ) / 2, i;
    unsigned int v;

    for (i = 0; i < n && i < cap; i++) { sscanf( s + 2 * i, "%2x", &v ); out[i] = v; }
    return i;
}

static BOOL wstr_is( const WCHAR *w, const char *a )
{
    while (*a) if (*w++ != (unsigned char)*a++) return FALSE;
    return !*w;
}

/* ---- 32-bit memory: guest address g is at arena + g - 0x10000 ---- */
static unsigned char arena[1 << 18] __attribute__((aligned(16)));
#define G(p) ((ULONG)((ULONG_PTR)(p) - fake_base))
static void use_arena( void ) { fake_base = (unsigned long)(ULONG_PTR)arena - 0x10000; }

/* ================================================================ credentials and sessions */

static unsigned char cert_der[2048];
static size_t cert_len;

/* the CAPI PRIVATEKEYBLOB of the fixture server key (get_x509_key's layout, little-endian) */
static BYTE *server_key_blob( ULONG *size )
{
    static const struct { const char *hex; ULONG len; } parts[] =
    {
        { fx_srv_n, 256 }, { fx_srv_p, 128 }, { fx_srv_q, 128 }, { fx_srv_dp, 128 },
        { fx_srv_dq, 128 }, { fx_srv_qinv, 128 }, { fx_srv_d, 256 },
    };
    BYTE *blob, *ptr, tmp[256];
    BLOBHEADER *hdr;
    RSAPUBKEY *rsa;
    unsigned int i, j;
    size_t n;

    *size = sizeof(BLOBHEADER) + sizeof(RSAPUBKEY) + 256 * 2 + 128 * 5;
    blob = calloc( 1, *size );
    hdr = (BLOBHEADER *)blob;
    hdr->bType = PRIVATEKEYBLOB;
    hdr->bVersion = 2;
    hdr->aiKeyAlg = CALG_RSA_KEYX;
    rsa = (RSAPUBKEY *)(hdr + 1);
    rsa->magic = 0x32415352;    /* RSA2 */
    rsa->bitlen = 2048;
    rsa->pubexp = 65537;
    ptr = (BYTE *)(rsa + 1);
    for (i = 0; i < ARRAY_SIZE(parts); i++)
    {
        memset( tmp, 0, sizeof(tmp) );
        n = unhex( parts[i].hex, tmp, sizeof(tmp) );
        /* big-endian value right-aligned in len bytes -> little-endian */
        for (j = 0; j < n; j++) ptr[j] = tmp[n - 1 - j];
        ptr += parts[i].len;
    }
    return blob;
}

static void make_creds( schan_credentials *c, ULONG use, DWORD protocols, BOOL with_cert )
{
    struct allocate_certificate_credentials_params p;
    ULONG key_size = 0;
    BYTE *key = with_cert ? server_key_blob( &key_size ) : NULL;

    memset( c, 0, sizeof(*c) );
    c->credential_use = use;
    c->enabled_protocols = protocols;
    p.c = c;
    p.cert_encoding = X509_ASN_ENCODING;
    p.cert_size = with_cert ? cert_len : 0;
    p.cert_blob = with_cert ? cert_der : NULL;
    p.key_size = key_size;
    p.key_blob = key;
    CHECK_EQ( CALL( allocate_certificate_credentials, &p ), STATUS_SUCCESS );
    free( key );
}

static void free_creds( schan_credentials *c )
{
    struct free_certificate_credentials_params p = { c };
    CHECK_EQ( CALL( free_certificate_credentials, &p ), STATUS_SUCCESS );
}

/* a side of the connection: its session, the bytes it has received and not consumed yet */
struct side
{
    schan_session session;
    BOOL wow;                       /* go through the 32-bit table */
    unsigned char in[1 << 16];
    size_t in_len;
    SECURITY_STATUS status;
};

static UINT64 create( schan_credentials *c )
{
    struct create_session_params p;
    schan_session s = 0;

    p.cred = c;
    p.session = &s;
    CHECK_EQ( CALL( create_session, &p ), STATUS_SUCCESS );
    return s;
}

static void dispose( schan_session s )
{
    struct session_params p = { s };
    CALL( dispose_session, &p );
}

static struct bear_session *bear( schan_session s )
{
    return session_from_handle( s );
}

/* ================================================================ the calls, as schannel.c makes them */

/* handshake: one TOKEN input buffer (or none), one TOKEN output buffer of out_cap bytes */
static SECURITY_STATUS do_handshake( struct side *sd, unsigned char *out, ULONG out_cap, ULONG *produced,
                                     enum control_token token, unsigned int type, unsigned int number )
{
    ULONG input_offset = 0, output_offset = 0;
    int output_buffer_idx = -1;
    SECURITY_STATUS st;

    if (!sd->wow)
    {
        SecBuffer inbuf = { sd->in_len, SECBUFFER_TOKEN, sd->in }, outbuf = { out_cap, SECBUFFER_TOKEN, out };
        SecBufferDesc indesc = { SECBUFFER_VERSION, 1, &inbuf }, outdesc = { SECBUFFER_VERSION, 1, &outbuf };
        struct handshake_params p;

        p.session = sd->session;
        p.input = sd->in_len ? &indesc : NULL;
        p.input_size = sd->in_len;
        p.output = &outdesc;
        p.input_offset = &input_offset;
        p.output_buffer_idx = &output_buffer_idx;
        p.output_offset = &output_offset;
        p.control_token = token;
        p.alert_type = type;
        p.alert_number = number;
        st = CALL( handshake, &p );
    }
    else
    {
        /* the same through the 32-bit table: everything in guest memory */
        struct
        {
            schan_session session;
            ULONG input, input_size, output, input_offset, output_buffer_idx, output_offset;
            enum control_token control_token;
            unsigned int alert_type, alert_number;
        } *p32 = (void *)arena;
        SecBufferDesc32 *in32 = (void *)(arena + 0x100), *out32 = (void *)(arena + 0x120);
        SecBuffer32 *inb32 = (void *)(arena + 0x140), *outb32 = (void *)(arena + 0x160);
        ULONG *in_off = (void *)(arena + 0x180), *out_off = (void *)(arena + 0x184);
        int *out_idx = (void *)(arena + 0x188);
        unsigned char *indata = arena + 0x1000, *outdata = arena + 0x11000;

        use_arena();
        memcpy( indata, sd->in, sd->in_len );
        inb32->cbBuffer = sd->in_len; inb32->BufferType = SECBUFFER_TOKEN; inb32->pvBuffer = G( indata );
        outb32->cbBuffer = out_cap; outb32->BufferType = SECBUFFER_TOKEN; outb32->pvBuffer = G( outdata );
        in32->ulVersion = out32->ulVersion = SECBUFFER_VERSION;
        in32->cBuffers = out32->cBuffers = 1;
        in32->pBuffers = G( inb32 ); out32->pBuffers = G( outb32 );
        *in_off = *out_off = 0;
        *out_idx = -1;
        p32->session = sd->session;
        p32->input = sd->in_len ? G( in32 ) : 0;
        p32->input_size = sd->in_len;
        p32->output = G( out32 );
        p32->input_offset = G( in_off ); p32->output_buffer_idx = G( out_idx ); p32->output_offset = G( out_off );
        p32->control_token = token; p32->alert_type = type; p32->alert_number = number;
        st = CALL32( handshake, p32 );
        input_offset = *in_off;
        output_offset = *out_off;
        output_buffer_idx = *out_idx;
        memcpy( out, outdata, output_buffer_idx != -1 ? output_offset : 0 );
        fake_base = 0;
    }

    CHECK( input_offset <= sd->in_len );
    memmove( sd->in, sd->in + input_offset, sd->in_len - input_offset );
    sd->in_len -= input_offset;
    *produced = output_buffer_idx != -1 ? output_offset : 0;
    return st;
}

/* send: { STREAM_HEADER 5, DATA len, STREAM_TRAILER trailer } -> the record bytes made */
static SECURITY_STATUS do_send( struct side *sd, const unsigned char *data, ULONG len, ULONG trailer,
                                unsigned char *rec, ULONG *rec_len )
{
    unsigned char *hdr = malloc( 5 ), *body = malloc( len ), *tr = malloc( trailer );
    ULONG output_offset = 0;
    int output_buffer_idx = -1;
    SECURITY_STATUS st;

    if (!sd->wow)
    {
        SecBuffer bufs[3] = { { 5, SECBUFFER_STREAM_HEADER, hdr }, { len, SECBUFFER_DATA, body },
                              { trailer, SECBUFFER_STREAM_TRAILER, tr } };
        SecBufferDesc desc = { SECBUFFER_VERSION, 3, bufs };
        struct send_params p = { sd->session, &desc, data, len, &output_buffer_idx, &output_offset };

        st = CALL( send, &p );
    }
    else
    {
        struct
        {
            schan_session session;
            ULONG output, buffer, length, output_buffer_idx, output_offset;
        } *p32 = (void *)arena;
        SecBufferDesc32 *d32 = (void *)(arena + 0x100);
        SecBuffer32 *b32 = (void *)(arena + 0x140);
        ULONG *out_off = (void *)(arena + 0x184);
        int *out_idx = (void *)(arena + 0x188);
        unsigned char *plain = arena + 0x1000, *h = arena + 0x9000, *b = arena + 0x9100, *t = arena + 0x19100;

        use_arena();
        memcpy( plain, data, len );
        b32[0].cbBuffer = 5; b32[0].BufferType = SECBUFFER_STREAM_HEADER; b32[0].pvBuffer = G( h );
        b32[1].cbBuffer = len; b32[1].BufferType = SECBUFFER_DATA; b32[1].pvBuffer = G( b );
        b32[2].cbBuffer = trailer; b32[2].BufferType = SECBUFFER_STREAM_TRAILER; b32[2].pvBuffer = G( t );
        d32->ulVersion = SECBUFFER_VERSION; d32->cBuffers = 3; d32->pBuffers = G( b32 );
        *out_off = 0; *out_idx = -1;
        p32->session = sd->session; p32->output = G( d32 ); p32->buffer = G( plain ); p32->length = len;
        p32->output_buffer_idx = G( out_idx ); p32->output_offset = G( out_off );
        st = CALL32( send, p32 );
        memcpy( hdr, h, 5 ); memcpy( body, b, len ); memcpy( tr, t, trailer );
        output_offset = *out_off;
        output_buffer_idx = *out_idx;
        fake_base = 0;
    }

    *rec_len = 0;
    if (!st)
    {
        /* the record runs contiguously through header, data and trailer */
        if (output_buffer_idx == 0) { memcpy( rec, hdr, output_offset ); *rec_len = output_offset; }
        else if (output_buffer_idx == 1) { memcpy( rec, hdr, 5 ); memcpy( rec + 5, body, output_offset ); *rec_len = 5 + output_offset; }
        else if (output_buffer_idx == 2)
        {
            memcpy( rec, hdr, 5 ); memcpy( rec + 5, body, len ); memcpy( rec + 5 + len, tr, output_offset );
            *rec_len = 5 + len + output_offset;
        }
    }
    free( hdr ); free( body ); free( tr );
    return st;
}

/* recv of one record (schannel.c cuts it out of the stream) */
static SECURITY_STATUS do_recv( struct side *sd, const unsigned char *rec, ULONG rec_len, unsigned char *data,
                                ULONG cap, ULONG *got )
{
    SECURITY_STATUS st;
    ULONG length = cap;

    if (!sd->wow)
    {
        SecBuffer inbuf = { rec_len, SECBUFFER_DATA, (void *)rec };
        SecBufferDesc indesc = { SECBUFFER_VERSION, 1, &inbuf };
        struct recv_params p = { sd->session, &indesc, rec_len, data, &length };

        st = CALL( recv, &p );
    }
    else
    {
        struct
        {
            schan_session session;
            ULONG input, input_size, buffer, length;
        } *p32 = (void *)arena;
        SecBufferDesc32 *d32 = (void *)(arena + 0x100);
        SecBuffer32 *b32 = (void *)(arena + 0x140);
        ULONG *len32 = (void *)(arena + 0x184);
        unsigned char *r = arena + 0x1000, *out = arena + 0x11000;

        use_arena();
        memcpy( r, rec, rec_len );
        b32->cbBuffer = rec_len; b32->BufferType = SECBUFFER_DATA; b32->pvBuffer = G( r );
        d32->ulVersion = SECBUFFER_VERSION; d32->cBuffers = 1; d32->pBuffers = G( b32 );
        *len32 = cap;
        p32->session = sd->session; p32->input = G( d32 ); p32->input_size = rec_len;
        p32->buffer = G( out ); p32->length = G( len32 );
        st = CALL32( recv, p32 );
        length = *len32;
        memcpy( data, out, length <= cap ? length : cap );
        fake_base = 0;
    }
    *got = length;
    return st;
}

/* the length of the TLS record at rec */
static ULONG record_len( const unsigned char *rec )
{
    return 5 + ((rec[3] << 8) | rec[4]);
}

/* run handshake calls on both sides, passing what each produced to the other, until both say
 * SEC_E_OK; out_cap bounds each call's output */
static BOOL handshake_pair( struct side *c, struct side *s, ULONG c_cap, ULONG s_cap )
{
    static unsigned char out[1 << 16];
    ULONG produced;
    unsigned int round;

    c->status = s->status = SEC_I_CONTINUE_NEEDED;
    for (round = 0; round < 400; round++)
    {
        c->status = do_handshake( c, out, c_cap, &produced, CONTROL_TOKEN_NONE, 0, 0 );
        memcpy( s->in + s->in_len, out, produced );
        s->in_len += produced;
        if (c->status != SEC_E_OK && c->status != SEC_I_CONTINUE_NEEDED) return FALSE;
        s->status = do_handshake( s, out, s_cap, &produced, CONTROL_TOKEN_NONE, 0, 0 );
        memcpy( c->in + c->in_len, out, produced );
        c->in_len += produced;
        if (s->status != SEC_E_OK && s->status != SEC_I_CONTINUE_NEEDED) return FALSE;
        if (c->status == SEC_E_OK && s->status == SEC_E_OK && !c->in_len && !s->in_len) return TRUE;
    }
    return FALSE;
}

/* app data from a to b through send and recv, one record at a time */
static BOOL transfer( struct side *a, struct side *b, const unsigned char *data, ULONG len )
{
    unsigned char *rec = malloc( len + 2048 ), *got = malloc( len + 16 );
    ULONG rec_len = 0, n = 0, off = 0, total = 0;
    BOOL ok;

    ok = do_send( a, data, len, 256 + 48, rec, &rec_len ) == SEC_E_OK && rec_len > len;
    while (ok && off < rec_len)
    {
        ULONG r = record_len( rec + off );
        ok = r <= rec_len - off && do_recv( b, rec + off, r, got + total, len + 16 - total, &n ) == SEC_E_OK;
        total += n;
        off += r;
    }
    ok = ok && total == len && !memcmp( got, data, len );
    free( rec ); free( got );
    return ok;
}

/* ================================================================ tests */

static const unsigned char alpn_client[] =
{
    /* extension_len, extension, list_len, "h2", "http/1.1" */
    18, 0, 0, 0, 2, 0, 0, 0, 12, 0, 2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1',
};
static const unsigned char alpn_server[] = { 9, 0, 0, 0, 2, 0, 0, 0, 3, 0, 2, 'h', '2' };

static void set_alpn( schan_session s, const unsigned char *list, unsigned int len )
{
    struct set_application_protocols_params p = { s, (unsigned char *)list, len };
    CHECK_EQ( CALL( set_application_protocols, &p ), STATUS_SUCCESS );
}

static void set_target( schan_session s, const char *name )
{
    struct set_session_target_params p = { s, name };
    CHECK_EQ( CALL( set_session_target, &p ), STATUS_SUCCESS );
}

/* connection and cipher info of a session against one row of clarification 10.4's table */
static void check_info( struct side *sd, DWORD protocol, DWORD version, ALG_ID cipher, DWORD strength, ALG_ID hash,
                        DWORD hash_strength, const char *hash_str, ALG_ID exch, const char *exch_str,
                        const char *suite_str )
{
    SecPkgContext_ConnectionInfo ci;
    SecPkgContext_CipherInfo info;
    struct get_connection_info_params cp = { sd->session, &ci };
    struct get_cipher_info_params ip = { sd->session, &info };
    struct session_params sp = { sd->session };

    memset( &ci, 0xcc, sizeof(ci) );
    memset( &info, 0, sizeof(info) );
    CHECK_EQ( CALL( get_connection_info, &cp ), SEC_E_OK );
    CHECK_EQ( ci.dwProtocol, protocol );
    CHECK_EQ( ci.aiCipher, cipher );
    CHECK_EQ( ci.dwCipherStrength, strength );
    CHECK_EQ( ci.aiHash, hash );
    CHECK_EQ( ci.dwHashStrength, hash_strength );
    CHECK_EQ( ci.aiExch, exch );
    CHECK_EQ( ci.dwExchStrength, 0 );
    CHECK_EQ( CALL( get_cipher_info, &ip ), SEC_E_OK );
    CHECK_EQ( info.dwProtocol, version );
    CHECK( wstr_is( info.szCipher, "AES" ) );
    CHECK_EQ( info.dwCipherLen, strength );
    CHECK_EQ( info.dwCipherBlockLen, 16 );
    CHECK( wstr_is( info.szHash, hash_str ) );
    CHECK_EQ( info.dwHashLen, hash_strength );
    CHECK( wstr_is( info.szExchange, exch_str ) );
    CHECK( wstr_is( info.szCertificate, "RSA" ) );
    CHECK_EQ( info.dwMinExchangeLen, 0 );
    CHECK_EQ( info.dwMaxExchangeLen, 65536 );
    CHECK( wstr_is( info.szCipherSuite, suite_str ) );
    if (!wstr_is( info.szCipherSuite, suite_str ))
    {
        unsigned int i;
        printf( "  suite string: " );
        for (i = 0; info.szCipherSuite[i]; i++) putchar( info.szCipherSuite[i] );
        printf( "\n" );
    }
    CHECK_EQ( CALL( get_key_signature_algorithm, &sp ), CALG_RSA_SIGN );
    CHECK_EQ( CALL( get_session_cipher_block_size, &sp ), 16 );
    CHECK_EQ( CALL( get_max_message_size, &sp ), 16384 );
}

static void tls_unique( struct side *sd, unsigned char *out )
{
    ULONG size = 0;
    struct get_unique_channel_binding_params p = { sd->session, NULL, &size };

    CHECK_EQ( CALL( get_unique_channel_binding, &p ), SEC_E_BUFFER_TOO_SMALL );
    CHECK_EQ( size, 12 );
    p.buffer = out;
    CHECK_EQ( CALL( get_unique_channel_binding, &p ), SEC_E_OK );
}

static struct side client, server;

/* TLS 1.2, ECDHE_RSA_AES_128_GCM: info, ALPN, SNI, peer chain, flight + app record in one
 * buffer, send of 1 and 16384 bytes, tls-unique, renegotiation, close_notify */
static void test_tls12_gcm( BOOL client_wow )
{
    schan_credentials cc, sc;
    unsigned char out[4096], buf[20000], cu[12], su[12], cu2[12], big[16384];
    SecPkgContext_ApplicationProtocol proto;
    struct get_application_protocol_params ap;
    struct get_session_peer_certificate_params pp;
    ULONG produced, size, count, got, rec_len, *sizes;
    unsigned int i;

    make_creds( &cc, SECPKG_CRED_OUTBOUND, SP_PROT_TLS1_2_CLIENT, FALSE );
    make_creds( &sc, SECPKG_CRED_INBOUND, SP_PROT_TLS1_0_SERVER | SP_PROT_TLS1_1_SERVER | SP_PROT_TLS1_2_SERVER, TRUE );
    memset( &client, 0, sizeof(client) );
    memset( &server, 0, sizeof(server) );
    client.session = create( &cc );
    server.session = create( &sc );
    client.wow = client_wow;
    set_target( client.session, "server.test" );
    set_alpn( client.session, alpn_client, sizeof(alpn_client) );
    set_alpn( server.session, alpn_server, sizeof(alpn_server) );

    /* before a handshake */
    {
        struct get_unique_channel_binding_params up = { client.session, cu, &size };
        size = 12;
        CHECK_EQ( CALL( get_unique_channel_binding, &up ), SEC_E_INTERNAL_ERROR );
    }

    /* drive the handshake by hand to the server's last flight, then add an application record */
    CHECK_EQ( do_handshake( &client, out, sizeof(out), &produced, 0, 0, 0 ), SEC_I_CONTINUE_NEEDED );
    memcpy( server.in, out, produced ); server.in_len = produced;                     /* ClientHello */
    CHECK_EQ( do_handshake( &server, out, sizeof(out), &produced, 0, 0, 0 ), SEC_I_CONTINUE_NEEDED );
    CHECK_EQ( server.in_len, 0 );
    memcpy( client.in, out, produced ); client.in_len = produced;                     /* ServerHello..Done */
    CHECK_EQ( do_handshake( &client, out, sizeof(out), &produced, 0, 0, 0 ), SEC_I_CONTINUE_NEEDED );
    CHECK_EQ( client.in_len, 0 );
    memcpy( server.in, out, produced ); server.in_len = produced;                     /* CKE, CCS, Finished */
    CHECK_EQ( do_handshake( &server, out, sizeof(out), &produced, 0, 0, 0 ), SEC_E_OK );
    CHECK_EQ( server.in_len, 0 );
    memcpy( client.in, out, produced ); client.in_len = produced;                     /* CCS, Finished */
    CHECK_EQ( do_send( &server, (const unsigned char *)"early data", 10, 304, client.in + produced, &rec_len ), SEC_E_OK );
    client.in_len += rec_len;
    CHECK_EQ( do_handshake( &client, out, sizeof(out), &produced, 0, 0, 0 ), SEC_E_OK );
    CHECK_EQ( produced, 0 );
    CHECK_EQ( client.in_len, rec_len );        /* input_offset stopped at the record */
    CHECK_EQ( do_recv( &client, client.in, client.in_len, buf, sizeof(buf), &got ), SEC_E_OK );
    CHECK( got == 10 && !memcmp( buf, "early data", 10 ) );
    client.in_len = 0;

    check_info( &client, SP_PROT_TLS1_2_CLIENT, 0x303, CALG_AES_128, 128, CALG_SHA_256, 0, "", CALG_ECDH_EPHEM,
                "ECDH", "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA384" );
    check_info( &server, SP_PROT_TLS1_2_SERVER, 0x303, CALG_AES_128, 128, CALG_SHA_256, 0, "", CALG_ECDH_EPHEM,
                "ECDH", "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA384" );

    /* ALPN h2 on both sides */
    ap.session = client.session; ap.protocol = &proto;
    CHECK_EQ( CALL( get_application_protocol, &ap ), SEC_E_OK );
    CHECK( proto.ProtoNegoStatus == SecApplicationProtocolNegotiationStatus_Success && proto.ProtocolIdSize == 2 &&
           !memcmp( proto.ProtocolId, "h2", 2 ) );
    ap.session = server.session;
    CHECK_EQ( CALL( get_application_protocol, &ap ), SEC_E_OK );
    CHECK( proto.ProtocolIdSize == 2 && !memcmp( proto.ProtocolId, "h2", 2 ) );

    /* SNI seen by the server */
    CHECK( !strcmp( br_ssl_engine_get_server_name( bear( server.session )->eng ), "server.test" ) );

    /* the peer chain in upstream's layout */
    size = 0;
    pp.session = client.session; pp.buffer = NULL; pp.bufsize = &size; pp.retcount = &count;
    CHECK_EQ( CALL( get_session_peer_certificate, &pp ), SEC_E_BUFFER_TOO_SMALL );
    CHECK_EQ( size, sizeof(ULONG) + cert_len );
    pp.buffer = buf;
    CHECK_EQ( CALL( get_session_peer_certificate, &pp ), SEC_E_OK );
    sizes = (ULONG *)buf;
    CHECK( count == 1 && sizes[0] == cert_len && !memcmp( buf + sizeof(ULONG), cert_der, cert_len ) );
    pp.session = server.session;
    CHECK_EQ( CALL( get_session_peer_certificate, &pp ), SEC_E_INTERNAL_ERROR );   /* no client certificate */

    /* tls-unique: 12 bytes, equal on both sides */
    tls_unique( &client, cu );
    tls_unique( &server, su );
    CHECK( !memcmp( cu, su, 12 ) );

    /* send 1 and 16384 bytes each way */
    for (i = 0; i < sizeof(big); i++) big[i] = i * 31 + 7;
    CHECK( transfer( &client, &server, big, 1 ) );
    CHECK( transfer( &client, &server, big, 16384 ) );
    CHECK( transfer( &server, &client, big, 16384 ) );
    CHECK( transfer( &server, &client, big + 5, 1 ) );

    /* the server asks for a new handshake: the client's recv says SEC_I_RENEGOTIATE, then the
     * handshake calls complete it */
    CHECK( br_ssl_engine_renegotiate( bear( server.session )->eng ) );
    /* HelloRequest out; the server then waits for the ClientHello, no application data meanwhile */
    CHECK_EQ( do_handshake( &server, out, sizeof(out), &produced, 0, 0, 0 ), SEC_I_CONTINUE_NEEDED );
    CHECK( produced > 0 );
    CHECK_EQ( do_recv( &client, out, produced, buf, sizeof(buf), &got ), SEC_I_RENEGOTIATE );
    CHECK_EQ( got, 0 );
    CHECK_EQ( do_handshake( &client, out, sizeof(out), &produced, 0, 0, 0 ), SEC_I_CONTINUE_NEEDED );  /* ClientHello */
    CHECK( produced > 0 );
    /* the server gets the ClientHello through DecryptMessage too */
    CHECK_EQ( do_recv( &server, out, produced, buf, sizeof(buf), &got ), SEC_I_RENEGOTIATE );
    CHECK( handshake_pair( &client, &server, sizeof(out), sizeof(out) ) );
    tls_unique( &client, cu2 );
    tls_unique( &server, su );
    CHECK( !memcmp( cu2, su, 12 ) && memcmp( cu2, cu, 12 ) );
    CHECK( transfer( &client, &server, big, 1000 ) );
    CHECK( transfer( &server, &client, big, 1000 ) );

    /* close_notify by control token, received by the peer as the end of the stream */
    CHECK_EQ( do_handshake( &client, out, sizeof(out), &produced, CONTROL_TOKEN_SHUTDOWN, TLS1_ALERT_WARNING,
                            TLS1_ALERT_CLOSE_NOTIFY ), SEC_E_OK );
    CHECK( produced > 0 );
    CHECK_EQ( do_recv( &server, out, produced, buf, sizeof(buf), &got ), SEC_E_OK );
    CHECK_EQ( got, 0 );

    dispose( client.session );
    dispose( server.session );
    free_creds( &cc );
    free_creds( &sc );
}

/* a fatal alert sent by control token and reported by the peer */
static void test_fatal_alert( void )
{
    schan_credentials cc, sc;
    unsigned char out[4096], buf[64];
    ULONG produced, got;

    make_creds( &cc, SECPKG_CRED_OUTBOUND, SP_PROT_TLS1_2_CLIENT, FALSE );
    make_creds( &sc, SECPKG_CRED_INBOUND, SP_PROT_TLS1_2_SERVER, TRUE );
    memset( &client, 0, sizeof(client) );
    memset( &server, 0, sizeof(server) );
    client.session = create( &cc );
    server.session = create( &sc );
    CHECK( handshake_pair( &client, &server, sizeof(out), sizeof(out) ) );

    CHECK_EQ( do_handshake( &client, out, sizeof(out), &produced, CONTROL_TOKEN_ALERT, TLS1_ALERT_FATAL,
                            TLS1_ALERT_BAD_CERTIFICATE ), SEC_E_OK );
    CHECK( produced > 5 && out[0] == 21 );
    CHECK_EQ( br_ssl_engine_last_error( bear( client.session )->eng ), BR_ERR_SEND_FATAL_ALERT + 42 );
    CHECK_EQ( do_recv( &server, out, produced, buf, sizeof(buf), &got ), SEC_E_INTERNAL_ERROR );
    CHECK_EQ( br_ssl_engine_last_error( bear( server.session )->eng ), BR_ERR_RECV_FATAL_ALERT + 42 );

    /* a warning other than close_notify: nothing is sent */
    dispose( client.session );
    client.session = create( &cc );
    CHECK_EQ( do_handshake( &client, out, sizeof(out), &produced, CONTROL_TOKEN_ALERT, TLS1_ALERT_WARNING,
                            TLS1_ALERT_USER_CANCELED ), SEC_E_OK );
    CHECK_EQ( produced, 0 );

    dispose( client.session );
    dispose( server.session );
    free_creds( &cc );
    free_creds( &sc );
}

/* output buffers too small: SEC_I_CONTINUE_NEEDED, the rest on the next call */
static void test_small_output( void )
{
    schan_credentials cc, sc;
    unsigned char out[64];
    ULONG produced;

    make_creds( &cc, SECPKG_CRED_OUTBOUND, SP_PROT_TLS1_2_CLIENT, FALSE );
    make_creds( &sc, SECPKG_CRED_INBOUND, SP_PROT_TLS1_2_SERVER, TRUE );
    memset( &client, 0, sizeof(client) );
    memset( &server, 0, sizeof(server) );
    client.session = create( &cc );
    server.session = create( &sc );

    CHECK_EQ( do_handshake( &client, out, 50, &produced, 0, 0, 0 ), SEC_I_CONTINUE_NEEDED );
    CHECK_EQ( produced, 50 );
    memcpy( server.in, out, produced );
    server.in_len = produced;
    CHECK( handshake_pair( &client, &server, 50, 64 ) );
    CHECK( transfer( &client, &server, (const unsigned char *)"after", 5 ) );

    dispose( client.session );
    dispose( server.session );
    free_creds( &cc );
    free_creds( &sc );
}

/* TLS 1.0 with RSA_AES_128_CBC_SHA, and TLS 1.2 with ECDHE_RSA_AES_256_CBC_SHA384 */
static void test_cbc_suites( void )
{
    static const uint16_t rsa_cbc[] = { BR_TLS_RSA_WITH_AES_128_CBC_SHA };
    static const uint16_t ecdhe_cbc384[] = { BR_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384 };
    schan_credentials cc, sc;
    unsigned char big[3000];
    unsigned int i;

    for (i = 0; i < sizeof(big); i++) big[i] = i ^ 0x5a;

    make_creds( &cc, SECPKG_CRED_OUTBOUND, SP_PROT_TLS1_0_CLIENT, FALSE );
    make_creds( &sc, SECPKG_CRED_INBOUND, SP_PROT_TLS1_0_SERVER | SP_PROT_TLS1_2_SERVER, TRUE );
    memset( &client, 0, sizeof(client) );
    memset( &server, 0, sizeof(server) );
    client.session = create( &cc );
    server.session = create( &sc );
    br_ssl_engine_set_suites( bear( server.session )->eng, rsa_cbc, 1 );
    CHECK( handshake_pair( &client, &server, 4096, 4096 ) );
    check_info( &client, SP_PROT_TLS1_0_CLIENT, 0x301, CALG_AES_128, 128, CALG_SHA1, 160, "SHA1", CALG_RSA_KEYX,
                "RSA", "TLS_RSA_RSA_WITH_AES_128_CBC_SHA" );
    CHECK( transfer( &client, &server, big, 1 ) );
    CHECK( transfer( &client, &server, big, 3000 ) );       /* 1/n-1 split: two records */
    CHECK( transfer( &server, &client, big, 3000 ) );
    dispose( client.session );
    dispose( server.session );
    free_creds( &cc );

    make_creds( &cc, SECPKG_CRED_OUTBOUND, SP_PROT_TLS1_1_CLIENT | SP_PROT_TLS1_2_CLIENT, FALSE );
    client.session = create( &cc );
    server.session = create( &sc );
    br_ssl_engine_set_suites( bear( server.session )->eng, ecdhe_cbc384, 1 );
    CHECK( handshake_pair( &client, &server, 4096, 4096 ) );
    check_info( &client, SP_PROT_TLS1_2_CLIENT, 0x303, CALG_AES_256, 256, CALG_SHA_384, 384, "SHA384",
                CALG_ECDH_EPHEM, "ECDH", "TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA384" );
    CHECK( transfer( &server, &client, big, 3000 ) );
    dispose( client.session );
    dispose( server.session );
    free_creds( &cc );
    free_creds( &sc );
}

static void test_protocols( void )
{
    schan_credentials c;
    struct create_session_params p;
    schan_session s = 1;
    struct set_dtls_mtu_params mp = { 0, 1200 };

    CHECK_EQ( CALL( process_attach, NULL ), STATUS_SUCCESS );
    CHECK_EQ( CALL32( process_attach, NULL ), STATUS_SUCCESS );
    CHECK_EQ( CALL( get_enabled_protocols, NULL ), SP_PROT_TLS1_0 | SP_PROT_TLS1_1 | SP_PROT_TLS1_2 );
    /* nothing this side can use (DTLS only, or the other side's flags) */
    make_creds( &c, SECPKG_CRED_OUTBOUND, SP_PROT_DTLS1_2_CLIENT | SP_PROT_TLS1_2_SERVER, FALSE );
    p.cred = &c;
    p.session = &s;
    CHECK_EQ( CALL( create_session, &p ), STATUS_INTERNAL_ERROR );
    CHECK_EQ( s, 0 );
    free_creds( &c );
    CHECK_EQ( CALL( set_dtls_mtu, &mp ), SEC_E_OK );

    /* a server without a certificate fails its first handshake call */
    make_creds( &c, SECPKG_CRED_INBOUND, SP_PROT_TLS1_2_SERVER, FALSE );
    memset( &server, 0, sizeof(server) );
    server.session = create( &c );
    {
        unsigned char out[64];
        ULONG produced;
        CHECK_EQ( do_handshake( &server, out, sizeof(out), &produced, 0, 0, 0 ), SEC_E_INTERNAL_ERROR );
    }
    dispose( server.session );
    free_creds( &c );
}

int main( void )
{
    cert_len = unhex( fx_srv_cert, cert_der, sizeof(cert_der) );

    test_protocols();
    test_tls12_gcm( FALSE );
    test_tls12_gcm( TRUE );         /* the client through the 32-bit thunks, B != 0 */
    test_fatal_alert();
    test_small_output();
    test_cbc_suites();
    CHECK_EQ( fake_ac_live(), 0 );
    return ios_test_done( "test_schannel" );
}
