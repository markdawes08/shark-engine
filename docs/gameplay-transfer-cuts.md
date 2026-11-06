# Gameplay transfer cuts

M05e adds consistent co-located transfer cuts over the existing
[regional owner](region-world.md), [movement](authoritative-movement.md),
[combat](authoritative-combat.md) and [lifecycle order](gameplay-lifecycle.md).
Follow the [M05 checkpoints](roadmap/seamless-world-plan.md) and
[testing policy](testing-policy.md). The same creating-thread simulation island
retains its Scene, native physics, Lua, sessions, interest publisher and durable
worker throughout a handoff. M05f owns transfer across actual separate processes.

## Final tick and owned observations

Preparation records an exact transfer identity, source grant and destination.
The source continues its ordinary ticks until an explicit cut at a completed
gameplay boundary. The destination must contain the final committed pose. A cut
requires healthy original owners, matching movement/combat/native tick numbers,
no pending Scene mutations and drained native contact events. It freezes the
whole shared island, including other players and NPCs, because one native world
and one gameplay order remain responsible for them.

`MovementCut` records Scene revision/scope, native-world identity, explicit origin,
traversal binding, shared coverage, completed tick, clock fence and execution
permit. Every actor retains its original connection/session binding, stream,
checked entity/body, full observed native body state, linear and angular velocity,
accepted/applied input watermarks, optional regional domain and all pending input.
The eight-slot queue is copied in FIFO order. Its original SessionEnvelopes and
input sequences remain unchanged; commit neither resubmits nor acknowledges them.

`CombatCut` records the same tick/context and clock fences, global admission-order
watermark, rules evaluation state and fixed optional script-owner identity. Every
actor includes its connection/session, combat stream and life ID, checked
entity/body, observed native state/velocity, captured body settings, spawn pose,
last movement correction, health, cooldown and respawn deadlines. It also retains
the accepted action watermark, original pending action/order, eight-outcome
history, detached/fenced state, detached pose and exact optional projectile.
Dead and detached actors remain records; transfer cannot heal them or reset a
deadline. Outcome history remains presentation history, not a retry receipt.

The enclosing `RegionCut` includes the explicit frame, resident traversal
generations, NPC bodies/velocities/pending intents, current actual sessions,
original durable pending records and completion routes, and regional writers.
It observes the complete island. This includes outgoing flights owned by the
transferring actor and incoming flights owned by other actors that target it.

Movement/combat records retain their binding-time SessionSnapshot. Its
`last_sequence` can precede later admissions. The coordinator separately observes
each attached actor through its actual ready mTLS transport and records the
current shared session command sequence. Movement input IDs, combat action IDs,
session commands and authoritative ticks remain distinct. Copied snapshots and
connection IDs never become reusable credentials.

## Shared execution fencing

`ExecutionFence` issues a process-local scope and nonwrapping generation.
Movement and combat adapters capture one permit at construction. Their admission,
step, release, reconnect, script/native work and damage paths check that captured
permit before applying effects. Freezing rejects mutation while allowing checked
cut observations. Rotation requires the current frozen permit and leaves the
island frozen. An adapter retaining an old permit cannot acquire authority merely
by observing the fence's new generation.

Only the trusted RegionWorld commit path can adopt the new permit in its retained
movement/combat owners. It does so after the persisted original transfer is
confirmed, then resumes the new execution generation and updates regional
movement coverage before reopening ingress. It does not bind or reconnect an
actor, replace its body, change streams/lives, run a gameplay tick or replay Lua.
The source grant and persisted writer fence then reject further old-owner work.
Existing hosts that explicitly omit a shared execution fence keep their earlier
single-owner mode; that mode supplies no regional transfer capability.

The host must continue routing gameplay through the retained coordinator. A
borrowed PhysicsWorld or ScriptRuntime is privileged host access, not a regional
writer endpoint; calling those owners separately during a frozen handoff would
violate the same ownership contract.

## Retained state and fingerprint limits

Cuts contain owned observations and explicit canonical big-endian fingerprint
bytes. Every exposed movement/combat field contributes; native struct padding,
addresses and native ABI layouts do not. The regional digest also binds the
original transfer identity, destination and enclosing cut values. Current lower
observations must still match before submission and activation. Encoded regional
cuts are bounded to 4 MiB, with at most eight retained handoff records and one
unresolved handoff in the island.

These bytes are neither an install format nor a portable native/Lua image.
Physics retains additional state, including queued next-tick force, optional
kinematic targets, previous poses, contact pairs and event sequencing. Draining
contacts does not imply zero queued force: a delivered script callback can apply
force for the next tick. The exact existing PhysicsWorld retains this opaque
state while execution is frozen.

CombatRules and entity ScriptRuntime VMs may contain mutable Lua globals.
Recreating them from source would change subsequent decisions and callbacks.
The same owners retain their VM state, initialized/suspended entries, fixed/contact
watermarks, mutation receipts, spawned-root ownership and private leases. A
projectile retains its original request, bounded decision, source/target
life/body/entity identities, position, direction, remaining ticks and original
admission order. Neither the rules nor launch effects execute again on commit.
Native and Lua partial-effect faults remain terminal; a snapshot is not rollback.

Original durable work also stays with its existing worker and RuntimeTicket.
Pending snapshots include admission context, request/OperationId, deadline,
writer, timed-out state and any observed outcome. A regional commit changes the
completion route, not the original operation or its admission writer. An uncertain
result retains that identity for reconciliation.

## Commit and recovery boundary

A known unsubmitted cut can abort and resume its original execution permit with
queues and writer unchanged. Once commit is submitted, timeout or lost completion
does not authorize that abort. The 500 ms quiescence budget marks reconciliation
required; it never silently resumes the source. Persisted confirmation must match
the exact original cut before destination activation. Unexpected effects or an
activation failure preserve the failure and require explicit reconciliation.

Terminal receipts retain the original identity and settled result until explicit
release; they release the large cut snapshot. Actual process loss, partitioned
owners, portable VM/native state and bounded post-cut input/completion routing
remain M05f in the [seamless-world plan](roadmap/seamless-world-plan.md).
