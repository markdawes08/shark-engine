# ADR 0069: connected authored-world streaming

Date: 2026-11-06. Status: accepted for M05g implementation; qualification is
recorded separately. Extends [ADR 0068](0068-process-region-execution.md),
[cell residency](0064-world-cell-residency.md) and
[traversal](0065-world-traversal.md). Follow the
[acceptance contract](../seamless-world-acceptance.md) and
[testing policy](../testing-policy.md).

## Decision

Connect saved authored cell files to the existing gateway and actual Server
executors. Each executor retains an immutable catalog of at most eight cell
keys, content revisions, collision-proxy identities and optional bounded absolute
file descriptors. A descriptor binds the complete file's SHA-256 digest and byte
count. Imports, source writes and discovery are outside the runtime reader.

Portable region image version 2 records that catalog, unloaded/resident/active
state, residency limits and value-only pursuit intent. Version 1 remains readable
with its original active inline-cell defaults. World-cell payload versions 1 and
2 retain their separate existing compatibility rules; adding a file descriptor
does not turn a geometry-free v1 payload into traversal-ready content.

The existing single reader worker prepares cells while requested native ticks
continue. Explicit creating-thread drain publishes complete residents; activation
and deactivation update the same traversal/native owners. Only validated catalog
collision proxies may enter or leave the otherwise fixed gameplay hierarchy.
Pins, native contacts, body footprints and retained spawn positions constrain
detach. Failed reads and rejected publications preserve last-valid active content.

Normal checkpoints preserve the committed resident/active state while jobs remain
ephemeral owner state. A transfer cut requires every original job to settle and
be explicitly taken. Do not migrate pending reads or copied job handles. Gateway
cell tickets include the executor incarnation, so an old ticket cannot address a
replacement reader. Teardown joins the reader before releasing its captured data.

## Pursuit and frames

NPC pursuit binds an existing native dynamic sphere to an exact current target
life. The executor resolves current native authored support and projects it onto
the bounded navigation surface. It rebuilds paths against current tiles and
adopts progress only after checked native movement agrees. Missing readiness,
blocked motion, cancellation and unavailable lives remain explicit outcomes.
An adopted offer retains a canonical world steering anchor. A fresh path may
start there only while actual current support still agrees within the existing
0.02 m horizontal and 0.04 m support bounds and the current graph accepts it;
otherwise the anchor is discarded. This retains checked portal progress across
native float rounding without changing path tolerances, moving a body or skipping
an unadopted segment. Portable state contains intent, committed counters and this
value-only anchor, never old navigation IDs or cursors.

Allow a lossless, cell-aligned server-frame change when staging a fresh child.
The frame stays fixed during each executor's lifetime. Check the complete image
before freezing/SQL: global positions, native float values, body targets, mesh
vertices and bounded local coordinates must remain exactly representable. Reject
a candidate that would requantize native data. Source handles remain relocation
keys and Jolt solver caches are rebuilt under ADR 0068.

The gateway retains its original client publication frame. Convert new server
poses and movement corrections into that frame before public observations.
Movement and replica wire version 1 remain unchanged; no player origin-shift
message or new session/replica identity is introduced. Source file anchors stay
unchanged. This refines ADR 0068's fixed-content/frame profile into a fixed catalog
and a fixed frame per incarnation, with explicit native proxy transitions.

## Acceptance boundary

Use actual saved near/far authored projects, native files, mutually authenticated
player traffic, real Server children and the existing durable ownership worker.
Connect streaming pressure, pursuit, cross-boundary combat, repeated handoffs,
stable public frames and original uncertain-route recovery. Seeded FaultLink
faults shape verified combat observations; reliable movement corrections apply
before shaping, and no TLS/TCP bytes are corrupted or reordered.
Reuse the qualified M05f source/destination crash matrix and add affected lost
combat/step replies rather than repeating unrelated evidence.

The connected fixture caps residency at two cells and pending bytes at one
maximum-cell payload plus retained PreparedAssets. A second pending reservation
rejects before reader IO. It samples each live child's working-set/VmRSS against
a 1 GiB functional budget. Report sampled RSS separately from conservative
logical residency/pending charges. Keep the provisional 500 ms
handoff quiescence budget observable; an overrun grants no timeout authority or
safety bypass.

This remains one complete interacting island executing in one owned child at a
time. Concurrent independent neighboring islands, production capacity, final
cooked manifests and later LOCAL-MMO-1 load/endurance retain their assigned
owners. Manual UI testing stays paused; native World cells interaction remains
NOT_RUN under [BL-M05D-UI-01](../backlog.md#deferred-items), independently of this
CPU gate. This ADR makes no test-pass claim.
