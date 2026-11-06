# Static world collision, navigation and traversal readiness

M05c connects [cell content](world-cells.md), [residency](world-cell-residency.md)
and the existing [physics owner](physics-simulation.md) under
[ADR 0065](architecture/0065-world-traversal.md). Follow the
[M05 checkpoints](roadmap/seamless-world-plan.md) and
[testing policy](testing-policy.md). Development content and local readiness do
not establish region transfer or the complete seamless-world gate.

## Prepared geometry and compatibility

`WorldCellGeometry` contains cell-local vertices, collision triangles and an
explicit oriented navigation subset. It accepts at most 4,096 vertices,
2,048 collision triangles and 256 navigation triangles. Vertices must be finite
on the exact 1/1024-meter grid within the inclusive [0,1024] cell cube. Collision
indices, geometric duplicates and degeneracy before/after float packing reject.
Navigation triangles must be unique collision faces, oriented upward with slope
at most 45 degrees. An explicit collision-only tile may have no walkable faces.

`SHKCELL1` now accepts envelope versions 1 and 2. The encoder retains the original
version-1 bytes when geometry is absent. Version 2 appends, after the existing
complete scene payload: uint32 vertex count and XYZ float64 values, uint32
collision count and three uint32 indices per triangle, then uint32 navigation
count and index triples. Geometry decoded storage is bounded by 256 KiB; total
cell bytes are at most 4 MiB + 256 KiB + 128 bytes. Both directions validate the
complete candidate; unsupported versions, counts, trailing bytes and malformed
geometry reject. Existing source, prepared-scene/mesh, database and gameplay-wire
formats remain unchanged. The digest detects corruption, not authenticity.

`tools::prepare_world_cell_geometry` captures an exact existing AuthoredScene
revision and requires explicit collision-entity and navigation-entity selections.
Navigation selection must be a unique collision subset. It resolves the supplied
immutable assets, including hidden meshes, and bakes complete hierarchy matrices
into cell-local geometry. Reflection reverses triangle winding to preserve the
selected surface orientation. Off-grid or out-of-cell results reject without
rounding. Preparation changes no source, dirty state, history or files and performs
no implicit import. The trusted caller supplies the intended asset closure; a
snapshot alone does not attest cache freshness or audience authorization.

These are owned development values prepared through shared CPU services. Final
cooked dependency manifests remain with 31/M08. No new editor panel or terminal
method is advertised; supported authoring adapters remain part of full M05
acceptance.

## Residency and native geometry ownership

Residency adds at most 32 exact `WorldCellPin` values. A pin binds its residency
scope, sequence, key and generation. Pinned content cannot be replaced or evicted;
stale, foreign and duplicate releases reject. Pins survive admission close and
must all be released before residency destruction. They grant content lifetime,
not player, region or durable authority.

Each resident charge is its encoded bytes, asset retained charge, a 16 MiB Scene
reservation and a 256 KiB geometry reservation. Existing pending/peak checks still
apply, including simultaneous old and candidate owners. These are logical
reservations and structural bounds, not process RSS or native heap measurements.
Decoder scratch and one complete owner candidate retain their existing independent
bounds. Native mesh copies and navigation graphs have separate triangle/tile
limits rather than being hidden inside residency accounting.

`PhysicsWorld::create_static_mesh_body` uses the active Scene physics lease and a
caller-supplied WorldEntityId to create one owned region proxy. That root has an
identity Transform and no RigidBody, Script, Camera or MeshRenderer. Its immutable
triangles are already in bounded Scene coordinates; all vertices fit the existing
8,192-meter native position limit. Mesh creation validates at most 4,096 vertices
and 4,096 triangles per native body, with at most 16,384 mesh triangles per world.
It uses a real static Jolt MeshShape, with explicit friction, restitution and
bilateral collision layer/mask settings. Native sanitization must not silently
remove selected triangles; invalid or quantization-degenerate geometry rejects.

