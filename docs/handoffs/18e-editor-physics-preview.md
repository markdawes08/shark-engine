# 18e: shared editor physics preview

Date: 2026-10-31. Base: `e045e00` (`feat(physics): add collision filters
triggers and owned contact events`), `main`. Four inspector files already
contained the interrupted 18e edits; this increment continues that work.
The user owns staging, commits and pushing.

## Delivered behavior

[ADR 0049](../architecture/0049-editor-physics-preview.md) and the
[preview contract](../physics-preview.md) add one `scene.physics` method with
status/start/step/remove/query/stop and truthful discovery. One retained project
owner captures a complete prepared runtime Scene and frozen client assets.
Native bodies use the existing leased Core simulation; source/history/dirty
state/files stay separate. Runtime removal preserves visual geometry, stop
discards play, and a later start recreates authored settings/poses.

Start is owner-scope/generation/revision-bound. Engine-generated preview UUIDs
and consecutive operation sequences fence native work. One original operation
receipt retains completed ticks/events/error for exact replay after uncertain
output. Changed, expired, gapped or stale input rejects. Scoped stop stays
available despite source-file/status problems; reverse project teardown releases
native physics before Scene/assets/project ownership. No native handle crosses
the terminal boundary. Camera/client content gates reuse existing export
preparation rather than a new importer or asset resolver.

The inspector now exposes all eleven RigidBody fields through one retained
atomic batch, including temporary removal of old constraints before a valid
combined body/pose edit. The portable physics panel drives the same RPC through
OfflineScheduler. Native Play/Pause/Step/Stop, runtime body removal, bounded
contact details and frozen runtime viewport share that owner. Source writes,
picking and gizmo commits are blocked during play. Interpolation remains
presentation-only; snapshots carry fresh runtime identity and physics markers.

Source/component/prepared versions and FeatureWorld's eighteen cases/fixture
IDs remain unchanged. Coverage adds `physics.authoring-preview` and
`terminal.scene.physics`, bringing the terminal inventory to 43 methods when
configured export/capture are included. Existing body component/command mappings
now include the actual inspector/preview fixtures. Lua and Server region-loop
integration remain their later owners.

## Selected verification

Selection follows the [testing policy](../testing-policy.md). Evidence paths
below are relative to `out/18e-work/`.

| Selection | Result / log |
| --- | --- |
| Windows x64 MSVC Debug affected subsystem | PASS, 20/20 CTests in 21.80s; `windows-tests.log/.xml` |
| Native Editor, GUI composition fixture and affected adapter/Tools targets | PASS build, no launch during this build; `build.log`, `regression-build.log`, `final-build.log` |
| WSL GCC 13 Debug portable selection | PASS, 4/4 in 1.13s; `wsl/linux-debug-tests.log/.xml` |
| WSL Clang 18 ASan+UBSan portable selection | PASS, 4/4 in 2.65s; `wsl/linux-asan-ubsan-tests.log/.xml` |
| Updated private Tools source/dependency boundary | PASS, selected `shark.tooling.source-document-dependencies` CTest in 82.49s; `source-dependencies.log/.xml` |
| Deferred 14b Client/export-worker sanitizer selection | PASS, 4/4 in 4.05s; `backlog-14b/{compiler,build,inventory,tests}.log`, `backlog-14b/tests.xml`; closes BL-14B-01 |
| Required-validation GUI scene composition, RTX 4070 Laptop | PASS, eight phases in 2.354s; `gui-rtx-qualified/summary.json`, `gui-rtx-qualified-token.json` |
| Required-validation GUI scene composition, Intel RaptorLake-S Mobile | PASS, eight phases in 2.472s; `gui-intel-qualified/summary.json`, `gui-intel-qualified-token.json` |
| Final repository links/coverage/ignore checks | PASS; `contract-final.log`, `repository-final.log` |
| Pinned changed-C++ formatting | PASS; `format-final.log` |

Windows covers the full existing Core physics family, portable editor
gizmo/drag/navigation/asset/inspector/physics/Scene/project owners, synthetic
ImGui composition, saved-project adapter/public-terminal handoff, retained
viewport/protocol and actual terminal discovery/edit/import/run/workflow pipes,
plus FeatureWorld and its real process. Inventory was listed before selection.
WSL selects inspector/physics/viewport and the actual editing process; leak and
undefined-behavior checks remain enabled in the sanitizer profile.

