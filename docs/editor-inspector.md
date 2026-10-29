# Editor inspector and history

Assignment 16c extends the [scene workspace](editor-scene-workspace.md) with
component drafts and the existing shared undo/redo. The
[handoff](handoffs/16c-editor-inspector.md) records selected verification.
Desktop interaction remains paused under the user's
[amendment](backlog.md#desktop-ui-deferral); actual UI acceptance is NOT_RUN.

## Drafts and validation

The Editor observes the selected entity through the same retained project owner.
An inspector draft is an owned set of component values bound to that entity ID
and the complete SceneRevision. It is not another mutable Scene, a saved source
document or a reservation to commit. Selection changes and intervening edits
cannot silently retarget or rebase a draft. Reload is an explicit decision to
replace draft values with a fresh observation.

The inspector uses the existing [component catalog](scene-inspection.md) for
supported identities, field names, types, units, defaults and constraints.
Name and Transform are mandatory. Camera and MeshRenderer can be added, edited
or removed. The same Core component validators and retained authoring commands
make the final decision; display metadata never becomes validation policy.

Transform rotation uses quaternion `[x,y,z,w]`, translation is in meters, and
scale permits negative values with the existing absolute magnitude floor.
Camera values retain their projection-dependent near bound and far/near
relationship, including validation of fields inactive for the chosen projection.
MeshRenderer requires an explicit asset UUID and a uint32 mesh index; editing
this reference does not import an asset or prove that its mesh can be resolved.
The viewport's existing fresh-asset checks remain separate.

Apply is explicit; editing a widget does not mutate the scene on every keystroke.
One retained four-command batch updates Name, Transform, Camera and MeshRenderer
atomically and creates one history group when changed. No-op Apply adds no group.
Rejected or stale edits preserve authoritative values and history. A successful
mutation is reported even if refreshing display values subsequently fails.
Save uses the shared source persistence path and does not implicitly apply a
draft; its notice identifies unapplied edits that remain. Closing a project with
unapplied edits first offers **Keep editing** or **Discard draft and continue**,
then resolves any unsaved scene through the existing save/discard/cancel flow.
16d generalizes this to **Discard drafts and continue** for inspector and
[asset metadata drafts](editor-assets-console.md), preserving their separate writes.
Navigation, gizmos and continuous-drag grouping remain assignment 17.

## One history and retained outcome

The inspector and hierarchy share the same ScenePanel retained-mutation path.
Property/component edits and undo/redo use the original ProjectSession owner,
fresh mutation identities and expected revisions. All three mutation outcome
levels are inspected. An uncertain outcome blocks further scene edits until
lookup reconciles the original identity; it is never silently resubmitted under
a replacement identity.

History observation supplies synchronization status, direction counts and next
tickets. Undo and redo require both the observed ticket and scene revision.
A stale ticket cannot undo a different group's work. Successful movement
invalidates selection/drafts rather than rebinding entities recreated with new
generations. The editor does not automatically clear conflicting history.
History is controlled by explicit buttons; this increment adds no shortcut.

Saving preserves history and acknowledges only the saved scene revision.
Undo/redo after a save makes the scene conservatively dirty; save again to mark
that revision clean. Close/reopen creates a fresh owner with empty history.
No history journal or draft is added to source files.

## Scope and acceptance

This is an Editor-private adapter over existing schemas, queries, commands and
history. There is no new terminal RPC, public component, source format, gameplay
protocol or FeatureWorld case. Existing terminal editing remains the semantic
counterpart. Actual bidirectional human/terminal acceptance belongs to 16e.

Selected CPU fixtures cover edits, rejection, stale selection/drafts, shared
history conflicts and save behavior. Native interaction, focus, layout and the
resulting scene display remain in the [UI backlog](backlog.md). CPU evidence does
not qualify those checks or close full REQ-16.
