# Connected viewport and terminal acceptance

17d connects the [viewport checkpoints](roadmap/viewport-interaction-plan.md).
**Actual UI/GPU acceptance is NOT_RUN.** Execute this procedure only after the
user explicitly resumes desktop testing. Headless adapter/process results do not
qualify native input, focus, layout, rendered previews or full REQ-17.

## Reference project and independent expectations

Create a fresh project under `out/17d-native/` using the
[public terminal example](authoring-protocol.md#complete-terminal-example), without
capture/export. Preserve its licensed glTF/binary and notices. Use the same
identities, paths, target and sidecars as [editor acceptance](editor-acceptance.md).
The camera and triangle IDs are `15e00000-0000-4000-8000-000000000004` and
`15e00000-0000-4000-8000-000000000005`.

Through the public terminal's existing retained command path, prepare this
reference state, save it and close the terminal owner:

- Create `17d00000-0000-4000-8000-000000000001`, Name `Transform parent`, as a
  root with translation `[0,0,0]`, quaternion `[0,0,sqrt(0.5),sqrt(0.5)]` and
  scale `[2,2,2]`.
- Set the triangle's local TRS to translation `[1,0,0]`, identity quaternion and
  scale `[1,1,1]`; reparent it to that parent using `keep_local`.
- Set only the camera's orthographic height to `16`. Its translation `[0,0,3]`,
  identity rotation/unit scale and other fields remain those of the public example.
  Preserve all existing names, mesh references, project/world/scene identities,
  source geometry, metadata and import target.

Use fresh `scene.mutation_status` identities and exact current revisions. Check
admission, retained original outcome and operation result separately. Each row
below starts from the preceding committed row, except the explicitly stated undo.
All quaternions use `[x,y,z,w]`; rotations are positive right-handed Z rotations.

| Actual gesture | Expected triangle local result | Independent world result |
| --- | --- | --- |
| World Translate Y, Snap 1 meter, release at +4 meters from the start | T `[3,0,0]`, R identity, S `[1,1,1]` | Pivot `[0,6,0]` |
| One Undo, then one Redo | Undo restores T `[1,0,0]`; Redo restores T `[3,0,0]` | Pivots `[0,2,0]`, then `[0,6,0]` |
| World Rotate Z, Snap 15 degrees, release at +90 degrees | T `[3,0,0]`, R `[0,0,sqrt(0.5),sqrt(0.5)]`, S `[1,1,1]` | Pivot `[0,6,0]`, world rotation 180 degrees |
| Local Translate X, Snap 1 meter, release at +2 meters | T `[3,2,0]`, same R/S | Pivot `[-4,6,0]` |
| Local Scale X/Y/Z in separate drags, Snap factor step 0.5, resulting factors `[-2,1.5,0.5]` | T `[3,2,0]`, same R, S `[-2,1.5,0.5]` | Pivot `[-4,6,0]`, world linear matrix `diag(4,-3,1)` |

The CPU process fixture supplies multiple factors in one mathematical operation;
the UI exposes single-axis scale handles, so its three scale gestures produce
three separate history groups. Each individual continuous drag must still undo
in one step. Compare semantic matrices using the existing transform tolerance;
do not require identical quaternion text or widen tolerances.

## Actual editor and terminal continuation

1. Launch the actual Editor using its [documented command](editor-shell.md#launch-and-use),
   verified GUI/scene shader bundles, required validation and an explicit current-PC
   GPU. Open the reference project. Load both `terminal-example` sidecars, select
   the camera and **View through camera**, then select the triangle.
2. Execute the gestures above through the actual colored handles. During a drag,
   require geometry preview with unchanged dirty state; release changes the
   authoritative transform once. **Inspect selection** and **Refresh history**
   after release, compare the literal expected values, and test the specified
   undo/redo. A known transform commit must retain navigation and refresh the
   committed revision before another gesture. Between gestures, pan/orbit the
   display view as needed to keep the pivot and complete rotation ring inside
   the image; display navigation must preserve the authored reference transforms.
3. Exercise Escape, leaving/reentering held, focus/text/modal/tab changes, image
   relocation/resize, tool/space/snap changes and close while dragging. Require
   the committed image to return without a mutation or history entry. A fresh
   press inside may start a new drag; a held button cannot resume one. Handle
   press/release must never also pick geometry or navigate the camera.
4. Reject zero scale and a world rotation requiring local shear under a rotated
   nonuniform parent. Restore the documented reference through explicit edits or
   undo before the final comparison. Verify an invalid newest candidate cannot
   commit an earlier pose. Drag scale through multiple sampled positions across
   snap-to-zero: committed rendering must return while invalid, continued held
   motion may produce a negative valid candidate, and release commits once.
   Releasing in the invalid interval must make no edit. Exercise a source/asset freshness failure before
   release and require visible rejection with no scene commit.
5. At the final reference result, save and verify history remains available;
   undo after save must become dirty. Redo/save back to the final result and close.
   A real competing `SharkAuthor` process while Editor owns the project must
   receive busy/conflict without changing the editor's state.
6. Reopen through a fresh public terminal owner. Independently inspect saved JSON
   and query the triangle's local/world transforms, parent and components. Require
   clean state, empty new history and final world pivot `[-4,6,0]` with linear
   `diag(4,-3,1)`. Reproduce that result using literal `set_transform` input from
   the same baseline and compare semantic state, rather than trusting the editor
   receipt alone.
7. Within that terminal owner, capture a revision, commit a newer valid edit, then
   submit a fresh currently admissible mutation ID with the old expected revision.
   Require a retained conflict and unchanged newer values/history. Replaying an
   old admitted ID tests retry replay instead of stale-edit rejection; identities
   from another owner require reconciliation. Never carry them across reopen.
8. Set the final triangle local translation to `[4,2,0]`, preserving its parent,
   rotation and signed scale. Save/close, then reopen in the actual Editor. Require
   world pivot `[-4,8,0]`, the same world linear matrix, clean state, empty history
   and a fresh rendered view. Continue editing successfully through the controls.

## Remaining native evidence

Run this connected procedure on both current-PC GPUs. Complete the applicable
[UI backlog](backlog.md#desktop-ui-deferral), including navigation/picking with
actual image offsets/DPI, affine and mirrored parents, newly revealed descendants,
preview rejection/cancellation and the retained required-validation scene-composition
lifecycle fixture. Reuse valid earlier evidence under the
[testing policy](testing-policy.md); do not regenerate baselines or run unrelated
engine matrices. Record commands/device, important actions, terminal replies and
failures concisely in the [17d handoff](handoffs/17d-connected-viewport-acceptance.md).
Full REQ-17 closes only when the required actual UI/rendering gates pass.
