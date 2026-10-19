# Spatial interest, privacy and connection budgets

M02d adds a server-owned CPU interest and publication path above the existing
[replication contract](network-replication.md), following
[ADR 0014](architecture/0014-spatial-interest.md). It reuses the Region Scene,
[owned sessions](network-sessions.md) and [TLS transport](network-transport.md).
M02d is qualified under the current-machine scope. Full REQ-M02 remains NOT_RUN;
the [checkpoint plan](roadmap/network-plan.md) retains M02e integrated adversity.

## Trusted catalog and recipient selection

The server supplies a bounded entity catalog. Each entry has an explicit
visibility policy: `hidden`, `public_pose` or `owner_pose`. Unclassified entities
are hidden. `public_pose` permits the existing public pose field to authorized
spatial selection; `owner_pose` additionally requires the entry's `CharacterId`
to match the current recipient's character. Visibility does not expose names,
hierarchy, durable IDs, local components or private scene revisions.

The recipient is obtained from the actual authenticated server-side transport
through `SessionAuthority`; copied session or peer values cannot authorize it.
Exactly one live catalog row designated `observer` must have an `owner` matching
the session character. Other rows may share that owner. A hidden observer supplies
the focus without publishing its pose. Clients cannot choose an arbitrary camera or
query point to discover another area. Catalog membership, ownership, observer
designation and priorities are trusted application inputs, not player messages.

Capture values through the Scene's owner and checked handles. All selection
belongs to one live Region Scene incarnation and its committed state. A catalog
is not a second authoritative registry or a serialization of the authoring
schema. Reject invalid, stale, foreign or ambiguous bindings before publishing.
Names and other nonreplicated fields never determine policy implicitly.

The local profile admits at most 128 candidates and 32 visible replicas per
recipient. Entry radius defaults to 100 scene-local meters and exit radius to
110 meters. Previously selected authorized entities use the exit threshold;
new entrants use the entry threshold. Authorization is independent of these
thresholds: hidden or wrong-owner state cannot stay visible through hysteresis.
Both radii are inclusive. Eligible rows rank by required membership first, then
descending `priority`, retained membership, nearest distance and ascending local
handle. `required` does not bypass authorization or spatial eligibility; more
eligible required rows than the visible limit rejects the candidate instead of
silently omitting one. Finite validated positions and this deterministic order
make crowded selection bounded and repeatable. Global cell/origin reconciliation
and region transfer remain M05.

## Privacy maintenance and lifecycle

Revalidate the live session, observer and complete current privacy policy before
each publication attempt and control call. This maintenance runs even when publication
is blocked by pacing, a byte budget, queue pressure or an outstanding ACK. A
rate limit must not turn a revoked permission into continued publication.

A recipient may have an acknowledged or pending pose whose authorization was
subsequently revoked. A newly forbidden live last-sent entity closes the actual
transport, revokes its exact session and forgets its stream, including after ACK.
A later login
must establish a new authenticated session and publication domain. Closing
discards application-queued output, but cannot recall data already accepted by
TLS/the OS or previously received by the client. Privacy assertions concern
authorization at admission and prevention of subsequent unauthorized publication,
not deletion of historical client knowledge.
An invalid catalog or Scene for an existing stream also closes its transport,
revokes the exact session and forgets the stream; malformed trusted policy cannot
keep an earlier publication authorized implicitly.

Call `publish` or `control` with the current complete catalog before polling
transport output after a policy change. The service retains no borrowed catalog
or Scene reference and cannot observe external policy edits without that call.
If either call returns a failed `Result`, stop polling output until the caller
has closed and released the connection or repaired the input and successfully
revalidated it. Owner or time errors can reject a call before policy validation;
an error never renews permission to flush previously queued state.

Spatial exit, destroyed handles and priority displacement use the existing
spawn/despawn lifecycle rather than triggering privacy closure.
Replica IDs, stream IDs, session epochs and revision fences retain the M02c
contract. A replacement connection cannot adopt an old stream or consume its
controls. Release ended recipient state with the exact session identity; stale
teardown must not remove a replacement. `forget` is ended-session cleanup;
do not discard a live stream's privacy history while leaving its transport open
and eligible for output polling.

## Managed publication and backpressure

`ReplicationServer::publish` is the managed admission boundary. It combines a
validated candidate, payload encoding, byte allowance and transport `send`, and
commits pending replication state only after successful local FIFO admission.
The low-level manual `prepare` path remains a trusted primitive; callers using
it directly still own their selection/privacy and enqueue decisions.
Do not mix manual `prepare` and managed `publish` on one session stream.

Managed publication requires an empty TLS application output queue. This avoids
putting a resync baseline behind an older queued state update. It cannot overtake
bytes already written into a reliable ordered TCP stream. One pending application
update remains the replication-history bound; neither slow readers nor repeated
calls allocate an unbounded set of snapshots.

