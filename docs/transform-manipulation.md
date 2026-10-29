# Transform manipulation and drag transactions

17b adds pure CPU manipulation in EngineTools and an editor-private drag adapter
over the existing ProjectSession/ScenePanel. Follow
[ADR 0043](architecture/0043-transform-manipulation.md), the
[transform rules](transforms.md) and [viewport checkpoints](roadmap/viewport-interaction-plan.md).
Gizmo controls and rendered candidate presentation belong to 17c. Actual desktop
acceptance remains deferred under the user's pause.

## Shared math

The public [helper](../engine_tools/include/shark/tools/transform_manipulation.hpp)
accepts the starting local Transform, the parent's full scene-local world matrix
(identity for a root), and a cumulative TransformManipulation. It returns an
owned local Transform or an explicit error. It does not observe or change a
Scene, allocate a mutation identity, or establish a successful scene preview.
Every update is relative to the same starting pose; repeated updates do not
accumulate snapping error.

| Operation | Local space | World space |
| --- | --- | --- |
| Translate | Rotate the supplied displacement by the starting local rotation, ignoring entity scale, then add it in the parent frame. | Apply the displacement in the scene's world axes and convert the direction through the full inverse parent matrix. |
| Rotate | Postmultiply the starting local rotation by an axis-angle rotation. | Rotate the full world basis about the entity's world position, then convert through the inverse parent and decompose. |
| Scale | Multiply starting local scale by the supplied factors. | Scale the full world basis along world axes about the entity's world position, then convert through the inverse parent and decompose. |

World here means the scene's common local frame. It is not an MMORPG global cell
coordinate or a render-origin-relative position. No cell anchor, player authority,
replication or live durable state changes.

Translation uses meters; rotation uses radians and a nonzero finite axis; scale
factors are dimensionless. Optional snap steps must be finite and positive.
Translation components and rotation angle snap to the nearest step, with halfway
values rounded away from zero. Scale snaps each factor's displacement from one,
so identity remains identity. A snapped no-op preserves the canonical starting
TRS, including its signed scale representation.

Finite negative and nonuniform scale are supported under the existing absolute
local scale floor of `1e-6`. Singular/unresolved matrices, nonfinite values,
overflow and a world result that requires local shear reject. Rotated nonuniform
or sheared parents are not flattened into rotations: the full affine parent is
used. Existing decomposition tolerances are unchanged. Valid local math still
requires complete descendant/hierarchy and history-budget validation by the
authoring owner before it can commit.

## Drag lifecycle

The editor's [TransformDrag](../editor/transform_drag.hpp) borrows ScenePanel;
neither object owns another mutable scene. Begin captures the selected stable ID,
full revision, selection observation, starting local TRS and full parent matrix.
Parent and child observations use the same expected revision. Updates calculate
from this baseline and call the existing `scene.preview` with one `set_transform`.
Only a successfully previewed owned value is eligible for commit. A rejected
newest update clears eligibility, preventing release from applying an older pose.

Preview changes no scene revision, dirty baseline, source, history or retained
mutation sequence. It reserves nothing. Commit revalidates against the captured
revision through ScenePanel's existing retained mutation path, sending exactly
one `set_transform`. A changed drag is one history entry containing one command;
an unchanged drag adds no undo entry. It does not rewrite Name, Camera or
MeshRenderer. Undo/redo and saves keep their existing shared semantics.

Selection or scene changes, close/reopen, stale observations and pending mutation
reconciliation reject an old drag. Cancel only discards the draft; it never writes
the starting pose back over intervening work. Any commit attempt ends the local
gesture. Uncertain delivery still belongs to ScenePanel's original mutation
identity and explicit outcome reconciliation. A known successful commit remains
successful when a later display refresh fails.

17c must connect Escape, focus/modal/layout/tab changes, selection changes and
close decisions to this lifecycle. Gate candidate display on `active() && !stale()`:
the read-only candidate accessor may retain an owned value until update/cancel.
Render only a current validated candidate without
temporarily mutating the live scene. This increment does not claim those controls
or GPU previews exist.

## Terminal equivalence

No additional RPC, persisted field or source/protocol version is introduced.
Terminal clients use the existing [authoring protocol](authoring-protocol.md):

1. Inspect the entity with `fields: ["transform", "hierarchy"]` and inspect its
   parent transform at the same `expected_revision`, if parented.
2. Compute the desired local TRS using the math above (C++ callers can use the
   shared helper), then call `scene.preview` with a one-command batch if desired.
3. Read `scene.mutation_status` for the next owner-scoped mutation identity and
   submit `scene.mutate` with operation type `command` and command type
   `set_transform`, the target `entity_id`, and the computed `transform`.
   Keep the captured expected revision. Translation/scale are three-number arrays;
   rotation is `[x,y,z,w]`.
4. Check admission, original retained outcome and operation result. Reconcile
   uncertainty using the same identity, then query fresh state before a new intent.

The resulting component, descendant validation, history and persistence are the
same for terminal and human adapters. Pointer gesture state is editor-private and
does not become a player protocol. Existing FeatureWorld `AuthoringCommands`
coverage remains the semantic command owner; external math/adapter fixtures cover
drag state and protocol equivalence without adding a duplicate command or case.

The focused families are `shark.tools.transform-manipulation` and
`shark.editor.transform-drag`, each seed 1337 and timeout 60 seconds. The
[17b handoff](handoffs/17b-transform-manipulation.md) records executed results and
the remaining actual UI acceptance.
