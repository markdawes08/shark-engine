# 16a: native editor shell and shared project owner

Date: 2026-10-29. Base: `83094fe` (`feat(authoring): qualify connected terminal workflow`),
initially clean `main`. **16a implementation is complete; remaining desktop
acceptance is DEFERRED under the user's 2026-10-29 amendment. Selected CPU,
native GUI rendering and dependency checks pass. Full REQ-16 remains NOT_RUN;
`current_assignment` is 16.** Coding can continue with 16b. The user owns staging,
commits and pushing.

## Delivered implementation

The [editor shell](../editor-shell.md) adds explicit `Editor --editor` startup,
project folder/path controls, open/save/close, saved/dirty status, owned errors and
save/discard/cancel decisions before normal exit. Native close cancellation keeps
the window alive while the owner resolves closure. Focus, text editing and modal
state gate shortcuts. Existing diagnostic/help/version paths remain available.

`ProjectSession` uses the existing AuthoringProtocol owner in process. It retains
the same ProjectFiles/AuthoredScene, common validation, revision fences and
publication/history/retry semantics. It neither writes source through another
path nor holds another Scene. Independent terminal writers receive the existing
busy conflict; save/close/reopen is the handoff. No protocol/source schema changes
are introduced. Shared session tests include dirty/cancel/discard, save/conflict,
history preservation, strict input rejection and creating-thread ownership.

Dear ImGui is exactly pinned and private to native Editor. The
[dependency review](../dependencies/imgui.md) records the archive, notices,
compile options and protected build gate. Client receives bounded engine-owned
GUI atlas/frame values, a separate offline shader profile and presentation through
the existing graphics owner. No ImGui/native handle enters public engine values.
The new native fixture captures actual presented alpha blending, clipping,
next-frame geometry, resize and close cancellation with independent pixel checks.

## Selected verification

Selection follows the [testing policy](../testing-policy.md): changed Editor
session/input/GUI behavior and directly affected Client window/presentation and
dependency boundaries. CPU checks do not qualify GPUs or the actual UI. Evidence
below is retained in `out/16a-work/`; these are affected selections, not a
whole-project inventory. The coalesced-shortcut correction passed its focused
final-input check and the observed Intel interactions described below. The user
paused desktop testing before completing restoration and clean exit.

| Selection | Result / evidence |
| --- | --- |
| Windows Debug affected build and selected inventory | PASS; `configure-final.log`, `build-debug-2.log`, `build-editor-final.log`, `windows-inventory-final.log` |
| Editor session/GUI/dependencies and directly affected Client regressions | 13/13 PASS, 9.21 s; `windows-tests.log`, `windows-tests.xml`. Existing Editor help/CLI check separately 1/1 PASS, 0.31 s; `editor-cli.log`. Corrected GUI adapter: 5 cases / 240 assertions PASS (seed 1337), 0.08 s; `build-chords.log`, `gui-adapter-chords.log`. |
| Selected WSL ASan+UBSan owner/Client CPU behavior | Build and 8/8 PASS, 2.18 s; `wsl/linux-asan-ubsan-build.log`, `wsl/linux-asan-ubsan-inventory.log`, `wsl/linux-asan-ubsan-tests.log/xml` |
| Selected WSL Debug owner/Client CPU behavior | Build and 8/8 PASS, 1.38 s; corresponding `wsl/linux-debug-build.log`, `wsl/linux-debug-inventory.log`, `wsl/linux-debug-tests.log/xml` |
| Native presented GUI fixture, RTX 4070 Laptop | PASS, 2.126 s; `gui-rtx/summary.json`, native logs/captures and `gui-rtx-token.json` |
| Native presented GUI fixture, Intel | PASS, 1.834 s; `gui-intel/summary.json`, native logs/captures and `gui-intel-token.json` |
| Actual shell open/save/close and focus interaction | PARTIAL; remaining checks DEFERRED/NOT_RUN under BL-16A-UI-01. RTX save/close/busy-release and corrected Intel text/shortcut/open/save/resize interactions were observed; Intel restoration/clean exit and final-input RTX shortcut continuation remain. See below. |
| Protected dependency boundary | Runtime-only `Runtime` and boundary build PASS; `runtime-boundary.log`. Four ImGui dependency methods pass within the Windows selection, including Editor-OFF/CPU exclusion and unverified input rejection. WSL owner tests compile without ImGui/Client linkage. |
| Repository/link/manifest, formatting and diff checks | PASS; `contract-final.log`, `repository-final.log`, `format-checked.log`, `git diff --check`. Final formatting is whitespace-only; no engine matrix was repeated for documentation/formatting. |

