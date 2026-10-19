# ADR 0014: Spatial interest, privacy and connection budgets

Date: 2026-10-18. Status: accepted, implemented and locally qualified in M02d.
Scope: this Windows PC and its existing WSL environment under
[ADR 0010](0010-current-machine-scope.md).

## Decision

Add a bounded, server-owned interest service in EngineCore above the existing
[replication publisher](0013-authoritative-replication.md). Reuse the Region
Scene, authenticated session authority and private TLS transport. The service
selects authorized relevant entities and paces publication through the same
replica mapping, baseline/delta codec and application ACK contract. No new wire
kind, component format, dependency or mutable scene is introduced.

Trusted server code supplies a bounded catalog with explicit `hidden`,
`public_pose` or `owner_pose` visibility. Hidden is the default. A catalog owner
uses the existing typed `CharacterId`; a recipient's identity comes from the
actual live authenticated session. An explicitly designated observer entity
provides that character's server-owned position. The player protocol supplies no
arbitrary observer position, catalog entry, visibility label or priority.

Authorization precedes spatial relevance. The local profile bounds the catalog
to 128 candidates and a recipient to 32 visible entities. Entry and exit radii
default to 100 and 110 scene-local meters. Hysteresis reduces boundary churn for
authorized entities; it cannot preserve authorization after privacy revocation.
Selection has an explicit bounded work budget and deterministic priority rules.
Eligible required rows rank first, followed by descending priority, retained
membership, distance and ascending handle. Required rows still need authorization
and spatial eligibility; an overfull required set rejects rather than dropping
one. A hidden observer can provide focus without appearing in public state.
World origins and transitions between authoritative regions remain M05.

## Privacy at the publication boundary

Revalidate current session ownership, observer binding and catalog authorization
before publication and before processing replication controls, including calls
that are otherwise too early to publish. Publication pacing must not delay an
authorization decision. The same policy applies to pending state as to a newly
captured selection.
Callers provide the complete current catalog to `publish` or `control` before
polling transport output following policy changes. Complete-policy validation
checks at most 128 rows/references; exposure maintenance checks at most 32
last-sent handles against that bounded catalog. Both have separate counters and
run outside the selection-work budget. Invalid policy or Scene state for an
existing stream also fails closed. The service retains no borrowed catalog or
Scene reference.
Failed calls do not authorize output polling: close/release or repair and
successfully revalidate before continuing, including after owner/time errors.

M02c's manual `prepare` API returns an owned pending update to its trusted caller.
The managed path instead owns selection, byte admission and the call to transport
`send`. It commits a pending revision only after successful FIFO admission.
No unbounded queue of obsolete snapshots is accumulated while a peer is slow.
The next eligible publication captures current state, coalescing intermediate
source changes before admission.
Manual and managed publication cannot be mixed in a session stream.

An existing queued or acknowledged pose can become private after it was admitted.
The service closes and fences a recipient whose retained publication is no longer
authorized, preventing another unsent publication or stale control from continuing
the stream. A fresh authenticated session is required for recovery. This cannot
recall bytes already accepted by TLS or the operating system, nor erase values
that the client legitimately received earlier. State this boundary explicitly;
privacy revocation is not a promise of retroactive secrecy.
Destroyed handles and ordinary spatial exits remain normal despawn transitions.

New managed state enters TLS only when its application output queue is empty.
ACK/resync still use the existing session sequence and replication fences. The
empty-queue rule prevents a replacement baseline from being admitted behind old
queued state after resync. It does not overtake bytes already written to the
ordered TCP stream. Pending application ACK, byte limits and queue pressure are
distinct results, not silent success.

## Pacing and measured transport limits

The initial profile publishes at most 10 times per second with a per-recipient
48 KiB/s framed-byte budget and 8 KiB burst allowance. One legal maximum delta
including its 20-byte outer header fits within the burst. The default per-call
work budget is 8192 explicit work units. These local limits bound the service;
they are not an assertion that arbitrary world density, simulation or transport
load has passed.

Count replicated payload bytes, outer framed bytes and actual socket bytes
separately. Socket counters include TLS overhead and exclude TCP/IP headers.
The gate measures per-recipient steady-state p95 at no more than 64 KiB/s
server-to-client and 16 KiB/s client-to-server, retaining join/burst measurements
separately. Do not replace these socket-byte gates with the smaller payload or
admission-budget counters.

Qualify an active local fixture with two authenticated client processes, 128
candidates, up to 32 visible active poses, a 20 Hz source and 10 Hz publication.
Use two seconds of warm-up followed by a 20-second measurement window. Report
actual sampling, work, byte counts, churn, pacing outcomes and queue behavior.
This exercises the network checkpoint; it is not the full 16-player/NPC
LOCAL-MMO-1 simulation, durable-operation or endurance workload.

Measure head-of-line behavior using a bounded real TCP relay that holds bytes for
100 ms and shapes transfer to 8 KiB/s. Preserve byte order and TLS end-to-end;
the relay does not decrypt messages, invent application ACKs or corrupt/drop TLS
bytes and label that packet loss. Record queue limits, delay and recovery on this
machine. Full seeded loss/jitter/reorder/partition and integrated adversity remain
M02e. If measured gates fail, investigate the transport or implementation instead
of changing the acceptance limits silently.

## Consequences

Human-written integrations and terminal-agent fixtures call the same owner-thread
C++ service and inspect bounded structured results. There is no required editor,
GPU, LLM SDK, new CLI or player-facing authoring endpoint. Test catalogs, relay
executables and public credential fixtures stay outside installed Runtime/Server
packages. The private TLS and EnTT boundaries, central GLM configuration and
existing license closure remain unchanged.

The [interest contract](../network-interest.md) owns the API, operational outcomes
and limits. M02d qualification passes 460 CPU CTests on Windows Debug/Release/ASan,
WSL CPU and Server Debug/ASan+UBSan, active local processes and protected packages.
The [handoff](../handoffs/M02d-spatial-interest.md) and
[receipt](../coverage/evidence/M02d-spatial-interest.json) retain exact results
and superseded attempts.
Full REQ-M02 remains NOT_RUN. No production capacity, WAN result or support for an
additional machine follows from this decision.
