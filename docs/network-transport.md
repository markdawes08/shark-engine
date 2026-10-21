# Network transport foundation

M02a adds CPU C++ APIs in [wire.hpp](../engine/core/include/shark/net/wire.hpp),
[transport.hpp](../engine/core/include/shark/net/transport.hpp) and
[fault_link.hpp](../engine/core/include/shark/net/fault_link.hpp). These work without
an editor, window, renderer or LLM SDK. They are not terminal authoring commands,
gameplay APIs or a scene replication service. Follow [ADR 0011](architecture/0011-network-foundation.md).
M02b extends this transport with explicit mutual TLS and
[owned sessions](network-sessions.md), under [ADR 0012](architecture/0012-owned-sessions.md).

## Wire version 1

Every frame has a 20-byte explicitly encoded big-endian header followed by its
declared payload. Native struct layout, pointers and registry handles are never
serialized. The wire version is independent of component schema, scene-file,
authoring protocol and database versions.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | 4 | ASCII `SHRK` magic |
| 4 | 2 | Exact version `1` |
| 6 | 1 | Kind: hello=1, hello_ack=2, probe=3, probe_reply=4, session_request=5, session_reply=6, replication_control=7, replication_state=8 |
| 7 | 1 | Reserved flags, must be zero |
| 8 | 8 | Nonzero sequence |
| 16 | 4 | Payload size, at most 4096 bytes |

Hello and hello_ack have empty payloads. Probes/replies carry 0..4096 opaque bytes
for diagnostics only. Session kinds carry separately versioned, bounded M02b
payloads and are accepted only by explicit mutual-TLS session-mode transports.
M02c adds client replication controls (7) and server replication state (8),
with their own [versioned public-field schema](network-replication.md).
Unknown kinds, versions, flags, zero sequences and oversized
claims fail before reading/allocating a body. `decode_frame` requires exactly one
complete frame. `FrameDecoder::consume` returns one owned frame at most, a consumed
byte count and retains a partial frame in a fixed 4116-byte array. Call again with
the remaining input for coalesced frames. `finish` rejects truncated EOF. A parse
error poisons the decoder; close rather than scanning for another magic marker.

Each direction starts at sequence 1 and requires exactly last+1, with independent
send/receive counters. Duplicates, gaps and reordering reject. UINT64_MAX is a valid
last sequence, but the next operation fails without wrap. New transport instances
start new domains; reconnect authorization is M02b. This sequence check is not
cryptographic authentication or durable operation deduplication.

## TLS connection ownership and limits

`TlsListener::listen_loopback` and `TlsTransport::connect_loopback` take explicit
certificate/trust values. Only IPv4 loopback is available; port zero requests an
ephemeral listener port. Clients supply the expected certificate DNS name, but the
socket destination remains numeric loopback. No hostname resolution occurs.

All methods run on the creating thread and reject another thread. The application
must synchronize destruction and never race a call against object lifetime.
Accepted transports retain the listener's TLS configuration after listener close
or destruction. Library operations are serialized internally across connections.
Socket operations and `accept` are nonblocking. Only the caller drives polling.

| Resource | Default | Supported bound |
| --- | --- | --- |
| Incoming queue | 32 frames / 64 KiB | 1..64 frames; 4116..262144 bytes |
| Outgoing queue | 32 frames / 64 KiB | Same independent bounds; includes partial write |
| Accepted live connections per listener | 16 | 1..16 |
| Handshake + wire negotiation deadline | 5 s | Positive, at most 60 s |
| Idle deadline | 30 s | Positive, at most 300 s |
| Oldest queued write deadline | 5 s | Positive, at most 60 s |
| Steps per `poll` | 16 | 1..64 |
| Each PEM input | Explicit | Nonempty, <=64 KiB, no embedded NUL |
| TLS record content limit | 16384 bytes per direction | Fixed private library configuration; headers/overhead are additional |

The OS is asked for 32 KiB send/receive socket buffers; actual kernel accounting
is platform-dependent. This table bounds application storage and configured
socket requests, not total process RSS or a measured MMO memory budget. Crypto
allocation failure is reported where supplied by the library; general C++
allocation exhaustion is not a promised recoverable workload.

