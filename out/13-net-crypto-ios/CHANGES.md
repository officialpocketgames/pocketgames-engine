# Spec 13: what changed, per Part

Networking and crypto unix sides for iOS: ws2_32, dnsapi, nsi, bcrypt, secur32 and crypt32.
Since clarification round 3 the crypto runs on Apple's public frameworks (Security.framework,
CommonCrypto), BearSSL (MIT, commit 7bea48e) and Wine's own LibTomCrypt 1.18.2 / LibTomMath copy;
GnuTLS, nettle and GMP are gone from the part and its build. Base: Wine 11.4 (`refs/wine-11.4`).
The iOS code can't be built on the machine it was written on (Windows, no Xcode); the host tests
in `tests/` run in WSL and CI, the iOS scripts in CI on macOS.

## Layout

| Path | What |
|---|---|
| `build-crypto-deps-ios.sh` | Clarification 7.1: `libbearssl.a` and `libtomcrypt_unix.a` for iOS arm64 |
| `build-ios.sh` | Part A2–A6, clarification 7.2: `libnetcrypto_unix.a`, the two archives next to it, `LINK.txt` |
| `check-symbols.sh` | Part A3/A4 and clarification 14 checks for CI (clashes, the twelve tables in the app, forbidden imports of the third-party archives) |
| `include/` | headers of our own: per-file configuration (A6), Part B, F2, G2, H2, I helpers, `net/route.h` (I5), the Apple layer (`ios_applecrypto.h`), DER (`ios_der.h`), LibTomCrypt (`ios_ltc.h`, `ios_tomcrypt_rename.h`) |
| `common/` | sources of our own shared by the DLLs (the Apple layer, DER, LibTomCrypt glue, F2, H2, I pure logic) |
| `bcrypt/unixlib_ios.c`, `secur32/schannel_bearssl.c`, `crypt32/unixlib.c`, `ws2_32/`, `dnsapi/` | modified copies of the upstream unix sides |
| `nsi/` | nsi.dll's unix-call table (Part I): dispatcher, TCP, scope table, modified `ndis.c` |
| `ntdll-unix/` | modified `socket.c` (Part G3) and its pure helper header, for spec 09's library |
| `tests/` | host tests (Part K, clarification 15), the fake of the Apple layer, fixtures |

## Part A: build

