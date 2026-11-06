# ADR 0065: prepared world surfaces and leased traversal readiness

Date: 2026-11-06. Status: accepted for M05c. Extends
[ADR 0063](0063-world-cell-content.md), [ADR 0064](0064-world-cell-residency.md)
and the [physics simulation contract](../physics-simulation.md). Follow the
[traversal contract](../world-traversal.md), [M05 checkpoints](../roadmap/seamless-world-plan.md)
and [testing policy](../testing-policy.md).

## Decision

Add explicit bounded cell-local collision geometry and an authored walk-surface
subset to the existing development cell payload. Exact 1/1024-meter vertices and
canonical world-coordinate edge matching preserve adjacent-cell topology without
forming global floats or silently welding geometry. Tools selects real prepared
mesh triangles through the existing exact scene capture and full hierarchy
transforms. Reflection preserves winding; unsnapped output rejects.

Use the existing leased PhysicsWorld and locked native MeshShape implementation
for static world collision. Native bodies own a checked region proxy entity with
no fake source RigidBody. PhysicsBodyState gains an explicit geometry discriminator
and optional component settings. Existing primitives retain their settings;
mesh proxies expose actual material/filter/bounds/triangle observations. Native
mesh visitors use the existing capped pair/contact buffers and stack traversal;
the reviewed Update arena bound remains applicable.

Build one complete immutable graph of at most eight single-layer authored tiles
and 2,048 navigation triangles. Deterministic bounded A* returns portal-midpoint
centerline routes or a complete error. The profile rejects unsupported topology,
stacked/overlapping walk surfaces and invalid endpoint membership. Graph/frame
and path/cursor identities prevent stale pursuit. Pure progress values do not
authorize Scene movement; traversal steering stops at the next portal and hosts
check committed physics before adopting progress.

Keep content lifetime explicit through at most 32 exact residency pins. Pins block
replacement/eviction until native consumers detach. WorldTraversal prepares its
new graph before native activation and retains a pin until native removal.
Residency charges add a 256 KiB geometry reservation to the prior Scene, payload
and asset charges; native triangle counts and graph limits remain independently
bounded. These logical reservations do not assert exact RSS/native heap usage.

Require all cells in a bounded swept horizontal body footprint to be ready before
movement admission. CharacterMovement fixes its optional traversal owner at
creation; movement and combat cannot omit or replace it later, or borrow another
Scene/native world's readiness observations. A readiness denial
consumes an already admitted input as a stop, preserving shared session/input
sequencing rather than granting movement later when a tile arrives.

## Compatibility and limits

Cell envelope version 2 appends validated geometry; version 1 remains readable
and retains identical output when geometry is absent. Source, prepared-scene/mesh,
database and gameplay-wire versions stay unchanged. Native public-state consumers
must check optional settings; no primitive component is invented for a mesh.
No new third-party dependency, scheduler, importer or host configuration is added.

This is a development payload and local readiness profile, not final cooked
closure or distributed ownership transfer. Point/centerline navigation requires
authored clearance; no general voxel bake, radius erosion, flying/jumping links,
stacked-floor routing or path-quality guarantee is implied. Shared backend
preparation adds no advertised terminal/editor method; full M05 retains those
adapters and its connected acceptance. M05d-g/M09 requirements remain binding.

Owner-thread construction and reverse teardown remain mandatory. Activation and
deactivation preserve prior ready content on pre-native rejection. Post-native
faults retain uncertainty: close physics, release traversal pins and discard or
reconcile the affected Scene rather than claiming rollback or replaying work.
