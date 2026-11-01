# Connected physics acceptance

18e extends [editor acceptance](editor-acceptance.md) and
[viewport acceptance](viewport-acceptance.md). The user resumed desktop testing
on 2026-10-31, after 18e implementation. Preserve earlier results and record
PASS, FAIL and NOT_RUN for the actual actions performed on each current-PC GPU.

Use a fresh copied saved project, verified GUI/scene shaders, required validation
and an explicit GPU UUID. Keep the public terminal owner closed while the Editor
owns that project; a competing writer must reject without losing editor state.

1. Add a visible root dynamic box and a static floor. Through the actual
   inspector, exercise all RigidBody fields: motion, shape, half extents, radius,
   capsule half height, mass, friction, restitution, collision layer/mask and
   trigger. Apply is one undo group. Undo/redo and save/reopen must preserve the
   complete settings; invalid dimensions, nonuniform/negative scale and a body
   under a parent must visibly reject without partial edits.
2. Select the explicit Camera/content target and Play. Observe gravity and floor
   collision in the actual viewport. Pause, single-step and resume; source
   transforms, dirty state and history remain unchanged by runtime movement.
   The play image must not permit authoring picking, gizmo commits or source
   mutation. Inspect the unchanged source pose after Stop.
3. Add an overlapping trigger and verify it detects without collision response.
   Exercise bilateral masks, then runtime body removal while contact exists.
   Observe owned enter/exit information and preserved visual geometry. Stop and
   start again: the source body returns and the runtime session/tick reset.
4. Exercise text/modal focus, Scene/Assets/Console tabs, minimize/restore and
   window/viewport resize during play. No hidden catch-up may result from a
   paused/blocked interval. Stop, close cancellation, save/discard and close with
   live play must cleanly release native state and the project writer.
5. Save/close and reopen through an independent public `SharkAuthor` process.
   Compare saved JSON and typed fields with the intended authored state, not a
   runtime pose. Start/step/remove/stop through `scene.physics`; verify source
   bytes/history/revisions remain unchanged. Exact duplicate operations must
   return their original receipt; changed/old session or sequence input rejects.
   Save a terminal component edit, close, and confirm it in the actual Editor.

Run on RTX 4070 Laptop and Intel. Complete the applicable earlier connected UI,
navigation/gizmo and scene-composition backlog checks without replacing their
fixed workloads or tolerances. Record concise actions, independent saved-state
comparisons, device/build/validation identity and any failures in the current
handoff. Do not qualify production MMORPG capacity, Lua execution or Server
region integration from this development preview.
