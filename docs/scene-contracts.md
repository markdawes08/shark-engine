# Scene ownership, identity and world coordinates

The assignment-07 EngineCore API is in [scene.hpp](../engine/core/include/shark/scene.hpp),
[scene_identity.hpp](../engine/core/include/shark/scene_identity.hpp) and
[world_position.hpp](../engine/core/include/shark/world_position.hpp). It is
CPU-only and shared by future human and terminal authoring services. EnTT is a
private implementation dependency; callers receive engine values, never registry
objects, component pointers or native entities. These C++ contracts are not a
scene file format, gameplay wire protocol, authoring command or editor UI.
13a adds optional [Camera/MeshRenderer values](render-components.md) under
[ADR 0032](architecture/0032-render-components.md) on this same CPU owner;
storing them neither loads an asset nor creates presentation services.
07c adds [fixed component schemas and bounded revisioned inspection](scene-inspection.md)
through [schema.hpp](../engine/core/include/shark/schema.hpp) and
[scene_query.hpp](../engine/core/include/shark/scene_query.hpp). Schema discovery
returns owned metadata; scene queries return owned immutable pages from the
existing owner. Neither exposes EnTT or creates a second scene model.

08b's EngineTools [authored Scene owner](authored-scenes.md) reuses these APIs for
revision-consistent capture and validated replacement. Core has no source reader
or authoring dependency. Source metadata and the replacement baseline stay in
Tools; ordinary Scene mutation and deferred-queue semantics remain unchanged.

## Identity and scope

Persistent UUIDv4 values use distinct domain types: SceneId/AuthoredEntityId for
authored content, WorldId/RegionId/RegionInstanceId/WorldEntityId for live world
scope, and ReplicaSessionId for a connection incarnation. IDs and authority
metadata are not credentials or proof of distributed ownership.

An authored scene accepts only AuthoredEntityId. An authoritative region scene
accepts WorldEntityId and derives its AuthorityStamp from the scene's region,
region instance and strictly positive epoch. A replica scene accepts WorldEntityId
plus the source AuthorityStamp and binds every entity to its world/session scope.
Scope and stamps are immutable. Replacing an incarnation or ownership stamp
requires destruction and recreation at this layer. M02b implements local
[authentication and owned sessions](network-sessions.md); its AccountId,
CharacterId and SessionId remain distinct from scene identities and ReplicaSessionId.
Session authority does not authorize arbitrary scene mutations. M02c replication
and M05 distributed fencing and seamless handoff retain their later ownership.

EntityHandle contains a process-local scene-instance token, slot and generation.
Every access checks all three. Every scene creation gets a fresh monotonic token,
even with the same persistent SceneId or replica session. Destroying an entity
advances its slot generation; an exhausted generation retires the slot. Tokens,
generations, mutation tickets and revisions never wrap. Handles are not durable
or wire identities and have no cross-process meaning.

EntityReference is an owned copy of handle, persistent identity, scene scope and
authority. Resolving it checks the entire current reference. Destroy/recreate,
reload, reconnect or a changed stamp cannot revive an old reference. `find` is a
fresh local identity lookup, not authorization or resolution of an old packet.
The generation and session lifetime foundation does not implement a network
replica table or player persistence.

## Writer and mutation boundary

Each scene belongs to its creating thread. All operations, including reads,
reject a different thread before accessing mutable state. Mutable scenes cannot
move or copy. Destroy a live scene on its owner; destruction from another thread
is a programmer error that terminates. After explicit owner-thread `close`, the
closed object may be destroyed elsewhere with normal caller synchronization.
Concurrent object destruction is never supported.
The scene must outlive all active calls, including its visitor callbacks.

Immediate create/destroy, component edits/removal and reparenting require no active
visit and no pending deferred work. See the [transform and hierarchy contract](transforms.md).
Visits use ascending slot order and pass owned reference values. Callbacks may
inspect and enqueue bounded mutations; they cannot mutate immediately, apply the
queue, close the scene or nest a visit. The scene remains unchanged during a visit.
If a callback fails or throws, only mutations queued by that visit are canceled;
older queued mutations remain. Canceled tickets are not reused.

`apply_deferred` runs at an explicit iteration boundary in FIFO order. Each
operation revalidates current identity/lifetime/capacity and returns an outcome
with its ticket. A failed operation does not prevent later valid operations.
This is per-operation commit, not the atomic authoring transaction system planned
for 09. A failed outcome allocation leaves the queue untouched. Successful create
or subtree destroy increments the scene revision exactly once. Component
and hierarchy changes do likewise; exact no-ops leave the revision unchanged. Close cancels pending
work, releases entities and invalidates every handle; repeated close succeeds.
Stats remain available on the owner after close; other operations reject it.

