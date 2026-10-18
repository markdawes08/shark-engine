# Assignment 07: Scene, transform and schema checkpoints

Date: 2026-10-18. Starting revision: `9e08702` on `main`.
Owner: `REQ-07`. Full assignment verification is **PASS** under the current-machine scope.
The current-machine scope and existing assignment order remain unchanged.
**07a, 07b and 07c are complete. The current assignment is M02; M02b owned
sessions is PASS under the current-machine scope. M02c replication is next.**
See the [M02b handoff](../handoffs/M02b-owned-sessions.md), [network plan](network-plan.md) and
[session contract](../network-sessions.md). The completed M02a transport gate and
historical assignment 07 evidence retain their scope; full REQ-M02 remains NOT_RUN.
The [07a handoff](../handoffs/07a-entity-ownership.md) records its 348 passing CPU
CTests. The [07b handoff](../handoffs/07b-transform-hierarchy.md) and
[receipt](../coverage/evidence/07b-transform-hierarchy.json) record 362 passing CPU
CTests with no skips and protected dependency/package qualification.
The [07c handoff](../handoffs/07c-schema-inspection.md) and
[receipt](../coverage/evidence/07c-schema-inspection.json) close full REQ-07 with
376 passing CPU CTests and no failures/skips, protected Runtime/Server builds and
fresh isolated package checks. All 221 qualification source inputs were unchanged.

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

Implemented: a fixed engine-owned Name/Transform catalog publishes version-1
schema IDs, property types, defaults, input ranges, units and scene-owner
mutability. Pure validation reuses the existing component validators. Owned,
immutable revisioned query pages provide bounded semantic inspection without a
viewport or registry access. See the [inspection contract](../scene-inspection.md)
and [07c handoff](../handoffs/07c-schema-inspection.md).

Gate: schema/validation agreement, deterministic bounded query results, stale
revision handling, owned snapshot lifetime and no EnTT or other backend leakage
beyond the documented public math types.
The existing Name/Transform component entries gain real schema/query coverage in
`shark.core.schema` and `shark.core.scene-query` (seed 1337, 20-second timeouts).
Qualification includes earlier lifetime/hierarchy behavior and the current
Windows/WSL protected target and package checks. Assignment 10 connects FeatureWorld.
The final gate is PASS: Windows Debug/Release/ASan pass 62 CTests each, WSL CPU
client Debug/ASan+UBSan pass 55 each, and WSL Server Debug/ASan+UBSan pass 40 each.
Schema coverage has 9 cases/654 assertions; query coverage has 19 cases/3,916
assertions. Source/link/license closure and isolated package execution pass.
Services/transactions, terminal adapters and editor UI remain owned by 09, 15 and
16. M02 follows the completed 07 checkpoint set and adds transport/session behavior.
