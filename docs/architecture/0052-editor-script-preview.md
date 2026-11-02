# ADR 0052: Shared Lua preview for human and terminal authors

Date: 2026-11-01. Status: accepted for checkpoint 19d.

Extend the existing retained [physics preview](0049-editor-physics-preview.md)
with one client-presentation ScriptRuntime over the same frozen runtime Scene.
The native Editor and terminal use the same scoped `scene.physics` owner,
receipts and scheduler-driven fixed steps. `scene.run` remains a static integrity
case. This development preview does not grant server gameplay authority or expose
player script uploads.

Capture enabled script source through ProjectFiles under the retained project
owner, with typed project/scene path exclusion, bounded source snapshots and an
exact content-revision recheck before publication. Text publication through the
shared owner is blocked during play; externally changed files are not silently
reloaded into the captured VMs and can be reconciled for a later preview. Source and history remain
separate from runtime effects. Maintain the existing retry, fault, source-revision,
physics and teardown boundaries; an error after work is not a rollback.

Client presentation callbacks cannot write authored or authoritative Scene
state. On each explicit fixed step, run `on_update` and `on_fixed_update` at the
existing offline tick, then step physics; on stop, tear down scripts before
native physics and the Scene. This is a deterministic fixed-rate preview update,
not a separate render-frame scheduler. Return bounded owned contextual logs and
errors through retained receipts. Preserve cleanup and recovery after syntax,
callback and instruction-limit failures.

Standalone Runtime and export continue to reject Script-bearing scenes: source
capture for local preview is not cooked content closure. Future server gameplay,
physics script bindings and packaged execution remain with their respective
owners. This does not promise a hostile-code sandbox or production capacity.
