# M02: Sessions, transport and replication checkpoints

Date: 2026-10-18. Base: `3c5767a` on `main`, initially clean.
Owner: `REQ-M02`. **M02a is complete under the current-machine scope.**
The [handoff](../handoffs/M02a-transport-wire.md) and
[receipt](../coverage/evidence/M02a-transport-wire.json) record 404 passing CPU
CTests and protected dependency/package qualification. **M02b is complete**: its
[handoff](../handoffs/M02b-owned-sessions.md) and
[receipt](../coverage/evidence/M02b-owned-sessions.json) record 425 passing CPU
CTests and protected package checks. **M02c replication is next.**
Full M02 remains NOT_RUN until all five checkpoints pass. Assignment 07 is complete.
The [MMORPG plan](mmorpg-plan.md) retains the assignment order and the
[current-machine scope](../architecture/0010-current-machine-scope.md).

## M02a — Transport and wire foundation

Select and pin maintained transport/security components with a threat assessment.
Implement explicit gameplay-wire versioning and bounded parsing, exact sequence
progression, nonblocking local connections, resource/deadline limits and a seeded
virtual-time fault model. Use real separate processes to verify TLS and wire
negotiation. No player intents are available before authentication lands.

Gate: Windows and existing WSL CPU/sanitizer tests, independent wire golden bytes,
fragmentation/coalescing/malformed input, zero/wrapped/replayed sequences, queue
pressure, owner/lifetime checks, trusted and rejected TLS handshakes, two real
clients exchanging bounded diagnostic messages through one real server fixture,
connection refusal, stalled handshake and abrupt disconnect. Record exact source,
dependency/license and package closure. A message-level fault model does not
establish packet-loss behavior of encrypted TCP; keep this distinction explicit.

Follow [ADR 0011](../architecture/0011-network-foundation.md) and the
[transport contract](../network-transport.md). This checkpoint has no authenticated
player, replicated entity, persistence, authoring RPC or playable networked client.

## M02b — Authentication and owned sessions

The selected design is [mutual TLS and explicit enrollment](../architecture/0012-owned-sessions.md),
with the [session contract](../network-sessions.md) defining ownership, bounds,
expiry, revocation and reconnect fencing. Qualification is PASS under the
current-machine scope, including 19 real-process scenarios in every full profile.

Build local player identity/session services on the selected TLS/X.509 foundation.
Define account/session/character ownership separately from sockets and scene slots.
Add bounded typed intents, expiry, revocation, reconnect authorization, rate limits
and explicit rejection of forged ownership, replay and incompatible versions.
No shipping test-auth bypass, external identity provider or paid service.

Gate: actual local processes authenticate distinct identities; invalid, expired,
revoked and wrong-owner credentials fail closed, and reconnect never revives stale
authority. Session controls do not grant source-authoring or administrative access.
Durable account/character storage remains M03.

## M02c — Authoritative replication and resynchronization

Add explicit replicated-field allowlists, stable session replica identities,
baseline/delta state, spawn/despawn, acknowledgements and bounded reconnect
resynchronization using the existing scene owner. Player requests cannot supply
authoritative snapshots. Authoring schemas are not a gameplay wire schema.

Gate: two real clients converge on active server-owned fixture changes. Missing
baselines, stale acknowledgements, repeated messages and reconnects have defined
outcomes; private fields never appear in a baseline or delta. Critical effects
must not rely on TCP receipt as an application commit acknowledgement.

## M02d — Spatial interest, privacy and connection budgets

Add server-owned spatial selection, entry/exit hysteresis, priorities and measured
per-connection byte/work budgets. Replaceable snapshots may be coalesced before
entering reliable FIFO transport; they cannot overtake already transmitted bytes.
Measure TCP head-of-line costs before claiming the selected transport satisfies
the local workload. Revisit the transport with evidence if the budgets fail.

Gate: only authorized relevant entities/fields are transmitted, private data stays
private, crowded selection/churn remain bounded and active entity updates produce
recorded wire/socket-byte and replication-work measurements. No MMO capacity claim
follows from idle sessions or an echo fixture.

## M02e — Integrated qualification

Close full REQ-M02 with real-process convergence and deterministic adversity:
loss, jitter, reordering, duplication, partitions, reconnect, sequence exhaustion,
slow readers, malformed/oversized input, forged ownership and revoked credentials.
Inject semantic message faults at the appropriate layer and shape actual local
transport through an explicit harness. Do not corrupt random TLS stream bytes and
label the result a packet-loss test. Define and measure memory/queue/history bounds.

Run the full current-machine CPU/sanitizer, protected build and package gates.
Record the achieved workload and remaining limits. Tests accompany every earlier
checkpoint; this final checkpoint integrates their evidence. Assignment 08 follows
completed M02; persistence, combat/prediction and seamless region transfer retain
M03, M04 and M05 respectively.