The caller supplies nonnegative, nondecreasing `MonotonicTime`. Regressed time and
invalid budgets reject without changing the connection. Deadlines are checked at
poll time, so an unpolled connection has no autonomous timer. Handshake progress
cannot extend the fixed connection deadline. Any socket-byte progress refreshes
idle time; an outgoing frame retains its original admission deadline even across
partial writes. Limit errors during send do not consume its sequence.

The state path is connecting -> handshaking -> negotiating -> ready. Accepted
connections begin handshaking. TLS verifies the server and, in M02b session mode,
the client certificate; then the client sends hello and the server returns
hello_ack automatically. Ready diagnostic clients can send probe, and servers
probe_reply. Ready session clients can send session_request, and servers
session_reply. Session clients also send replication_control; session servers
send replication_state. Negotiation still admits only hello/hello_ack.
Unsupported direction or mode/kind rejects. Player authorization
uses the separate [session authority](network-sessions.md). Incoming queue
pressure stops reads until the owner drains frames, preserving order and bounds.

`send` acknowledges local FIFO admission only. TLS/TCP reliability does not imply
peer application consumption, committed mutations or durability. It cannot supply
exactly-once effects after reconnect. `receive` transfers an owned value. Stats
record actual socket payload bytes including TLS overhead (excluding TCP/IP
headers), completed frames, queue sizes and steps in the last poll. Counters
saturate instead of wrapping; application-level bandwidth is a different metric.

`close` is an idempotent bounded local abort, dropping queued data and releasing
the socket. It does not wait for a TLS close-notify exchange. Abrupt peer EOF is
an error, never accepted as a complete authenticated stream. A valid peer TLS
close-notify still requires a complete wire frame.
Complete validated frames remain drainable after a clean peer close, including
when the final frame and close-notify arrive during one poll. Failed connections release
their listener admission slot and require a new transport to retry.

## Deterministic fault model

`FaultLink` models one unidirectional semantic message/datagram path using virtual
time and a fixed SplitMix64 mapping. Timestamped calls and seed reproduce delay,
jitter, loss, duplication, extra reorder delay and explicit partitions. This is
a test harness, not a reliability protocol or TLS byte-stream proxy. In
particular, arbitrarily deleting/reordering encrypted TCP bytes tests corruption,
not real IP loss recovery.

Default pending byte budget is 64 KiB. Hard ceilings are 8192 bytes per packet,
4096 pending packets, 32 MiB pending bytes and 1024 deliveries per drain. Creation
can lower each bound. Admission reports queued, loss drop, partition drop or
backpressure; duplicate copies are admitted atomically. Capacity pressure keeps
the random stream unchanged while advancing virtual time and pressure counters.
Invalid/deadline/allocation failure preserves state. Partition intervals `[now, until)`
stop delivery, preserve queued packets and drop new sends; `until == now` clears a partition.
Drain orders by due time then admission order with a bounded output count.

All calls, including stats, reject another thread. Tests use virtual time without
sleeps and compare independent golden values and a seeded 1000-step schedule.
Real socket fixtures use bounded wall-clock deadlines separately. The completed
[M02e qualification](network-qualification.md) combines semantic faults, shaped
encrypted transport and active replicated-state convergence.

## Verification and continuation

Unit families are `shark.core.net-wire`, `shark.core.net-fault` and
`shark.core.net-transport`, with seed 1337. The test-only NetworkProbe executable
and Python coordinator verify real local TLS processes. Test certificates and
keys are explicitly public fixtures; packages reject their presence.

M02a passes current-machine qualification: 404 CPU CTests and protected
dependency/package checks. See the [handoff](handoffs/M02a-transport-wire.md) and
[coverage receipt](coverage/evidence/M02a-transport-wire.json).
M02b's [handoff](handoffs/M02b-owned-sessions.md) records completed player
authentication and owned-session qualification. M02c [replication](network-replication.md)
is also locally qualified, with explicit public fields, baselines, deltas and bounded
resynchronization. M02d spatial interest/budgets and M02e integrated
qualification are complete. Full REQ-M02 is PASS under the current-machine
scope; see the [M02e handoff](handoffs/M02e-integrated-qualification.md) and
[checkpoint plan](roadmap/network-plan.md). Assignment 08 is next.
