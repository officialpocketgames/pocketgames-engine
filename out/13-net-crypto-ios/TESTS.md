# Spec 13 tests

The host tests (section 3) run in WSL (Ubuntu, gcc 15 and clang 21) with the Wine tree and a
configured Wine build dir, and pass, together with `check-app-store.sh` ("clean"). The iOS builds
(sections 1 and 2) and the device checks (section 4) are for CI on macOS and for the device:
Linux is not Darwin, so the routing sysctls, Apple's `res_9_*` resolver, libobjc's autorelease
pools, Mach-O `nm` listings, the iPhoneOS SDK headers, and everything Security.framework and
CommonCrypto really do (the host tests run on a fake of `include/ios_applecrypto.h` built on
BearSSL) are only checked there.

## 1. Build (CI, macOS with Xcode)

```sh
out/13-net-crypto-ios/build-crypto-deps-ios.sh "$DEPS" refs/wine-11.4
out/13-net-crypto-ios/build-ios.sh refs/wine-11.4 "$WINE_BUILD" "$OUT13" "$DEPS"
```

- `build-crypto-deps-ios.sh` clones BearSSL, checks `git rev-parse HEAD` against
  `7bea48e5e850ab4cafbe68d3765cdaba13a86d6f`, and ends by listing `libbearssl.a` and
  `libtomcrypt_unix.a`; a second run prints "already built" for both. A failed LibTomCrypt build
  names any `ltc_` / `mp_` / `der_` name left undefined (none expected: the list was computed on
  the host with the same defines).
- `build-ios.sh` prints `<file> OK` for each of its 16 objects, `libbearssl.a: no process or
  loader imports` and the same for `libtomcrypt_unix.a` (clarification 14), and writes
  `libnetcrypto_unix.a`, the two archives and `LINK.txt`.
- Spec 09 integration (G3, spec 13 clarification 4): spec 09's `build-ios.sh` takes
  `out/13-net-crypto-ios/ntdll-unix/socket.c` and `ios_afd.h` itself, in place of upstream's
  `socket.c`; no copy step. `socket OK` must appear in its output.

## 2. Link checks (CI)

```sh
out/13-net-crypto-ios/check-symbols.sh clashes "$OUT13/libnetcrypto_unix.a" \
    "$OUT09/libntdll_unix.a" "$OUT12/libwin32u_unix.a" "$OUT08/<server library>.a"
out/13-net-crypto-ios/check-symbols.sh tables "$APP_BINARY"
```

- `clashes` (which also reads `libbearssl.a` and `libtomcrypt_unix.a` next to the first
  archive) must print `no clashes` (A4). A LibTomCrypt symbol it lists gets a line in
  `include/ios_tomcrypt_rename.h`; one of our own gets a macro rename.
- The app is linked with the archives and flags of `LINK.txt` (A5): the three archives, the
  twelve `-Wl,-u,_X_unix_call_funcs` / `_wow64_` flags, `-framework Security`, `-framework
  CoreFoundation`, `-lobjc`, `-lresolv`. `tables` must print `found` for all twelve names (A3).
  `ci/link-check.sh` does this link and checks the imports against `the App Store rules`.
