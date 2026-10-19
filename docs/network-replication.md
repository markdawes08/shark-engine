# Authoritative replication and resynchronization

M02c adds CPU C++ replication in
[replication.hpp](../engine/core/include/shark/net/replication.hpp), following
[ADR 0013](architecture/0013-authoritative-replication.md). It extends the
[transport](network-transport.md) and [owned sessions](network-sessions.md).
M02c qualification is PASS under current-machine scope; its
[handoff](handoffs/M02c-authoritative-replication.md) and
[receipt](coverage/evidence/M02c-authoritative-replication.json) record 446 passing
CPU CTests and protected packages. Full REQ-M02 remains NOT_RUN. The
[checkpoint plan](roadmap/network-plan.md) retains later spatial-interest and
integrated adversity gates.

## Publication and privacy

Create `ReplicationServer` on the thread that owns the Region Scene,
`SessionAuthority` and server-side mutual-TLS transport. Call `prepare` with an
explicit span of live entity handles selected by trusted server policy. The
selection must fit the configured limit and contain no duplicate, stale or
foreign handle. A closed or non-Region Scene rejects. Inspection reads committed
state through the existing Scene API, including when deferred writes are pending;
it does not flush queued mutations.

Only public pose field 1 is replicated: the upper 3x4 of each entity's validated
scene-local world matrix, in row-major coefficient order. Translation occupies
indices 3, 7 and 11; the implicit last row is `[0, 0, 0, 1]`. Coefficients use
the shared [transform validator](transforms.md). Finite affine matrices may
include hierarchy-induced shear; they are not decomposed into a lossy local TRS.
`canonicalize_replicated_pose` normalizes signed zero before wire encoding.
Wire input containing negative zero, nonfinite values or an invalid basis rejects.

Names, local TRS, hierarchy, authored/world IDs, Scene handles and private Scene
revisions never enter this schema. Account and character IDs are not state fields.
The session/epoch/stream envelope identifies this recipient's replication domain.
Private-only Scene changes produce no delta when the selected public poses are
unchanged. The source schema catalog never determines visibility automatically.
The trusted selection must itself be authorized; M02d's
[interest service](network-interest.md) adds spatial selection and recipient-policy
enforcement. Scene-local poses do not supply global MMO origin
or region-transfer semantics, which remain M05.

## Streams, revisions and recovery

M02d also adds `ReplicationServer::publish`, the managed capture/encode/send
boundary used by `InterestServer`. It commits IDs, revisions and ACK state only
after successful TLS FIFO admission, charges the complete outer frame against
the supplied allowance, and returns explicit pending/byte/queue outcomes.
For `byte_limited`, its `framed_bytes` reports the candidate's required allowance;
no bytes were admitted. `sent` reports the actual admitted frame size.
New state requires an empty application output queue, including after resync.
Unsent candidates can therefore use current Scene values without replacing or
overtaking already admitted bytes. Manual `prepare` and managed `publish` cannot
be mixed within one session stream. The [interest contract](network-interest.md)
defines recipient policy, pacing and privacy maintenance before output polling.

Each publication revalidates the actual transport through
`SessionAuthority::current(transport, now)`. That call checks current identity
and lease expiry without spending a command sequence or extending the lease.
Replication additionally requires readiness. A stream binds one authenticated
session, ownership epoch and Scene incarnation. Its generated `ReplicaSessionId`
is not a durable ID or authority credential.

Replica IDs start nonzero and increase without reuse or wrap. A retained entity
keeps its stream-local ID. Once an issued state removes an entity, later
republishing it allocates a newer ID; local slot reuse cannot resurrect an old
replica. Revisions similarly
increase without wrap. The publisher maintains one acknowledged state and at
most one immutable pending update. `prepare` returns that pending value until ACK,
or `nullopt` when acknowledged public state is unchanged. The caller must avoid
continually enqueueing the same pending update into the bounded transport FIFO.