`query_entities`, `continue_query` and `inspect_entity` read committed state on the
scene owner, with explicit field selection, bounded results and scene-incarnation
revision checks. They are allowed during a visit and while deferred operations
are pending. Pending mutations do not appear until committed. A successful
no-op leaves continuation valid; a committed change rejects stale continuation.
The [inspection contract](scene-inspection.md) defines limits, filters, ordering,
cursor lifetime and failure precedence. Pure schema validation never changes the
scene or reserves its revision and cannot guarantee hierarchy/world feasibility.

Default limits are 4096 entity slots and 1024 pending mutations, configurable up
to 1,000,000 and 65,536 respectively. Zero limits are rejected. Retired slots
consume slot capacity. Hierarchy depth defaults to 256 with a hard maximum of
1024 and root depth one. These are storage bounds, not measured MMORPG capacity.
Expected failures return the existing owned Result/Error with operation `scene`
or its specific create/find/resolve/deferred operation and known persistent entity ID;
stale identities use `invalid_id`, owner/boundary/capacity conflicts use `conflict`.

## Global cell coordinates

Coordinate contract version 1 pairs WorldId with integer cells and local double
offsets. Each cell is exactly 1024 meters. Axes are stored as int64, with allowed
cell indices [-2^31, 2^31-1]. Canonical offsets are finite, positive-zero or
positive values in [0, 1024). These numerical representation bounds are not a
world-content extent or a performance target. Spatial axes follow ADR 0005.

Arithmetic requires IEEE binary64 round-to-nearest mode and rejects other active
rounding modes. Normalization carries whole cells using checked integer arithmetic;
it does not construct a global floating-point position. Invalid input cells, nonfinite
offsets and out-of-range results fail. There is no epsilon snap. A negative
remainder whose addition to 1024 rounds exactly to the upper boundary carries
to the next cell at positive zero. Translation requires canonical input and
splits delta cells/remainders before local addition. IEEE-754 rounding still
applies; the contract does not promise bitwise arbitrary-platform simulation.

07b implements [local Transform TRS and hierarchy](transforms.md); its world
matrices stay within a common local scene frame. 07c publishes their fixed local
schemas and inspection; it adds no global-coordinate conversion. M05 owns render/physics
origin selection, generation-checked bounded float conversion, streaming and
actual region handoff. No global float vector or premature rebasing service is
introduced here.

## Dependency and verification

EnTT 4.0.0 is pinned to commit
`85c6bba014049b5de8fad49d25424df2f1f6a8c1` and an exact SHA-256 archive in
[dependencies.lock.json](../dependencies.lock.json). Only its header-only registry
implementation is consumed; its upstream C/testbed project is not configured.
The MIT license accompanies Runtime and Server packages. 07b also pins GLM
through the public, centrally configured math wrapper; see [transforms](transforms.md)
and [building](building.md) for its independent archive and package notice.
See the [official release](https://github.com/skypjack/entt/releases/tag/v4.0.0),
[pinned build file](https://github.com/skypjack/entt/blob/85c6bba014049b5de8fad49d25424df2f1f6a8c1/CMakeLists.txt)
and [license](https://github.com/skypjack/entt/blob/85c6bba014049b5de8fad49d25424df2f1f6a8c1/LICENSE).

`shark.core.scene`, `shark.core.world-position`, `shark.core.hierarchy`,
`shark.core.transform`, `shark.core.schema`, `shark.core.scene-query` and
`shark.tooling.scene-dependencies` exercise these contracts. The
[07a handoff](handoffs/07a-entity-ownership.md) and
[07b handoff](handoffs/07b-transform-hierarchy.md) record earlier local results and
limits; the [07c handoff](handoffs/07c-schema-inspection.md) and
[receipt](coverage/evidence/07c-schema-inspection.json) close the final gate.
07c and full REQ-07 are PASS under the current-machine scope, with 376 passing
CPU CTests, protected builds and isolated package checks. FeatureWorld connects
implemented components in assignment 10. M02 is complete: the transport foundation
is complete, and [M02b owned sessions](handoffs/M02b-owned-sessions.md) is **PASS**
under the current-machine scope. M02c [replication](network-replication.md) is also
complete: explicit server-selected affine world poses are read through this owner.
Names, durable IDs, hierarchy and private scene revisions stay off the public state
wire. M02d spatial interest/budgets and M02e integrated qualification are
complete under the [network plan](roadmap/network-plan.md). Full REQ-M02 is
PASS under the current-machine scope in the
[M02e handoff](handoffs/M02e-integrated-qualification.md). Assignment 08,
Project and scene persistence, is next.
