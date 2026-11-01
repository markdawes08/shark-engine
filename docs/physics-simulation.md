# Fixed-step physics and Scene synchronization

18c extends the existing [world](physics-world.md) and [bodies](physics-bodies.md)
under [ADR 0047](architecture/0047-fixed-step-physics.md). This is CPU runtime
simulation, using the [existing schedulers](time-and-work.md), with no window,
second registry, authored history or new player/terminal protocol.

## One writer and explicit boundaries

Prepare a mutable Core Scene and create native snapshots for every RigidBody.
`begin_simulation(scene)` validates exact entity/source incarnations, settings and
poses, complete component/native inventory, no pending work, a maximum of 4096
live entities and 65536 scanned slots. It then acquires one Scene write lease and
activates nonstatic bodies. Replica scenes and private authoring candidates
reject. Admission failure before acquisition changes neither Scene nor native
poses and does not fault the world.

While leased, ordinary Scene writes, deferred enqueue/apply and authoring
candidate publication reject. Reads remain available. Physics obtains no second
mutable Scene, and no borrowed Scene pointer is retained. Its private Core port
validates canonical root poses and complete descendant matrices, then publishes
all changed poses/world matrices in one revision. A stationary tick need not
advance Scene revision, but every successful tick advances the physics counter.

`end_simulation(scene)` ends the lease, clears queued targets/interpolation history
and stops kinematic velocity. Dynamic native velocity is retained. End before
ordinary component/entity/scale edits or native creation/removal, then explicitly
rebuild changed snapshots and begin again. Close/destruction destroys native
bodies/services before releasing its strong lease; ordinary Scene access resumes
when the weak lease expires, including when explicit end was omitted. Scene close
or destruction remains safe before world cleanup; a dead Scene cannot step.

The API requires mutable Core ownership. AuthoredScene exposes only a const Scene;
its source/history/dirty state is not a simulation destination. FeatureWorld
prepares/loads a separate Core instance and checks the authored owner remains
unchanged. Native editor play controls retain 18e. `scene.run` remains static
integrity validation, not a gameplay or physics execution capability.

## Modes and fixed updates

| Mode | Pose authority |
| --- | --- |
| Static | Initial Scene pose, then explicit `set_static_pose`. Gravity does not move it. Reposition wakes nonstatic bodies for subsequent collision processing. |
| Dynamic | Native integration supplies committed fixed-tick Scene poses. `teleport` explicitly resets pose, linear/angular velocity and interpolation endpoints. |
| Kinematic | `set_kinematic_target` queues one validated target for the next fixed update. Native kinematic movement supplies that update; absence of a new target holds the committed pose. |
| Presentation | `render_poses(alpha)` reads previous/current endpoints; it never writes simulation or source state. |

The root/positive uniform scale and local bounds remain the body contract.
18d adds [filtering, triggers and owned contact events](physics-contacts.md), with
sleeping disabled to keep stable overlap from producing false exits. Active
body removal now has an explicit leased Scene boundary; detached rebuild remains
the settings/creation path.
Live scale changes reject and require shape rebuild. Native bodies explicitly use
discrete motion, zero linear/angular damping, a 256 m/s linear speed ceiling and
64 rad/s angular ceiling. These are bounded initial runtime policies, not a combat
model, CCD qualification or production capacity. Kinematic targets additionally
check displacement/shortest rotation per fixed duration against those ceilings.
All malformed or rejected newest target requests clear an older queued target.
Static reposition and dynamic teleport are explicit discontinuities and reset
both presentation endpoints. A target changes no Scene until a successful step.

`step(scene)` executes exactly one `1/tick_hz` native update with the configured
collision substeps, validates complete output and commits one Scene boundary.
Global world/cell positions are never added to native floats. Source references
continue to fence scene incarnation, entity generation and region authority.
`body_state` observes native values against the current committed Scene pose.
The legacy parameterless `step()` remains empty/detached only.

After native work, an update error, diagnostic, arena proof violation, allocation
failure or rejected output faults the world. Scene retains its last complete
state, tick completion does not advance, and further simulation/readback/target
operations reject. Native progress may have occurred: close and explicitly
reconcile/replace; there is no retry or claim of rollback. Pre-native target/reset
validation rejects without publication. Reset candidates validate descendants
before native changes; unexpected failure after that point also faults the owner.

