# Assignment 13: scene rendering checkpoints

Date: 2026-10-27. Base: `c1a3e64`, clean `main`. Owner: REQ-13.
The user approved five subchunks. Follow the [testing policy](../testing-policy.md),
[render conventions](../architecture/0005-simulation-and-rendering.md),
[MMORPG plan](mmorpg-plan.md) and [human/agent plan](human-agent-plan.md).
Full REQ-13 remains NOT_RUN until the connected rendering acceptance in 13e.
**13a, 13b and 13c are PASS under current-machine scope**; see their
[component](../handoffs/13a-render-components.md),
[prepared-content](../handoffs/13b-prepared-content.md) and
[extraction](../handoffs/13c-scene-extraction.md) handoffs. Next: 13d.

| Checkpoint | Deliverable and gate |
| --- | --- |
| 13a: Components and authoring | Optional Camera and MeshRenderer CPU values, validation/schema discovery, checked immediate/deferred Scene mutation, shared create/set/remove commands, preview/history/retries, source capture/activation/save/reload and FeatureWorld. Verify atomic rejection, lifetime/revision behavior and persistence without a GPU. |
| 13b: Runtime asset ownership | Runtime-safe prepared asset/scene representation and loading, immutable asset ownership and stable built-in cube/plane IDs. Tools prepares data; Runtime cannot depend on Tools, raw glTF, JSON source readers or image importers. Verify bounded decode, missing/corrupt references, sharing and lifetime. This is not assignment 14's export workflow or 31's final cook. |
| 13c: Scene extraction and cameras | Extract scene-local transforms and referenced meshes, perspective/orthographic view/projection, bounds and frustum culling. Keep the global cell anchor distinct from bounded local render coordinates. Verify independent geometry/math expectations, mirrored/sheared transforms, clipping and culling. M05/M06 retain streaming, origin rebasing, LOD and animation. |
| 13d: Offscreen rendering | Depth-tested static mesh draws and a resizable offscreen viewport using the existing device/resource/shader owners. Qualify current-PC GPUs with geometric readbacks for depth, winding, visibility and resize. Temporary debug/unlit materials explicitly do not claim PBR or final transparency/lighting quality. |
| 13e: Connected Runtime acceptance | Load FeatureWorld and an imported scene in Runtime, exercise built-ins in a fresh project, camera transforms/clipping/culling/resize and destruction/reload of visible entities without resource errors. Reuse valid earlier evidence and close the complete affected rendering subsystem under the testing policy. |

13a uses the [component contract](../render-components.md) and
[ADR 0032](../architecture/0032-render-components.md). 13b uses the
[prepared content contract](../prepared-content.md) and
[ADR 0033](../architecture/0033-prepared-runtime-content.md). Each later checkpoint
updates its concrete contract before changing that boundary. 13c uses the
[CPU extraction contract](../scene-rendering.md) and
[ADR 0034](../architecture/0034-scene-extraction.md). No editor or terminal
adapter is introduced before its assigned owner (16 and 15 respectively).

Verification is focused per checkpoint: affected primary Debug regressions,
portable WSL smoke and one relevant sanitizer selection for CPU work; actual
device checks only when rendering behavior lands. Shared ownership, persistence
and target-boundary changes trigger their affected consumer checks. Keep the
existing cases, seeds, timeouts and independent expectations. Do not repeat
unrelated storage/network/configuration matrices or generate source-freeze ledgers.