The first WSL build caught an unused private Result<void> helper under GCC
`-Werror`. Removing it corrected the cause; the original failure is retained as
`wsl/linux-debug-build-initial-failure.log`. Initial five-family Windows checks
also passed; the broader final selection supersedes those for changed inputs.

Canonical builds/caches were reused with two build jobs. No dependency, compiler
flags, host/WSL resources or kernel settings changed. Core/Runtime/Server receive
no new dependency or public API; their unchanged protected closures are reused
from 18d. No whole-project/hosted CI run was selected. CPU results do not qualify
actual GUI input, GPUs, Lua/gameplay or production MMO capacity.

## Testing backlog progress, 2026-10-31

The retained WSL Clang 18.1.3 ASan+UBSan build compiled the formerly blocked
exporter on its first current-input attempt. The selected prepared-package,
scene-launch, shader-bundle and real export-worker process checks all passed.
Runtime CPU package inspection initialized no renderer. BL-14B-01 is closed;
its [dated 14b addendum](14b-standalone-scene-export.md#backlog-closure-2026-10-31)
retains the scope and historical compiler failures. No toolchain repair or
underlying cause is claimed.

Both GPUs passed the 16b GUI scene-composition fixture through the standard-user
supervisor with fresh current-lock validation tools and GUI/scene shader bundles.
RTX UUID is `863d6f59a51894ab8b4829aa9460c78d`, driver 616.56; Intel UUID is
`868088a7040000000002000000000000`, driver 101.6790. Each run completed all eight
phases and 56 presentations, drained all submissions and retired both generations,
with zero validation warnings/errors/dropped messages. Each checked 1,344 fixed
interior color pixels and all 432,000 depth pixels; maximum color error was one
byte against the unchanged two-byte tolerance. Lifecycle phases, viewport sizes,
literal geometry, expected colors, clipping and depth oracles are unchanged.

Initial RTX attempts failed before presentation because the fixture assumed its
initial 160x120 request was already a framebuffer extent. The retained diagnostic
reported 240x180 at 150% DPI; explicit resize then exposed Windows' 176-pixel
caption minimum. The fixture now explicitly resizes after creation and uses
240x180/300x216 capture hosts above that minimum, keeping the geometry and viewport
workload unchanged. It adds bounded final window/presentation diagnostics on
timeout. Production window behavior is unchanged. Preserve `gui-rtx/`,
`gui-rtx-diagnostic/`, `gui-rtx-dpi-corrected/`, `gui-rtx-fixed-chrome/` and
`gui-rtx-final/` failures plus intermediate diagnostic runs; only the final
`gui-rtx-qualified/` and `gui-intel-qualified/` results qualify the corrected fixture.

The actual RTX Editor continued the terminal-example project with one retained
Apply group for Name `Editor handoff` and translation `[-0.5,-1,0]`. A later
unapplied Name draft stayed local: Ctrl+S while text focused was suppressed;
after focus left the field, saving published only applied values and reported
the remaining draft. Ctrl+W / Keep editing retained it, and explicit Reload
selection discarded it. Undo after save became dirty with zero undo/one redo
entry; Redo restored the edit with one undo/zero redo.

Actual Assets controls loaded the target/sidecars, read metadata and published
origin `16e Editor handoff`. Changing only the copied glTF generator to
`16e UI import` preserved binary bytes. Explicit inventory/cache refresh exposed
staleness; Import reached Ready 2/2 before publication, Publish then reported one
import/one reuse and a fresh key, and Take retained the original file-flushed
publication receipt. Console Details showed the drain receipt; clearing its 14
entries changed no dirty state. A competing terminal writer was rejected while
the Editor held ownership. Dirty close / Cancel retained changes, then Save and
close released them. A fresh terminal independently compared saved source and
metadata with clean state/empty history, held the writer while actual Editor
Open visibly rejected conflict, then renamed to `Terminal return`, saved and
closed. Actual Editor reopen displayed that name, saved state and empty history.
Native teardown exited zero with all 65,201 submitted presentations completed
and zero validation diagnostics. Evidence is `editor-rtx-worker/native.{stdout,stderr}.log`,
`editor-rtx-result.json`, `ui-rtx-busy.log`, `ui-rtx-saved-state.log`,
`ui-rtx-source-change.log` and `ui-rtx-terminal-return.{json,log}`. This is partial
connected acceptance: it does not establish every component rejection, metadata
draft/close, hierarchy/layout/focus decision, Intel interaction or full REQ-16.

Actual RTX physics interaction exercised all eleven inspector body settings with
a capsule: half extents `[0.6,0.7,0.8]`, radius `0.2`, half height `0.3`, mass `2`,
friction `0.3`, restitution `0.1`, layer `2`, mask `1` and trigger `false`, with
dynamic motion. Source remained Saved while the draft was unapplied; Apply created one
undo group. Actual Undo showed zero undo/one redo entry, and Redo restored one
undo/zero redo. Play showed the actor falling and landing. Pause held tick 4010;
one Step advanced to 4011. The contact panel retained floor enter `#3` from tick
48. Removing the selected actor's runtime body added exit `#4` at tick 4011 with
`body_removed`, while visual geometry remained. Stop restored authored view and
preserved history.

Ctrl+S saved the authored actor translation `[0,3,0]` and all eleven body fields,
independently checked in source JSON rather than inferred from the rendered pose.
A fresh Play recreated the body and advanced again; minimize/restore was observed
qualitatively without an exact catch-up claim. Closing while live exited cleanly,
with all 56,261 submitted presentations completed and zero validation diagnostics.
Evidence is `physics-rtx-worker/native.{stdout,stderr}.log`, its command/environment
files, `physics-rtx-result.json`, `ui-physics-baseline.json` and the retained
`ui-physics-project/` source.

The real public `SharkAuthor` process then reopened this native-saved project and
independently matched all eleven body fields and authored translation `[0,3,0]`.
It executed exactly 120 runtime ticks, observed trigger enter/exit and floor enter,
then removed the runtime body and observed its owned removal exit. Source bytes,
status, history, mutation ledger and authored inspection remained unchanged
through simulation/removal. After Stop, it renamed to `Terminal physics return`,
saved and released ownership. The retained result is
`ui-physics-terminal-return/summary.json` (**PASS**); the subsequent actual Intel
Editor reopen displayed that name, Saved state, empty undo/redo and `[0,3,0]`.

In the actual Intel inspector, changing only scale X to `2` and applying visibly
rejected with `invalid_argument` for the positive uniform-scale rule. The draft
remained while source stayed Saved with zero history; explicit Reload discarded
it. Choosing the camera as parent and applying also visibly rejected the body
parenting constraint without changing source/history. Console displayed both
errors. The viewport showed the cube at authored Y=3; Play showed falling/landing.
Clicking the moving runtime geometry retained the camera selection, confirming
that play picking stayed blocked. Pause held tick 1203 and one Step advanced to
1204. Switching Assets/Console tabs and minimizing/restoring retained paused tick
1204, Saved state and empty history. Alt+F4 while paused/live closed cleanly:
13,350 submitted/presented/completed, no validation warnings/errors/dropped messages,
exit zero. Evidence is `physics-intel-worker/native.{stdout,stderr}.log`, its
command/environment files and `physics-intel-result.json`.

These are partial actual physics results on both GPUs and a connected terminal
continuation, not full REQ-18 closure. Invalid dimensions, negative scale and other
remaining focus/input/settings cases still require their documented acceptance.

## Native viewport gesture investigation

The actual RTX Editor opened the exact saved 17d reference at
`ui-viewport-project/project/`, prepared and independently reopened through the
public terminal (`ui-viewport-project/reference-summary.json`). Actual controls
loaded both `terminal-example` sidecars, selected the camera and displayed the
triangle. Picking the visible triangle under its Rz90/uniform-scale-2 parent
selected the intended entity. Wheel zoom changed only the display, and Reset
restored the view with source Saved and zero history entries.

Translate, World and one-meter snapping were selected through actual controls.
The first native drag attempt, `sky.drag` from window coordinates `(1116,774)`
to `(1116,702)`, did not start the intended gesture: selection cleared with
`No visible mesh`, geometry stayed unchanged and source remained Saved/history
zero. Reselecting the triangle worked. This is a **failed acceptance attempt**,
not a passing transform drag or an identified engine/input-tool defect. No native
event trace was captured, so coalescing or another cause remains **UNDETERMINED**.
BL-17-UI-DRAG-01 records the new investigation; it is not excused by the earlier
desktop deferral.

The documented Computer Use API exposes an atomic drag, with no held-button,
duration or right-drag parameter. Further continuous-drag/cancellation cases were
not performed or claimed. The session closed cleanly with 28,444
submitted/presented/completed and zero validation diagnostics; command/environment
and native logs are under `viewport-rtx-worker/`, with
`viewport-rtx-result.json`. Native viewport transforms, full hierarchy behavior and
Intel 16e acceptance remain unqualified.

## 05c diagnostic revisit, 2026-10-31

The user requested the testing backlog, and the installed NVIDIA driver changed
from the original 610.62 to 616.56. The existing independent raw Win32/Vulkan
reproducer completed its unchanged eight fresh window/instance/surface/device/
swapchain lifetimes per GPU with required pinned validation, no image acquisition,
submission or presentation, and the original 120-second native deadline. It links
no Shark, GLFW or NVRHI implementation. Both traces passed the existing ordered
trace/parser/cleanup checks with zero validation diagnostics.

On NVIDIA 616.56, closed GDI counts remain `3,4,5,6,7,8,9,10`; closed kernel handles
increase from 323 to 342. Thus the new driver does not remove the observed
independent resource growth. Intel 101.6790's eight-lifetime run kept GDI at 3 and
handles at 310, with closed USER counts varying from 10 to 11. The existing Intel
forty-lifetime reproducer then completed its unchanged workload: GDI stayed 3 and
handles 316, while closed USER counts ranged 10..14 and changed from 11 on the
first close to 13 on the last. This still supplies no zero-growth qualification.

Evidence is `05c-raw8-current-driver/summary.json` and
`05c-intel-raw40/summary.json`, with their raw native/stdout and token records.
Both results are **DIAGNOSTIC_ONLY / qualification NOT_RUN**. Original failures,
driver attribution and resource budgets remain intact. These observations do not
establish a supported correction or close BL-05C-01/02/04. Further unchanged
sustained stress is not repeated without an actionable resolution; no driver/OS
change, foreign-handle cleanup or allowance increase was made. The original
unexplained ASan exit timeout is investigated separately below.

### ASan exit observation

The existing maximum-64-child observer stopped at child 55, the current Intel
`surface-error` case, after its unchanged 30-second deadline (30.590s observed).
The preceding 54 children completed their expected fault exits in 0.791–1.582s.
The failed child emitted all seven teardown calls, zero owned native resources
and no validation diagnostics. Cleanup had no errors. This is
**CAPTURED_OBSERVATION / qualification NOT_RUN**, not a completed 64-child pass.

At the eight-second capture threshold, the native process reported exit status
1 while the Python worker remained inside `WaitForSingleObjectEx`; its wait
chain points to that native process object. No `child_exited` or worker-`atexit`
event occurred. The captured worker dump narrows the outstanding wait to native
process completion. It does not identify a responsible DLL, driver or kernel
operation. Microsoft documents status publication before process-object
signaling, so the reported exit code alone cannot establish completion.
[ExitProcess ordering](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitprocess)

This exposed a diagnostic blind spot: both observer layers skipped native
capture whenever exit status differed from `STILL_ACTIVE`. They now record a
zero-time process wait with `SYNCHRONIZE` access, preserve PID/creation fencing
and capture nonsignaled processes regardless of published status. Image-query
failure no longer discards a valid identity. Capture retains dump failures,
bounded thread/wait-chain observations and final process signaling. The
production process runner and all deadlines remain unchanged; trusting the exit
code instead of waiting would conceal incomplete termination.

Verification: the current Windows ASan `PresentationFaults` target rebuilt
successfully, the existing tooling family passed 16/16 tests including the
captured status-1/nonsignaled boundary, and the final device-free observer
selfcheck passed with valid native/worker captures and identity rejection.
Evidence is `asan-backlog-final-build.log`, `asan-exit-native-final/summary.json`
(including child 55's dump/stacks/wait chain), and
`asan-exit-signal-final-selfcheck/summary.json`. An initial preparation correctly
stopped before launching a child after a line-ending change invalidated source
metadata; that failure remains in `asan-exit-native/`.

After the diagnostic correction, one authorized invocation kept the original
64-child workload, order, deadlines and thresholds. All 64 children completed
their expected fault outcomes in 0.793–1.465s: 32 per GPU and 32 per variant,
with no slow captures, timeouts, protocol/observer/cleanup errors or changed
inputs. The ordinary native executable's bytes match the child-55 failed run.
Evidence is `asan-exit-native-signal/summary.json` and
`asan-backlog-native-signal.log`. Its status remains **UNRESOLVED / qualification
NOT_RUN**: the late exit did not recur, so its responsible operation remains
unidentified. No further native repetition was made. BL-05C-03, the original
timeout and the new captured timeout stay open.

## Connected acceptance and next work

The user explicitly resumed desktop testing in the latest request, after 18e
implementation. The [testing policy](../testing-policy.md) and
[backlog](../backlog.md#desktop-ui-deferral) record this amendment. Actual
connected editor/viewport/physics acceptance follows
[editor acceptance](../editor-acceptance.md), [viewport acceptance](../viewport-acceptance.md)
and [physics acceptance](../physics-acceptance.md). Full REQ-16/17/18 remain
NOT_RUN until their required native/connected evidence exists.

18e implementation and its selected headless checks are complete. The
2026-11-01 addenda below record the pointer defect, correction and further
backlog evidence. Remaining work is the unperformed connected UI acceptance on
both GPUs; no headless result substitutes for it. Full REQ-16/17/18 remain NOT_RUN.
Assignment 19 follows 18 in the manifest and is not started by this increment.

`.gitignore` was reviewed: existing build/out/cache/local-roadmap patterns cover
all generated outputs while source, fixtures and documentation remain eligible
for version control. No staging, commits or pushes occurred.

Suggested commit: `feat(editor): add shared physics preview and rigid body controls`.

## 05c backlog qualification and allocator diagnosis, 2026-11-01

Base revision: `1848352`. The user's full-backlog request authorized one serial
strict Debug/Release/ASan selection per GPU, preserving required validation and
all workloads/limits. These executions preceded the input correction and new
allocator diagnostic: they remain evidence for unchanged resource/lifetime code,
not final-worktree identity or later input behavior. Exact identities/logs are in
`out/19-testing-work/05c-qualification/` and its compact `summary.json`.

| Strict selection | Outer duration | First failure; native elapsed where available |
| --- | --- | --- |
| Debug RTX, NVIDIA 616.56 | 21.790s | Restore 6 at 17.955s: USER 41 -> 42 |
| Debug Intel, 101.6790 | 8.451s | Resize 8 at 4.917s: USER 48 -> 50 |
| Release RTX, NVIDIA 616.56 | 606.100s | Closed lifetime 6: GDI 15 -> 16 |
| Release Intel, 101.6790 | 33.680s | Restore 1 at 30.004s: USER 48 -> 49 |
| ASan RTX, NVIDIA 616.56 | 7.527s | Resize 5 at 3.125s: private bytes +258.539MiB, above 256MiB |
| ASan Intel, 101.6790 | 13.316s | Resize 15 at 9.102s: private bytes +278.887MiB, above 256MiB |

All six are **FAIL / qualification NOT_RUN**. Release RTX completed the full
600.001s sustained phase/100 resizes/20 restores; other selections stopped at
their first failure. All explicit failure cleanups succeeded with zero tracked
native resources and validation warnings/errors/dropped messages. USER growth
does not establish a leak or the original NVIDIA DC mechanism; ASan memory growth
is a separate finding. No blind retries were made.

The exit investigation added a prearmed, owned ETW memory ring: 128MiB,
ProcessThread/Loader/CSwitch/ReadyThread events with scheduling and exit stacks,
without sampled CPU profiling or persistent settings. The controlled CPU-stall
selfcheck passed with actual stacks/zero lost events or buffers. One unchanged
64-child observer invocation then completed all expected exits without recurrence
or observer/cleanup errors. The owned instance stopped and was verified inactive.
The 144MiB merged ETL reports zero lost events/buffers, but retains a circular
**tail**, not all 64 children. Evidence under `out/19-testing-work/` is
`05c-etw/cpu-selfcheck-recording/` and `05c-etw-native/`. Original/child-55 exit
timeouts remain **UNRESOLVED / NOT_RUN**, without a causal rundown stack or fix.

To investigate the new memory failures, the existing stress fixture/runner gained
ASan-only `--diagnose-asan-allocator`. It adds separate owned allocator counters
at existing sample/failure/close points and at most three accumulated-stat dumps
(warmup/failure/close). It preserves strict first-budget failure, rejects growth
continuation and can never qualify. Public API headers/DLL exports were checked;
the affected Python family passed **28/28** on final inputs. The ASan target built
in 37.419s, then passed device-free description/three argument rejections
(`05c-allocator-prepare/`); no unrelated build/full CPU matrix was repeated.
The new non-ASan conditional branch also built in Debug (3.488s); device-free
description and allocator-mode rejection passed before GPU access. Release reuses
that branch coverage. Records are `05c-allocator-debug-*`.

After the shared Core/Client inputs were held stable, one new diagnostic capture
per GPU retained the unchanged strict memory failure:

| Allocator diagnostic | Failure point | Private-byte growth | Allocator-mapped growth | Global-quarantine growth |
| --- | --- | --- | --- | --- |
| RTX, 7.584s outer | Resize 5, 3.118s | 256.813MiB | 222.664MiB | 167.980MiB |
| Intel, 12.769s outer | Resize 14, 8.549s | 262.531MiB | 232.320MiB | 174.645MiB |

Runtime statistics resolve the unmodified quarantine limits to global 256MiB and
thread-local 1024KiB. Retained freed/quarantined storage is a demonstrated large
contributor; mapped, free and quarantine values overlap and must not be added as
independent totals. The public live-byte counter stays zero while accumulated
freed bytes exceed malloced bytes, so zero cannot prove an empty live heap.
[Upstream ASan](https://github.com/llvm/llvm-project/blob/main/compiler-rt/lib/asan/asan_stats.cpp)
clamps inconsistent/racy totals to zero; this does not establish the installed
MSVC fork's cause. Implausible raw RSS is excluded. Both captures exited 1 with
zero tracked resources after cleanup and clean validation. Evidence is
`05c-allocator-native/accounting-summary.json` and per-GPU records/stderr;
`05c-asan-memory/` retains earlier runtime-help/strict-sample reductions.

No engine ownership fix is established. Cause-level memory work needs caller
evidence; exit work needs a captured recurrence. Further unchanged stress is not
useful without new actionable evidence. Fixed limits, historical receipts and
full **REQ-05 NOT_RUN** remain unchanged. No driver/OS, allocator option, budget
or foreign-handle cleanup change was made. GPU/build/input holds were released.

Suggested commit: `test(presentation): add bounded ASan allocator accounting`.

## Pointer capture correction and resumed acceptance, 2026-11-01

Base: `1848352`, initially clean `main`. All `out/19-testing-work/` paths below
belong to testing-backlog work, not assignment 19. The user owns staging,
commits and pushing. Existing `.gitignore` rules cover these generated artifacts;
the new private Editor header remains eligible for version control.

The bounded RTX trace reproduced BL-17-UI-DRAG-01: the neutral cursor was on the
world-Y shaft at `[1673,1126]`, but the first pressed/held snapshot already had
current cursor `[1673,1018]`. Hit testing used that endpoint and forwarded the
miss to picking. This was motion before capture, rather than both button edges
coalescing or an established wrong tool start. The failed session closed with
23,715 completed presentations and zero validation diagnostics. Original evidence
is `viewport-trace-rtx-worker/`; the earlier failed attempt remains unchanged.

`InputSnapshot` now adds bounded, owned first press/release positions and actual
edge-seen/ambiguity flags for eight mouse buttons. Positions use the last accepted
cursor at ingestion, with no new OS polling or ordered event queue. Duplicate
notifications cannot replace them; unknown origins stay invalid. Focus-generated
release flags remain distinct from real edges. Capture, fixed-frame pending
merging, validation and cancellation preserve that meaning. See the updated
[input contract](../input-contracts.md).

Gizmo hit/begin and picking use the owned press; gesture motion includes movement
before capture and uses the owned release endpoint. One complete unambiguous
gesture may commit once. Orbit/pan consumes the same endpoints. Mixed or ambiguous
pointer activity cancels conservatively; the existing focus/image/revision fences
and shared history owner remain. The ImGui adapter queues one button's owned
positions around its edges, then restores the current cursor. Ambiguous/mixed
input prunes only pending mouse events through pinned private Editor internals;
keyboard, Unicode and focus events survive. No authoring protocol/source format,
FeatureWorld case, mutable Scene owner or public native handle was added.

Optional `--input-trace on|off` defaults off. One application owner emits at most
256 changed observations across reopen, with at most 16KiB payload each, recording
captured input, actual image bounds, routing gates and retained gizmo outcomes.
It cannot inject input; output failures cannot alter authoring outcomes. It is
explicitly a captured-interval diagnostic, not an ordered callback log.

| Final input/consumer selection | Result; evidence under `out/19-testing-work/` |
| --- | --- |
| Windows Debug affected input/window/frame/GUI and scene-render CLI | PASS, 5/5 in 2.84s; `fast-pointer-debug-final-tests.log/.xml` |
| Retained WSL Debug input/frame | PASS, 2/2; `fast-pointer-wsl-final/` |
| Windows ASan GUI adapter | PASS, 1/1 in 4.48s; `fast-pointer-asan-tests.log/.xml` |
| Windows ASan input/frame | PASS, 2/2 in 0.14s; `fast-pointer-asan-input-tests.log`, `fast-pointer-asan-input.xml` |
| GUI regression against the immutable old adapter | Expected rejection of both new cases, seven failed assertions; `fast-pointer-gui-red-control.log`; managed scratch removed |

Canonical builds and existing seeds/timeouts were retained. The first new frame
test exposed synthetic focus releases being counted as physical cycles; actual
edge-seen flags correct that cause. Failed Windows/WSL logs remain in
`fast-pointer-debug-tests.log/.xml` and `fast-pointer-wsl/`. A later Debug build
hit MSVC C1090/PDB RPC failure in unchanged Client files during cross-profile
compiler activity. After coordinating compiler profiles, serial compilation
passed without changing flags/settings; its external cause is unestablished.
Both failure and reconciled build logs remain. The final Debug build includes
Editor, EditorGuiTests, Tests and SceneRenderSmoke; the latter's metadata was
refreshed after the input header changed. Renderer/physics behavior was unchanged,
so the earlier native readbacks below are reused rather than reported as rerun.

Actual RTX repeated the exact `(1116,774)` -> `(1116,702)` World Y/Snap 1 drag.
The corrected trace retains press `[1673,1126]` while current cursor is the
endpoint, starts at frame 6928/revision 3 and commits once at frame 6929/revision 4
with no pick/navigation. The rendered triangle moved; history showed one group.
Inspect/Undo/reload displayed local T `[1,0,0]`; Redo/save retained one group.
Independent saved JSON is T `[2.9999999999999996,-4.440892098500624e-16,0]`,
identity rotation/unit scale, within the existing tolerance for `[3,0,0]`.
Evidence: `viewport-fixed-rtx-worker/` and `viewport-rtx-world-y-source.json`.
Further actual outcomes and remaining native gates are recorded in the backlog.

## Additional Intel editor and physics rendering evidence, 2026-11-01

The actual Intel imported-project workflow exercised one-group Name/translation
Apply, text-focus save suppression, unapplied inspector Save/Keep/discard,
Undo/Redo across Save and explicit metadata publication. Empty license rejected
without publication; an external sidecar LF invalidated the exact file token
without replacing the original successful 580-byte receipt. An unapplied metadata
close decision retained the draft on Keep editing; discarding it preserved the
already written metadata. Dirty-close Cancel retained the project, then Save and
close released it.

A Ready import was cancelled, explicitly drained to Cancelled, refreshed and
taken; a second Ready result published only on explicit request, importing one
changed glTF node and reusing its unchanged binary. Terminal continuation checked
saved state, MIT/provenance, the generator-only source change, original binary
bytes and fresh root/dependency cache descriptors. It saved `Terminal return`;
the actual Intel Editor reopened Saved with empty history and fresh imported
geometry. Reciprocal competing writers were rejected. Final native close exited
zero with 98,888 completed submissions/presentations and no validation diagnostics.
Evidence is `ui-intel-terminal-return.json`, `ui-intel-busy.json` and
`out/18e-work/backlog-intel16-worker/` plus its result. These are partial connected
results; remaining hierarchy/component/focus and full native gates stay open.

A final independent public-process control compared complete typed/source camera,
actor, hierarchy, IDs, literal world matrices, metadata and fresh cache closures.
A temporary Name edit made default dirty close reject without changing status,
history, ledger or saved files. Retained-ticket Undo restored the actor; explicit
Save preserved history and all seven source files were byte-identical to those
already reopened in the actual Editor. The public owner closed cleanly.
`ui-intel-final-cpu-control.json/.log` records this CPU continuation; it does not
claim another native run.

The existing SceneRenderSmoke now provides opt-in `--physics`, supervised by
`tools/scene_render_smoke.py --mode physics`. Both GPUs passed eight fixed
128x96 readbacks for static reset, dynamic teleport, kinematic targets and
alpha 0/0.5/1 interpolated visual children/child Camera. One Core world executes
seven 60 Hz updates with explicit gravity and disabled collision masks; contacts
remain separate. Independent literal pose/color/depth oracles compared 96,576
pixel pairs per GPU with zero observed error, using the unchanged one-byte/color
and 1e-5/depth tolerances. All depth values were finite/in range, native physics
closed and the sole GPU target generation retired with zero validation diagnostics.
RTX completed in 2.426s, Intel in 2.232s under required validation and standard-user
supervision: `physics-render-{rtx,intel}/summary.json` and token records.
The existing Python family/CLI selection passed before native runs; coverage maps
this separate fixture without adding public physics APIs or FeatureWorld cases.
It qualifies these rendered physics paths, not actual UI parity or full REQ-18.

Suggested commit: `fix(input): preserve pointer edges and advance native testing`.

## Current desktop interruption and exact next work

RTX World Z/Snap 15 rotation and Local X/Snap 1 translation subsequently committed
once each, with history advancing to three groups and no pick/navigation. Their
release receipts are revisions 7 and 8; rotation visibly quarter-turned and the
local move shifted the pivot left. Final exact saved TRS/public-terminal comparison
for those two edits is **NOT_RUN**: they remain unsaved in the live generated
project. `viewport-native-partial-summary.md` and
`viewport-native-retained-receipts.json` preserve the actual action/trace scope.

Computer Use rejected three intended scale drags with its manual-input guard,
including after fresh observations; independent pointer movement was observed.
No scale press or history change occurred. All desktop inputs and refresh retries
are stopped pending the user's availability reply. The live Editor is PID 38396,
window 332404, exec session 13690; its original supervised launch has a deadline
of **2026-11-01 04:09:43 EST / 09:09:43 UTC**. Preserve the current test edits
before that deadline if the desktop becomes available. No post-interruption
save/discard/close/termination has been attempted or claimed.

After explicit resumption, first save/inspect the retained RTX edits, then finish
the remaining scale/reference/terminal continuation and Intel correction check.
Intel's fresh reference is prepared at
`out/19-testing-work/viewport-intel-native/project/Terminal.sharkproject`.
The separate 65-entity hierarchy/optional-component project is
`ui-editor-paths/project/Terminal.sharkproject`; fresh per-GPU physics inputs and
literal expectations are in `physics-ui-remaining/UI-guide.md`. Their unperformed
native gates remain NOT_RUN. Held-button cancellation, right/middle navigation
and multi-sample zero crossing also need an input method that can actually hold
buttons across actions; the documented atomic desktop drag cannot establish them.

Final contract/link/coverage, repository/ignore and changed-C++ formatting/diff
checks pass. A final edited-file LF normalization changes no C++ tokens or input
behavior and requires no engine rerun. Existing headless, rendering and connected
results are reused only for their stated scope. No driver/OS, allocator setting,
resource allowance, workload or source-control ownership changed. Full
REQ-05/16/17/18 remain NOT_RUN; assignment 19 is not started.

Suggested commit: `fix(input): retain pointer positions and record testing backlog results`.