`PhysicsBodyState.geometry` distinguishes primitives from static meshes. Its
`settings` is now optional: primitive bodies retain their actual RigidBody;
runtime mesh proxies return no invented primitive component. Material, filtering,
triangle count and native bounds remain explicit observations. Query/contact
results retain checked body generations and owned proxy entity references.
Static meshes cannot be repositioned through primitive reset/teleport controls;
rebuild is explicit. Generic Lua body removal/recreation cannot adopt mesh
attachments. Primitive source/component behavior remains unchanged.

The locked single-threaded native arena audit extends the existing capped
body-pair/contact buffers to discrete static mesh collisions. Mesh visitors use
bounded stack traversal and add no new Update arena arrays. The conservative
temporary requirement, native registration leases, fixed-step scheduling and
post-native fault semantics remain in force; no dependency or host setting changes.

## Navigation profile and pursuit values

`WorldNavigation::create` builds a complete immutable graph from at most eight
tiles and 2,048 total navigation triangles. Shared edges match exact canonical
WorldPositions across cell boundaries. Duplicate/nonmanifold topology and
positive-area XZ overlap reject, including stacked walk surfaces. No approximate
edge welding or nearest-surface teleport fills gaps.

Path queries require surface membership for both endpoints and use deterministic
bounded A* with explicit expansion and waypoint limits, each at most 2,048.
Exhaustion or disconnection returns an error rather than a partial route. The
route contains the start, shared-edge portal midpoints and goal; consecutive
segments stay inside their convex authored triangles. This is a bounded valid
centerline route, not an optimal funnel/geodesic or an automatic navigation bake.
The fixed 1e-7-meter membership allowance handles numerical comparisons without
welding topology or choosing another floor.

Paths carry immutable graph/frame identities. Cursors additionally bind a path
identity, current position and next waypoint. New graph publication invalidates
old routes through the traversal owner. Pure `WorldNavigation::advance` may
consume several route segments; it offers an owned progress value and never moves
a Scene. `WorldTraversal::advance` limits its steering offer to the current
segment/first portal so one native movement cannot cut across several corners.
A host adopts progress only after checking committed native movement; a blocked
body does not earn planner progress.

The profile supplies point/centerline navigation. Authors must provide appropriate
clearance for the intended actor; no radius-clearance, obstacle erosion, flying,
jumping or stacked-floor navigation bake is claimed. Actual collision still
governs native movement. These limits do not waive later gameplay, cross-region
or connected acceptance requirements.

## Traversal owner and movement admission

`WorldTraversal` borrows one residency, Scene and PhysicsWorld on their creating
thread; all three must outlive it. Creation requires a matching leased region
world and an explicit fixed `WorldLocalFrame`. It does not rebase live bodies.

Activation pins an exact resident generation, prepares the complete replacement
navigation graph and converts geometry through the fixed local frame before
creating its native mesh proxy. Publication follows successful native creation
with nonallocating moves. Content without geometry cannot activate. Existing
ready cells survive rejected candidates. Deactivation prepares the remaining
graph before removing native collision and releasing the pin; old graph paths
then become stale. Owned readiness checks revalidate the actual body/Scene
generation rather than trusting a cached display flag.

`allows_motion` checks the conservative swept XZ body footprint. Every cell in
its bounded footprint inventory must be traversal-ready; an inventory over eight
cells or any missing cell returns false. Native and canonical coordinate errors
reject. This is readiness admission, not proof of walkability, collision-free
travel, player ownership or a navigation-radius guarantee.

CharacterMovement declares its optional traversal instance at construction. Every
subsequent movement step, and CharacterCombat before its script/native work,
requires that exact retained owner, fixed origin, Scene and PhysicsWorld.
Omission, replacement, later opt-in or a foreign readiness context rejects before
work. With a valid owner, a false readiness gate consumes the admitted movement
input as a horizontal stop;
acknowledged input cannot become a delayed authority grant when content arrives.
Existing actual-session sequencing, body/life identities, gravity and fixed tick
rules continue to apply.

Close traversal before ending simulation or releasing residency. Healthy cleanup
removes native attachments and releases pins before dropping navigation/content
ownership. An error after native work is not rollback. If physics faults and
normal removal cannot finish, the host closes PhysicsWorld, closes traversal to
release pins and discards/reconciles the affected Scene. No uncertain native
operation or gameplay tick may be replayed automatically.
