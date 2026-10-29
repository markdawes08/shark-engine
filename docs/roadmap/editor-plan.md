# Assignment 16: human editor checkpoints

Date: 2026-10-29. Base: `83094fe`, initially clean `main`. Owner: REQ-16.
This five-checkpoint plan follows the completed
[terminal acceptance](../handoffs/15e-terminal-acceptance.md),
[human/agent contract](human-agent-plan.md), [MMORPG plan](mmorpg-plan.md) and
[testing policy](../testing-policy.md). Scope is the current Windows PC; portable
shared-owner behavior retains WSL coverage. Full REQ-16 remains NOT_RUN;
`current_assignment` is 16 because its first checkpoint is now active.

| Checkpoint | Deliverable and acceptance |
| --- | --- |
| 16a: Shell and project ownership | Native Editor UI through the existing window/input/graphics owner, exact Editor-only ImGui dependency, project open/save/close and dirty-state decisions. One shared project owner preserves revisions, publication outcomes and busy conflicts; focus/text/modal routing gates shortcuts. Verify owned GUI geometry, actual blending/clipping/update/resize on both GPUs and native close cancellation. |
| 16b: Viewport, hierarchy and selection | Display the active authored scene with the existing extraction/viewport services. Bounded hierarchy queries, stable identity selection, create/delete/reparent through shared commands, and explicit stale-selection recovery. Keep presentation separate from authoritative scene state; viewport navigation/picking remain 17. |
| 16c: Inspector and history | Schema-driven editing where practical, common property/component validation, retained command outcomes and shared undo/redo. Cover valid/rejected/stale edits, conflicting undo, dirty-state changes and save preserving history. UI drafts are temporary values, never another Scene. |
| 16d: Assets, jobs and console | Adapt the existing catalog/import/job and diagnostic services into asset and console panels. Preserve bounded paging, cancellation, explicit completion drain/take, last-valid cache output and owner release. Advertise only implemented actions; no second scheduler/importer or gameplay console. |
| 16e: Connected human/terminal acceptance | Use actual UI interaction and public terminal requests to continue the same saved project in both directions. Independently compare semantic state after edit/import/save/reopen, test busy ownership and unsaved/history conflicts, and finish affected subsystem coverage. Reuse valid earlier focused evidence. |

16a implementation and its deferred desktop qualification are tracked in the
[shell handoff](../handoffs/16a-editor-shell.md), [contract](../editor-shell.md)
and [ADR 0040](../architecture/0040-human-editor-shell.md). Later rows remain
planned. They do not advertise implemented panels or close the full requirement.

The user's 2026-10-29 [desktop-testing amendment](../backlog.md) permits coding
16b and later checkpoints while desktop interaction is paused. Continue affected
headless checks and append each checkpoint's unexecuted desktop acceptance to
that backlog. Do not launch the Editor or windowed GPU fixtures until the user
explicitly resumes desktop testing. Existing passes remain evidence for their
original inputs; later UI behavior and full REQ-16 remain unqualified until the
deferred acceptance is completed.

All authoring writes use the retained shared service from assignment 15.
Closing releases the cooperating writer; reopen creates a new scene incarnation
and history scope. The editor does not attach concurrently to another writer.
Failures retain useful errors and original publication/mutation outcomes rather
than silently reopening, retrying, saving or discarding.

No player protocol, server authority, world streaming or MMO capacity changes
belong here. Camera navigation, ray picking, translate/rotate/scale gizmos,
snapping and continuous-drag undo transactions belong to assignment 17. ImGuizmo
is not introduced before that owner.

Select verification by the changed owners. Native GUI changes need the available
GPUs and real window lifecycle evidence; CPU adapter checks do not replace those
results. A later UI acceptance pass is mandatory even when earlier shared-owner
unit/process checks pass. Preserve failure evidence and stop once the selected
checks pass; do not add repeated whole-project matrices or another evidence system.
