# Connected editor and terminal acceptance

Assignment 16e continues one saved project through the human Editor and public
`SharkAuthor` requests in both directions. **Actual UI acceptance is NOT_RUN.**
The [desktop pause](backlog.md#desktop-ui-deferral) must be explicitly lifted by
the user before launching any Editor, native fixture or desktop automation.
The connected headless adapter fixture is preparation; it does not establish UI
parity, focus/layout behavior, GPU qualification or full REQ-16 completion.

## Prepare one reference project

Use the [terminal example](authoring-protocol.md#complete-terminal-example) with
the CPU `SharkAuthor` and a fresh output beneath `out/16e-work/`. Omit capture and
export options. Preserve its summary and licensed source notices. Follow the
existing [Editor launch instructions](editor-shell.md#launch-and-use), using
verified GUI/scene shaders and an explicit current-PC adapter. Do not duplicate
shader preparation or regenerate baselines merely for this checkpoint.

The project is `project/Terminal.sharkproject`, with `scenes/Main.sharkscene`.
IDs below share prefix `15e00000-0000-4000-8000-`; concatenate each suffix exactly:

| Identity | Suffix | Initial independently expected state |
| --- | --- | --- |
| Project / world / scene | `000000000001` / `000000000002` / `000000000003` | Same identities throughout all handoffs |
| Camera entity | `000000000004` | Name `Terminal camera`; root; translation `[0,0,3]` |
| Triangle entity | `000000000005` | Name `Unsaved triangle 雪`; root; translation `[-1,-1,0]`; scale `[2,2,2]` |
| Mesh / resource assets | `000000000006` / `000000000007` | glTF root and its binary dependency; client audience, MIT notice |

Both rotations are `[0,0,0,1]`; camera scale is `[1,1,1]`. Camera is orthographic,
height `3`, near `1`, far `9`, vertical FOV `pi/3`. The triangle has no Camera;
its visible MeshRenderer uses the mesh ID and index `0`. Camera has no
MeshRenderer. Inventory is `models/triangle.gltf.sharkasset` and
`models/triangle.bin.sharkasset`; target is `terminal-example`, side `client`.

Use the example's bounded `Author`, `entities`, `snapshot` and `mutate` helpers
for terminal exchanges, or the exact [public protocol](authoring-protocol.md).
Preserve complete replies, including nested mutation results and publication
durability. Do not equate request correlation with a mutation retry identity.

## Terminal to Editor, then back

1. Open the saved project in a live terminal owner. While it remains open, use
   **Open project** in the actual Editor; require a visible busy/conflict result
   and no lost terminal state. Explicitly `project.close` in the terminal, then
   open successfully in the Editor. Confirm the identities and initial values.
2. In Scene, set **Viewport resources / Import target** to `terminal-example`,
   enter both sidecars, and choose **Load asset inventory**. Select the camera
   and **View through camera**. Require the actual imported triangle to appear.
3. Select the triangle, **Inspect selection**, change Name to `Editor handoff`
   and translation to `[-0.5,-1,0]`, then **Apply changes**. Keep all other values
   above unchanged. Check hierarchy, dirty state and inspector values; reselect
   the camera explicitly after revision changes and refresh the viewport.
4. Make another inspector draft without applying it. **Save scene** must save
   only applied values and report the remaining draft. Attempt **Close project**,
   choose **Keep editing**, and verify both the project and draft remain. Use
   **Reload selection / discard draft** explicitly before continuing.
5. **Refresh history**, **Undo**, then **Redo**; verify the expected before/after
   values. Saving must retain history; undo after saving must mark the scene
   dirty. A stale selection/draft must require refresh rather than retargeting.
   Finish with `Editor handoff`, translation `[-0.5,-1,0]` and a dirty scene.
6. In Assets load the complete inventory and matching target. **Read selected
   metadata**, change only root metadata `origin` to `16e Editor handoff`, and
   **Write metadata file**. Preserve its publication receipt. Reload inventory;
   scene save/undo must not undo that separate metadata publication.
7. In the copied glTF source only, change `asset.generator` to `16e UI import`
   using ordinary filesystem editing. Record the change; leave geometry and the
   binary untouched. Reload inventory, **Check selected cache**, then **Import
   selected**. **Refresh jobs** must not publish a ready result. Use **Publish
   ready result**, verify success/freshness/new root key and unchanged dependency
   identity, then **Acknowledge result**. Inspect the retained artifact and Console
   diagnostics. Refresh the explicit viewport; retain the unsaved scene/history.
8. While Editor owns the project, a separate terminal `project.open` must return
   conflict. In Editor, **Close project / Cancel** must retain edits. Then **Save
   and close** must publish the intended scene and release ownership.

## Terminal continuation and final Editor reopen

Reopen through a fresh terminal owner. Query fields `name`, `transform`,
`hierarchy`, `camera`, `mesh_renderer`, and independently inspect saved JSON.
Compare every value above plus the declared Editor edits, metadata origin,
generator change and resource bytes. Require clean state, empty new history and
fresh owner/retry scopes. Refresh the explicit inventory; check the same target's
fresh cache and matching root/dependency descriptors, rather than trusting a UI
label or the previous process's report alone.

Through `scene.mutate`, rename the triangle to `Terminal return`; verify dirty
close rejects, then explicitly `scene.save` with the current revision and
`project.close`. Reopen in Editor: require that name, all other final values,
fresh cache/viewport, clean state and empty history. Exercise an unapplied metadata
draft close decision too; discarding that draft must not revert its earlier write.

## Evidence and remaining qualification

Run this connected workflow on both available GPUs, recording device, build,
actions, terminal replies and failures concisely. Complete the existing
[16b native scene-composition smoke](editor-scene-workspace.md#deferred-native-qualification)
and outstanding [UI backlog](backlog.md#desktop-ui-deferral) on final relevant
inputs; those checks cover rendering/lifetimes that this workflow cannot replace.
Reuse applicable earlier passes under the [testing policy](testing-policy.md).
Do not rerun unrelated engine matrices. Keep PASS, FAIL and NOT_RUN distinct;
full REQ-16 closes only after required connected UI and outstanding native gates.