`poll_offline` and `poll_server` are adapters to existing scheduler instances.
Require 60 Hz for OfflineScheduler, the configured ServerScheduler rate for
server work, and matching `next_tick`/physics completion counters before polling.
They add no accumulator or wall clock. Offline keeps its bounded catch-up,
discard report, pause and single-step rules; Server retains unexecuted debt and
its overload policy, independent of focus/window state. Callers can also invoke
`step(scene)` from an existing owner callback. Server tick frequency and collision
substeps are independent: the fixtures include 20 Hz with three collision steps.

## Temporary work admission

The locked Jolt 5.6.0 Update, ContactConstraintManager and IslandBuilder allocation
paths use this conservative requirement (native sizes stay private):

```text
65536
+ 2 * collision_substeps * sizeof(native update Step)
+ max_body_pairs * sizeof(native BodyPair)
+ max_contact_constraints * (native maximum contact-constraint size + 16)
+ max_bodies * 64
```

This includes simultaneous contact buffers/index/link arrays, island body/end/
sort arrays, active-body indices, step-array growth, alignment and fixed margin.
Large-island splitting is explicitly off; discrete rigid primitives use no CCD
body buffers, soft bodies, external constraints or step listeners. Collision
substeps reuse transient buffers. Any later change to those supported paths must
review the requirement before work admission. The dependency/options remain
locked; there is no fallback allocator, changed host resource setting or hidden
heap escape from the simulation arena.

Nonempty begin rejects when configured `temporary_bytes` is below the requirement.
Configured capacities include all possible active bodies/contacts, not only a
currently small workload. Empty detached stepping retains its existing low-arena
support. Stats expose `temporary_required_bytes`, metered native
`temporary_high_water_bytes` and `simulating` alongside existing fields. This is
arena usage, not native heap/RSS or an MMO population performance measurement.
Unexpected native invariant/OOM aborts retain the foundation's stated limits;
the preflight is not a recoverable native OOM claim.

## Presentation snapshots and extraction

`PhysicsRenderSnapshot` owns checked entity references and local poses, with world
identity, exact source SceneRevision, completed tick and finite alpha in [0,1].
Translation uses lerp; normalized quaternion rotation uses shortest-arc slerp,
including the equivalent-sign boundary. Scale stays fixed. Snapshot generation
and CPU extraction leave Scene revision, source and history unchanged. Values can
outlive their native owner; retained ticks are historical presentation provenance.

Pass a snapshot through `RenderExtractionOptions::physics_poses`. Extraction
checks source revision, scoped references, unique rigid-body roots, scale and
finite bounds, then recomposes complete subtrees before cameras, bounds, normals
and culling. Visual children keep affine/mirrored transforms. An explicit display
camera still takes precedence. Mixing authoring transform_preview and physics
poses rejects. Omitted poses preserve committed extraction. RenderScene exposes
optional `physics_tick`; even tick zero marks presentation-only content. Picking
rejects these snapshots, like authoring previews, instead of authorizing edits
from interpolated geometry. Physics snapshots are not gameplay/network authority.

## Qualification and remaining scope

The focused physics family uses seed 1337 and the existing 60-second timeout.
Independent semi-implicit gravity checks use a 1e-3-meter absolute tolerance over
two seconds; 30/60/144/240 Hz presentation schedules must produce 120 fixed ticks
and positions within 1e-5 meters of each other. Native sphere-on-box resting height
allows 0.04 meters for the pinned solver's contact slop. These are declared CPU
oracles, not regenerated GPU baselines or cross-platform bit-identical lockstep.
Tests include exact arena admission with 64 active bodies/16 substeps, one-update
Scene publication, full hierarchy recomposition, failed newest targets, quaternion
sign crossing, teardown and post-native failure without partial Scene publication.

18d implements [layers/triggers/owned events](physics-contacts.md); actual editor/rendered play and human/
terminal acceptance retain 18e and the [desktop backlog](backlog.md#desktop-ui-deferral).
Queries/Lua retain 20, streaming/origin transfer M05. Full REQ-18 remains NOT_RUN.
