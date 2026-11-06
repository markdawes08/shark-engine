# Development backlog

On 2026-10-18, the user deferred the remaining 05c work and authorized continued
development, including 06b and later increments. These items are **deferred and
nonblocking**, not fixed or passed. Full `REQ-05` remains **NOT_RUN**; historical
failed receipts, fixed resource budgets and both current-PC GPU targets remain
unchanged. This decision supersedes the earlier CPU-only 06a sequencing exception
and the progression restrictions in historical handoffs.

On 2026-10-28, the user also explicitly deferred the outstanding 14b Client and
export-worker sanitizer checks and authorized 14c. Those checks were **NOT_RUN**
at deferral; the selected sanitizer gate passed on 2026-10-31 as recorded below.
Historical compiler crashes remain failure evidence, not reported engine
sanitizer defects. The original amendment removed only that progression gate.

## Desktop UI deferral

On 2026-11-01, the user again instructed agents to hold off on manual UI testing,
record the outstanding work here, and proceed with the next subchunk. This
explicitly defers 20d's native RTX and Intel overlay checks and authorizes 20e
implementation without those checks. The [testing policy](testing-policy.md#manual-ui-testing-pause-2026-11-01)
sets the current manual-interaction pause and permits appropriate automated and
headless work. Deferred manual evidence remains **NOT_RUN** until it passes;
earlier completed results and failures remain unchanged. This is a progression
amendment, not a GPU qualification or waiver of REQ-20 acceptance.

On 2026-10-29, the user paused desktop/UI testing and authorized continued coding
with the remaining checks in this nonblocking backlog. On 2026-10-31, the user
explicitly requested finishing the current subchunk and then tackling the testing
backlog. **Desktop testing resumed after 18e implementation.** The
[testing policy](testing-policy.md#desktop-interaction-pause-2026-10-29) permitted
actual Editor input/focus/window interactions and automated windowed GPU fixtures
for that work; the newer manual UI pause above now applies. Deferred checks remain
**NOT_RUN** until their required evidence passes. Completed native GUI evidence on both GPUs remains valid for its tested
inputs; historical failures and interrupted runs remain intact.

Later checkpoints append their required UI acceptance scope to BL-16A-UI-01
without claiming it passed. This authorizes implementation progression, not full
REQ-16 closure before its required connected UI/terminal evidence exists.

The additions below retain the original deferred scope. Their pause/NOT_RUN
wording describes the original deferral; the current table, progress and linked
handoff record subsequently qualified families. Reuse those completed results
under the testing policy. Unperformed required interactions remain NOT_RUN.

## Deferred items

Do not repeat these known items in routine progress updates, handoffs or final
responses. Keep this backlog as their current status record. Revisit an item when
its trigger occurs, when new evidence materially changes its impact, or when the
user asks. A new regression in current work still requires investigation; it must
not be dismissed solely because it resembles a deferred issue.

| ID | Deferred work and retained evidence | Revisit trigger / completion evidence |
| --- | --- | --- |
| BL-M05D-UI-01 | **DEFERRED / NOT_RUN, 2026-11-06:** actual native World cells tab interaction under the current manual UI pause. Shared adapter, protocol and public-pipe coverage follow the [authoring contract](world-cell-authoring.md) and [M05d handoff](handoffs/M05d-colocated-regions.md); CPU evidence does not qualify native interaction. | After the user resumes manual UI testing, open a camera-free static cell scene, select explicit collision/navigation meshes, export to a fresh `.sharkcell` path and inspect the artifact. Verify visible stale-revision/digest/path failures, original publication/durability reporting, source/history preservation and terminal inspection after closing the Editor. |
| BL-21D-UI-01 | **DEFERRED / NOT_RUN, 2026-11-05:** actual native Prefabs tab acceptance under the current manual UI pause. Implementation and portable adapter/protocol checks are recorded in the [21d handoff](handoffs/21d-prefab-authoring.md); they do not qualify actual desktop interaction. | After the user resumes manual UI testing, use the native Editor to capture a selected subtree, create matching metadata in Assets, publish/reopen source, instantiate with one-group undo/redo, resolve stale source and Scene conflicts, confirm Play draft gating and close/discard decisions, then save/close and continue from the terminal. Preserve source independence and original publication/mutation receipts. |
| BL-05C-01 | The original NVIDIA 610.62 investigation identified driver-owned desktop DC retention. The 2026-10-31 raw eight-lifetime repeat on 616.56 still grows closed GDI 3..10 and kernel handles 323..342. On 2026-11-01, strict Release RTX completed the 600.001s sustained phase/100 resizes/20 restores, then failed closed lifetime 6 with GDI 15 -> 16; Debug failed USER zero-growth, without identifying the same DC mechanism. No supported correction is established. [Current qualification](handoffs/18e-editor-physics-preview.md#05c-backlog-qualification-and-allocator-diagnosis-2026-11-01), [raw diagnostic](handoffs/18e-editor-physics-preview.md#05c-diagnostic-revisit-2026-10-31), [investigation](handoffs/05c-failure-investigation.md), [receipt](coverage/evidence/05c-investigation.json). | An independently authorized driver/OS change, a supported ownership correction, or material impact during normal development. Rerun the independent reproducer and the unchanged qualifying workload; retain old failures. |
| BL-05C-02 | Intel 101.6790 retains USER objects in raw Win32/Vulkan lifetimes. Original tracing identifies failed driver-owned hook removal after target threads begin exit. The 2026-10-31 raw forty-lifetime repeat has closed USER range 10..14, first 11/final 13, while GDI 3 and handles 316 stay flat; this is diagnostic only. The 2026-11-01 strict Debug/Release selections also failed USER zero-growth without establishing a supported engine correction. [Current qualification](handoffs/18e-editor-physics-preview.md#05c-backlog-qualification-and-allocator-diagnosis-2026-11-01), [raw result](handoffs/18e-editor-physics-preview.md#05c-diagnostic-revisit-2026-10-31), [investigation](handoffs/05c-intel-asan-investigation.md), [receipt](coverage/evidence/05c-intel-user-investigation.json). | An independently authorized driver/OS change, a supported driver-lifetime correction, or material impact during normal development. Repeat the independent reproduction and qualifying workload; require bounded resource behavior. |
| BL-05C-03 | **OPEN / UNRESOLVED, 2026-11-01:** the Intel `surface-error` child-55 timeout retained zero engine-owned resources, a worker dump/wait chain and native exit status 1 published before process signaling. The responsible rundown operation remains unidentified. The exit-code-only capture blind spot is fixed and CPU-qualified. One corrected 64-child invocation and one newly prearmed 128MiB ETW 64-child invocation completed expected exits without recurrence; the ETW circular tail has zero reported lost events/buffers but no causal timeout stack. These are not a fix. [Latest evidence](handoffs/18e-editor-physics-preview.md#05c-backlog-qualification-and-allocator-diagnosis-2026-11-01), [captured timeout](handoffs/18e-editor-physics-preview.md#asan-exit-observation), [original investigation](handoffs/05c-intel-asan-investigation.md), [original receipt](coverage/evidence/05c-asan-exit-investigation.json). | Capture native process signaling/rundown on a recurrence with the corrected bounded tools and identify an actionable cause. Preserve both failures; successful repetitions alone do not close this finding. |
| BL-05C-04 | **OPEN / six strict selections FAIL, 2026-11-01:** unchanged Debug/Release/ASan workloads on both GPUs failed their resource limits. Release RTX completed sustained work before a closed-lifetime GDI failure; other selections stopped at their first failure. Both ASan profiles newly exceeded the fixed 256MiB private-byte allowance. Separate once-only allocator diagnostics demonstrate substantial quarantine/mapped growth, but inconsistent runtime live-byte totals prevent a leak-freedom claim and no engine ownership fix is established. Earlier partial/failed receipts remain intact. [Current results and accounting limits](handoffs/18e-editor-physics-preview.md#05c-backlog-qualification-and-allocator-diagnosis-2026-11-01), [qualification contract](presentation-qualification.md), [original handoff](handoffs/05c-presentation-qualification.md), [prior investigation](handoffs/05c-intel-asan-investigation.md). | Further memory diagnosis needs allocation-caller evidence, not another identical run or weaker quarantine/budgets. Resume qualification after an actionable resolution or explicit user request, preserving existing workloads and applicable regressions/packages. Only complete qualifying evidence can change full `REQ-05` to PASS. |
| BL-14B-01 | **CLOSED / PASS, 2026-10-31:** the four deferred Client package/launch/shader-bundle and actual export-worker process sanitizer checks passed under the retained WSL Clang 18.1.3 ASan+UBSan profile. The once-only current build compiled the previously blocked exporter successfully, with no compiler/settings repair. [Closure addendum](handoffs/14b-standalone-scene-export.md#backlog-closure-2026-10-31), [18e handoff](handoffs/18e-editor-physics-preview.md). Logs/inventory/JUnit: `out/18e-work/backlog-14b/`. | No remaining selected sanitizer gate. Historical Clang/MSVC crashes and fixed-input repro evidence under `out/14b-work/` remain intact; their underlying cause is unestablished. This pass does not claim Windows ASan compiler repair or GPU qualification. |
| BL-16A-UI-01 | **OPEN / PARTIAL, 2026-11-01:** both GPUs passed composition, imported-project handoffs, full reference gestures/terminal returns and compact affine/reflected/sheared picking, culling, shear rejection and angle wrap. Selected common hierarchy/inspector and asset/job/console controls pass under complementary Windows Debug coverage. **Full REQ-18 PASS**, including both-GPU native physics/body-setting handoffs and required retained fixtures. Preserve original results and failures; [current handoff](handoffs/18e-editor-physics-preview.md). | Exact remaining native 16/17 gates are the unsupported post-request status-read failure trigger, held preview/cancel/freshness/intermediate-scale and physical right/middle navigation. The desktop drag API cannot hold across actions or select right/middle drag. Normal status refresh and viewport errors do not qualify status-failure recovery. Completed families, including 18, are reused rather than rerun merely because original deferral text below says NOT_RUN. |
| BL-20D-UI-01 | **DEFERRED / NOT_RUN, 2026-11-01:** actual interactive 20d physics inspection/overlay verification on RTX 4070 Laptop and Intel. The RTX Editor reached `editor_ready` with required validation under the standard token, but the helper returned `failed to activate captured window`; the temporary process was terminated. Intel interaction was not attempted. Preserve the [20d handoff](handoffs/20d-physics-inspection.md) and its original startup, token and cancelled-tool receipts; CPU ImGui, geometry and protocol results are not GPU evidence. | After the user explicitly resumes manual UI testing, resolve the foreground/helper issue or use a user-assisted session. On both GPUs, serially use the actual Editor under required validation to Play/Pause, inspect runtime bodies, run ray and overlap queries, verify collider/query/hit overlay and toggle, and independently confirm source/history isolation and normal cleanup. Record results; do not mark this PASS based on `editor_ready` or CPU fixtures. |
| BL-17-UI-DRAG-01 | **CLOSED / original press-position defect corrected and verified on both GPUs, 2026-11-01:** motion before capture replaced the cursor used for press hit testing. Owned pointer-edge positions now preserve start/release through input, frame merging, gizmos/picking and ImGui. The original World Y drag commits once and supports one-group Undo/Redo/save on RTX and Intel. Saved full reference TRS and literal world matrices also match after rotation and all three scale axes. [Correction and tests](handoffs/18e-editor-physics-preview.md#pointer-capture-correction-and-resumed-acceptance-2026-11-01). Original failed attempts remain intact. | No remaining reproduction of this defect. Other connected gesture/cancellation, navigation and focus/layout gates remain under BL-16A-UI-01; full REQ-17 remains NOT_RUN. |

### BL-16A-UI-01: progress, 2026-10-31

The 18e coding/headless selection passed: Windows Debug 20/20 affected CTests,
WSL Debug 4/4 and WSL ASan+UBSan 4/4. These results cover shared inspector/preview,
retained protocol and process behavior; they do not qualify desktop input.

The corrected 16b native GUI scene-composition fixture is **PASS on both GPUs**:
`out/18e-work/gui-rtx-qualified/summary.json` and `gui-intel-qualified/summary.json`.
Each completed eight phases, 56 presentations and clean teardown under required
validation. Initial failures identified 150% DPI scaling and the Windows caption
minimum; the fixture explicitly normalizes a larger physical capture host while
preserving viewport workload, literal pixels and tolerances. Original failures
remain intact. The [18e handoff](handoffs/18e-editor-physics-preview.md#testing-backlog-progress-2026-10-31)
records exact devices, limits, logs and the correction.

Actual RTX interaction exercised one-group Name/translation Apply, unapplied
draft Save/Keep editing/discard, shared Undo/Redo across Save, metadata publication,
explicit Ready/Publish/Take import control and console detail/clear without scene
mutation. Both directions of actual Editor/public-terminal writer exclusion and
saved semantic continuation passed, including terminal `Terminal return` displayed
in the reopened Editor with empty history. The Editor exited cleanly with zero
validation diagnostics. This is **partial 16e evidence**, not full acceptance:
remaining component rejection, metadata draft/close, hierarchy/layout/focus
decisions, Intel interaction and the full viewport/physics procedures remain open.

Actual RTX 18e interaction also exercised all eleven capsule body fields, draft
nonpublication versus Apply, one-group Undo/Redo, falling/landing, Pause and one Step,
retained enter/exit observations, runtime body removal with visual geometry
retained, Stop restoring authored state/history, independent saved-source checking,
fresh Play, minimize/restore and clean close while live under required validation.
The [18e handoff](handoffs/18e-editor-physics-preview.md#testing-backlog-progress-2026-10-31)
records exact values/ticks and evidence under `out/18e-work/physics-rtx-worker/`.
The actual native-saved project then passed public-terminal continuation: exact
body/pose inspection, 120 ticks and owned trigger/floor/removal observations with
source bytes/status/history/ledger unchanged, followed by a saved terminal rename.
The actual Intel Editor reopened `Terminal physics return` with Saved state and
empty history. Its inspector rejected nonuniform scale and parenting visibly,
retaining the invalid draft without source/history changes until explicit Reload.
Intel play showed falling/landing and blocked picking; Pause/Step held 1203 then
1204, and tab/minimize/restore retained the paused state. Close while live/paused
exited cleanly with 13,350 completed presentations and zero validation diagnostics.
Evidence is `out/18e-work/ui-physics-terminal-return/summary.json` and
`physics-intel-worker/` plus its result; exact scope is in the handoff above.
This is **partial actual physics acceptance on both GPUs**; invalid dimensions,
negative scale, remaining focus/settings and other required cases stay open.
No REQ-16/17/18 closure is claimed.

### BL-17-UI-DRAG-01: native drag investigation

The exact 17d reference loaded through actual RTX controls, including both sidecars
and its camera. Actual picking selected the parented triangle; wheel zoom and
Reset changed display only with Saved state/history zero. World Translate/Snap 1
was selected, but the first atomic Computer Use drag from `(1116,774)` to
`(1116,702)` cleared selection instead of starting a gesture. Reselect worked.
No event trace exists to attribute the failure; input coalescing is a hypothesis,
not a demonstrated cause. The session closed cleanly with 28,444 completed
presentations and zero validation diagnostics.

The documented input API lacks held-button/duration/right-drag controls; the other
continuous cases were not performed. Evidence and exact next work are in the
[18e handoff](handoffs/18e-editor-physics-preview.md#native-viewport-gesture-investigation)
and `out/18e-work/viewport-rtx-worker/`. Diagnose this new native gesture result
with a bounded event trace before claiming transform acceptance, then finish the
remaining connected UI checks on both GPUs. 18e implementation/headless work is
complete; full REQ-16/17/18 remain NOT_RUN.

### Current testing progress, 2026-11-01

The dated investigation above describes its original evidence. The new bounded
trace established the press-position defect; the
[18e addendum](handoffs/18e-editor-physics-preview.md#pointer-capture-correction-and-resumed-acceptance-2026-11-01)
records the additive bounded input API, correction, final Debug/WSL/ASan selections
and actual RTX confirmation. World Y/Snap 1 reached local T `[3,0,0]` within the
existing tolerance, one Undo restored `[1,0,0]`, and Redo/save retained history.
World Z/Snap 15 rotation and Local X/Snap 1 translation each committed once.
The subsequent user request resumed work after three manual-input guard
rejections; no scale edge or mutation occurred during those rejected attempts.
The retained RTX edits were saved before the old supervisor deadline, and all
completed sessions closed cleanly. Intel repeated the original drag/Undo/Redo.
Both GPUs then rejected a zero-scale release without mutation and committed
separate X=-2 and Y=1.5 factors. A temporary explicit Camera angle exposed the
otherwise edge-on Z handle; its factor 0.5 committed as one group on each GPU.
Public owners restored the exact original Camera and verified local
T `[3,2,0]`, Rz90, S `[-2,1.5,0.5]`, world pivot `[-4,6,0]` and linear
`diag(4,-3,1)` within the existing tolerance. The camera setup is not evidence
of orbit/pan or continuous held-scale behavior.

A distinct mixed-button cancellation defect was reproduced and corrected in the
raw gizmo; final selected Debug and Windows ASan GUI checks pass. The
[correction addendum](handoffs/18e-editor-physics-preview.md#mixed-button-cancellation-correction-2026-11-01)
records the original failed control, final-input evidence and native rebuild.
The selected common editor controls and compact viewport cases now also have
native evidence in the linked handoff. The table above records exact remaining
gates; prepared inputs alone never qualify them.

Intel's imported-project workflow now covers actual Name/translation Apply,
unapplied inspector and metadata close decisions, save/history, invalid license,
exact metadata-token conflict, Ready cancellation/drain/take, explicit replacement
publication and reciprocal terminal ownership exclusion. Independent terminal
continuation checked saved scene/metadata/source bytes and fresh mesh/dependency
cache descriptors, then the actual Editor reopened `Terminal return` Saved with
empty history and fresh geometry. The native session closed cleanly with zero
validation diagnostics. Exact scope and evidence are in the
[Intel addendum](handoffs/18e-editor-physics-preview.md#additional-intel-editor-and-physics-rendering-evidence-2026-11-01).
The final public-process control also checked complete literal camera/actor/world
state and dirty-close rejection, then retained Undo/save restored all seven source
files byte-for-byte. It closed its writer; no additional UI run is claimed.

Both GPUs also passed the separate eight-image physics renderer fixture for static
reset, teleport, kinematic movement and interpolated child geometry/Camera, with
literal pose/color/depth oracles and clean ownership/validation. This closes that
rendering gap. Subsequent both-GPU native physics and complete source/body-setting
handoffs now close full REQ-18, as recorded in the
[closure addendum](handoffs/18e-editor-physics-preview.md#intel-physics-and-full-req-18-closure-2026-11-01).
The [compact viewport addendum](handoffs/18e-editor-physics-preview.md#compact-native-viewport-acceptance-2026-11-01)
also records both-GPU picking/affine Camera, committed culling, shear rejection
and angle wrap, plus RTX Delete/Undo/fresh picking. Full REQ-16/17 remain NOT_RUN
for held preview/cancel/freshness/intermediate scale, right-orbit/middle-pan and
the native post-request status-read failure without a supported external trigger.
No additional binding tool-feasible family was identified in the final scope
review. Existing fixed DPI/geometry fixtures remain applicable; no unavailable
hardware/OS-DPI condition, multi-turn rotation or physical mixed-button test is
added as a new gate. All test scenes were restored and all windows/writers closed.
Assignment 19 has not started.

### BL-16A-UI-01: 16b acceptance additions

The [16b workspace](editor-scene-workspace.md) and
[handoff](handoffs/16b-editor-scene-workspace.md) add these deferred checks on the
RTX 4070 Laptop and Intel GPUs. The first native fixture is now **PASS on both**;
remaining actual interactions are **NOT_RUN** except the partial progress above:

- **PASS:** run the existing GUI fixture's optional scene mode with required validation and
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

### BL-16A-UI-01: 18b constrained body transforms

18b adds shared root/positive-uniform-scale validation for authored RigidBody
entities. After desktop resumption, exercise existing inspector/gizmo transform
edits on a terminal-authored body: reject parenting and signed/nonuniform scale
without changing committed state/history, retain valid uniform edits and visual
children, and compare saved/reopened settings with terminal queries. Status:
**NOT_RUN**. Native body-setting controls and connected simulation acceptance
retain 18e in the [physics plan](roadmap/physics-plan.md).

### BL-16A-UI-01: 18c simulation presentation

18c supplies headless fixed-step simulation and CPU interpolation extraction.
After 18e's native play adapters and explicit desktop resumption, verify both-GPU
presentation of dynamic motion, static reposition, kinematic targets, pause and
single step; verify interpolated descendants/cameras and picking rejection.
Confirm stopping play restores authoring and leaves saved source/history intact.
Status: **NOT_RUN**; current CPU results do not qualify native play or rendering.

### BL-16A-UI-01: 18d collision settings and play observations

After 18e exposes the shared body/filter controls and desktop testing explicitly
resumes, exercise layer/mask/trigger authoring, Apply/undo/save/reopen and matching
terminal continuation. In native play, verify sensor nonresponse, filter changes
through explicit rebuild, owned enter/exit delivery and stop/removal with pending
events. Status: **NOT_RUN**; CPU physics checks do not qualify actual editor input
or rendered play. Full REQ-18 remains open.
