# ADR 0007: Seamless open-world MMORPG direction

Date: 2026-10-17. Status: accepted product direction and planning amendment.
The user selected an MMORPG engine and **seamless open world**. All MMO runtime
behavior below is planned; M01 implements only the target split and idle Server
lifecycle described in its [handoff](../handoffs/M01-headless-server.md). This amendment replaces
the offline-only release assumptions in ADRs 0001-0006; unaffected client graphics,
tooling and platform requirements remain. The [execution plan](../roadmap/mmorpg-plan.md)
and [requirements manifest](../coverage/requirements.json) assign the work.

## Product boundary

Shark is an engine and authoring toolchain for a persistent multiplayer world,
with an authoritative dedicated server and graphical clients. Deliver a small
playable MMORPG reference world to prove the engine. A commercial game's content,
monetization, customer support and global hosting operation are separate product
work; the engine still needs tested deployment, recovery and observability hooks.

Humans and terminal LLM agents author that world through the shared development
contract in [ADR 0008](0008-human-agent-authoring.md). Terminal reachability extends
to world/cell, character, script and reference-game content as those features land.
Development tools do not acquire gameplay or production administration authority;
M09 must prove both terminal creation and human continuation of the same world.

Seamless means normal travel through adjacent world regions keeps the same
character, session and continuous world view, without a loading screen, forced
reconnect or scene reset at an internal cell boundary. A process failure may cause
a visible recovery state; it must not silently corrupt or duplicate durable state.
Optional explicit teleports/instances may exist later, but cannot substitute for
the continuous-world acceptance case. Seamlessness does not promise an infinite
world, unlimited players, one global process or zero latency across continents.

Use internal spatial cells for content streaming and regions for simulation
ownership. Their sizes and boundaries need not match. A realm/shard is a separate
persistent world population; capacity and realm topology remain measured product
decisions. Start with adjacent regions in one process, then prove the same
ownership contract across processes. Do not build a general distributed actor
framework, Kubernetes platform or service mesh as a prerequisite.

## Authority and target boundaries

[ADR 0001](0001-targets-and-boundaries.md) defines the implemented `EngineCore`,
`EngineClient` and `Server` separation. Engine is an interface client composition;
EngineTools, Editor, Runtime and Tests remain. Server must build,
start and test without GPU/display/audio support or their SDK/download dependencies.
The initial server qualification target is Ubuntu 24.04 x86-64, separately from
the Windows/Linux client and editor hardware matrix. The later user-directed
[ADR 0009](0009-windows-linux-platform-scope.md) removes Mac requirements without
changing server authority, seamless-world behavior or the remaining platform gates.

Server owns position acceptance, collision, combat, cooldowns, AI, inventory,
currency, loot, progression and persistent world mutations. Clients send bounded,
typed intents with session identity, sequence and relevant tick/operation IDs;
they cannot submit authoritative component snapshots, item grants or timestamps
that override server time. Client prediction is presentation of an unconfirmed
result. Server corrections and durable transaction outcomes remain authoritative.

Keep three trust surfaces separate:

| Surface | Allowed behavior |
| --- | --- |
| Local editor/CLI/JSON-RPC | Trusted development authoring transactions, undo and source edits; never a public game endpoint |
| Player connection | Authenticated session, owned-character intents, bounded queries and selected replicated state |
| Operational administration | Separate authenticated and authorized access, narrowly scoped commands, audit records and rate limits; never arbitrary Lua/source upload from a player connection |

Authentication, encryption, transport and database libraries must be selected and
pinned at their integration gates. Reuse maintained protocols and cryptographic
implementations; do not invent cryptography. No specific cloud, database or network
library is selected by this amendment. The logical services may initially be
modules in a few processes; a separate microservice per responsibility is not a
requirement. Tests use explicit isolated identities and data, never production
credentials or a hidden authentication bypass in a shipping configuration.

## Simulation, replication and interest

[ADR 0005](0005-simulation-and-rendering.md) separates server ticks, client
prediction, network snapshots, animation and rendering. The initial server tuning
point is a fixed **20 Hz**, configurable per world profile and recorded in protocol
negotiation and evidence. This is provisional, not a decision about combat style.
Choose physics substeps and input/history windows from movement/combat tests;
do not assume Jolt is bitwise deterministic between machines or use client lockstep.

