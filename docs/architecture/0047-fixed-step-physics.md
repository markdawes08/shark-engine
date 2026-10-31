# ADR 0047: Fixed-step Scene ownership and presentation poses

Date: 2026-10-31. Status: accepted. Owner: 18c.

Extend the existing PhysicsWorld rather than add a simulation scheduler or a
second Scene model. Explicit begin/end boundaries acquire/release one Scene write
lease after complete native/component inventory validation. The Scene retains
only a weak lease; the native owner retains no Scene pointer. Ordinary immediate,
deferred and authoring-candidate writes reject during simulation. Closing the
world restores ordinary access without dereferencing a possibly destroyed Scene.

Runtime code binds a mutable Core Scene, normally a prepared-content instance.
AuthoredScene's existing const borrow cannot bind. Source authoring, history and
retained mutation identity remain separate from transient simulation. Static
reposition and dynamic teleport are explicit simulation-owner operations;
kinematic targets are bounded and consumed at fixed ticks. Collider scale or
inventory changes require end/edit/rebuild/begin. Native pose output validates
every affected subtree before one Scene publication. A failure after native work
faults the owner and preserves the last complete Scene; it is not native rollback.

Admit nonempty work against a conservative arena requirement derived from the
locked native allocation paths. Keep discrete primitive bodies, single-threaded
native jobs and no native constraints/listeners/soft bodies. Disable large-island
splitting, bound native in-flight pairs explicitly, and meter the fixed allocator
without a heap fallback. Native invariant/OOM limits remain explicit. Details
and the equation belong in the [simulation contract](../physics-simulation.md).

Physics steps use the existing configured fixed tick and independent collision
substeps. Thin adapters consume OfflineScheduler/ServerScheduler, rejecting rate
or tick-index mismatch before clock admission. Offline pause/discard and server
retained debt/overload/cancellation semantics remain those schedulers' policies.
The Server bootstrap and terminal static integrity runner do not become gameplay
loops merely because this CPU integration exists.

Owned interpolation snapshots retain Scene revision, world identity, tick and
alpha. Translation lerp and shortest-arc quaternion slerp produce presentation
poses, never Scene writes. CPU extraction validates/recomposes all affected
subtrees before camera derivation and culling; snapshots carry physics provenance
and cannot authorize viewport picking. Authored previews and simulation overrides
cannot overlap. No source, prepared, player-wire or terminal protocol version
changes. Native editor/play acceptance retains 18e and the desktop backlog.
