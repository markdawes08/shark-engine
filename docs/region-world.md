# Co-located regional gameplay

M05d adds `server::RegionWorld` under [ADR 0066](architecture/0066-colocated-regions.md).
M05e adds explicit persistent ownership and local handoffs under
[ADR 0067](architecture/0067-fenced-region-handoffs.md) and the
[durable ownership contract](regional-ownership.md).
It connects the existing [movement](authoritative-movement.md),
[combat](authoritative-combat.md), [lifecycle](gameplay-lifecycle.md),
[interest](network-interest.md) and [traversal](world-traversal.md) owners.
Follow the [M05 checkpoints](roadmap/seamless-world-plan.md) and
[testing policy](testing-policy.md).

## Ownership and coordinates

A RegionWorld is a creating-thread owner of private CharacterMovement,
CharacterCombat and InterestServer instances. Its borrowed Scene, PhysicsWorld,
WorldTraversal and actual CharacterAuthority remain host-owned. Close the regional
owner before traversal and native owners; close traversal before its residency
pins and borrowed owners. No mutable lower gameplay owner is exposed.

The common RegionSceneScope identifies one simulation island. Child regional
authority is an exact `RegionGrant {scope, entity, region, generation}`, distinct
from that Scene scope. Its scope/generation are fresh nonwrapping process-local
identities. Grants admit regional gameplay only; they confer no standalone native,
session, Store or distributed authority. Direct host maintenance APIs remain
privileged and must not be used as alternate regional gameplay ingress.

Creation requires the traversal's exact fixed WorldLocalFrame, Scene and native
world. Two to eight RegionDefinitions specify finite half-open XZ rectangles in
that frame, each within the existing 8,192-meter native bound. IDs are unique,
interiors do not overlap and the graph of shared edges with positive length is
connected. Corner contact alone is not adjacency. Every actor center belongs to
exactly one child rectangle. Cells remain content/readiness units, independent
of these authority boundaries; no rebasing or Scene reset occurs on crossing.

Each region declares an interest overlap at least InterestLimits.exit_radius and
at most 8,192 meters. Existing InterestServer validates the complete radius and
quota configuration. The overlap states the neighbor coverage requirement; it
does not install a second visibility cutoff or independent scheduler.

## Player admission and continuity

`bind` requires a real ready CharacterAuthority session, authenticated transport
and checked native player entity. At most 32 player records are retained across
all regions; dead and detached records count toward this same limit. Duplicate
player/NPC identity registration rejects. Each player keeps its original checked
entity reference, actual session identity, movement/combat binding and regional
grant. Creation defaults to the original M05d functional mode. The explicit
`persistent_ownership=true` mode requires a preclaimed cached RegionWriter for
the exact character, entity and initial region, and fixes the actual retained
CharacterAuthority. It does not infer a writer from Scene metadata or login state.

Movement, combat, interest publication/control and durable transfer operations
first check the exact current grant, then actual session/entity authorization.
Movement/combat also retain their stream and shared command-sequence admission.
A stale grant is rejected before consuming another session command. NPC grants
cannot admit player operations. Queued input never becomes later authority merely
because its actor crosses a boundary.

`step` supplies one bounded actual transport inventory to the existing M04 host
ordering. Duplicate live connections reject. Existing detach, fixed callbacks,
movement, native collision/contact delivery, combat and lifecycle behavior remain
owned by those adapters. The optional script owner cannot be omitted or replaced
after its first admitted step. Native physics runs once per tick, at the existing
20 Hz / three collision-substep profile.

In default functional mode, successful gameplay prepares player and NPC membership
candidates from committed poses and publishes fresh child grants for crossings.
Persistent mode retains a player's source grant/writer until explicit committed
handoff; its geometric position may already be in the destination overlap.
NPC grants continue their co-located post-tick routing without invented durable
character identities. Movement queues, input receipts, native body
generations, session/ownership epoch, replica streams, combat targets, health,
cooldowns and lifecycle deadlines do not reset for a regional crossing. Same-frame
native contacts and checked range/LOS remain valid across the shared boundary.

Explicit reconnect follows M04's fresh ready session/transport and retained-entity
rules. It updates ephemeral gameplay bindings and always rotates the regional
grant while preserving the character's retained life state. It does not heal,
respawn or clear cooldowns implicitly.