- `LINK.txt`'s commented list of other undefined symbols must contain only ntdll's unix exports
  (`wine_server_call`, `__wine_dbg_*`, `ntdll_umbstowcs`, `ios_wow_base`, `ios_wow_guest_addr`,
  `ios_unix_exit`, …) and libSystem functions (CommonCrypto's `CCCryptor*` among them).

## 3. Host tests (CI, Part K and clarification 15)

```sh
out/13-net-crypto-ios/tests/run-host-tests.sh refs/wine-11.4 "$WINE_BUILD"
```

Every line must be `PASS …`; the script exits non-zero otherwise. Without the two arguments the
Wine-header tests are skipped (`SKIP …`) and the rest still run. The crypto tests first build
BearSSL and LibTomCrypt for the host from copies of `refs/bearssl-7bea48e/` and
`refs/wine-11.4/libs/tomcrypt/` (`tests/host-deps.sh`, cached in
`~/.cache/myiosdeck-13-host-deps/<compiler>/` or `$NETCRYPTO_DEPS_CACHE`; nothing is written in
`refs/`). The keys and the certificate they use are `tests/fixtures/fixtures.h`, made by
`tests/fixtures/make-fixtures.sh` (openssl; the command is in the script).

| Test | What it checks (spec K item / clarification 15) |
|---|---|
| `test_guest` | NULL both ways, `B + a`, `host − B`, identity with `B = 0`, dnsapi's NULL `set_serverlist` block arriving as `B` (B, H3) |
| `test_afd` | the GET_EVENTS handle rule below, at, inside and beyond `B + 4 GB`, with `B = 0`, with the switch off and its parsing; guest addresses of AFD requests (G3) |
| `test_rootenum` | one thread sees every certificate once per pass; two threads interleaved each see all; a too-small buffer repeats the same certificate; the end resets; 10 s idle restarts; 33 threads reuse the oldest position; a second pass after the first ends sees all again (F2) |
| `test_nsi` | the dispatcher's checks (sizes, NULL buffers, offset overflow, `param_type`), the TCP filter and size rules and the server outcome, the cache (hit within the TTL, miss after, a different size misses, overflow keeps the count, TTL 0, tables over 1 MB not cached, eviction of the oldest of 8, the env value parsed and capped), the `_Static_assert`ed I1 and I5 layouts, the bounded routing-message walk (I1–I6) |
| `test_nsi_dispatch` (+ `off`) | with the Wine headers and fake providers: unserved tables and modules, a table without the operation, the checks through the real table, the cache through it (TCP never cached, failures never cached, the providers' own lookups never cached), the 32-bit entries with a fake window base and the count written back; `off`: `MADEIRA_NSI_NETWORK_TABLES=0` (I1–I4) |
| `test_resolv` | the resolver layer (clarification 3) with fake `res_ninit` / `res_nquery` / `res_ndestroy`: one zeroed state per thread, stable across calls; init on that state; a failed query's `res_h_errno` (`TRY_AGAIN`, `HOST_NOT_FOUND`) becomes our `h_errno`, a successful one leaves it; another thread's state is its own and is released with `res_ndestroy` at thread exit; an uninitialised state is freed without it (H2) |
| `test_dnsapi` (+ `off`) | with the Wine headers and a fake resolver: with no servers configured a query gives `DNS_ERROR_RCODE_SERVER_FAILURE` and the server list `DNS_ERROR_NO_DNS_SERVERS`; the NULL `IP4_ARRAY` block arriving as `B`; a configured server appears in the list as a Windows `SOCKADDR_IN`; `off`: `MADEIRA_DNSAPI_UNIXLIB=0` gives `STATUS_NOT_SUPPORTED` everywhere (H) |
| `test_bcrypt` | `bcrypt/unixlib_ios.c` through its tables (clarification 8): AES-CBC/ECB/CFB8 with SP 800-38A vectors, a chain split over calls, a change of direction, in-place decryption, `vector_reset`; AES-GCM with McGrew–Viega cases 1–4 and 6 (60-byte IV), encrypt and decrypt tags, 12- and 16-byte tags, NULL auth data, auth data only, input split inside a block; 3DES-CBC round trip and the 16-byte-key failure; the unsupported modes' status; RSA `RSAPRIVATE` completion to the fixture's d, dp, dq, qinv, `RSAFULLPRIVATE` round trip, duplicate, export sizes; PKCS1 and OAEP-SHA-256 (SecKey route counted in the fake), OAEP with a label and OAEP-MD5 (BearSSL route, no SecKey call), MD2 refused, PAD_NONE with the modulus rule; sign/verify PKCS1 SHA-256, MD5 (the DigestInfo seen under the signature), NULL algorithm id, PSS salt 32 (SecKey) and 0 / 20 / MD5 (BearSSL), changed bytes → `STATUS_INVALID_SIGNATURE`; RSA generate on Security and the BearSSL fallback; ECDSA P-256/384/521 generate, sign, verify, blob round trips, a high-bit r and a short s both ways through DER, a signature with a leading zero half; ECDH RFC 5903 §8.1 (generic and P-256 ids) and §8.2, P-521 generated keys agree; X25519 RFC 7748 §5.2 and §6.1 (public keys derived from the private imports), an all-zero secret refused, generated keys agree; DH: group 2's prime for a 1024-bit key without parameters, parameter and private blob round trips, two 2048-bit keys agree, a secret with a leading zero byte padded to 96 bytes; DSA 1024: generate, sign, verify, a changed hash fails, BCRYPT and CAPI v2 blob round trips with `DSSSEED`, `y` recomputed after a private import, bitlen 1000 refused; the 32-bit table with B ≠ 0 (AES encrypt, RSA sign with the padding string in guest memory); the 7 self-test cases; no CF-style object left alive after each group |
| `bcrypt self-test line` | the attach logged `bcrypt self-test: 7/7 passed` (8.6) |
| `test_schannel` | `secur32/schannel_bearssl.c` as an in-process client and server through the tables, with `schannel.c`'s buffer layouts (clarification 10): TLS 1.2 ECDHE_RSA_AES_128_GCM, TLS 1.0 RSA_AES_128_CBC_SHA, TLS 1.2 ECDHE_RSA_AES_256_CBC_SHA384, each with the connection and cipher info of the table; one input holding the last flight plus an application record (`input_offset` stops at the record); output buffers of 50 and 64 bytes (`SEC_I_CONTINUE_NEEDED`, the rest on the next call); send of 1 and 16384 bytes both ways, the 1/n−1 split of TLS 1.0 received record by record; ALPN h2 on both sides; SNI seen by the server; the peer chain in upstream's layout; tls-unique 12 bytes, equal on both sides, before the handshake an error; server-requested renegotiation (`SEC_I_RENEGOTIATE` on both sides, then the handshake calls complete it, new tls-unique); close_notify by control token received as the end of the stream; fatal alert 42 sent and reported by the peer (`BR_ERR_RECV_FATAL_ALERT + 42`); a warning other than close_notify not sent; DTLS-only credentials refused, a server without a certificate failing its first handshake call; the client through the 32-bit thunks with B ≠ 0 (handshake, send, receive) |
| `test_crypt32` | `crypt32/unixlib.c` (clarifications 11, 12): the root filter with a fake that distrusts marked certificates (7 of 10 kept with the log line and a dropped certificate's SHA-256 prefix, exactly half kept, fewer than half → the whole bundle, `MADEIRA_CA_FILTER=0` with no trust call, unset, a directory refused, a second enumeration pass without a reload); PFX import with a non-ASCII UTF-16 password, `PRIVATEKEYBLOB` from the fixture's PKCS #1 key (layout and size), deduplicated certificates, wrong password, an `OSStatus` failure, certificates only (no key), no certificate; the 32-bit `open_cert_store` with B ≠ 0 |
| `check-app-store` | `check-app-store.sh` over the part's sources prints "clean" (spec 00 F) |

`ios_resolv.c` itself (not the fakes) was also built with gcc and clang, `-Wall -Wextra
-Werror`, against glibc's libresolv (`-lresolv`) in WSL: `res_ninit` read `/etc/resolv.conf`
and found its server.

The `_Static_assert`s of `include/net/route.h` and `include/ios_nsi_pure.h` are also checked
by every compile of the NSI files for iOS.

## 4. On the device

Each check names the log line (in `<Documents>/wine-log.txt`) that shows it. Lines tagged
`[crypt32-ios]`, `[secur32-ios]`, `[bcrypt-ios]`, `[dnsapi-ios]`, `[nsi-ios]` come from this
spec.

1. **HTTPS from a 64-bit program.** Stage a CA bundle (curl's `cacert-YYYY-MM-DD.pem`) and set
   `MADEIRA_CA_BUNDLE` to it. A 64-bit test program fetches `https://example.com/` with winhttp:
   HTTP 200, certificate check passed. Log: `[crypt32-ios] MADEIRA_CA_BUNDLE=<path>: M
   certificates in the bundle, kept N of M roots, …` with N ≥ 100, `[secur32-ios] secur32:
   BearSSL 7bea48e (TLS 1.0-1.2)`, `[secur32-ios] TLS handshake complete (client), version
   0x303, suite 0x…`. With `MADEIRA_CA_BUNDLE` unset (new app run): `[crypt32-ios]
   MADEIRA_CA_BUNDLE is not set: no root certificates`, and the request fails with a
   certificate error (`ERROR_WINHTTP_SECURE_FAILURE`, untrusted root). A directory as the value
   gives `… not a regular file, refused`.
2. **The Root store isn't emptied.** In the same session, a second program does check 1 and
   passes; then, after the first program exited, a third one passes too; then the same from a
   32-bit program. Log: the `kept N of M roots` line appears once per app run (no second
   load), and every program's handshake gives `TLS handshake complete`. `reg query
   "HKLM\Software\Microsoft\SystemCertificates\Root\Certificates"` in the prefix lists the same
   N entries after each program.
3. **`getaddrinfo` from short-lived threads.** A test program starts 1000 threads in a loop,
   each resolving `localhost` and `example.com` with `getaddrinfo` (and one `gethostbyname`)
   and exiting. No crash at thread exit; the program prints `done 1000` and the log has no crash
   report after it.
4. **Adapters and routes.** `GetAdaptersAddresses` lists the Wi-Fi interface (`en0`) with its
   IPv4 address and at least one DNS server; `GetIpForwardTable` has a `0.0.0.0/0` route.
   Log: no `[dnsapi-ios] res_ninit failed` line, `[nsi-ios] enumerate
   ndis table 0: status 0, count N`, `[nsi-ios] enumerate ipv4 table 16: status 0, count N`. A
   program polling both at 1 kHz for 30 s: CPU stays low, the first 16 `[nsi-ios]` lines show
   `(cached)` answers and then stop (`no more enumerates will be logged`).
5. **TCP owners.** Two programs connect over loopback; `GetExtendedTcpTable(…,
   TCP_TABLE_OWNER_PID_CONNECTIONS)` from a third shows both ends with each side's real process
   id (compare with `GetCurrentProcessId()` printed by each). Steam's client UI connects to its
   local web helper. Log: `[nsi-ios] enumerate tcp table 3: status 0, count N` (never
   `(cached)`).
6. **Notifications and DNS.** `NotifyAddrChange` (overlapped) stays pending for a minute;
   `DnsQuery_A("example.com", DNS_TYPE_A)` returns an address; `DnsQueryConfig(DnsConfigDnsServerList)`
   returns the device's IPv4 servers. Log: no `[dnsapi-ios] res_ninit failed` line (the app is
   linked with `-lresolv`; a missing link is a link error, not a run-time state).
7. **A 32-bit program.** TCP send and receive to a local echo server; `WSAEventSelect` +
   `WSAEnumNetworkEvents` with an event report `FD_READ` (not `WSAENOTSOCK`), and with
   `MADEIRA_AFD_EVENT_HANDLE=0` the same call fails with `WSAENOTSOCK` (the correction is what
   fixes it); `getaddrinfo` and `gethostbyname` results readable (names, addresses, the
   `ai_next` chain); an HTTPS request (check 1); `BCryptHash` SHA-256 of "abc" and an AES-CBC
   encrypt/decrypt round trip give the known answers. Log: `[bcrypt-ios] bcrypt:
   Security+CommonCrypto, BearSSL 7bea48e, LibTomCrypt 1.18.2`, `[secur32-ios] TLS handshake
   complete (client), …`. (The socket path logs nothing by design, G3.)
8. **TLS while a program exits.** Two programs download over HTTPS in a loop while a third
   program that also used TLS exits. Neither of the two loses TLS: their requests keep
   succeeding and their `TLS handshake complete` lines continue. Log: each DLL's backends line
   appears once per app run, and there is no failure after the exit.
9. **bcrypt self-test.** The first bcrypt attach of the app run logs `[bcrypt-ios] bcrypt
   self-test: 7/7 passed`. Any other count names the failed cases; that is a bug in the
   Security / CommonCrypto route of that case (the host fake can't see it) to report with the
   line.
10. **bcrypt from a 64-bit and a 32-bit program.** RSA-2048 generate, PKCS1 SHA-256
    sign/verify, OAEP-SHA-256 round trip; ECDSA P-256/384/521 sign/verify, including a 64-byte
    digest on P-256; ECDH P-256 and X25519 agreement; AES-GCM round trip. With fixed imported
    keys (the host tests' fixtures) the outputs that are deterministic (RSA PKCS1 signatures,
    ECDH and X25519 secrets, AES-GCM ciphertext and tag) match a reference run on Windows.
    Log: no `RSA key size refused by Security.framework` line for 2048 bits.
11. **PFX import.** `PFXImportCertStore` imports a PFX with key and chain encrypted with
    AES-256/PBKDF2 (current OpenSSL or Windows tools), one with 3DES and RC2-40 (older Windows
    exports) and a certificate-only PFX (its certificates are in the store; if it yields none,
    that goes to `QUESTIONS.md`, clarification 11.4); a wrong password fails (log:
    `[crypt32-ios] PFX import failed, OSStatus N`, N being `errSecAuthFailed`, -25293). The key of the first one signs with
    `CryptSignHash` and verifies against its certificate.
12. **wininet against test servers.** A TLS 1.0-only server and a TLS 1.2 server (for example
    badssl.com's `tls-v1-0` on port 1010 and `tls-v1-2` on port 1012) both load (the log shows
    versions `0x301` and `0x303`), and a request with ALPN h2 to a public HTTP/2 server reports
    `h2` (`SECPKG_ATTR_APPLICATION_PROTOCOL`).
13. **App size.** The app binary's size before and after this change (the same build
    configuration), written into `CHANGES.md` ("App size").