A new stream starts with a full baseline, including an empty baseline for an
empty selection. Subsequent deltas use the acknowledged revision as their base
and contain only spawn, changed-pose and despawn records. An expired pending ACK
deadline rejects further publication and a late ACK of that pending revision.
The owner must explicitly resynchronize or tear down the stream.

An admitted resync drops the pending update and makes the next publication a
newer full baseline in the same stream. It retains the last-issued revision and
identity fences. `known_revision` may be zero but cannot claim a revision beyond
the server's last issued value. A superseded pending ACK cannot acknowledge the
replacement. An ACK for the latest already-acknowledged revision is an idempotent
duplicate, subject to fresh session command admission.

Reconnect creates a fresh session and stream. The old account stream is replaced
only after a valid candidate has been prepared, reclaiming its bounded slot.
Call `forget(old_session_id)` when its owner ends/disconnects; an old ID cannot
remove the replacement. Publication never extends session lifetime merely by
sending state. Call session expiry during idle periods and explicitly reclaim
ended streams; no background timer or cleanup thread is created.

## Client cache and controls

Construct `ReplicaClient` from this connection's accepted active, ready
`SessionSnapshot`. Feed it decoded state received from the verified TLS server.
A copied snapshot is only cache initialization data and grants no server-side
authority. A fresh login uses a fresh cache.

`apply` validates the full candidate before replacing its owned snapshot. It
returns `applied`, `duplicate` for an exact repeat of the latest accepted update,
or `resync_required` for a missing/mismatched baseline. Conflicting or older
revisions, foreign session/epoch/stream, invalid lifecycle operations and capacity
overflow reject without partial publication. A newer baseline may skip revisions;
it cannot change the stream within a session or revive a retired replica ID.

After `discard_baseline`, values are absent but stream, revision and lifecycle
fences remain. Even an exact repeat of the last update yields `resync_required`;
only a newer full baseline restores the cache. `snapshot` returns an owned copy
with explicit `has_baseline`, stream and revision. Presentation consumes that
copy, without mutating the authoritative Scene.

Send ACK only after successful application, or for an exact duplicate while the
baseline remains present. Send resync after `resync_required`. Both controls carry
the received update's validated stream identity, including when the local cache
has not yet accepted a baseline. They also carry
the session's exact-next application sequence, shared with SetReady, Heartbeat and
Logout. Keep one sequencer for that session; outer frame sequence and replication
revision are different counters. Do not treat sending a control as evidence that
the server admitted it, and do not blindly retry an ambiguous sequence.

`ReplicationServer::control` checks the live transport/session and stream, spends
its bounded control budget, then validates control semantics before calling the
existing Heartbeat admission path. An admitted control consumes the common
session sequence/rate budget and refreshes its idle lease. Invalid controls can
spend their separate control/resync budget, but cannot acknowledge state, advance
the accepted session sequence or refresh the lease. An application ACK proves
replica consumption only; durable gameplay outcomes require M03's transaction
contract.

## Limits and wire version 1

| Resource | Default | Supported bound |
| --- | --- | --- |
| Live replicas per stream | 32 | 1..32 |
| Retained streams | 16 | 1..64 |
| Pending updates per stream | 1 | Fixed |
| ACK deadline | 5 seconds | Positive, at most 60 seconds |
| Controls per window | 64 per second | 1..65536; positive window at most one hour |
| Resyncs per window | 4 per 10 seconds | 1..64; positive window at most one hour |
| Baseline records / payload | At most 32 spawns | 3454 bytes maximum |
| Delta records / payload | At most 32 pose records plus 32 despawns | 64 records / 3742 bytes maximum |
| ACK or resync payload | 76 bytes | Exact |

These bound application state and message work; they are not measured CPU/RSS,
frequency, bandwidth or production-capacity guarantees. The 32-entity cap matches
the current LOCAL-MMO-1 visible-entity target without claiming its load gate.

Outer wire version 1 adds client-to-server `replication_control = 7` and
server-to-client `replication_state = 8`, accepted only in mutual-TLS session
mode after negotiation. Sending state from a client rejects. The existing 20-byte
outer header and 4096-byte payload bound remain unchanged.

