# Connected gameplay lifecycles

M04d connects the existing authoritative movement, combat, entity scripts and
replication owners under [ADR 0062](architecture/0062-gameplay-lifecycles.md).
Follow the [combat contract](authoritative-combat.md),
[script runtime](script-runtime.md), [runtime spawning](runtime-spawning.md) and
[M04 checkpoints](roadmap/authoritative-gameplay-plan.md). All services remain
creating-thread owners. These APIs are trusted host adapters, not player commands.

## Fixed host order

`CharacterCombat::step` optionally accepts the retained server-gameplay
`ScriptRuntime` for the same Region Scene. The host starts it before gameplay and
supplies that same owner consistently. The first admitted step/reconnect fixes
the optional runtime incarnation; later omission, replacement or opt-in rejects.
A stopped, faulted, foreign or client runtime rejects before gameplay callbacks.
The transport inventory is bounded,
nonnull and checked for duplicate connections before callbacks.

Each step:

1. Fences missing, expired or replaced actual sessions. Suspend their scripts,
   release movement and remove living bodies before another fixed callback.
2. Runs entity fixed callbacks at the current completed tick with a 1/20 s delta.
   Captures deferred entity-mutation receipts and dispatches queued removal exits.
3. Calls the existing movement/native owner once, advancing to the next tick.
4. Delivers its owned contact batches, then refreshes checked movement observations
   after deferred contact writes.
5. Resolves admitted combat actions in their original global order. Death suspends
   scripts before movement/body teardown. Respawn completes body creation,
   teleport and movement binding before fresh script initialization.
6. Delivers removal exits before another native tick.

`dispatch_contact_events(scene, world, events)` accepts at most 256 owned events
per call. It shares the native-dispatch watermark and weak world lease, validates
the complete structural batch before callbacks and consumes its watermark before
delivery. Exact or overlapping replay rejects. Current participant generations
are checked again; stale observations remain in reports without gaining authority.
The host splits larger native batches and bounds repeated exit draining. Native
callbacks never execute Lua. Deferred physics and entity writes keep their existing
ordered queue, grants and limits.

`CombatTick` retains original contact, entity-lifecycle and entity-mutation reports,
movement completion and combat/native milestones. Callback failure or a later
publication failure faults the owner while preserving earlier effects. A native
tick may complete before movement publication fails. Neither an outer error nor
`movement_completed == false` promises rollback; never replay an uncertain tick.
Explicit scoped teardown remains available.

## Entity scripts and owned spawns

`suspend_entity` walks the checked root subtree in reverse initialization order,
runs eligible destroy callbacks and releases its VMs without changing enabled
Script/source records. It also tears down runtime roots requested by that subtree,
including transitive roots whose intermediate requester has already retired.
Cleanup remains possible after spawn-grant revocation. Ownership records are
independent of the bounded, drainable mutation-receipt ledger.

`resume_entity` reloads the original captured source and initializes in forward
order. It requires the existing authority for any runtime-spawned entries.
Suspended entries receive no frame/fixed/contact callback. Fresh VMs do not reset
mutation-ID watermarks or make old spawn requests new intents. Lifecycle reports
retain attempted/completed callbacks and partial spawn teardown counts.

Combat manages its actors' body generations explicitly. A Lua remove/recreate on
a combat-managed actor invalidates its retained binding; the host fails closed
rather than silently adopting another body generation. Generic ScriptRuntime body
operations keep their existing contract. Ordinary Name writes still reject under
the physics lease.

## Disconnect and reconnect

Detached actor records retain the exact Scene entity, durable account/character,
health, cooldown, death deadline and last living pose. They have no movement or
collider and are omitted from `active_entities`, the trusted combat-player
replication selection. The source entity stays in the same leased Scene.
Attached plus detached records share the existing 32-actor limit. Permanent
retirement requires owner teardown; no unbounded dormant registry is introduced.

After a tick has detached the old binding, `reconnect` requires a fresh actual
ready mTLS connection and SessionId for that same account, character and checked
entity. It does not accept copied peer metadata or silently detach another live
binding. Living reconnect recreates the captured body at the last detached pose,
binds fresh movement and resumes fresh scripts. Dead reconnect stays at zero
health with no body/VM and preserves the original respawn deadline. Once active,
the first fixed tick at or after that deadline performs the ordinary respawn.
Reconnect cannot heal or restart a cooldown.

Combat stream, target-life and living movement identities rotate; ephemeral
action/outcome counters restart only within their new domains. Old action,
movement, target and body identities cannot address the replacement. Replication
publishes the absence and later fresh replica identity through the existing
allowlist owner, without creating another authoritative Scene entity.
`CombatReconnectReport` preserves partial body/script milestones and any error.
These health/death records survive reconnect in the retained realm, not a server
process restart; no durable health schema is added.

## Presentation and original durable operations

Core's `net::CombatClient` consumes only verified-server replies for its accepted
session. Creation baselines retained outcomes without emitting old effects.
`apply` stages complete state and at most eight owned presentation outcomes,
emitting each new outcome once. It checks session/epoch, stream, self life,
movement stream, monotonic movement acknowledgements bounded by tick progression,
tick progression and overlapping original outcome values.
A fresh life or session constructs a fresh cache rather than rebinding the old
one. Effects grant no Scene, Lua, gameplay or durable authority.

The host publishes one canonical immutable `CombatState` per recipient/completed
tick. Coalesce changed visibility/reconnect observations into the next ordinary
snapshot; different values at the same tick conflict. Fresh-binding initialization
is explicit and separate. Exact duplicates emit nothing. A missing outcome prefix
retains valid authoritative state, records the first gap and freezes effects.
An explicit same-binding `resynchronize` baselines skipped history without replay;
the first gap remains observable. Generic rejection also freezes effects. This
cache does not reconcile the shared session command sequence or retry an action.

Economic work continues to use the existing durable-operation owner. Retain the
exact original request and OperationId through an uncertain worker reply and
session replacement. Retry that identical request using the fresh authenticated
envelope; never invent another operation ID to resolve uncertainty. The connected
fixture performs a zero-reward interaction, loses publication of an independently
submitted conserved transfer, reconnects gameplay while SQL publication is blocked,
and verifies one committed transfer, exact replay and altered-payload rejection.
It adds no loot/reward endpoint.

The [M04d handoff](handoffs/M04d-gameplay-lifecycle.md) records selected CPU/native
physics and real mTLS evidence. [M04e](handoffs/M04e-gameplay-acceptance.md) records
passing connected two-client adversity, pause/disconnect and headless continuity
acceptance; full REQ-M04 PASS under that functional scope.