Each authoritative region has one writer and a monotonic integer tick sequence.
Network, database and worker completions enter bounded queues with ownership epoch
and request context. They do not mutate a registry directly. Server scheduling
does not depend on rendering, client focus, minimize or pause. Overload must produce
measured lag, backpressure and admission control, not silent dropped authoritative
ticks or enlarged physics steps. Offline editor play can retain local pause/step.

M02 establishes explicit versioned messages, authentication/authorization,
bounded parsing, disconnect/reconnect, snapshots and interest filtering. M04 adds
owned-character prediction/reconciliation, remote interpolation and validated
movement/combat. Separate reliable state-changing operations from replaceable
snapshots; define ordering, acknowledgement, retry, deduplication and timeout
semantics per message class. Test loss, reorder, duplicates, jitter, reconnect,
slow readers, sequence wrap and incompatible versions. Avoid unbounded history
buffers, queues, catch-up work or packet amplification.

Interest management is a server-side spatial query with ownership/privacy rules,
entry/exit hysteresis, priority and a per-connection byte/CPU budget. Do not broadcast
every entity to every player or reveal hidden inventories, private messages or
unseen authoritative state merely because it exists on the server. Capture actual
bytes on the wire, baseline/delta costs, spawn/despawn churn and crowded hot spots.
Density must not silently remove gameplay-relevant nearby actors; overload policy
and quality degradation need explicit observable behavior.

## Persistent identity and transactions

[ADR 0004](0004-formats-and-identity.md) separates account, character, durable
world/entity, asset, region authority and connection-scoped replica identities.
An ECS slot, socket, display name, source-scene UUID or client-supplied handle is
not proof of account/character ownership. Session revocation, reconnect and region
restart cannot revive stale authority or handles.

Authoring scene JSON describes world content. It is not the live character/world
database. M03 selects a durable transactional store and defines independently
versioned schemas, migrations, operation IDs, account/character ownership and
recovery. Persist acknowledged economy/progression changes with atomic validation,
concurrency control and an audit trail. Retries after timeout must resolve the
same operation rather than repeat the grant. Do not promise exactly-once network
delivery: use idempotent operations and enforce single durable effects.

Balances never become negative through a race; item ownership and trade outcomes
must conserve the modeled inventory/currency across disconnects, crashes and
duplicate requests. Persistence callbacks return results to the owning simulation
boundary. Critical operations fail closed when durability/ownership cannot be
established. Movement checkpoints may use a declared recovery window; M03 must
specify which state can roll back and which acknowledged state cannot. Scene-file
atomic replacement is not a substitute for multi-entity transactional persistence.

## Continuous world and region transfer

Use bounded local float coordinates for rendering/physics with stable global
cell coordinates plus double-precision offsets, as defined by ADRs 0004/0005.
Establish precision and origin-shift tests before committing transform, save or
wire layouts. Spatial queries, network quantization, physics contacts, attachments,
audio and picking must agree across cell/origin boundaries.

M05 supplies asynchronous validated engine-owned development-payload streaming,
prefetch, cancellation, memory/IO budgets and last-valid content retention. These
are prepared by EngineTools, following the pre-31 export boundary; no source
importer enters Runtime or Server. Assignment 31 and M08 replace and requalify
this path with final versioned cooked cell/dependency manifests. Render cells, collision and
navigation must be ready before they become traversable; rendering distance alone
cannot determine server simulation activation. Terrain/static world collision,
tiled navigation and bounded NPC pathfinding are required for the reference world.
The early primitive-only Jolt profile is extended deliberately with tested static
world collision data; it is not sufficient for traversable terrain by itself.

Adjacent simulation regions overlap interest and read-only boundary information.
A transfer includes durable identity, authority epoch, tick, state and outstanding
operation context. The destination prepares; ownership commits through one fenced
authority decision; the old owner loses write rights before the new owner accepts
authoritative intents. Define a consistent transfer cut at an authoritative tick
and accepted-input sequence, including final engine-owned state, velocities,
scheduled effects and pending durable-operation context. Preparation may overlap
simulation, but the source quiesces at that cut and the destination cannot activate
an earlier preparation snapshot. Post-cut intents and storage/worker completions
are routed, deduplicated and reconciled against the committed owner; acknowledged
operations cannot disappear between snapshot and commit. Test events at every cut
and commit boundary, including a late durable success and source death. Transfer
buffering and quiescence have explicit bounds measured against travel continuity.
The chosen ownership registry and recovery algorithm must prevent
both split authority and permanently orphaned characters under partitions/restarts.
Leases alone are not a proof: stale writers must be rejected at mutation/persistence
boundaries. No cross-region distributed database design is preselected.

