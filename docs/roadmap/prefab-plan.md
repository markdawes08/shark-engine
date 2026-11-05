# Assignment 21: prefabs and spawning

Base: `9bf652a`, initially clean `main`. Follow the [testing policy](../testing-policy.md)
and [prefab source contract](../prefab-assets.md). Full REQ-21 remains NOT_RUN until
its connected acceptance passes. Manual UI testing remains paused; record any
unperformed interactive checks in the existing [backlog](../backlog.md).

| Checkpoint | Scope |
| --- | --- |
| 21a | Versioned prefab source values, bounded shared component validation, stable internal references and explicit rejection of nesting. |
| 21b | Instance creation through the shared authoring owner, fresh entity IDs, reference remapping, simple overrides and one undo/redo group. Complete; selected affected checks pass in the [handoff](../handoffs/21b-prefab-instances.md). |
| 21c | Runtime and Lua spawn/destroy with bounded deferred requests, transform/physics/script initialization and reverse teardown, checked authority, replicas, ownership epochs and replay outcomes. |
| 21d | Shared Editor and terminal prefab file, creation and instantiation workflows, discovery, exact revision/retry reconciliation and saved-project handoff. |
| 21e | Connected instance independence, callback mutation, reload and repeated lifetime acceptance, including script, physics and render resource cleanup. |

21a is a pure CPU source codec. It creates no Scene or runtime entity, reads no
files and advertises no terminal method or importer. Subsequent checkpoints must
connect the existing owners rather than introducing another mutation path.

The first format contains one rooted hierarchy and the existing source component
set. Parent links are the current typed internal entity references. Preserve
asset IDs and project-relative script references; arbitrary extras are opaque.
Nested prefabs are rejected. Keep overrides small and explicit when 21b lands;
do not add an inheritance or automatic asset-update propagation system.

Runtime creation must preserve the Scene and physics write leases, checked
generations and bounded contact mutation queue. Initialize complete transforms
before native bodies and script callbacks, and define failure receipts before
exposing runtime creation. A local prefab ID or Region stamp grants no gameplay
authority. Explicitly test reconnection/replay/handoff against duplicate effects;
an ownership fence alone does not establish durable deduplication. M04/M05 retain
their connected movement and cross-region work.

Use focused affected-owner checks as each increment lands. Reuse their valid
results at 21e and add the connected behavior checks. The current manual UI pause
does not turn unperformed interactive evidence into a pass.
