# Co-located regional gameplay

M05d adds `server::RegionWorld` under [ADR 0066](architecture/0066-colocated-regions.md).
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
grant.

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

After successful gameplay, all player and NPC membership candidates are prepared
from committed poses. Crossings publish fresh child grants together; unchanged
members retain their grants. Movement queues, input receipts, native body
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
generations. A committed center outside the configured union faults explicitly
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
views include nearby attached dead
players so their zero-health state remains observable; detached targets are
excluded.

`submit_transfer` retains at most the existing 16 runtime pending tickets, each
bound to its original entity and character on successful submission. `take_transfer`
requires that mapping and the current regional grant. An unsettled original
ticket remains retained across a boundary or reconnect; a settled outcome releases
the local slot. Another actor cannot consume it. The original request and
OperationId retain existing Store idempotence and reconciliation semantics.
This co-located route does not install a new persistent ownership epoch or claim
exactly-once networking.

## Partial effects and remaining gates

Pre-native validation errors reject normally. Once an NPC velocity is written,
later failure returns a RegionTick containing `npc_velocity_writes`, the original
gameplay prefix and an error, and faults the regional owner. Combat/native or
post-commit membership failures likewise preserve their original prefix and
fault. No report promises native rollback. Do not replay a partial tick; reconcile
or replace the affected host owners with explicit teardown.

M05d is a co-located functional profile with one Scene and native world, not
independently scheduled region servers. M05e retains consistent prepare/cut/commit
transfer, pending-operation routing and persistent writer fencing; M05f retains
separate-process failure/recovery qualification. M05g owns connected acceptance.
Final cooked dependency closure remains with 31/M08, and later social/economic
features retain their own authority gates. RegionWorld changes no existing
source/prepared/database format and grants no distributed capacity evidence.