Each replication payload begins with version u16 big-endian, kind u8 and
reserved-zero u8. Update kind 1 is baseline and kind 2 is delta. The 62-byte
update header continues with session UUID, ownership epoch u64, stream UUID,
revision u64, base revision u64 and record count u16. Baselines require base zero;
deltas require a nonzero lower base and at least one record. Revision and epoch
are nonzero. Records are strictly increasing by nonzero replica ID.

Each record is kind u8 (`spawn = 1`, `update = 2`, `despawn = 3`) and replica
ID u64. Spawn/update add a one-byte mask exactly `1`, followed by twelve IEEE754
binary64 coefficients. Despawn has no pose. Controls use kind 1 for ACK and 2
for resync, followed by the session envelope (session UUID, epoch u64, sequence
u64, character UUID), stream UUID and revision u64. ACK revision is nonzero.
All integers and floating-point bit representations use big-endian order; UUID
bytes follow canonical display order. Native structs and GLM memory are never
serialized. Unknown versions/kinds/masks, invalid UUIDs, invalid counts, truncation
and trailing bytes reject. There is no implicit migration from another payload
version.

## CPU integration example

Link EngineCore and include the public header. The following functions prepare
owned payload values and apply decoded state; the caller retains transport
polling, FIFO admission, session sequencing and ACK/resync decisions.

```cpp
#include "shark/net/replication.hpp"

#include <optional>
#include <span>
#include <utility>
#include <vector>

shark::Result<std::optional<std::vector<std::byte>>> prepare_public_state(
    shark::net::ReplicationServer& publisher, shark::Scene& scene,
    std::span<const shark::EntityHandle> authorized_entities,
    shark::net::SessionAuthority& sessions, shark::net::TlsTransport& transport,
    shark::MonotonicTime now) {
    auto update = publisher.prepare(scene, authorized_entities, sessions, transport, now);
    if (!update) return update.error();
    if (!update.value()) return std::optional<std::vector<std::byte>>{};
    auto bytes = shark::net::encode_replication_update(*update.value());
    if (!bytes) return bytes.error();
    return std::optional<std::vector<std::byte>>{std::move(bytes).value()};
}

shark::Result<shark::net::ReplicaApplyResult> apply_public_state(
    shark::net::ReplicaClient& cache, std::span<const std::byte> payload) {
    auto update = shark::net::decode_replication_update(payload);
    if (!update) return update.error();
    return cache.apply(update.value());
}
```

## Errors, ownership and qualification

Objects are noncopyable/nonmovable and owner-thread checked. Synchronize object
destruction and never race access against lifetime. Returned updates and snapshots
own their values; mutating a copy does not mutate service state. Monotonic time
must be nonnegative and nondecreasing; deadline overflow rejects. Expected errors
use owned `Result`/`Error` values. Match `error_code_name` and operation context,
not human diagnostic text.

Invalid limits return `invalid_configuration`; malformed protocol values,
invalid selections and regressed time use `invalid_argument`. A stale or foreign
selected handle uses `invalid_id`. Owner, session, stream, revision, capacity,
ACK-timeout and rate conflicts use `conflict`. Operational allocation failures
caught by the services use `execution_failed`; this is not a general recovery
guarantee for process memory exhaustion. Operations identify the failure under
`net.replication.*` or `net.replica.*`. Keep detailed server errors in trusted
diagnostics; they are not a new player-visible rejection wire schema.

The three CTest families are `shark.core.net-replication-wire`,
`shark.core.net-replication` and `shark.core.net-replication-process`.
Qualification passes across Windows Debug/Release/ASan, WSL CPU/Server
Debug and ASan/UBSan, real local authenticated client/server processes, and
protected standalone packages. Public fixture keys stay under tests and are
excluded from those packages. No graphics rerun, additional machine, public
certificate, cloud service or hosted runner is required for this CPU checkpoint.
Human and terminal-agent development use these same services; authoring CLI,
editor adapters and later gameplay operations retain their assigned owners.
