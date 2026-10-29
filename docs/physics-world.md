# CPU physics world foundation

18a adds [physics.hpp](../engine/core/include/shark/physics.hpp) to EngineCore.
Follow [ADR 0045](architecture/0045-physics-foundation.md), the
[physics checkpoints](roadmap/physics-plan.md) and the existing
[time/work](time-and-work.md) and [transform](transforms.md) contracts.

## Supported owner and native boundary

`PhysicsWorld::create` constructs a real, initially empty Jolt PhysicsSystem,
fixed temporary arena and single-threaded native job system. `step()` calls the
native update for exactly one configured tick, and `stats()` returns owned config,
completed ticks, current body count and closed/faulted state. There is no body
creation surface in 18a, so the observable body count is zero. Components,
colliders, scene synchronization, queries, triggers and terminal physics commands
land with their assigned checkpoints; no placeholder capability is advertised.

All methods require the creating thread and return owned Result/Error values.
Foreign calls reject without changing state. Close releases the native system,
jobs and arena before its registration lease and is idempotent on the owner
thread. Closed owners retain config/tick statistics and reject stepping. A live
owner must be destroyed on its creating thread; violation is a programmer
invariant and terminates without throwing. An explicitly closed owner can be
destroyed elsewhere. Partial construction unwinds in reverse ownership order.

Jolt is private: public headers expose Shark values and a PIMPL, never body IDs,
native handles or SDK headers. The [complete public example](../examples/physics_world.cpp)
creates a world, performs three real empty updates, checks its owned stats and
closes it, linking only EngineCore.

A private mutex/refcount lease owns Jolt's process factory/type registrations.
It rejects an existing foreign factory and saves allocator, trace and assertion
callbacks before installing its own. Multiple live worlds share the registration;
the last release destroys native factory/material resources before restoring the
saved callbacks. Engine-native traces are bounded ASCII diagnostics and do not
write to protocol stdout. Assertion failures remain programmer invariants.
External SDK users must not replace registrations while these leases are live.

## Configuration, work and failure limits

| Field | Accepted range / default |
| --- | --- |
| `tick_hz` | Integral 1..1000; default 60 |
| `collision_substeps` | Integral 1..16; default 1 |
| `max_bodies` | 1..4096; default 4096 |
| `max_body_pairs` | 4..65536; default 16384 |
| `max_contact_constraints` | 4..65536; default 8192 |
| `temporary_bytes` | 64 KiB..64 MiB; default 8 MiB |
| Gravity | Meters/second squared; each component finite, absolute value <=1,000,000, and representable as zero or normal float; default `[0,-9.81,0]` |

The minimum pair/contact capacities match the native four-bucket minimum rather
than silently admitting values that Jolt would enlarge. Validate before native
allocation. Configured bounds constrain one owner; they do not measure process
RSS or qualify a population of bodies that is not implemented yet.

Each call advances the native world by float `1/tick_hz` seconds using the explicit
collision-substep count. Completed tick identity increments only on success and
never wraps. A failed update faults the owner: partial progress is not rollback,
and another step rejects until the owner is closed/replaced. There is no retry,
wall clock, render delta, pause policy or second scheduler in this service.

The fixed native temporary allocator has no unbounded fallback. The pinned empty
update path uses no temporary jobs/arena allocations; this qualifies the 18a
surface only. Native allocation exhaustion and invariant aborts are not promised
to be recoverable. Body-capacity/temp-work admission must be qualified when 18b
adds bodies; do not infer those guarantees from empty updates.

## Authority and integration decisions

The following rules guide 18b-18c; those integrations are not implemented in 18a:

| State/mode | Writer and behavior |
| --- | --- |
| Authored settings and edit-mode Transform | Existing AuthoredScene/project writer, shared by human/terminal commands and history. Preview does not advance physics. |
| Static runtime body | Simulation owner initializes the fixed pose. Reposition/rebuild is explicit at a mutation boundary; rendering never moves it. |
| Dynamic runtime body | Physics supplies committed poses at fixed simulation boundaries. Competing ordinary scene writes reject unless a deliberate teleport/reset operation supplies the agreed synchronization. |
| Kinematic runtime body | Simulation owner supplies bounded fixed-step targets; physics performs movement. Arbitrary render-frame edits cannot become authoritative targets. |
| Render pose | Read-only interpolation between committed simulation poses; it does not overwrite authored state or grant gameplay outcomes. |
| Server/client | Server owns authoritative simulation. Later client prediction reconciles corrections; cross-platform bit-identical lockstep is not assumed. |

Initial rigid bodies are scene roots with positive uniform scale; collider sizes
bake that scale. Parented bodies, local shear and negative/nonuniform rigid-body
scale reject. Visual children and render-only transforms keep their existing
affine support. 18b must define and validate explicit finite local pose/shape
bounds before exposing bodies. Single-precision native positions never contain
global world/cell coordinates; streaming/rebasing and static-world collision retain
M05. This empty owner adds no world-anchor or implicit-origin conversion API.

Offline scheduling remains 60 Hz; the provisional authoritative profile remains
20 Hz. Tick frequency and collision substeps are separate explicit policies.
Existing schedulers keep their debt, overload and pause rules; focus/minimize
never pauses server physics. Server bootstrap does not create a pretend world.

## Dependency and qualification

[Jolt 5.6.0](https://github.com/jrouwe/JoltPhysics/releases/tag/v5.6.0), commit
`e77f175595e64cb44218cc9d9d56fc365ad0e36a`, is locked with its exact archive,
source inventory, reviewed options and MIT notice. The CPU profile uses single
precision, x86-64 SSE2, static linkage and the matching dynamic MSVC CRT. Compute
backends, viewer/samples/tests, object streams, profiling/debug renderer, PCH,
unity and IPO are disabled. C++ RTTI and exceptions match the Core boundary;
Shark's selected sanitizers, including UBSan vptr checks, also instrument Jolt.
Vendor compilation remains separate from engine warnings-as-errors.

Use `SHARK_JOLT_ARCHIVE` for the exact offline archive; the canonical local cache
is `out/dependency-cache/`. Population and reuse verify locked bytes before use.
Server/Runtime closures and packages retain the Jolt notice while excluding SDK
headers and tools. The [18a handoff](handoffs/18a-physics-foundation.md) records
actual selected results and omissions. New physics fixtures use seed 1337 and
60 seconds; the public example has fixed inputs and a 15-second timeout.