The Windows selection is reproducible with:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark\.(editor\.(project-session|gui-adapter|dependencies|window-cli|presentation-cli)|client\.(gui-draw|input|window|graphics|presentation|gpu-resources|offscreen|scene-viewport))$" --output-on-failure --no-tests=error
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark\.editor\.cli$" --output-on-failure --no-tests=error
```

The retained WSL driver builds `EditorProjectSessionTests Tests` and runs
`^shark\.(editor\.project-session|client\.(gui-draw|window|graphics|presentation|gpu-resources|offscreen|scene-viewport))$`
in the canonical `linux-debug` and `linux-asan-ubsan` builds. Exact invocations
are in `wsl-commands.sh` and `wsl-driver.sh`. This qualifies the portable owner
and CPU contracts; it does not instrument ImGui or the Windows native backend.
No fresh Windows ASan/native GUI sanitizer result is claimed. Server behavior,
package deployment and unrelated subsystem matrices were not rerun because this
change adds no Server behavior or packaging deliverable.

Both native fixtures use `tools/gui_smoke.py`, the freshly verified
`gui-shaders/` generation and `validation-tools/summary.json` through the existing
standard-user supervisor. The reports retain exact commands, shader/lock/tool
identities and driver records. Devices are RTX UUID
`863d6f59a51894ab8b4829aa9460c78d` (NVIDIA 616.56) and Intel UUID
`868088a7040000000002000000000000` (101.6790). Each executes baseline, changed,
resized and close-cancelled presentations. Across four captures, 608 fixed
interior color pixels and all 96,000 depth pixels are checked. Maximum color
error is one byte against the fixed two-byte tolerance; depth remains 1.0 and
alpha 255. Both runs have zero validation warnings/errors/dropped messages,
four completed submissions and two retired presentation generations. No
output-derived baseline or independent-frame substitute is used.

Shader and validation receipts use the new full dependency-lock identity;
earlier receipts remain unchanged. The canonical validator build and exact
archive caches are reused. This lock addition does not trigger unrelated engine
qualification.

### Actual UI progress and investigated failures

The RTX Editor ran with required validation under a normal user token. Actual
project opening, Save and Close succeeded. A concurrent terminal owner returned
`conflict`; after Editor release, a new terminal owner reopened the saved clean
project. `editor-rtx/process.json` and its `stdout.log` retain the clean exit,
publication receipt and zero validation diagnostics; `editor-rtx-busy.log` and
`editor-rtx-released.log` retain the independent ownership controls.

This interaction also exposed a coalesced keyboard-chord issue: a complete
modifier/key press and release between polls did not reliably route the shortcut.
The adapter now preserves modifier presses around coalesced button events and
restores their final state; global shortcuts observe the same bounded snapshot
semantics. The focused regression exercises real ImGui mouse focus and Ctrl+A
text replacement, including a complete chord within one snapshot. The first
successful RTX flow is retained, not rewritten as evidence for this later fix.

Actual corrected Intel interaction opened the scratch project under Unicode
folder `project-café`, used Ctrl+W/Ctrl+O, tab navigation, mouse field focus,
Ctrl+A and text entry, rejected a missing project visibly, reopened the valid
project, saved through Ctrl+S, maximized and minimized the window. Computer Use
was then stopped by the user's physical Escape key before restoration. The
user explicitly requested coding-only work until desktop testing is resumed.
No further desktop input was issued. The existing supervisor later reached its
600-second deadline and terminated its owned process; `editor-intel/process.json`
retains exit 124, `timed_out: true`, no cleanup errors and no clean-close event.
Its `stdout.log` retains readiness and the publication receipt; this interrupted
run is not a passing lifecycle qualification. No Editor process remained at the
subsequent read-only process check.

[BL-16A-UI-01](../backlog.md) retains restoration/focus/clean-close and final-input
RTX shortcut continuation. The user's amendment makes these nonblocking for
16b coding, while full UI acceptance remains unqualified. No new windowed tests
or desktop automation may run until the user explicitly resumes them.

Two earlier integration failures are resolved and their logs retained:

- The pinned NVRHI `static_vector` has no `clear()` method; the scissor reset now
  uses `resize(0)`. `build-debug.log` retains the compile rejection and
  `build-debug-2.log` the succeeding build.
- Initial GUI SPIR-V descriptor bindings differed from the declared recipe.
  Explicit bindings corrected the shader interface; `shaders.log` retains the
  rejection and `shaders-fixed.log` the successful independent validation.

No tolerance, workload or validation requirement was weakened to resolve either
failure.

## Limits, retention and next work

16a is the initial project shell. It has no hierarchy/selection, scene viewport,
property inspector, asset browser or console panel, and no graphical scene edit
to create dirty state yet. Session-level tests establish dirty-state decisions;
later UI-driven edits must exercise them in connected acceptance. Full human/
terminal semantic parity belongs to 16e. Navigation, picking and gizmos belong
to 17. Native Linux graphics, Mac and production MMO capacity remain outside scope.

`.gitignore` was reviewed: build/out/.tools, Python bytecode and reserved project
coordination rules already cover ImGui archives/populations, GUI shader stages,
captures and local sessions. Source, tests, shaders and documentation remain
eligible for version control. ImGui ini/log writing is disabled; no new ignore
pattern or storage root is needed.

Continue coding 16b under the user's desktop-testing amendment, following
the [editor plan](../roadmap/editor-plan.md): active-scene viewport, bounded
hierarchy/selection and entity lifecycle through the same owner. Preserve the
terminal path and explicit project handoff.

Suggested commit: `feat(editor): add native shell over shared project ownership`.
