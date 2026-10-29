# ADR 0045: Private bounded CPU physics owner

Date: 2026-10-29. Status: accepted. Owner: assignment 18a.

Integrate exact Jolt 5.6.0 as a private static EngineCore dependency, with single
precision and the reviewed SSE2 CPU profile. Disable compute backends, native
tools/samples, object streams and profiling/debug rendering. Reuse the sole
dependency lock, exact offline archive population, target boundaries and package
notice checks. Public API and Server dependencies expose no Jolt types/headers.

Provide a uniquely owned real empty PhysicsSystem, fixed temporary arena and
single-threaded native jobs, accessed only by its creating thread. A private
process lease registers factory/types and restores prior native callbacks after
the final world releases resources. Reject foreign registration ownership.
Explicit close reports errors; destruction does not throw or silently transfer
a live owner to another thread.

Step uses a fixed configured integral tick rate and explicit collision substeps,
not elapsed/render time. Existing offline/server schedulers remain authoritative.
Native update failure faults the owner instead of retrying possible partial work.
The [physics contract](../physics-world.md) defines bounds and native allocation
limits. Empty stepping does not qualify body simulation, collisions or capacity.

Before bodies land, retain ADR 0005's root/positive-uniform-scale restriction and
define mode-specific writers: static poses come from explicit simulation-owner
boundaries, dynamic poses from physics, kinematic targets from the simulation
owner, and interpolated render poses are read-only presentation values. Authored
settings use shared human/terminal services; server gameplay authority remains
separate. Float local poses must remain distinct from global world/cell identity
and its double-precision offset.

No new scene component, serialization/protocol version, terminal capability or
Lua placeholder is added in 18a. Bodies/authoring, synchronization, layers/triggers
and editor/connected acceptance follow the [five checkpoints](../roadmap/physics-plan.md).
Queries/Lua bindings remain assignment 20 and static-world collision/rebasing M05.