M05 must prove crossing while moving, fighting, pursuing an NPC and carrying
pending durable operations from the M03 fixture, including projectiles, targeting
and relevant AI queries across the boundary. M07/M09 add actual trade and party
continuity once those game-module features exist.
An actor keeps its identity and visible continuity; transient replica handles may
change safely. Faults before/after ownership commit must either resume with one
owner or recover explicitly with one durable outcome. A loading screen or two
independent scenes is not a passing seamless-world fixture.

## Characters, content and operations

Static glTF support in 12 is an intermediate milestone. M06 adds a bounded skeletal
animation profile, skinned characters, interpolation/blending and actor/animation
LOD. Animation events do not grant authoritative hits or rewards; server gameplay
drives replicated semantic state. Morphs and advanced retargeting can remain
outside the first profile with explicit diagnostics. Crowded scenes need measured
visibility, mesh/material batching, LOD and animation costs, not only attractive
single-object graphics fixtures. Preserve the required PBR/IBL/shadow/AO work and
add crowd/streaming measurements alongside it.

M07 supplies a deliberately small reference game module: character selection,
movement, combat/death/respawn, one NPC quest loop, inventory/equipment, atomic
trade, party and bounded authenticated chat. Client UI shows confirmed state and
connection/recovery status. These examples exercise engine contracts; they do not
hard-code a commercial game's rules into EngineCore. User scripts are trusted
server content, not code downloaded from players. Server-only rules/secrets and
database credentials must not enter client cooks.

M08 adds health/readiness, metrics, structured events, graceful admission stop and
drain, restart recovery, backup/restore, schema migration, compatible content
rollout and rollback. Unready or overloaded processes cannot accept new ownership.
Logs redact credentials/tokens and expose operational identifiers needed for
diagnosis. Signed content/manifests and executable update trust are distinct from
ordinary cache hashes. Source asset import and arbitrary remote script execution
are never part of the player protocol.

## Capacity and acceptance policy

The user's world choice is settled. Combat style, total world size, target
concurrency, geographic latency, client hardware and operational cost ceilings
remain open. M01 records the initial load profile; M02/M04/M05 revise it from
measured protocol, simulation and streaming baselines. Do not market a capacity
number, silently reduce a gate or substitute idle sockets for active players.

Use these **provisional engineering targets**, to be confirmed or explicitly
amended before M09/M10 qualification:

| Workload | Initial target and evidence |
| --- | --- |
| Multi-region slice | 256 active synthetic players across at least two adjacent regions; 128 players concentrated in one area; client render fixture includes 128 visible animated actors |
| Server scheduling | 20 Hz; on named hardware p95 tick work <= 35 ms and p99 <= 50 ms, bounded lag and no sustained tick backlog under the declared simulation/NPC workload |
| Replication | Steady-state per-player p95 <= 64 KiB/s server-to-client and <= 16 KiB/s client-to-server; record content/join bursts separately with explicit caps |
| Network adversity | Baseline plus 100/200 ms RTT, 30 ms jitter, 1/5% loss, reorder, duplicates and temporary disconnection; correctness holds and correction/latency distributions are reported |
| Seam travel | At least 1,000 adjacent-region crossings with no duplicate authority, durable effect or forced reconnect; record stalls, correction distances and asset/collision readiness |
| Endurance | 24-hour active-client/NPC soak plus measured overload ramp and crash/restore drills; after warm-up no sustained unbounded memory, handle, queue or durable-state growth |

M06/M10 must set client frame, streaming stall, memory and visual LOD budgets on
named hardware before accepting their gates. All figures are targets, currently
NOT RUN. Thousands of concurrent players, global single-shard operation and
large-scale PvP battles require additional measured profiles; the initial fixture
does not establish those capacities. M10 records achievable capacity, limits,
cost per active player and the remaining gap to the chosen product target.

## References and decision scope

External references support the design concepts; they are not library selections
or evidence that Shark has implemented them. Epic documents authoritative server
state and dedicated servers without presentation work in its
[networking overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/networking-overview-for-unreal-engine).
Its [World Partition documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine)
describes content streaming; streaming alone does not prove distributed authority
handoff. Its [large-world coordinate documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/large-world-coordinates-in-unreal-engine-5)
is a precision reference. Shark's contracts and provisional budgets above are
project decisions, not performance claims derived from those systems.
