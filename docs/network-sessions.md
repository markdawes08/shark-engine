# Player authentication and owned sessions

M02b builds on the [transport foundation](network-transport.md), using shared
CPU C++ services in [session.hpp](../engine/core/include/shark/net/session.hpp).
Human developers and terminal agents exercise the same services and noninteractive
fixtures; no editor, renderer, model SDK or external identity account is needed.
The decision and trust boundaries are in [ADR 0012](architecture/0012-owned-sessions.md).
M02b is PASS under the current-machine scope; the [handoff](handoffs/M02b-owned-sessions.md)
and [receipt](coverage/evidence/M02b-owned-sessions.json) record 425 passing CPU
CTests and protected package qualification.

## TLS credential boundary

Call `TlsListener::listen_loopback_authenticated` with the server certificate/key
and an explicit client CA. Call `TlsTransport::connect_loopback_authenticated`
with server CA/DNS trust and an explicit `ClientIdentity` certificate/key pair.
Only numeric IPv4 loopback is supported. Existing transport input, queue, deadline
and owner-thread limits remain in effect. Both directions verify peer trust;
the server also requires a non-CA client leaf with explicit clientAuth EKU and
digitalSignature usage. There is no password/token parser or certificate-name
mapping to an account.

`certificate_fingerprint` accepts one bounded PEM certificate and returns the
library-computed SHA-256 digest of its DER bytes for trusted enrollment. This
helper alone verifies neither trust nor possession of a private key. The actual
server connection's `authenticated_peer()` succeeds only while ready in mutual
TLS mode, and rechecks its copied certificate validity times. It returns owned
fingerprint data and a nonzero process-local connection token that never repeats
or wraps. Client-side, diagnostic, closed and failed connections reject.

The diagnostic listener retains probe/reply behavior. Session mode permits only
session_request/reply after the existing hello/ack negotiation. The public Server
and Runtime bootstraps still have no player listener or built-in credential.

## Authoritative ownership

Create a bounded `SessionAuthority`, provision typed `AccountId` values, assign
`CharacterId` values to those accounts and enroll credential fingerprints. These
are trusted owner operations. Duplicate, conflicting, revoked or over-capacity
records reject. Account/character/session UUID domains differ from scene slots,
authored IDs, replica IDs and the TLS connection token. No public API accepts a
copied `PeerIdentity` as proof of authority.

`apply(transport, request, now)` checks the actual transport on every request.
Login validates the claimed account and character against certificate enrollment
and server ownership before issuing a fresh `SessionId` and increasing the
account-wide ownership epoch. There is one active session per account. A later
successful login replaces it only after full validation and requires a newer
connection token. Older connections, old session IDs, epochs and stale queued
intents reject. A failed login preserves any still-valid previous session. Old-ID
`revoke_session` cannot revoke a replacement.

SetReady, Heartbeat and Logout carry session ID, epoch, exact next application
sequence and character ID. The first command uses sequence 1; replay, gaps,
zero and exhaustion reject without wrap. Outer TLS-frame sequence and session
command sequence are separate. Successful SetReady changes only session readiness;
Heartbeat only keeps a valid session active. Logout ends that session.

Certificate/account revocation immediately removes matching sessions. Enrollment
and character ownership are immutable within the service. Every request checks
the remaining live session, certificate validity, lifetime and idle expiry.
Call `expire(now)` during idle periods to reclaim expired sessions; connection
owners can revoke their last session ID on disconnect without affecting a newer
replacement. Transport lifetime remains caller-owned and no borrowed pointer is
retained by the authority. A closed transport cannot authorize more requests.

All operations use the creating thread. Time is nonnegative and nondecreasing.
Authentication windows apply globally, per enrolled certificate and per mapped
account, including failed attempts, and persist across reconnects. Session command
budgets count rejected commands as applicable. Rejection never refreshes a lease,
advances the accepted sequence or replaces ownership. Registry/tombstone storage
is bounded and retained until service destruction; no eviction resets security
state. Durable provisioning, recovery and revocation storage remain M03.

Defaults are 64 accounts, 128 characters, 128 certificates and 64 live sessions;
128 login attempts per 10-second authority window, eight per credential/account,
and 64 commands per one-second session window. Session lifetime is one hour and
idle timeout is 60 seconds. Hard caps are 1024 accounts, 4096 characters and 4096
certificates; live sessions cannot exceed the configured account capacity.
Authority/command windows permit at most 65536 attempts, credential/account
windows at most 4096. Both window durations are positive and at most one hour;
lifetime is positive and at most 24 hours, idle is positive and no greater than
lifetime. Deadline addition and ownership-epoch increment reject overflow.
Limits are validated at creation. Network polling and session
expiry remain caller-driven; these limits are not hard CPU-time guarantees.

## Session wire version 1

Outer wire version 1 gains kind 5 (client session request) and 6 (server session
reply). Existing diagnostic bytes remain compatible. Session payloads start with
version u16 big-endian, kind u8 and reserved-zero u8. All multibyte fields are
big-endian; UUID bytes follow canonical display order. Nil/non-v4 UUIDs, unknown
versions/kinds, invalid booleans, extra bytes and truncation reject.
Accepted snapshots require a nonzero epoch; readiness requires an accepted
command sequence. An inactive snapshot must be unready with a nonzero sequence.

| Payload | Kind | Bytes after the four-byte prefix | Total bytes |
| --- | --- | --- | --- |
| Login | 1 | Account UUID, character UUID | 36 |
| SetReady | 2 | Session UUID, epoch u64, sequence u64, character UUID, ready u8 | 53 |
| Heartbeat | 3 | Same authority envelope without ready | 52 |
| Logout | 4 | Same authority envelope without ready | 52 |
| Accepted reply | 1 | Session/account/character UUIDs, epoch u64, last sequence u64, ready u8, active u8 | 70 |
| Rejected reply | 2 | Nothing | 4 |

`public_session_reply` maps every internal rejection to the same four-byte reply.
Detailed errors stay within trusted server diagnostics; rejected messages reveal
no foreign account/character IDs or revocation distinctions. Codecs own their
returned values and never interpret TLS or authoring state as session authority.

`SessionProbe` and `tools/test_network_sessions.py` exercise actual local
processes. All keys under `tests/fixtures/network/session` are explicitly public
test fixtures, not deployment credentials. Build/package closure excludes them.
M02c [replication](network-replication.md) is implemented and locally qualified.
M02d [interest/privacy/budgets](network-interest.md) is also locally qualified.
M02e closes full REQ-M02 under the current-machine scope; the
[handoff](handoffs/M02e-integrated-qualification.md) records the integrated
qualification and refreshed earlier network families.

M02c adds `SessionAuthority::current(transport, now)` for trusted services to
validate the actual live mTLS connection and inspect its current session. It
performs expiry maintenance but consumes no command sequence/rate or lease
refresh; callers must explicitly require readiness. The [replication service](network-replication.md)
uses this accessor, then admits validated ACK/resync controls through Heartbeat
with the same exact next session sequence. Readiness alone grants no arbitrary
scene mutation.