## Movement coverage and native NPCs

RegionWorld supplies the rectangle union as CharacterMovement's fixed domain.
The coverage check tests the complete horizontal segment, including intervals
between rectangle-edge crossings, so a concave unowned gap cannot be crossed
through owned endpoints. A normal domain/readiness denial consumes the admitted
input as a horizontal stop, preserving gravity and command/input sequencing.
Traversal still checks the bounded swept body footprint against exact resident
generations. Persistent players also have a fixed per-actor source corridor: its
source rectangle expanded on XZ by interest_overlap, clipped to native bounds and
intersected with the existing world union. Full-segment coverage applies to both
domains. Crossing the corridor's outer edge stops normal horizontal input rather
than allowing an uncommitted writer to roam indefinitely. After committed transfer,
the corridor changes to the new source region. A committed center outside the
configured union or its persistent source corridor faults explicitly
rather than inventing a destination owner. NPC native segments receive the
additional post-step readiness check described below.

The host can register at most 16 separate NPC roots using `bind_npc`. Registration
requires a world identity, a checked live dynamic primitive body and a ready
bounded footprint. Body/entity generation and captured primitive settings remain
fenced. NPCs have no fake CharacterId, login session, combat target or durable
economy access.

`submit_npc_velocity` accepts one finite horizontal intent per tick, with zero Y
and XZ speed at most movement_speed (6 m/s). Omission stops horizontal motion;
native vertical velocity is preserved within the existing vertical ceiling.
Every NPC candidate is preflighted for identity, segment coverage and traversal
readiness before any NPC velocity write. Then all velocities enter the same
shared native/combat tick. Post-tick native poses are checked before their region
grants change. A route cursor is a separate pure pursuit value: hosts must check
actual committed motion before adopting its proposed progress, as in M05c.
NPC retirement and general AI behavior authoring are outside this adapter.

## Interest and durable outcomes

Each publication uses the private InterestServer through CharacterAuthority's
private session adapter, preserving actual recipient validation, ACKs, reliable
FIFO, quotas and privacy checks. Managed player and NPC rows are public and
required. Player ownership remains the actual CharacterId; only the exact
recipient row is the observer. NPC rows have no owner or observer identity.

Dead/detached player rows remain legally public catalog entries with
`present=false`. This availability flag only excludes spatial selection; privacy
checks still run on those rows and retained disclosures. Ordinary lifecycle or
distance exit can therefore despawn a replica without falsely reporting a privacy
revocation. Spatial entry/exit hysteresis spans neighboring regions, retaining
the same replica stream and entity mapping on a crossing. Exceeding the configured
visible limit (at most 32 eligible required entities) fails explicitly. Combat
views include nearby attached dead players so their zero-health state remains
observable; detached targets are excluded.

`submit_transfer` retains at most the existing 16 runtime pending tickets, each
bound to its original entity and character on successful submission. `take_transfer`
requires that mapping and the current regional grant. An unsettled original
ticket remains retained across a boundary or reconnect; a settled outcome releases
the local slot. Another actor cannot consume it. The original request and
OperationId retain existing Store idempotence and reconciliation semantics.
In persistent mode, submission supplies the current RegionWriter to the actual
worker; take supplies the current writer while retaining the original command's
writer in its result. A stale regional grant rejects before either operation.
Settled historical outcomes can still require reconciliation and do not grant
authority to apply them to an old session/Scene. No exactly-once networking is
claimed.

## Persistent prepare, cut and commit

The persistent profile allows one active handoff and at most eight retained
records. `prepare_handoff` requires an attached player, its exact current source
grant/writer and an adjacent destination. The host supplies a fresh transfer ID
and explicit destination instance. Prepare permits ordinary ticks and input
admission. Exact retries return the retained phase; changed inputs under that ID
reject. A prepared observation is not the final cut.