- **A1** (clarification 7.1) `build-crypto-deps-ios.sh <prefix> [<wine-11.4 tree>]` builds two
  static libraries for iOS arm64 (`xcrun -sdk iphoneos clang -arch arm64
  -miphoneos-version-min=17.0 -O2 -fPIC`) under `<prefix>`:
  - `lib/libbearssl.a`: BearSSL cloned from `https://www.bearssl.org/git/BearSSL` into
    `<prefix>/.src/BearSSL` (a clone there is reused), checked out at
    `7bea48e5e850ab4cafbe68d3765cdaba13a86d6f`, refused unless `git rev-parse HEAD` prints exactly
    that; every `src/**/*.c` with `-Iinc -Isrc -DBR_USE_GETENTROPY=0 -DBR_USE_URANDOM=0`. No
    Makefile, no `T0Comp.exe`. Headers and `LICENSE.txt` in `include/bearssl/`.
  - `lib/libtomcrypt_unix.a`: a copy of the Wine tree's `libs/tomcrypt/` in `<prefix>/.src/`,
    built with Wine's defines minus `-D_ACRTIMP=` (`-DLTC_NO_PROTOTYPES -DLTC_SOURCE
    -DLTC_NO_TEST -DMP_32BIT=`) and the A4 rename header force-included. The script names the
    86 files: `mpi.c`, the SHA-1/SHA-2 hashes, `ltm_desc.c`, `multi.c`, `rand_bn.c`,
    `rand_prime.c`, `src/pk/dh/*`, `src/pk/dsa/*`, and the `der_*`, `crypt_*`, `zeromem`,
    `ltc_ecc_*` and `rsa_*` files they reference (the closure, computed by compiling Wine's
    tomcrypt and resolving undefined names). The build fails if the archive leaves an `ltc_`,
    `mp_`, `s_mp_` or `der_` name undefined. `hash_memory`, which `dsa_generate_pqg` calls and
    Wine's copy lacks, is defined in `common/ios_ltc.c`. Headers and `LICENSE` in
    `include/tomcrypt/`.
  A prefix or work path inside `refs/` is refused; stamps `<prefix>/.stamp-{bearssl,tomcrypt}`
  make reruns skip finished libraries; the archives built are printed at the end.
  `--print-tomcrypt-files` prints the list (`tests/host-deps.sh` builds the same for the host).
- **A2** (clarification 7.2) `build-ios.sh <wine tree> <wine build dir> <out dir> <deps
  prefix>` compiles with `-arch arm64 -miphoneos-version-min=17.0 -O2 -std=gnu11 -fPIC
  -ffixed-x18` (the flags of spec 09's ntdll build), `-DWINE_IOS=1 -DWINE_UNIX_LIB
  -D__WINESRC__`, and force-includes, in this order, the wine build dir's `config.h`, spec 09's
  `ios_noexit.h` (from `out/09-ntdll-processes/ntdll-unix/`, or `NOEXIT_H`), then the per-file
  header. Sources: `ws2_32/unixlib.c`, `dnsapi/libresolv.c`, `bcrypt/unixlib_ios.c`,
  `secur32/schannel_bearssl.c`, `crypt32/unixlib.c` (these three with `-I<deps>/include` for
  `bearssl/bearssl.h`), the NSI side (`nsi/*.c` plus upstream `dlls/nsiproxy.sys/ip.c`
  unchanged), `common/ios_applecrypto.c`, `ios_der.c`, `ios_ltc.c` (with LibTomCrypt's headers,
  defines and the rename header) and the other `common/*.c`, into `libnetcrypto_unix.a`.
  `libbearssl.a` and `libtomcrypt_unix.a` are copied next to it, unmerged, and
  `check-symbols.sh imports` must pass on both (clarification 14).
- **A3** Each file is compiled with `-D__wine_unix_call_funcs=X_unix_call_funcs
  -D__wine_unix_call_wow64_funcs=X_unix_call_wow64_funcs` for X in ws2_32, dnsapi, bcrypt,
  secur32, crypt32, nsi. `wine/unixlib.h` declares both names `DECLSPEC_EXPORT` (default
  visibility), and the macro renames that declaration too, so the tables stay default-visible
  whatever else is hidden. Nothing in the app references the tables (spec 09 finds them with
  `dlsym`), so the final link must name each with `-Wl,-u,_X_unix_call_funcs` or the linker
  never loads their objects from the archive and dead-strips them: `LINK.txt` lists the twelve
  flags. `check-symbols.sh tables <app>` is the CI check that all twelve are in the binary.
- **A4** Name clashes: everything of the upstream-derived DLL files is `static` except the
  tables; the NSI providers' globals (`nsi_enumerate_all_ex`, `convert_*`, the `*_module`
  objects, the `NPI_MS_*_MODULEID` objects, the scope-table functions) get an `ios_nsi_` prefix
  through `include/ios_nsi_rename.h`. Our own globals all start with `ios_`. BearSSL's names
  start with `br_`; LibTomCrypt's and LibTomMath's (`mp_*`, `ltc_*`, `der_*`, `dh_*`, `dsa_*`,
  `sha1_*`, `crypt_*`, …) can be renamed in `include/ios_tomcrypt_rename.h`, force-included when
  `libtomcrypt_unix.a` is built and included by `common/ios_ltc.c` before `<tomcrypt.h>`; it is
  empty until CI reports a clash. `check-symbols.sh clashes libnetcrypto_unix.a
  libntdll_unix.a libwin32u_unix.a <server lib>` checks our archive and the two next to it.
- **A5** (clarification 7.3) `build-ios.sh` writes `LINK.txt`: the archives in link order
  (`libnetcrypto_unix.a libbearssl.a libtomcrypt_unix.a`), the twelve `-u` flags, `-framework
  Security`, `-framework CoreFoundation` (CommonCrypto is in libSystem), `-lobjc` (G2),
  `-lresolv` (clarification 3), `-liconv` / `-lz` only if something references them, then every
  other undefined symbol as a comment (provided by ntdll's unix side or libSystem).
- **A6** Per-file headers, force-included after `config.h` (which has an include guard, so the
  files' own `#include "config.h"` adds nothing): `include/ios_cfg_dnsapi.h` (H1),
  `include/ios_cfg_nsi.h` (I5). The crypto files need none any more (the GnuTLS one is gone). The
  build dir's `config.h` is never edited.
- **A7** (clarification 14) Licences: `PROVENANCE.md`.

## Clarification round 3: what changed against upstream's GnuTLS behaviour

Only these, each one named by clarifications 5–14 or forced by them:

1. **bcrypt RSA `BCRYPT_PAD_NONE`** is raw RSA (input zero-padded on the left to the modulus
   length; input not below the modulus, or longer than it, → `STATUS_INVALID_PARAMETER`).
   Upstream's GnuTLS path fell back to PKCS #1 v1.5.
2. **bcrypt RSA OAEP without a label** is OAEP. Upstream used OAEP only when a label set
   GnuTLS's parameters and otherwise fell back to PKCS #1 v1.5.
3. **bcrypt DH generated without parameters** uses the RFC 3526 / RFC 7296 MODP group of exactly
   that size (768, 1024, 1536, 2048, 3072, 4096, 6144, 8192 bits), else p and g from
   LibTomCrypt's `dsa_generate_pqg` (modulus bitlen/8 bytes, group 32 bytes). Upstream generated
   fresh parameters.
4. **bcrypt CAPI DSA public import** reads `y` at the key's size; upstream read 128 bytes
   whatever the size (QUESTIONS.md, Spec 13 entry 7).
5. **bcrypt ECDSA verify** builds the DER signature from minimal integers (leading zero bytes
   of r and s dropped); upstream passed both halves at full width.
6. **bcrypt RSA generate**: Security first; a size it refuses goes to BearSSL (e = 65537, 512–4096
   bits, logged once), above that `STATUS_NOT_SUPPORTED`.
7. **crypt32 PFX**: the password reaches `SecPKCS12Import` as the caller's UTF-16 string
   (upstream squeezed it to ASCII); `PRIVATEKEYBLOB.pubexp` is the key's exponent (upstream
   wrote 65537 always); a part shorter than its field is zero-padded (upstream left the rest of
   the blob unwritten), one too long gives `STATUS_INVALID_PARAMETER`.
8. **crypt32 roots** (clarification 12): only the bundle's certificates the device trusts as
   anchors (fallback to the whole bundle when fewer than half pass; `MADEIRA_CA_FILTER=0`);
   no macOS trust-settings store and no Linux known-locations fallback.
9. **Schannel**:
   - DTLS is dropped: `get_enabled_protocols` reports TLS 1.0–1.2 only, so
     `AcquireCredentialsHandle` for DTLS fails with `SEC_E_ALGORITHM_MISMATCH` through the PE
     side's own path; `set_dtls_mtu` / `set_dtls_timeouts` do nothing. No SSL 3, no TLS 1.3
     (Wine 11.4's Schannel never enables it).
   - Suites: the 18 of clarification 10.4 only (no ChaCha20, CCM, 3DES, static ECDH, DHE).
     RSA key exchange shows "RSA" where upstream printed `<unknown>`.
   - BearSSL ignores warning alerts other than close_notify; upstream ignored only
     unrecognized_name and failed on the rest. A warning other than close_notify can't be sent
     (`SEC_E_OK`, nothing sent, logged once).
   - A fatal alert by control token is put on the wire through the engine's record encryptor
     (QUESTIONS.md, Spec 13 entry 6).
   - The version is reported as `…_SERVER` for an inbound session (upstream always `…_CLIENT`).
   - A peer chain over 16 certificates or 64 KB fails the handshake.
   - A session's TLS range is the credential's versions for its side; a gap widens to the whole
     range (logged once); none → `create_session` returns `STATUS_INTERNAL_ERROR`.

**Export compliance (for the owner):** the app contained TLS before, through GnuTLS; it now
contains it through BearSSL and Apple's frameworks. What the app does with cryptography is the
same, so the swap shouldn't change what is declared, but the owner should know the library
changed.

**App size:** before and after this change, from device check 13 (`TESTS.md`): *not measured
yet*.

## Part B: pointers in 32-bit unix calls

`include/ios_guest.h` holds both conversions, used by every 32-bit thunk of this spec:
`ios_g2h(guest)` = `B + guest` (0 stays NULL) replaces `ULongToPtr` on a guest address, and
`ios_h2g(host)` = `ios_wow_guest_addr(host)` (NULL stays 0) replaces `PtrToUlong` on a host
pointer written back for the program. `B = ios_wow_base()` of the calling thread; with `B = 0`
both are upstream's conversions. The pure forms `ios_guest_to_host_at()` /
`ios_host_to_guest_at()` / `ios_guest_args_is_null()` take `B` explicitly and are what
`tests/test_guest.c` checks. Handles (`cert_store_data_t`, `schan_session`, key private data,
`ULongToHandle( params->file )`), sizes and flags pass unchanged.

The converted sites of each file are listed under that file's Part (D, E, F, G, H, I). In every
file of this spec no `ULongToPtr` / `PtrToUlong` on a guest address is left in the WoW64 code:
`grep -n "ULongToPtr\|PtrToUlong"` over `bcrypt/ secur32/ crypt32/ ws2_32/ dnsapi/ nsi/` finds
none, and in `ntdll-unix/socket.c` none in a 32-bit path (G3).

## Part C (replaced by clarification 13): attach and detach

No `dlopen` / `dlsym` is left in the three crypto files; the GnuTLS name table, its generator and
the wrappers are gone. Each DLL's `process_attach` runs the one-time set-up of what it uses and
returns `STATUS_SUCCESS`, the first time and every later time; `process_detach` releases nothing:

- bcrypt: an init state set by compare-and-swap (0 → 1 → 2; a concurrent caller waits for 2 with
  `sched_yield`, no `pthread_once`: spec 09's no-exit rule), which runs `ios_ltc_init()`
  (`ltc_mp = ltm_desc`, SHA-1/256/384/512 registered, the PRNG descriptor of clarification 8.7),
  logs `[bcrypt-ios] bcrypt: Security+CommonCrypto, BearSSL 7bea48e, LibTomCrypt 1.18.2`, and
  runs the self-test (Part D).
- secur32: logs `[secur32-ios] secur32: BearSSL 7bea48e (TLS 1.0-1.2)` once; nothing to set up.
- crypt32: logs `[crypt32-ios] crypt32: Security (PKCS #12 import, anchor trust), BearSSL
  7bea48e (SHA-256)` once; nothing to set up.

## The Apple layer (clarifications 9 and 15)

Every Security / CommonCrypto call goes through `include/ios_applecrypto.h`:
`common/ios_applecrypto.c` on iOS, `tests/fake_applecrypto.c` (on BearSSL) in the host tests.
Cryptors (`CCCryptorCreateWithMode` with `ccNoPadding`, `CCCryptorUpdate`, `CCCryptorRelease`),
keys (`SecKeyCreateWithData` from PKCS #1 DER or X9.63 with `kSecAttrKeyType` and
`kSecAttrKeyClass` only, never `kSecAttrIsPermanent`; `SecKeyCreateRandomKey`;
`SecKeyCopyExternalRepresentation`; `SecKeyCopyAttributes`), encrypt / decrypt / sign / verify by
`SecKeyAlgorithm`, `SecKeyCopyKeyExchangeResult`, `SecRandomCopyBytes`, the anchor check
(`SecCertificateCreateWithData`, `SecPolicyCreateBasicX509`, `SecTrustCreateWithCertificates`,
`SecTrustSetNetworkFetchAllowed(false)`, `SecTrustEvaluateWithError`) and `SecPKCS12Import`. No
keychain item is created, read or deleted; nothing needs an entitlement; nothing touches the
network. Every CF object is released on every path: the fake counts live objects and every test
ends with none.

## Part D: bcrypt (`bcrypt/unixlib_ios.c`)

Starts from upstream `dlls/bcrypt/gnutls.c` (LGPL header kept, marked as changed). Every argument
check, status code, size rule, BCRYPT / CAPI blob layout, the unix-call table and its order and
the WoW64 thunks are upstream's; only what called GnuTLS is replaced (clarification 8):

- **Key state** (8.2): `key->private[0]` points to a `struct key_impl`: for a symmetric key the
  CommonCrypto cryptor with the chain state (last ciphertext block for CBC, last 16 ciphertext
  bytes for CFB8, kept so a change of direction continues the chain, as one GnuTLS handle did)
  or the BearSSL GCM context; for an asymmetric key the canonical material of 8.3 (big-endian
  RSA `n e d p q dp dq qinv`, EC `X Y [D]`, X25519 public and private, DH `p g y [x]`, DSA
  `p q g y [x]`) plus the `SecKeyRef`s made from it on first use. `destroy` frees everything;
  `duplicate` copies the material and shares the `SecKeyRef`s (`CFRetain`).
- **Routes** (8.4): AES-CBC/ECB/CFB8 and 3DES-CBC on CommonCrypto; AES-GCM on BearSSL
  `br_gcm_*` over `br_aes_ct64_ctr` (`get_tag` works on a copy of the context, so it can be
  asked again); RSA on SecKey (PKCS #1 v1.5, OAEP SHA-1/2 without a label, raw, PKCS1v15Raw over
  the RFC 8017 §9.2 DigestInfo for MD5/MD2 and over the input for a NULL algorithm id, PSS with
  salt = hash length), on BearSSL for OAEP with a label or MD5, PSS with another salt or MD5 (a
  key over 4096 bits there: `STATUS_NOT_SUPPORTED`); `RSAPRIVATE` completed with LibTomMath
  (`common/ios_ltc.c`); ECDSA / ECDH on the P-curves via SecKey (X9.63; DER ↔ r ‖ s in
  `common/ios_der.c` and upstream's `buffer_append_asn1_r_s`); X25519 on BearSSL's
  `br_ec_c25519_m31` (its `mul` takes the scalar big-endian, so the RFC 7748 private key is
  reversed for it; an all-zero secret → `STATUS_INTERNAL_ERROR`); DH and DSA on LibTomCrypt.
- **Self-test** (8.6): the first attach runs AES-128-CBC (SP 800-38A F.2.1), AES-128-CFB8
  (F.3.7), AES-128-GCM (McGrew–Viega test case 4), ECDH P-256 (RFC 5903 §8.1), X25519 (RFC 7748
  §5.2), an ECDSA P-256 sign-then-verify and an RSA-2048 PKCS #1 SHA-256 signature from a fixed
  key imported as `RSAPRIVATE` (so the LibTomMath completion is checked too) through the same
  entries, and logs `[bcrypt-ios] bcrypt self-test: 7/7 passed` (failures by name). A failure
  doesn't fail the attach.
- **Randomness** (8.7): LibTomCrypt's PRNG descriptor `ios-secrandom` reads
  `SecRandomCopyBytes`; BearSSL key generation, OAEP and PSS get an HMAC-DRBG (SHA-256) seeded
  with 32 fresh bytes per call. A `SecRandomCopyBytes` failure → `STATUS_INTERNAL_ERROR`.
- **B** 46 guest addresses → `ios_g2h` in the WoW64 section, the same sites as before:
  - `get_padding`: `oaep.pszAlgId`, `oaep.pbLabel`, `pkcs1.pszAlgId`, `pss.pszAlgId`;
  - `get_symmetric_key`: `key32->u.s.vector`, `key32->u.s.secret`;
  - `wow64_key_symmetric_set_auth_data`: `key`, `auth_data`;
  - `wow64_key_symmetric_encrypt` / `_decrypt`: `key`, `input`, `output` (each);
  - `wow64_key_symmetric_get_tag`: `key`, `tag`;
  - `wow64_key_asymmetric_decrypt` / `_encrypt`: `key`, `input`, `padding`, `output`, `ret_len`
    (each);
  - `wow64_key_asymmetric_duplicate`: `key_orig`, `key_copy`;
  - `wow64_key_asymmetric_sign`: `key`, `padding`, `input`, `output`, `ret_len`;
  - `wow64_key_asymmetric_verify`: `key`, `padding`, `hash`, `signature`;
  - `wow64_key_asymmetric_export`: `key`, `buf`, `ret_len`;
  - `wow64_key_asymmetric_import`: `key`, `buf`;
  - `wow64_key_asymmetric_derive_key`: `privkey`, `pubkey`, `output`, `ret_len`.

  The entries that receive the `key32` itself as `args` get it from the WoW64 layer, already
  converted. `key->private` (a host pointer to our key state) and `dss_seed`, sizes and flags
  are copied as upstream does. Nothing is written back as a pointer, so there is no `ios_h2g`.

## Part E: secur32 / Schannel (`secur32/schannel_bearssl.c`)

Starts from upstream `dlls/secur32/schannel_gnutls.c` (LGPL header kept, marked as changed). Kept:
the unix-call table, the SecBuffer walk (`init_schan_buffers`, `get_next_buffer`, `get_buffer`,
the input limit), the alert-number mapping, ALPN list parsing, the info layouts, the WoW64
thunks. Clarification 10:

- **Credentials**: an object holding the certificate DER (`X509_ASN_ENCODING` only) and the RSA
  key from the CAPI `PRIVATEKEYBLOB` (upstream's `get_x509_key` layout) as a
  `br_rsa_private_key`.
- **Session**: a `br_ssl_client_context` (outbound; `br_ssl_client_init_full`, then
  `br_ssl_client_set_single_rsa` with a client certificate) or `br_ssl_server_context`
  (inbound; `br_ssl_server_init_full_rsa`), a `BR_SSL_BUFSIZE_BIDI` buffer of its own, the
  suites of 10.4 in order, the versions of 10.5, an X.509 class of our own (10.6: copies the
  peer chain, leaf first, decodes the leaf's key, accepts the chain), 32 bytes of
  `SecRandomCopyBytes`. The engine is reset at the first handshake call (`br_ssl_client_reset(cc,
  target, 0)`: never resumed; `br_ssl_server_reset`), after SNI and ALPN were set. A server
  session without a certificate fails that call.
- **Handshake / send / recv** (10.7–10.9): input fed never beyond what
  `br_ssl_engine_recvrec_buf` asks for (so `input_offset` stops exactly after the records used);
  `SENDREC` drained into the output buffers, `SEC_I_CONTINUE_NEEDED` with the rest kept in the
  engine when out of room; `SENDAPP`/`RECVAPP` → `SEC_E_OK`; closed → `SEC_E_INTERNAL_ERROR`.
  `recv` delivers application data up to `*length`; a peer close_notify ends with `SEC_E_OK`;
  a handshake record after the handshake with records to send and no data delivered →
  `SEC_I_RENEGOTIATE` (the next handshake call completes it; `BR_OPT_NO_RENEGOTIATION` off).
  Control tokens: shutdown or a close_notify warning → `br_ssl_engine_close` and drain; a fatal
  alert → the alert record, then `br_ssl_engine_fail(cc, BR_ERR_SEND_FATAL_ALERT + n)`.
- **Info**: the table of 10.4 (`get_connection_info`, `get_cipher_info` with upstream's
  `szCipherSuite` builder, `get_key_signature_algorithm`); block size 16, max message 16384;
  tls-unique = the first 12 bytes of `saved_finished` (before a handshake:
  `SEC_E_INTERNAL_ERROR`); ALPN from `br_ssl_engine_get_selected_protocol`.
- **Log** (Part E), the first 32 of each kind per app run: `[secur32-ios] TLS handshake
  complete (client|server), version 0x303, suite 0xc02f`, `… TLS fatal alert N received`,
  `… TLS fatal alert N sent`, `… TLS failed, BearSSL error N`.
- **B** 30 guest addresses → `ios_g2h` in the WoW64 section, the same sites as before:
  - `wow64_schan_allocate_certificate_credentials`: `c`, `cert_blob`, `key_blob`;
  - `wow64_schan_create_session`: `cred`, `session`;
  - `wow64_schan_free_certificate_credentials`: `c`;
  - `wow64_schan_get_application_protocol`: `protocol`;
  - `wow64_schan_get_connection_info`, `wow64_schan_get_cipher_info`: `info`;
  - `wow64_schan_get_session_peer_certificate`: `buffer`, `bufsize`, `retcount`;
  - `wow64_schan_get_unique_channel_binding`: `buffer`, `bufsize`;
  - `secbufferdesc_32to64`: the buffer array (`pBuffers + i * sizeof(SecBuffer32)`, guest
    arithmetic first) and each buffer's `pvBuffer`;
  - `wow64_schan_handshake`: the input and output `SecBufferDesc32`, `input_offset`,
    `output_buffer_idx`, `output_offset`;
  - `wow64_schan_recv`: the input `SecBufferDesc32`, `buffer`, `length`;
  - `wow64_schan_send`: the output `SecBufferDesc32`, `buffer`, `output_buffer_idx`,
    `output_offset`;
  - `wow64_schan_set_application_protocols`: `buffer`;
  - `wow64_schan_set_session_target`: `target`.

  `params32->input ? &input : NULL` still tests the raw guest value (0 is NULL in both).
  `schan_session` and `params->c->credentials` are handles. Nothing is written back as a
  pointer.

## Part F: crypt32 (`crypt32/unixlib.c`)

Modified copy of upstream `dlls/crypt32/unixlib.c`.

- **PFX** (clarification 11): `open_cert_store` calls `SecPKCS12Import` with the caller's UTF-16
  password (NULL = empty); any failure → `STATUS_INVALID_PARAMETER`, logged with the
  `OSStatus`; no certificate → `STATUS_INVALID_PARAMETER` (upstream's rule). Certificates: every
  item's chain, in order, duplicates removed. Key: the first identity's, which must be RSA
  (`SecKeyCopyAttributes`), bit length from `kSecAttrKeySizeInBits`; more identities are logged
  once. `import_store_key` exports PKCS #1 and writes upstream's `PRIVATEKEYBLOB` (size rule
  `bitlen * 9 / 16` after the headers); `import_store_cert` copies the DER.
- **F1 and clarification 12**: `MADEIRA_CA_BUNDLE` unset or empty → no roots (`[crypt32-ios]
  MADEIRA_CA_BUNDLE is not set: no root certificates`); not there or not a regular file →
  refused, no roots; otherwise upstream's PEM parser reads it, then each certificate is kept
  only if the device trusts it as an anchor (fewer than half kept → the whole bundle;
  `MADEIRA_CA_FILTER=0` → no filter). One line: `[crypt32-ios] MADEIRA_CA_BUNDLE=<path>: M
  certificates in the bundle, kept N of M roots, dropped D, first SHA-256: <16 hex>…, T ms`
  (or the fallback / filter-off wording). Once per app run, under `ios_roots_lock`, at the first
  enumeration.
- **F2** `enum_root_certs` no longer removes the list's head. After the load the list is kept
  for the whole run and indexed by two arrays (`ios_roots_sizes`, `ios_roots_data`); each call
  takes `ios_roots_lock` and asks `ios_rootenum_next()` (`common/ios_rootenum.c`, pure, rules in
  `include/ios_rootenum.h`) for the calling thread's next certificate: size in `needed`,
  copied and advanced only when it fits, `STATUS_NO_MORE_ENTRIES` at the end with the position
  forgotten, a new pass after 10 s idle, 32 positions with the least recently used one taken
  for a new thread. The thread is identified by `pthread_threadid_np` (unique for the run;
  `pthread_self()` values can be reused after a thread exits). The same function serves the
  64-bit and the 32-bit table.
- **B** 11 guest addresses → `ios_g2h`: `wow64_open_cert_store` (`pfx`, the blob's `pbData`,
  `password`, `data_ret`, `key_count_ret`), `wow64_import_store_key` (`buf`, `buf_size`),
  `wow64_import_store_cert` (`buf`, `buf_size`), `wow64_enum_root_certs` (`buffer`, `needed`).
  `cert_store_data_t` is a handle.

## Part G: ws2_32 and ntdll's sockets

### G1/G2 `ws2_32/unixlib.c` (modified copy of upstream `dlls/ws2_32/unixlib.c`)

- **G2** `include/ios_objc_pool.h`: `ios_pool_getaddrinfo`, `ios_pool_getnameinfo`,
  `ios_pool_gethostbyname`, `ios_pool_gethostbyaddr` push an autorelease pool
  (`objc_autoreleasePoolPush`, libobjc), make the call, keep its `errno` (and `h_errno` for
  `gethostby*`, which the callers read afterwards), pop the pool, restore them. Used at the four
  call sites: `unix_getaddrinfo`, `unix_gethostbyaddr` and `unix_gethostbyname` (the
  `host_mutex` branches, the ones built on Darwin), `unix_getnameinfo`. `freeaddrinfo` frees
  malloc'ed memory and needs no pool. Off Apple the wrappers are the plain calls.
- **B, both directions** 18 `ULongToPtr` → `ios_g2h`: `wow64_unix_getaddrinfo` (`node`,
  `service`, `size`, `hints`, and `info`, `size` passed to `put_addrinfo32`),
  `wow64_unix_gethostbyaddr` (`addr`, `size`, and `host`, `size` for `put_hostent32`),
  `wow64_unix_gethostbyname` (`name`, `size`, `host`, `size`), `wow64_unix_gethostname`
  (`name`), `wow64_unix_getnameinfo` (`addr`, `host`, `serv`). 8 `PtrToUlong` → `ios_h2g`, the
  pointers written into the caller's buffer: `put_addrinfo32` (`ai_canonname`, `ai_addr`,
  `ai_next`), `put_hostent32` (`h_aliases`, `h_addr_list`, each `h_addr_list[i]`, each
  `h_aliases[i]`, `h_name`).
- Statics: `host_mutex` (app-wide: it serialises libc's non-reentrant `gethostby*` storage,
  which is app-wide), `hash_init_once` / `byte_hash` (app-wide; upstream randomises the address
  order "once per process" so that one process always sees the same order; here the order is
  stable for the whole app run, which keeps that property for every program), the flag maps
  (const).

### G3 `ntdll-unix/socket.c` (modified copy of upstream `dlls/ntdll/unix/socket.c`)

Helper header `ntdll-unix/ios_afd.h` (pure `*_at` functions plus the two that read
`ios_wow_base()`).

- **Who is 32-bit**: the four `in_wow64_call()` tests become `ios_afd_is32()`
  (`ios_wow_base() != 0`): `try_recv` (control messages translated back to the 32-bit layout),
  `sock_ioctl_recv` and `sock_ioctl_send` (reading a 32-bit WSABUF array),
  `IOCTL_AFD_RECV` (`afd_recv_params_32`).
- **Guest addresses**: `u64_to_user_ptr()` is redefined as `ios_afd_user_ptr()` (B + low 32
  bits, 0 → NULL, unchanged for B = 0). Every use of it in the file is a pointer field of an
  AFD request: `IOCTL_AFD_WINE_RECVMSG` (`ws_flags_ptr`, `buffers_ptr`, `control_ptr`,
  `addr_ptr`, `addr_len_ptr`), `IOCTL_AFD_WINE_SENDMSG` (`buffers_ptr`, `addr_ptr`),
  `sock_transmit` (`head_ptr`, `tail_ptr`). The four `ULongToPtr` on guest addresses become
  `ios_afd_user_ptr`: each 32-bit WSABUF's `buf` in `sock_ioctl_recv` and `sock_ioctl_send`,
  `params32->buffers` in `IOCTL_AFD_RECV`, `control32->buf` in `wow64_translate_control`.
  `ULongToHandle( params->file )` is a handle.
- **`IOCTL_AFD_GET_EVENTS`**: `reset_event = ios_afd_event_handle_at( ios_wow_base(),
  in_buffer, enabled )`: for B ≠ 0 a value in `[B, B + 0xffffffff]` becomes `(LONG)(v − B)`
  sign-extended (B itself → NULL), anything else is unchanged; B = 0 unchanged.
  `MADEIRA_AFD_EVENT_HANDLE=0` turns it off (read once, cached in a static with relaxed
  atomics).
- Nothing added to the send and receive paths does I/O or takes a lock: `ios_wow_base()` and
  arithmetic only. The `static int once` flags of upstream's FIXMEs stay app-wide (log once).

### Integration of G3

`libntdll_unix.a` is spec 09's library. Its `build-ios.sh` (spec 09 clarification 2; spec 13
clarification 4, answering QUESTIONS 13-1) copies `out/13-net-crypto-ios/ntdll-unix/socket.c`
and `ios_afd.h` into its merged source itself, in place of upstream's `socket.c`, and compiles
them with spec 09's flags and headers; `socket.c` needs `ios_wow_base()` from the same library
(spec 10). There is no CI copy step and no copy under `out/09-ntdll-processes/ntdll-unix/`: this
folder is the only place the file lives, so two copies can't drift. `ntdll-unix/` holds exactly
those two files, since spec 09 takes no others from it.

## Part H: dnsapi (`dnsapi/libresolv.c`)

Modified copy of upstream `dlls/dnsapi/libresolv.c` (search list, server list, set servers,
query), so `DnsQueryConfig`'s server list and with it `GetAdaptersAddresses` work.

- **H1** `include/ios_cfg_dnsapi.h`: `HAVE_RESOLV`, `HAVE_RESOLV_H`, `HAVE_NETINET_IN_H`,
  `HAVE_ARPA_NAMESER_H`, `HAVE_NETDB_H` defined; `HAVE_RES_GETSERVERS`,
  `HAVE_STRUCT___RES_STATE__U__EXT_NSCOUNT6` undefined, so the server list comes from the
  state's IPv4 servers (`nscount`, `nsaddr_list`). An IPv6-only DNS setup has an empty list;
  queries still use the state's own servers.
- **H2** `include/ios_resolv.h` is included after all of the file's headers and maps `_res` →
  `*ios_resolv_state()`, `res_init()` → `ios_resolv_init()`, `res_query` → `ios_resolv_query`,
  `h_errno` → our thread-local value (`common/ios_resolv.c`). **Clarification 3 (spec 00 D2):**
  libresolv is linked (`-lresolv`, listed in `LINK.txt` and noted at the end of `build-ios.sh`)
  and called directly through the public `res_ninit` / `res_nquery` / `res_ndestroy` of
  `<resolv.h>` (Apple's header maps them to `res_9_*`); nothing is opened by path and the
  "library missing" path is gone. "The state" is a resolver state of ours per thread, allocated
  zeroed on the thread's first use and handed to a pthread key whose destructor calls
  `res_ndestroy` (only on a state `res_ninit` initialised: a zeroed one would close fd 0) and
  frees it at thread exit; if the allocation fails, a thread-local static state is used
  instead. "Init" is `res_ninit(state)` (a failure is logged once: `[dnsapi-ios] res_ninit
  failed (N): no DNS servers`), "query" is `res_nquery(state, …)` whose failure code
  `state->res_h_errno` is copied to our `h_errno` (upstream's `map_h_errno` then gives, for
  example, `DNS_ERROR_RCODE_SERVER_FAILURE` for `TRY_AGAIN`). The process-wide `_res` /
  `h_errno` symbols are never touched. Host builds (glibc has no `res_ndestroy`) use
  `res_nclose`; the host tests replace all three with fakes (`IOS_RESOLV_TEST`).
- **H3** 7 `ULongToPtr` → `ios_g2h`: `wow64_resolv_get_searchlist` (`list`, `len`),
  `wow64_resolv_get_serverlist` (`addrs`, `len`), `wow64_resolv_query` (`name`, `buf`, `len`).
  The 32-bit table's `set_serverlist` is the new `wow64_resolv_set_serverlist`: its argument
  block is the caller's optional `IP4_ARRAY`, and `args` equal to `B` (a 32-bit NULL converted
  by the WoW64 layer) or NULL becomes NULL (`ios_guest_args_is_null`).
- **H4** Both tables point at `ios_gated_*` wrappers (macro `IOS_GATED`) that return
  `STATUS_NOT_SUPPORTED` when `MADEIRA_DNSAPI_UNIXLIB=0` (read once per run, logged once).

## Part I: the network tables (nsi)

`nsiproxy.sys` is not built. nsi.dll's own table `nsi_unix_call_funcs` /
`nsi_unix_call_wow64_funcs` is `nsi/nsi_unix.c`.

- **I1** Three entries: 0 enumerate (`struct nsi_enumerate_all_ex *`), 1 one row
  (`struct nsi_get_all_parameters_ex *`), 2 one field (`struct nsi_get_parameter_ex *`). The
  32-bit structs (`include/ios_nsi_pure.h`: pointers and `UINT_PTR` as `uint32_t`) are
  `_Static_assert`ed at 60, 56 and 48 bytes with key offsets. The 32-bit entries build the
  64-bit struct with `ios_g2h` on `module`, the four data pointers / `key` / `data`; the two
  `unknown` pointers are passed as NULL; entry 0 writes `count` back after every call (on
  overflow it is the caller's own value, untouched). Return values are the dispatcher's:
  `STATUS_SUCCESS`, `STATUS_BUFFER_OVERFLOW`, `STATUS_NOT_SUPPORTED`, or a provider's status.
- **I2** `nsi/nsi_tcp.c` (LGPL: its row building follows upstream `tcp.c`): `tcp_module`
  (`NPI_MS_TCP_MODULEID`, which `netiodef.h` defines together with the other four in
  `nsi_unix.c`; QUESTIONS Spec 13 #2) with `NSI_TCP_ALL_TABLE` (filter 0),
  `NSI_TCP_ESTAB_TABLE` (`MIB_TCP_STATE_ESTAB`), `NSI_TCP_LISTEN_TABLE`
  (`MIB_TCP_STATE_LISTEN`), sizes `{ key, 0, dynamic, static }`. One `get_tcp_connections`
  request on the calling thread's server connection (`wine_server_call`); key rows from the
  addresses and ports, IPv6 scope ids from the reply's `local_scope_id` / `remote_scope_id`
  (upstream looked them up with `getifaddrs`), dynamic rows the state, static rows the owner's
  pid with create time and module info 0. `ios_nsi_tcp_outcome()`: no buffers → the count only;
  buffers too small → `STATUS_BUFFER_OVERFLOW`, count untouched. The reply buffer is the
  caller's room (bounded at 2^20 rows only so its byte size can't overflow); a caller with
  buffers and room 0 gets no allocation. Other TCP tables (stats) are not served.
- **I3** The dispatcher: modules NDIS (`ndis.c`), IPv4 and IPv6 (upstream `ip.c`, unchanged),
  TCP. A module or table not found, or a table without the asked operation (for example the
  compartment table has no enumerate; QUESTIONS Spec 13 #4) → `STATUS_NOT_SUPPORTED`. The
  checks (`ios_nsi_check_*`, pure): enumerate and one row — each part 0 (buffer ignored) or
  exactly the table's size with a buffer, the key of one row always the table's size; one
  field — `param_type` ≤ 2, key size the table's with a buffer, a buffer for a non-zero size,
  offset and offset + size (64-bit sum) inside the row. Otherwise `STATUS_INVALID_PARAMETER`.
  `ios_nsi_enumerate_all_ex`, `ios_nsi_get_all_parameters_ex`, `ios_nsi_get_parameter_ex` (the
  renamed upstream names that `unix_private.h`'s helpers call) are the same dispatcher, so the
  providers' own lookups get the same checks (but not the cache or the log).
  `MADEIRA_NSI_NETWORK_TABLES=0` (read once, logged) makes the NDIS, IPv4 and IPv6 modules not
  served; TCP stays.
- **I4** The cache (`ios_nsi_cache_*`, pure, under `ios_nsi_lock`): only nsi.dll's entry 0, never
  TCP. Key: module, table, both arguments, the four sizes, rows or count only. A hit younger
  than `MADEIRA_NSI_CACHE_MS` (default 500, 0 off, capped at 5000, junk → 500; read once)
  answers with the provider's semantics: count only → the count; rows → all rows and the
  count, or the rows that fit and `STATUS_BUFFER_OVERFLOW` with the count untouched. A
  successful enumerate is stored (rows copied) in the same request's slot, else a free one,
  else the oldest of 8; failures are never stored; a result over 1 MB is not stored and drops an
  older result of the same request. Entries 1 and 2 always ask the provider. The first 16
  enumerates of the run are logged: `[nsi-ios] enumerate <module> table <n>: status <s>, count
  <c>[ (cached)]`.
- **I5** `include/ios_cfg_nsi.h` undefines `HAVE_NET_IF_ARP_H`, `HAVE_NETINET_IF_ETHER_H`,
  `HAVE_NETINET_IP_VAR_H`, `HAVE_NETINET_ICMP_VAR_H` (and for `ip.c`, compiled with
  `-DIOS_NSI_IP_C`, `HAVE_STRUCT_IPSTAT_IPS_TOTAL`, `HAVE_STRUCT_IP_STATS_IPS_TOTAL`), defines
  `HAVE_NET_ROUTE_H`, and includes the NSI rename header (A4). `include/net/route.h` provides
  `struct rt_metrics` (56 bytes), `struct rt_msghdr` (92 bytes, `rtm_flags` at 8, `rtm_rmx` at
  36), `RTM_GET`, `RTM_IFINFO`, `RTF_GATEWAY`, `RTF_LLINFO`, `RTF_MULTICAST`, `RTA_DST`,
  `RTA_GATEWAY`, `RTA_NETMASK`, `RTA_IFP` and `struct sockaddr_inarp` (16 bytes), every size and
  offset `_Static_assert`ed. `SIN_ROUTER` is not provided (QUESTIONS Spec 13 #3).
  `nsi/nsi_ip6scope.c` holds `get_ipv6_addr_scope_table` / `find_ipv6_addr_scope` from upstream
  `tcp.c`'s `getifaddrs` branch, with `*size` set on every path (0 on failure) and interfaces
  without an address skipped.
- **I6** `nsi/ndis.c` (modified copy): `update_if_table` adds nothing when `if_nameindex()`
  returns NULL. The `NET_RT_IFLIST` walk of `ifinfo_fill_dynamic` uses its own cursor checked by
  `ios_route_msg_at()` (stop when fewer bytes than an `if_msghdr` remain, or a message's length
  is below the header or runs past the end) and frees the buffer that was allocated (upstream
  advanced `buf` and freed the advanced pointer). The same bounded walk is used in
  `if_get_physical`, which walks the same reply (QUESTIONS Spec 13 #5).
- **I7** The interface list of `ndis.c`, the cache and the resolver handle are app-wide; the TCP
  request uses the calling thread's own server connection.

## Part J: one copy for every program (spec 09 Part B3 audit)

Every global and static of every file this spec compiles, classified as **app-wide**, **per
thread** or **per process**. Nothing per process is left: what upstream keeps per process (the
attach set-up, the root list) is app-wide on purpose (C, F2). Everything read from the
environment is read once per app run (`MADEIRA_CA_BUNDLE` and `MADEIRA_CA_FILTER` at the first
root enumeration, the other `MADEIRA_*` switches at first use, cached). Every other object of the
crypto files is per key, per session, per credential or per store, created by the call that owns
it.

| File | Global / static | Class | Why |
|---|---|---|---|
| all upstream-derived files, `ip.c` | `__wine_dbch_*` debug channels (`WINE_DEFAULT/DECLARE_DEBUG_CHANNEL`) | app-wide | `WINEDEBUG` is one for the app (spec 02) |
| `bcrypt/unixlib_ios.c` | `attach_state` | app-wide | clarification 13: the one-time set-up and self-test |
| | `fallback_logged` (in `generate_rsa`) | app-wide | log-once flag |
| | `digestinfo_md2`, `digestinfo_md5`, the self-test vectors, function-local `zero` arrays | app-wide | const |
| `secur32/schannel_bearssl.c` | `attach_logged`, `gap_logged`, `no_cert_logged`, `warning_logged`, `logged` (in `peer_start_cert`) | app-wide | log-once flags |
| | `ios_handshake_logged[4]` | app-wide | "first 32 of each kind per app run" (E) |
| | `suites`, `peer_chain_vtable`, the WCHAR strings | app-wide | const |
| `crypt32/unixlib.c` | `attach_logged`, `identities_logged` (in `open_cert_store`) | app-wide | log-once flags |
| | `root_cert_list`, `ios_roots_loaded`, `ios_roots_count`, `ios_roots_sizes`, `ios_roots_data`, `ios_roots_lock` | app-wide | one root list for every program, loaded and filtered once (F1, clarification 12, F2) |
| | `ios_roots_positions` | per thread (32 slots) | F2: each thread's position, app-wide table of positions |
| | `header`, `trailer` | app-wide | const |
| `common/ios_ltc.c` | the init state of `ios_ltc_init`, the `ios-secrandom` PRNG descriptor | app-wide | clarification 13: `ltc_mp`, the hashes and the PRNG registered once |
| LibTomCrypt (`libtomcrypt_unix.a`) | `ltc_mp`, `hash_descriptor[]`, `prng_descriptor[]` | app-wide | written once by `ios_ltc_init`, read-only afterwards |
| `common/ios_applecrypto.c`, `common/ios_der.c` | none | — | per call |
| BearSSL (`libbearssl.a`) | none written | — | its tables are const; every context is the caller's |
| `ws2_32/unixlib.c` | `host_mutex` | app-wide | serialises libc's non-reentrant `gethostby*` storage, which is app-wide |
| | `hash_init_once`, `byte_hash` | app-wide | upstream randomises address order once per process so a process always sees one order; app-wide keeps that for every program (the order is stable for the run) |
| | the five flag/family/type/protocol maps | app-wide | const |
| `dnsapi/libresolv.c` | `disabled` (in `ios_dnsapi_disabled`) | app-wide | H4 switch, read once |
| `common/ios_resolv.c` | `ios_resolv_once`, `ios_resolv_key`, `ios_resolv_key_ok`, `logged` (in `ios_resolv_init`) | app-wide | the pthread key that releases each thread's state; the one-time log |
| | `ios_state`, `ios_static_state`, `ios_h_errno` | per thread | the thread's resolver state (H2, clarification 3) and our `h_errno` |
| `common/ios_rootenum.c`, `common/ios_nsi_pure.c` | none | — | pure |
| `nsi/nsi_unix.c` | `ios_nsi_modules`, `ios_nsi_module_names` | app-wide | const |
| | `on`, `ms` (the two switches) | app-wide | read once |
| | `ios_nsi_lock`, `ios_nsi_cache`, `ios_nsi_logged` | app-wide | I4, I7 |
| | `NPI_MS_*_MODULEID` (`ios_nsi_` prefix) | app-wide | const |
| `nsi/ndis.c` | `if_list`, `if_list_lock` | app-wide | the host's interfaces, the same for every program (I7) |
| | `tables`, `ndis_module` | app-wide | const |
| `nsi/nsi_tcp.c` | `tcp_tables`, `tcp_module` | app-wide | const |
| `dlls/nsiproxy.sys/ip.c` (unchanged) | `ipv4_multicast_addresses`, `ipv4_tables`, `ipv6_tables`, `ipv4_module`, `ipv6_module`, function-local `hdr`, `in_list`, `out_list` | app-wide | constant data (the tables are not written) |
| `ntdll-unix/socket.c` | `enabled` (in `ios_afd_event_fix_enabled`) | app-wide | G3 switch, read once |
| | four function-local `static int once` | app-wide | "FIXME once" log flags |

Objects that belong to a program but live behind handles (crypt32's PFX stores, secur32's
credentials and sessions, bcrypt's keys with their `SecKeyRef`s) are freed when the program
closes the handle; a pseudo-process killed without closing them leaves them allocated (nothing
in these libraries learns that a pseudo-process ended), as an exiting process's memory would
have been reclaimed upstream. They hold no per-process global state.

## Spec 17 Part A1

- spec 17 A1 (spec 13 clarification 1), `build-ios.sh`: `out/09-ntdll-processes/include/` (spec 17's
  shared `wine/unixlib.h`, with `ios_wow_host_ptr` / `ios_wow_guest_ptr32`, and `winternl.h`) is
  on every compile's include path before the Wine tree's `include/`. Part B's conversions keep
  their own helpers; the new ones are available.
