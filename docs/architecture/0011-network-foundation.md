# ADR 0011: Local TLS transport and gameplay-wire foundation

Date: 2026-10-18. Status: implemented and qualified for M02a; see the
[handoff](../handoffs/M02a-transport-wire.md). Scope: current Windows PC and existing WSL, per ADR 0010.

## Decision and alternatives

Use privately integrated **Mbed TLS 4.1.1 LTS**, including its bundled TF-PSA-Crypto,
for TLS 1.3, certificate validation and cryptographic randomness. The authoritative
[dependency lock](../../dependencies.lock.json) records the official release archive,
hash, commits, bundled inputs, build configuration and redistribution notices.
The official release archive is required: GitHub-generated source archives omit
required generated/submodule content. The release notes and bundled crypto version
metadata disagree, so exact archive and source identities determine the integration.
[Official release](https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-4.1.1),
[upstream project](https://github.com/Mbed-TLS/mbedtls).

Mbed TLS supplies the local X.509/security foundation without provider credentials.
Engine-owned socket glue uses nonblocking numeric IPv4 loopback connections;
there is no DNS resolution or blocking `mbedtls_net_connect` call. TLS and native
socket types never enter public engine headers. No custom cryptographic primitive,
key exchange, encryption packet format or cryptographic random generator is added.

Alternatives considered: GameNetworkingSockets supplies game-oriented reliable
and unreliable UDP semantics but adds a larger protobuf/crypto integration and
separate identity work; MsQuic supplies QUIC streams/datagrams with a larger
platform/TLS/threading integration; Asio with OpenSSL adds two libraries while
retaining TCP ordering. This checkpoint selects a bounded reliable stream suitable
for the first local foundation, subject to measured M02d/M02e budgets.
[GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets),
[MsQuic](https://github.com/microsoft/msquic),
[Asio SSL documentation](https://think-async.com/Asio/asio-1.36.0/doc/asio/overview/ssl.html).

TCP head-of-line blocking is an explicit tradeoff. A newer snapshot can replace
unsent application work only before entering the reliable FIFO; it cannot jump
ahead of bytes already sent. This is no unreliable datagram implementation or
MMO scale result. A future transport change requires its own measured evidence.

## Threat and trust boundaries

Assume hostile local clients, forged/malformed messages, untrusted certificates,
stalled handshakes, slow readers, replayed sequences and accidental API misuse.
Bind exclusively to `127.0.0.1`; arbitrary remote addresses and public listeners
are outside this checkpoint. Locality is not player authentication.

Clients require an explicit CA certificate and expected DNS name. TLS uses only
version 1.3, AES-128-GCM/SHA-256, P-256 key exchange and ECDSA/P-256/SHA-256
handshake signatures. Certificate chain, name and validity checks fail closed.
There is no system trust fallback, insecure verification switch, session
resumption or 0-RTT path. Servers issue no session tickets; clients discard
TLS 1.3 peer ticket notifications within their poll budget and never reuse them.
The server certificate/private key are explicit inputs;
the pair must match. Errors do not include private keys or raw peer payloads.

M02a authenticates the server endpoint to the client. It does not authenticate a
player to the server. Its only wire messages are diagnostic hello/ack/probe/reply;
there is no gameplay dispatcher or public authoring/admin endpoint. The echo
fixture is test-only and not installed or packaged. Explicitly public test keys
are isolated under `tests/fixtures/network`, never a default runtime identity.
M02b owns player credentials, authorization, revocation, reconnect identity and
bounded intents. M03 owns durable identity storage; TLS does not provide it.

## Bounded processing and lifecycle

Use the existing monotonic-time contract and single owner thread. Polling performs
a caller-bounded number of TLS/I/O/decoder steps; no network worker or second
scheduler is introduced. A process-local mutex serializes the private TLS/PSA
library operations. This deliberately favors a simple audited lifetime over an
unmeasured parallel crypto design. Step counts are not hard CPU-time guarantees.

Fixed parser/TLS buffers, bounded certificate inputs, fixed connection admission,
independent bounded inbound/outbound queues and handshake/idle/write deadlines
bound work and storage. No native handles or borrowed library buffers escape.
Transport closure is explicit; destroying a listener does not destroy its accepted
connections. The shared immutable TLS configuration remains alive until its last
connection ends. Sequence exhaustion fails rather than wrapping, and a fresh
connection starts a fresh sequence domain without restoring player authority.

See the [transport contract](../network-transport.md) for exact limits and
[M02 checkpoint plan](../roadmap/network-plan.md) for remaining gates.

## M02b extension

M02b implements a separate authenticated session mode under
[ADR 0012](0012-owned-sessions.md) and the [session contract](../network-sessions.md).
Its current-machine qualification is **PASS** in the
[M02b handoff](../handoffs/M02b-owned-sessions.md); the M02a evidence above retains
its original diagnostic scope. Mutual TLS verifies explicitly enrolled
client certificates. A bounded owner-thread authority maps certificate fingerprints
to accounts and owned characters, binds sessions to actual ready server transports,
and enforces expiry, revocation, exact request sequences and reconnect fencing.

The outer wire remains version 1 with additive session request/reply kinds 5 and
6; their typed payload has an independent session-protocol version 1. Diagnostic
and authenticated modes reject each other's application messages. Session readiness
does not grant movement, combat, component mutation or administrative access.
Enrollment and ownership remain in memory until M03 supplies durable storage.
M02c [replication](../network-replication.md) is now locally qualified under
[ADR 0013](0013-authoritative-replication.md); the
[handoff](../handoffs/M02c-authoritative-replication.md) retains its separate evidence.
M02d spatial interest/budgets and M02e integrated qualification are complete.
The [M02e handoff](../handoffs/M02e-integrated-qualification.md) records full
REQ-M02 PASS under the current-machine scope; the original M02a evidence
retains its narrower diagnostic scope.