`cut_handoff` requires the latest committed pose to lie in the destination and
an exact bounded actual transport inventory for all attached players. Missing,
replaced or duplicate live sessions reject. A healthy tick boundary must have
no pending Scene mutations or native contact events. Retained ownership-control
jobs, including settled but untaken claims/commits, reject; admitted gameplay
operations may remain pending.
The transferring player's complete sphere footprint must be traversal-ready,
using retained Combat settings even during a dead life without a native body.
Cut publication, SQL submission and destination activation recheck readiness and
the exact captured frame/cell generations/native attachments. A host-side tile
change cannot authorize adoption of an older ready observation; a post-commit
mismatch leaves execution frozen for explicit reconciliation.

Cut freezes the whole shared island before publishing an immutable RegionCut.
No regional movement/combat/durable/interest/reconnect or NPC ingress may mutate
the frozen state or spend a session command. No native step, gravity/contact
progress or entity callback runs until explicit activation or safe abort. The
existing exact native, Lua, interest, session and worker owners stay retained.

The cut contains final movement streams, accepted/applied watermarks and original
FIFO inputs; complete combat health/cooldown/death/detach state, pending action,
global order and retained outcomes; original in-flight projectile requests,
evaluated decisions, life/body/target references, trajectory and remaining ticks;
and NPC native pose, velocity and pending intent. It also captures current actual
session sequences, worker tickets/deadlines/admission writers, original transfers
and save revision/state, settled values/errors, regional routing and binding
shadows, execution/counter state, frame and exact resident generations.

Canonical explicit values and lower-owner cut bytes form the SHA256 digest used
in the persisted request. Padding, pointers and native ABI layouts are excluded.
The digest identifies this local observation; it is not an authenticity proof or
portable VM/native snapshot. In particular, mutable Lua state and native contact
internals continue in the exact retained owners, with no source reload.

`begin_commit` submits the exact frozen request through CharacterAuthority's
existing worker. `poll_handoff` drains the original result. SQL checks the source
writer and atomically publishes the next epoch and exact last transfer; delayed
checkpoint/economic jobs check their own captured writer inside their transaction.
An exact known commit rotates the shared execution permit and privately adopts
it on movement/combat, then changes the player's corridor, grant and writer. It
does not bind, reconnect, heal, requeue inputs or relaunch projectiles. Lower
owners holding the old permit remain stale.

`abort_handoff` resumes an unsubmitted cut, or a known noncommitted rejection
whose source writer remains current. A pending/uncertain submitted request cannot
be timeout-aborted. `reconcile_handoff` can adopt the exact cached persisted
last-transfer receipt after explicit worker/storage recovery. A partial native
or adoption fault still requires explicit owner reconciliation/teardown; a receipt
cannot undo earlier effects. No uncertain operation is automatically replayed.

Reports expose cut_at, settled_at and a 500 ms local budget. settled_at records
actual activation or safe abort. Overdue pending/uncertain work remains frozen with
reconciliation_required; a late known commit can activate the current owner while
preserving budget_exceeded. A known rejection retains rejected while frozen until
explicit abort, and its budget keeps counting. Deadlines never cancel SQL.
One cut has at most 4 MiB of canonical encoded values. Existing 32-player,
16-NPC, eight-input, eight-outcome and sixteen-worker limits separately bound owned
snapshots and duplicate lower-owner byte blobs; this is not measured RSS. Terminal
records drop their internal cut pointer. Caller-retained immutable observations
have caller-owned lifetime. `release_handoff` explicitly acknowledges only a
committed/aborted record; unresolved records cannot be released, and forgotten
IDs return unknown rather than historical success.

## Partial effects and remaining gates

Pre-native validation errors reject normally. Once an NPC velocity is written,
later failure returns a RegionTick containing `npc_velocity_writes`, the original
gameplay prefix and an error, and faults the regional owner. Combat/native or
post-commit membership failures likewise preserve their original prefix and
fault. No report promises native rollback. Do not replay a partial tick; reconcile
or replace the affected host owners with explicit teardown.

Both modes retain one Scene and native world. M05e's persisted writer receipt and
local consistent cut do not reconstruct independent region servers. M05f retains
actual separate-process transfer, bounded post-cut routing, supported portable
state/import and failure recovery. M05g owns connected acceptance.
Final cooked dependency closure remains with 31/M08, and later social/economic
features retain their own authority gates. Source/prepared content and player wire
formats remain separate from the schema-4 durable ownership change. RegionWorld
grants no distributed capacity evidence. The manual UI pause remains unchanged.
