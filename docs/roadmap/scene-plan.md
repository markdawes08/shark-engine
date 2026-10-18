# Assignment 07: Scene, transform and schema checkpoints

Date: 2026-10-18. Starting revision: `9e08702` on `main`.
Owner: `REQ-07`. Full assignment verification remains **NOT_RUN** until 07c.
The current-machine scope and existing assignment order remain unchanged.
**07a and 07b are complete under the current-machine scope. 07c is next.**
The [07a handoff](../handoffs/07a-entity-ownership.md) records its 348 passing CPU
CTests. The [07b handoff](../handoffs/07b-transform-hierarchy.md) and
[receipt](../coverage/evidence/07b-transform-hierarchy.json) record 362 passing CPU
CTests with no skips and protected dependency/package qualification.

## 07a - Entity ownership and identity

Implement private, exactly pinned EnTT in EngineCore behind an engine-owned scene
interface. Establish unique scene incarnations, checked slot generations,
distinct authored and durable identities, world/region/replica scope, positive
authority epochs and immutable reference values. Make create/destroy during
iteration explicit through a bounded deferred queue and one owner thread.
Specify and test global integer cells with canonical local double coordinates
before selecting Transform storage.

Gate: stale and malformed handles, slot reuse and generation exhaustion,
cross-scene/reload/reconnect references, authority metadata mismatch, duplicate
identities, capacity limits, mutation boundaries, callback failure and wrong-thread
access. Check numerical boundaries, overflow, nonfinite input and deterministic
coordinate properties. Run Windows CPU/sanitizer and existing WSL CPU/headless
checks, protected Runtime/Server builds and dependency/license closure. Keep EnTT
out of public headers and public usage requirements. See the
[scene contract](../scene-contracts.md) and [07a handoff](../handoffs/07a-entity-ownership.md).

## 07b - Name, Transform and hierarchy

Implemented: centrally configured GLM, mandatory Name and local TRS, validated
parent/child ownership, cycle prevention and atomic subtree deletion. Keep-local
and keep-world reparenting reject nonrepresentable local TRS before publication.
The existing scene writer, deferred queue and revision own these operations.
The [transform contract](../transforms.md) applies ADR 0005's meters, axes,
handedness, quaternion/matrix and scale conventions. Cached world matrices remain
in the scene's local frame; global WorldPosition and future M05 origins are separate.

Gate: parent/child consistency, cycles, stale parents, deletion, scene isolation,
nonuniform/negative scale, nonfinite/degenerate transforms, matrix propagation and
both reparent policies. Update independent lifetime tests and the future schema
inputs without exposing EnTT. Current-machine CPU/sanitizer, dependency and
package qualification is PASS in the linked handoff and receipt. No physics body
or live world handoff is implied.

## 07c - Engine schemas and semantic inspection

Publish Name/Transform schema IDs, fields, defaults, ranges, units, mutability and
validation through engine-owned values. Add bounded immutable revisioned queries
for semantic inspection without a viewport or registry access. Complete REQ-07's
coverage and qualification, including earlier lifetime/hierarchy behavior.

Gate: schema/validation agreement, deterministic bounded query results, stale
revision handling, owned snapshot lifetime and no EnTT or other backend leakage
beyond the documented public math types.
Extend the existing Name/Transform component entries with real schema/query
coverage; assignment 10 connects FeatureWorld.
Services/transactions, terminal adapters and editor UI remain owned by 09, 15 and
16. M02 follows the completed 07 checkpoint set and adds transport/session behavior.