The replication admission result distinguishes `sent`, `unchanged`, `pending_ack`,
`byte_limited` and `queue_limited`. `InterestServer` adds `cadence_limited`,
`work_limited` and `privacy_closed`. Its owned result reports selected entities,
eligible and omitted entity counts, framed bytes, records and explicit work
counters. `sent` means local admission
only. It is not peer consumption,
application ACK, durable commit or a guarantee of delivery. The caller continues
nonblocking transport polling and drains bounded received frames on the owner
thread. Existing ACK/resync validation and the common session command sequence
remain authoritative.

Source changes that occur while no new state can be admitted are coalesced: the
next eligible publication reads current values. Do not repeatedly enqueue a
retained pending snapshot, accumulate per-tick copies or create a second sequence
space. ACK timeout, invalid ownership and malformed controls remain explicit
failures; queue and byte pressure are ordinary bounded deferral outcomes.
Resynchronization preserves the recipient's publication cadence. An attempt that
exhausts the selection-work budget also consumes its cadence interval while
leaving selection, publication revisions and replica IDs unchanged.

## Limits and accounting

| Profile setting | Default | Accepted range |
| --- | --- | --- |
| Candidate catalog | 128 entities | 1..128 |
| Visible selection | 32 entities per recipient | 1..32, through `replication.entities_per_stream` |
| Recipient streams | 16 | 1..64, through `replication.streams` |
| Entry / exit distance | 100 / 110 scene-local meters | Finite, positive entry; entry <= exit <= 1,000,000 |
| Publication period | 100 ms (10 Hz) | 1 ms..1 s |
| Framed-byte rate | 48 KiB/s per recipient | 1..1,048,576 bytes/s |
| Framed-byte burst | 8 KiB per recipient | 3762..1,048,576 bytes |
| Per-selection work budget | 8192 work units | 1..65,536 |
| Replication history | One acknowledged state and one pending update | Fixed |

The byte allowance includes the 20-byte outer frame header. The maximum legal
M02c update is 3742 payload bytes, or 3762 framed bytes, and fits within the
default burst. Baselines and despawn-heavy replacement updates must remain
admissible; a budget must not silently discard required lifecycle records.
Configured limits are validated, and counters/time arithmetic must not wrap.

The nested `ReplicationLimits` also owns ACK deadlines and control/resync limits;
there is no second visible-set or stream limit. Token budgets debit the exact
outer-framed byte count only on admission. A rejected or deferred candidate does
not allocate a new stream revision or consume replica IDs.

Keep three measurements separate: replication payload bytes, framed admission
bytes and actual transport socket bytes. The last includes TLS overhead but
excludes TCP/IP headers. `InterestWork::selection_units()` sums catalog rows,
catalog comparisons, checked Scene reads, distance tests and ranking comparisons.
Scene scope/revision probes use identity-only queries with a one-slot scan budget,
including for empty scenes or a hole in the first slot; they do not scan scene
statistics. Policy and privacy maintenance are separate and unthrottled. `policy_rows`,
`policy_scene_reads` and `policy_comparisons` record full-catalog validation:
at most 128 rows and checked references, with two bounded merge sorts and adjacent
checks. `privacy_checks` covers at most 32 last-sent handles, each with a bounded
catalog scan and checked Scene read. A small selection budget cannot suppress
this fail-closed maintenance. Report these counters alongside elapsed work;
they are not a platform-independent CPU-time guarantee.
`InterestStats` provides owned aggregate stream, entity, publication, byte, record,
selection, policy, omitted-entity, coalescing and privacy-closure counters.

## Public owner-thread API

[interest.hpp](../engine/core/include/shark/net/interest.hpp) exposes
`InterestServer::create`, `publish`, `control`, `forget` and `stats`. The service,
Scene, session authority and transport remain on their owning thread. Read-only
selection captures committed world transforms; it does not mutate the Scene or
retain component pointers. `Result<T>` carries explicit failures using the
[Core error contract](core-contracts.md); invalid configuration, malformed or
ambiguous catalogs, invalid authority and stale controls are failures rather than
successful empty selections. Ordinary pacing/backpressure and privacy closure
have the distinct publication outcomes above. `forget` uses an exact `SessionId`
so stale cleanup cannot remove a replacement session's state.
Interest errors use `net.interest.*` operation names for create, owner, time,
session, Scene, catalog, observer, required-selection, capacity, control, privacy
and publication checks. Underlying Scene/session/replication failures preserve
their owned diagnostics.

This complete translation unit shows a three-entity trusted catalog. A real
application supplies its whole current catalog every call. The handles and owner
below must come from server state; player messages never set these policy values.
Create the `InterestServer` with validated limits at region startup, retain it on
the region owner, and pass authenticated server-side transport/session services.

