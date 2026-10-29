# Development backlog

On 2026-10-18, the user deferred the remaining 05c work and authorized continued
development, including 06b and later increments. These items are **deferred and
nonblocking**, not fixed or passed. Full `REQ-05` remains **NOT_RUN**; historical
failed receipts, fixed resource budgets and both current-PC GPU targets remain
unchanged. This decision supersedes the earlier CPU-only 06a sequencing exception
and the progression restrictions in historical handoffs.

On 2026-10-28, the user also explicitly deferred the outstanding 14b Client and
export-worker sanitizer checks and authorized 14c. Those checks remain
**NOT_RUN**; compiler crashes are not passing evidence or reported engine
sanitizer defects. This amendment removes that progression gate only.

## Desktop UI deferral

On 2026-10-29, the user explicitly paused desktop/UI testing until further notice
and authorized continued coding with the remaining checks in this nonblocking
backlog. The [testing policy](testing-policy.md#desktop-interaction-pause-2026-10-29)
forbids desktop input/focus/window control and windowed automated GPU fixtures
while paused. Headless work may continue. Resume desktop testing only when the
user explicitly requests it; office hours, elapsed time and ordinary coding
requests do not end the pause. Deferred UI checks remain **NOT_RUN**. Completed
native GUI evidence on both GPUs remains valid for its tested inputs.

Later checkpoints append their required UI acceptance scope to BL-16A-UI-01
without claiming it passed. This authorizes implementation progression, not full
REQ-16 closure before its required connected UI/terminal evidence exists.

## Deferred items

Do not repeat these known items in routine progress updates, handoffs or final
responses. Keep this backlog as their current status record. Revisit an item when
its trigger occurs, when new evidence materially changes its impact, or when the
user asks. A new regression in current work still requires investigation; it must
not be dismissed solely because it resembles a deferred issue.

| ID | Deferred work and retained evidence | Revisit trigger / completion evidence |
| --- | --- | --- |
| BL-05C-01 | NVIDIA 610.62 retains a desktop DC per swapchain lifetime, independently reproduced outside Shark. Driver acquisition is identified; no supported engine correction is established. [Investigation](handoffs/05c-failure-investigation.md), [receipt](coverage/evidence/05c-investigation.json). | An independently authorized driver/OS change, a supported ownership correction, or material impact during normal development. Rerun the independent reproducer and the unchanged qualifying workload; retain old failures. |
| BL-05C-02 | Intel 101.6790 retains USER objects in raw Win32/Vulkan lifetimes. Tracing identifies failed driver-owned hook removal after target threads begin exit. [Investigation](handoffs/05c-intel-asan-investigation.md), [receipt](coverage/evidence/05c-intel-user-investigation.json). | An independently authorized driver/OS change, a supported driver-lifetime correction, or material impact during normal development. Repeat the independent reproduction and qualifying workload; require bounded resource behavior. |
| BL-05C-03 | An original Intel ASan fault child exceeded its exit deadline after native cleanup. The fixed 64-child diagnostic sequence did not reproduce or explain it. The separate queue-worker deadline defect is already fixed. [Investigation](handoffs/05c-intel-asan-investigation.md), [receipt](coverage/evidence/05c-asan-exit-investigation.json). | Another equivalent timeout, actionable process/thread capture, or a relevant shutdown change. Use the existing bounded capture tools to identify the stalled process and demonstrate a targeted correction; successful retries alone do not close the original finding. |
| BL-05C-04 | Remaining full Release/ASan stress qualification on both GPUs, including the sustained workload and repeated lifetimes. Earlier partial and failed runs remain intact. [Qualification contract](presentation-qualification.md), [original handoff](handoffs/05c-presentation-qualification.md), [latest handoff](handoffs/05c-intel-asan-investigation.md). | Resume when the underlying findings have an actionable resolution or when the user requests qualification. Execute the existing workloads and budgets, Windows/WSL regressions and applicable package checks. Only complete qualifying evidence can change full `REQ-05` to PASS. |
| BL-14B-01 | Selected Client package/launch/shader-bundle and actual export-worker process sanitizer checks remain NOT_RUN after Clang and MSVC compiler crashes. A fixed-input Clang replay also crashed in code generation; no underlying cause was established. The Core startup ASan pass remains valid. [Investigation and original results](handoffs/14b-standalone-scene-export.md#failures-and-deferred-verification); retained logs and compiler repro inputs are under `out/14b-work/`, including `sanitizer-followup/fixed-input-codegen.log`. | A relevant compiler update, an independently authorized environment repair, materially new diagnostic evidence, or the user's request. Reproduce the compiler issue with retained inputs as applicable, then finish the selected Client and actual worker-process sanitizer checks. Keep historical failures and fixed workloads; successful compilation alone does not qualify behavior. |
| BL-16A-UI-01 | Remaining actual Editor UI checks are NOT_RUN under the desktop pause: finish Intel restore/focus/clean-close after the interrupted interaction and run the RTX keyboard smoke against the final coalesced-chord correction. Retain the completed native GUI readbacks, earlier RTX project open/save/close and terminal busy/release evidence, plus the corrected adapter's CPU regression. See the [16a handoff](handoffs/16a-editor-shell.md); evidence remains under `out/16a-work/`. **16b adds the native composition and actual scene-workspace acceptance listed below**, also NOT_RUN. | Only the user's explicit instruction to resume desktop testing. Then run the focused outstanding checks on the final relevant inputs, retaining prior results and any interruption/failure evidence. Later scene-editing checkpoints and 16e still require actual UI-driven editing and bidirectional terminal handoff; neither CPU tests nor deferred status establish those passes or full REQ-16 closure. |

### BL-16A-UI-01: 16b acceptance additions

The [16b workspace](editor-scene-workspace.md) and
[handoff](handoffs/16b-editor-scene-workspace.md) add these deferred checks on the
RTX 4070 Laptop and Intel GPUs. All are **NOT_RUN** while desktop testing is paused:

- Run the existing GUI fixture's optional scene mode with required validation and
  verified GUI/scene bundles. Its eight phases are baseline, same-revision camera
  change, same-revision content change, viewport extent resize, framebuffer
  resize, viewport removal, restoration and cancelled close. Check acquired-image
  reuse, fixed shared-depth/overlay/clipping/sRGB pixels, expected generations and
  clean teardown without validation diagnostics. Keep the original 16a mode.
- In the actual Editor, open a project, page/select its parent-first hierarchy,
  explicitly select a Camera and show the current retained scene. Exercise the
  explicit target/metadata inventory controls with imported content and visible
  failure for missing/stale inventory. Resize the viewport/window and verify
  revision changes clear the old camera/selection rather than silently rebinding.
- Create, reparent and delete through the hierarchy UI, including rejected/stale
  decisions; verify displayed hierarchy and dirty state. Save/reopen the authored
  result and exercise save/discard/cancel dirty dialogs and close cancellation.
  Preserve focus/text/modal shortcut gates and complete the retained 16a lifecycle
  checks. Actual bidirectional terminal continuation remains 16e.

### BL-16A-UI-01: 16c acceptance additions

The [inspector and history](editor-inspector.md) add these actual Editor checks,
all **NOT_RUN** until the user resumes desktop testing:

- Select entities and inspect/edit Name, local Transform, Camera and MeshRenderer.
  Exercise optional-component addition/removal, signed scale, quaternion order,
  camera cross-field rejection and invalid asset IDs. Verify errors retain the
  draft and leave authoritative state unchanged; successful edits update the
  hierarchy, dirty indicator and explicitly refreshed viewport.
- Change selection or scene revision while a draft exists; verify it cannot be
  silently applied to another entity or newer revision. Exercise explicit reload
  and draft handling around hierarchy edits, undo/redo and project close.
- Use shared undo/redo across hierarchy and inspector edits, including stale
  history decisions. Verify saving keeps history, undo after save becomes dirty,
  and close/reopen resets owner-local history. Check focus/text/modal behavior,
  scrolling/layout and save/discard/cancel with the final controls on both GPUs.
  Connected terminal continuation remains 16e.

### BL-16A-UI-01: 16d acceptance additions

The [asset/job/console panels](editor-assets-console.md) add these actual Editor
checks, all **NOT_RUN** until desktop testing explicitly resumes:

- Load an explicit metadata inventory, page/select assets, inspect cache state
  and register/update a real sidecar through the metadata controls. Exercise
  stale-file rejection, required provenance/license/audience, unapplied draft
  close decisions, and save/undo remaining separate from metadata publication.
- Submit imports, observe progress/ready states without publication, explicitly
  publish, cancel, and acknowledge outcomes. Check failed/stale imports preserve
  last-valid output and display the original error/publication receipt. Close
  with active work and verify the owner is released after worker teardown.
- Switch Scene/Assets/Console tabs, verify content changes refresh the explicit
  viewport, and exercise console paging/detail/clear and retained diagnostics.
  Check focus, text/modal routing, layout and stale project-status recovery on
  both current GPUs. Connected terminal continuation remains 16e.

### BL-16A-UI-01: 16e connected acceptance

The [connected acceptance procedure](editor-acceptance.md) prepares the final
assignment-16 gate. Its CPU adapter/public-terminal process case is headless
evidence only. These actual interactions remain **NOT_RUN** until the user
explicitly resumes desktop testing:

- Continue the public terminal example in the actual Editor: inspect the saved
  hierarchy/components, apply scene and metadata edits, explicitly publish an
  import, save and close. Confirm an independent terminal writer is rejected
  while the Editor owns the project without disturbing unsaved state/history.
- Reopen through the public terminal, independently compare semantic state and
  import identity, edit/save/close, then continue in the actual Editor. Check
  reverse busy rejection, preserved history across save, fresh history on reopen,
  unapplied drafts and dirty close decisions through the actual controls.
- Complete the applicable 16a-16d native/focus/layout/viewport checks on both
  current-PC GPUs, reusing valid prior results. CPU snapshots and process handoff
  do not establish GUI state equivalence or replace required rendering evidence.

### BL-16A-UI-01: 17a viewport interaction additions

The [navigation and picking contract](viewport-navigation.md) adds these actual
Editor checks on both current-PC GPUs. They remain **NOT_RUN** while paused:

- Seed and reset from perspective and orthographic authored Cameras. Orbit, pan
  and dolly to reveal content outside the old frustum; verify the rendered image,
  depth/face policy and authored Camera/source/history remaining unchanged.
- Pick overlapping and empty/silhouette geometry, including rotated/nonuniform/
  mirrored parents and an affine camera. Verify hierarchy selection and retained
  inspector drafts, exact viewport offsets, framebuffer scaling, the 2048-pixel
  clamp, resize and scroll/layout behavior. Repeat after scene edits and reopen
  so stale views cannot select recreated entities.
- Exercise drag starts, Escape, leaving/reentering the image, focus loss/regain,
  minimize/restore, text input, close/delete modals and Scene/Assets/Console tabs.
  A held button must not resume a cancelled gesture. Confirm viewport wheel input
  does not unexpectedly scroll other panes or affect authoring controls.

Retain the earlier required-validation scene-composition gates and valid prior
results. CPU geometry and synthetic routing do not qualify these native outcomes
or close full REQ-17.

### BL-16A-UI-01: 17b transform acceptance additions

The [transform/drag contract](transform-manipulation.md) supplies headless services;
17c now supplies gizmo controls and rendered candidate integration. Once
desktop testing resumes, these checks remain required:

- On both GPUs, exercise local/world translation, rotation and scale with snapping,
  rotated/nonuniform/mirrored parents and deliberate shear/singular rejection.
  Verify a drag preview leaves dirty/history/source unchanged until one release
  commits one history group, and one undo reverses the entire gesture.
- Cancel with Escape, selection/revision changes, focus loss, modal/layout/tab
  changes and project close. A failed latest candidate must not commit an older
  pose; cancellation must not overwrite intervening edits or clear an unresolved
  retained mutation. Check fresh-press behavior after cancellation.
- Compare actual gizmo results with terminal `set_transform`, including stale
  requests, save/reopen and undo/redo. Headless protocol parity is preparation,
  not a passing UI or rendered-preview result. Status: **NOT_RUN**.

### BL-16A-UI-01: 17c gizmo acceptance additions

The [native gizmo contract](viewport-gizmos.md) adds these checks on both GPUs,
all **NOT_RUN** under the desktop pause:

- Verify visible X/Y/Z shaft/ring overlays, active highlights and clipping in the
  actual scaled image, including nonuniform DPI, viewport offsets and resizing.
  Exercise Local/World and meter/degree/factor snapping, unavailable edge-on axes,
  affine/mirrored parents and rotation through the angle wrap boundary.
- Drag parent transforms so previously culled descendants enter the view; check
  geometry, normals/winding and camera behavior. Cancel and reject candidates and
  verify the committed image returns immediately with no dirty/history change.
  Check source/asset changes before release reject invalid render preparation.
- Verify a handle press/release never also picks or navigates, including entering
  and pressing in one frame, leaving/reentering held, focus/text/modal/tab changes,
  close cancellation and tool/snap changes. One release commits one undo group,
  retains navigation and refreshes the new revision before further interaction.
  Complete 17d's connected terminal/save/reopen/undo comparison with actual input.

This deferral does not authorize driver installation, host resource changes,
foreign-handle cleanup or relaxed acceptance limits. The active implementation
sequence for 05c's immediate successors is recorded in the
[06 checkpoint plan](roadmap/shader-resource-plan.md); current export work follows
the [export checkpoint plan](roadmap/export-plan.md).

### BL-16A-UI-01: 17d connected acceptance

17d completes headless connected preparation through the existing editor adapter
and public terminal processes. The [viewport acceptance procedure](viewport-acceptance.md)
still requires actual human controls and rendering on both current-PC GPUs after
explicit desktop resumption. Complete the parented local/world/snapping transform
comparison, stale requests, single-drag undo, save/reopen and reverse continuation,
along with the earlier native navigation/picking/gizmo/focus/layout checks and
required-validation scene composition. Status: **NOT_RUN**; full REQ-17 stays open.
The existing amendment authorizes later coding without relabeling these results.
