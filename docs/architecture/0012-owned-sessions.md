# ADR 0012: Local player authentication and owned sessions

Date: 2026-10-18. Status: accepted; M02b qualified under current-machine scope.
Scope: the current Windows PC and existing WSL, following [ADR 0010](0010-current-machine-scope.md).

## Decision

Use the existing pinned Mbed TLS 4.1.1 certificate implementation for mutual
TLS, with explicit local CA trust and server-owned certificate enrollment.
The server maps a verified leaf certificate's SHA-256 fingerprint to a distinct
account identity and checks its own character ownership map. A fingerprint,
display name, claimed account ID or socket number is never a credential.
Proof of the client's private key is supplied by the TLS handshake.

This extends [ADR 0011](0011-network-foundation.md); no new dependency, password
scheme, bearer-token format, identity provider or custom cryptography is added.
The private library verifies certificate chains, time and handshake signatures.
Shark additionally requires an explicit clientAuth EKU, digitalSignature key
usage and a non-CA leaf. Missing EKU and anyExtendedKeyUsage alone are rejected.
Peer fingerprints and validity intervals are copied before the next TLS call;
no borrowed certificate pointer enters a public interface. Both TLS modes use
ephemeral certificate-based TLS 1.3, with no resumption or 0-RTT authorization.
See the pinned [SSL API](https://github.com/Mbed-TLS/mbedtls/blob/mbedtls-4.1.1/include/mbedtls/ssl.h)
and [X.509 API](https://github.com/Mbed-TLS/mbedtls/blob/mbedtls-4.1.1/include/mbedtls/x509_crt.h).

The local credential profile keeps authentication independently testable without
an external account service. Certificate issuance/distribution and durable account
storage are separate trusted operations; this checkpoint does not expose a public
enrollment endpoint or package test keys as defaults. A later player-facing login
experience may add a separately reviewed credential adapter to the same ownership
service. It must preserve these validation and revocation boundaries.

## Authority and reconnect

`SessionAuthority` accepts the actual ready server-side mutual-TLS transport for
every request. The transport rechecks the copied certificate validity interval;
login checks enrollment and character ownership, while each command checks the
live session, deadlines, connection binding, character identity, epoch and exact
next application sequence. Enrollment and ownership records are immutable within
the service; revocation synchronously erases affected sessions. M03 must preserve
this authorization invariant when mutable durable ownership is introduced.
The old diagnostic transport cannot provide authenticated peer identity or
carry session messages. Public server/client executables gain no insecure login
flag, default identity or test-auth bypass.

Account, character and session IDs use distinct typed UUID domains. One account
has at most one active session. A successful login receives a fresh OS-entropy
session UUID and a new nonwrapping account-wide ownership epoch. It must use a
server-side connection created after that account's last successful login.
The process-local connection token is monotonic and never reused. Reconnect
validates the complete candidate before replacing the existing session; old
connections, queued requests and old session revocations cannot act on the new
session. Login on an already-open older connection rejects and requires fresh TLS.

Trusted provisioning and revocation remain owner-thread C++ operations. They are
not player messages, authoring transactions or remote administrative commands.
In-memory registry records and revocation tombstones have explicit capacities;
re-enrollment cannot reset rate windows or ownership fences. Monotonic lifetime
and idle deadlines complement certificate wall-clock validity. Unknown login
attempts consume an authority-wide budget, and enrolled credentials/accounts
retain their authentication budgets across reconnects.

## Bounded protocol and scope

The additive outer-wire kinds 5 and 6 carry independently versioned session
requests/replies. Session version 1 supports Login, SetReady, Heartbeat and
Logout. Ready is session state; it grants no movement, combat, inventory,
progression, scene-editing or administrative capability. Explicit byte widths,
lengths, UUID validation, enum/boolean checks and bounded queues replace native
struct serialization. Public failures use one generic rejection without foreign
account/character state; internal diagnostics retain stable error operations.

No mutation retry or durable exactly-once promise follows from these session
controls. Lost outcomes can require a new login; later durable operations belong
to M03. M02c [replication and resynchronization](../network-replication.md) now
extend these session checks and are locally qualified under
[ADR 0013](0013-authoritative-replication.md). M02d adds qualified spatial interest
and budgets under [ADR 0014](0014-spatial-interest.md). Integrated adversity remains
M02e; full REQ-M02 remains NOT_RUN.

The [M02b handoff](../handoffs/M02b-owned-sessions.md) records completed
qualification. The [session contract](../network-sessions.md) and
[checkpoint plan](../roadmap/network-plan.md) own the exact API and acceptance.