```cpp
#include "shark/net/interest.hpp"

#include <array>

shark::Result<shark::net::InterestPublishResult> publish_interest_example(
    shark::net::InterestServer& interest, shark::Scene& scene,
    shark::EntityHandle observer, shark::EntityHandle public_actor,
    shark::EntityHandle private_actor, shark::net::CharacterId character,
    shark::net::SessionAuthority& sessions, shark::net::TlsTransport& transport,
    shark::MonotonicTime now) {
    using shark::net::InterestEntry;
    using shark::net::InterestVisibility;
    const std::array<InterestEntry, 3> catalog{{
        {observer, InterestVisibility::hidden, character, 0, false, true},
        {public_actor, InterestVisibility::public_pose, std::nullopt, 10, false, false},
        {private_actor, InterestVisibility::owner_pose, character, 20, false, false},
    }};
    return interest.publish(scene, catalog, sessions, transport, now);
}
```

Handle the returned outcome before polling output, especially `privacy_closed`.
Decode received replication controls through the existing codec and send them to
`InterestServer::control` with that same current catalog and actual transport.
The managed control path applies privacy maintenance before existing ACK/resync
validation. Failed controls do not consume the common session command sequence.

## Local measurement and head-of-line fixture

The qualified active fixture has two distinct authenticated client processes and
real loopback TLS connections. It exercises 128 candidates, up to 32 visible
active poses, 20 Hz source mutation and 10 Hz publication, with two seconds of
warm-up and a 20-second measurement window. Report each recipient separately,
including authorized/private field checks, crowded selection, priority/membership churn,
control traffic, coalescing and actual bytes. Join/burst traffic is retained
separately from steady-state samples and bounded to 32 KiB per direction. Each
recipient must apply at least 190 useful updates during measurement and converge
at source tick 440. A deliberate 175 ms warm-up ACK hold checks that authoritative
source updates continue while the pending publication coalesces.

The fixed steady-state gates are per-recipient p95 at most **64 KiB/s down** and
**16 KiB/s up**, measured using actual socket-byte deltas. Record sample duration,
window count and percentile method with the receipt. An admission budget passing
does not establish either gate by itself. This fixture is a network checkpoint,
not the full 16-player/NPC LOCAL-MMO-1 workload, two-region simulation, durable
operation schedule or endurance soak.
Use nearest rank over 20 complete windows, normalizing each socket-byte delta by
that window's measured elapsed seconds. Report actual selection-call wall-time
p95/p99 separately from the much more frequent cadence-limited maintenance calls.
The process fixture bounds transport queues to four frames and 16 KiB, records
the full 128-row policy checks and at most 32 exposure checks, and verifies all
sessions/streams are released.

A separate bounded TCP relay introduces a 100 ms hold and 8 KiB/s shaping to
measure ordered-stream head-of-line delay and recovery. TLS remains end-to-end;
the relay forwards opaque bytes in order and does not fabricate protocol results.
Its queue, deadline and child-process limits must be recorded. Holding or pacing
these bytes is not a packet-loss/reorder simulation. M02e owns the complete
seeded adversity matrix and integrated failure qualification.
The relay buffers at most 16 KiB in each direction and retains received/forwarded
byte totals. Its observed shaped-byte count must fit the 8 KiB/s token schedule
after the 100 ms hold, allowing eight bytes for timing/rounding. The affected
state/control exchange must complete in 300..3000 ms, with the other recipient
making progress independently. The fixture retains unshaped control round-trip
and state-ACK references. A 40-second child watchdog, 45-second process limit and
120-second CTest limit bound failures.
These measurements expose ordered-stream delay on this machine; they do not
establish suitability for combat latency. Integrated network and gameplay timing
retain their M02e and M04 gates.

## Integration and qualification status

The public C++ interface and managed
[replication admission](../engine/core/include/shark/net/replication.hpp) share
the existing EngineCore boundary. The example's independent public-header compile
check passes with warnings treated as errors. Human applications and terminal-agent fixtures use this same
CPU path and owned structured results; no GUI, LLM SDK, premature CLI adapter or
new player authoring permission is introduced.

Qualification passes all 460 CPU CTests across Windows Debug/Release/ASan, WSL CPU
and Server Debug/ASan+UBSan, including active authenticated processes. Protected
builds and isolated packages also pass. The [handoff](handoffs/M02d-spatial-interest.md)
and [receipt](coverage/evidence/M02d-spatial-interest.json) record measured ranges,
source hashes and the superseded attempts. Existing TLS, session and replication
regression evidence is refreshed against the final frozen sources.
Runtime/Server packages retain the existing private static
TLS and EnTT boundary, central public GLM and exact license notices; test keys,
probe executables and relay tools are not installed. No new dependencies,
hardware, public certificate service or hosted runner are required.
