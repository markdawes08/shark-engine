# Rigid bodies and colliders

18b extends the [physics owner](physics-world.md) with real native body lifetime
and one optional `RigidBody` component. [ADR 0046](architecture/0046-rigid-bodies.md)
and the [checkpoint plan](roadmap/physics-plan.md) separate this surface from
18c's [nonempty simulation and Scene synchronization](physics-simulation.md).

## Authored values and validation

`shark.rigid_body`, schema ID 5/version 1, atomically contains motion and collider
settings. A separate collider component is not required. All fields, including
inactive shape fields, are validated and retained; changing the shape does not
silently reset the other fields. There is no native body ID in authored data.

| Property / stable property ID | Values, units and default |
| --- | --- |
| `motion` / 13 | `static` (0, default), `dynamic` (1), `kinematic` (2) |
| `shape` / 14 | `box` (0, default), `sphere` (1), `capsule` (2) |
| `half_extents` / 15 | Three finite meters in [0.01,256]; default [0.5,0.5,0.5] |
| `radius` / 16 | Finite meters in [0.01,256]; default 0.5 |
| `half_height` / 17 | Capsule's straight section half height in [0.01,256] meters; default 0.5 |
| `mass` / 18 | Finite kilograms in [0.01,1000000]; default 1 |
| `friction` / 19 | Finite dimensionless [0,1]; default 0.5 |
| `restitution` / 20 | Finite dimensionless [0,1]; default 0 |
| `collision_layer` / 21 | uint32 [0,15]; default 0 |
| `collision_mask` / 22 | uint32 [0,65535]; default 65535 |
| `is_trigger` / 23 | Boolean; default false |

18d's [contact contract](physics-contacts.md) defines bilateral masks, native
triggers and bounded event delivery. The same optional fields flow through
source, commands/history/retries, terminal queries and prepared version 3.

Capsules align with local Y and their total half height is `half_height + radius`.
Box sizes are half extents, not full lengths. Native shapes use single precision;
the box's internal convex radius is at most 0.05 meters and one tenth of its
smallest scaled half extent, while its outer bounds retain the declared dimensions.

Scene enforces an authored or authoritative root, positive exactly uniform scale,
and translation components within [-8192,8192] local meters. Shape dimensions bake
that scale and must still fit [0.01,256] meters; a scaled capsule's total half
height cannot exceed 256 meters. These local values never contain the global
world/cell coordinate. Pure component validation covers field values; pose and
root feasibility additionally depend on Scene. Replica scenes reject these
components rather than acquiring simulation authority. Visual children and other
render-only entities retain their existing affine/signed-scale support.

Immediate/deferred create, set/remove, transform and reparent paths enforce the
same constraints before publication. Failed changes preserve state/revision;
exact no-ops preserve revision. Removing the component permits ordinary
render-only transforms again. No physics body is created by storing settings.

## Shared authoring and persistence

`CreateEntityCommand::rigid_body` and `SetRigidBodyCommand` use the existing single
AuthoredScene owner. An empty optional removes the component. Atomic batches,
pure preview, one-group history, fresh generations on recreation and retained
mutation outcomes keep their existing rules. History reconstruction clears a
candidate's old body settings before restoring its target pose/settings, so an
intermediate signed-scale change cannot invalidate a valid historical state.
Nothing is published until that complete private candidate validates.

Retained matching includes presence, both enums, all three half extents and all
five scalar doubles plus both collision integers and the trigger flag on create
and set/remove. Floating inputs compare raw bits,
including signed zero and NaN payloads. Existing `sizeof`-based command/snapshot
charges include these fixed-size fields; no independent mutable model is added.

Source scene schema 1 stores an optional `shark.rigid_body` object with component
`schema_version:1` and the named fields above. Omitted fields default; unknown
fields/versions and malformed types reject. Earlier documents still decode, and
older readers reject the unknown component rather than dropping it. Capture,
save/reopen, metadata/history reconstruction and exact file receipts retain all
settings. Source schema 0 retains its existing migration; it gains no legacy body
encoding.

Prepared scenes preserve these values through the Core-only codec and loader.
`SHKSCNE1` envelope version 2 adds a presence byte after each entity's renderer,
then motion/shape uint32 and eight float64 values (half extents, radius, half
height, mass, friction, restitution). Version 1 still decodes with no body values.
Encoding a scene without bodies emits the original byte-identical version 1;
encoding bodies with default collision settings emits version 2. Version 3 adds
nondefault filter/trigger settings under the contact contract. Unsupported versions reject, and old version-1
readers explicitly reject version 2. Bounds/digests and full candidate validation
remain. Runtime gains no JSON/source parser, authoring service or SDK headers.

Terminal schema discovery exposes the same component and units. `scene.query`
and `scene.inspect` opt into `rigid_body`, selected null means absent, and omitting
the field omits its output. Up to six distinct field names are supported. Through
`scene.mutate`, `create_entity` accepts optional `rigid_body`; `set_rigid_body`
requires the complete value or null. Its value has the original eight required fields plus the three optional
collision fields above, without a source `schema_version`. These are trusted local development operations,
not player intents. The existing 42-method protocol inventory is unchanged.
Native inspector controls retain 18e; the current widget does not misrepresent
the new schema as its existing renderer editor. Actual desktop work remains paused.

## Native body ownership and limits

`PhysicsWorld::create_body(scene, entity)` validates a committed live source,
captures its checked EntityReference/settings/pose and creates a real Jolt shape
and body with explicit mass/material/motion settings. Bodies are initially inactive.
One world may contain snapshots from one Scene incarnation at a time, with one
body per entity. The configured body capacity includes retired handle slots.
Rejections publish no body or Scene mutation. Native heap exhaustion/invariant
aborts retain the foundation's stated limits; they are not a recoverable OOM claim.

`PhysicsBodyHandle` carries world identity, slot and generation, never a Jolt ID.
Foreign/stale/reused tokens reject. `body_state(scene, token)` rechecks the source
entity incarnation, scope/authority, authored settings and pose, then returns owned
native pose/bounds/motion/inverse mass/friction/restitution. Name-only edits do not
invalidate a body. Changed/deleted/recreated sources reject observation until
explicit removal/recreation; queries never overwrite newer authored values.
The native mass observation is zero inverse mass for static/kinematic bodies.

`remove_body(token)` removes and destroys the native body, then invalidates the
slot generation. It remains available after source deletion or Scene destruction.
No Scene pointer is retained. Close/destruction releases every body/shape before
the system, jobs, arena and final registration lease. All operations retain the
creating-thread guard; closed owners cannot create/read/remove bodies.

Body creation/removal does not use the temporary simulation arena. Nonempty
`step()` rejects before native work and leaves the tick counter unchanged. This
was the explicit 18b boundary for work admission, gravity, scene synchronization,
mode-specific pose writers and interpolation; empty stepping remains supported.
Collision layers/triggers/events remain 18d, queries/Lua 20 and world rebasing M05.
No collision, body-population performance or MMO capacity qualification is claimed
by these creation/lifetime checks.

18c adds explicit begin/end simulation and `step(scene)` through the same owner.
Detached creation/read/removal retain the rules above; creation/removal require
ending an active simulation lease. `step()` without a Scene still rejects bodies.
The [simulation contract](physics-simulation.md) defines dynamic pose publication,
explicit static/kinematic writes, populated arena admission and interpolation.
