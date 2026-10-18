# 04b - Native GLFW window integration

Date: 2026-10-18. Base: `b189d75` on `main`, initially clean.
Result: uncommitted worktree; the user owns staging, commits and pushes.

**04b is PASS under the current-machine scope.** The exact next checkpoint is
**04c, frame-loop integration and qualification** under the
[04 plan](../roadmap/window-input-plan.md). Full `REQ-04` stays NOT_RUN until 04c
defines and verifies fixed-tick input consumption, offline pause/step and long-frame
behavior. No assignment 05 renderer implementation starts here.

## Delivered behavior

The [window guide](../window-lifecycle.md) specifies the new public API and launch
commands. EngineClient's unique `Window` owns native GLFW initialization, its
window, callback bindings and reverse cleanup. A process lease prevents a second
owner from disturbing the first. Calls validate the controlling thread; initial
creation on the process main thread remains an explicit caller precondition.
Malformed UTF-8 titles/dimensions reject before native startup. Every attempted
startup phase rolls back, including partial failure. Owned errors retain stable
operation context and numeric native error codes after cleanup.

No graphics context is created. Native callbacks translate all supported raw
keys/buttons, repeat, Unicode text, cursor and scroll into 04a's existing state.
Focus loss and minimization neutralize held input without touching any scheduler.
Close requests latch until the owner tears down. Content dimensions, framebuffer
pixels and content scale remain distinct. Cursor conversion uses the actual
framebuffer/content ratio, rejecting zero/nonfinite/overflow results. Callback
rejections accumulate in fixed saturating counters; owned diagnostics are created
only outside callbacks. The native error bridge retains a lock-free numeric code
and restores the prior GLFW error callback when releasing the lease.

Runtime and Editor now accept `--window [--window-ms 1..60000]`, sharing the same
event-pump helper. Help, version and default informational invocation do not open
a display. The bounded mode measures the event-pump interval after creation;
synchronous platform calls rely on an external watchdog during qualification.
There is no rendering, scene, editor UI, game loading, simulation tick or pause
policy in this demonstration. Ordinary CTest remains display-free.

`SHARK_NATIVE_WINDOW` defaults ON only for Windows client builds. OFF compiles an
explicit native-unavailable adapter while keeping injectable CPU contracts. Core,
Server and WSL CPU clients neither discover nor fetch GLFW. Shared locked-source
handling supplies the same static GLFW to native clients and the opt-in graphics
probe once. No source pin, options or patch changed; GLFW lock owner metadata now
records its actual EngineClient use. Ordinary client builds fetch neither NVRHI
nor Vulkan. The graphics probe still uses its separately selected dependencies.

Changed implementation includes the public window/launch headers, private backend
and native/unavailable adapters, lifecycle/mapping sources, shared launcher and
Runtime/Editor entry points. CMake adds native capability gating and shared lock
modules. Tests add window/launch fixtures, window CLI rejection checks and an
explicit native `WindowSmoke` executable with a bounded Python evidence runner.
Existing dependency rejection tests gain native-option and C-compiler exclusion
coverage. No persistent format, authoring command or gameplay protocol changes.

## Verification and evidence

The [durable receipt](../coverage/evidence/04b-local.json) records the full base
revision, changed implementation/test/tool/lock hashes, prior 04a receipt hash,
exact commands, toolchains, raw artifacts and limitations. The final source was
frozen for both native and WSL verification; documentation/status updates are
covered by final contract/repository checks.

| Configuration/check | Result |
| --- | --- |
| Windows Debug, Release, AddressSanitizer | 29/29 CTests each PASS |
| WSL client CPU GCC Debug, Clang ASan+UBSan | 29/29 CTests each PASS |
| WSL headless GCC Debug, Clang ASan+UBSan | 23/23 CTests each PASS |
| Window family in five client builds | 29 cases, 1,765 assertions each; seed 1337; no skips |
| Input family in five client builds | 17 cases, 34,172 assertions each; seed 1337; no skips |
| Native Windows smoke in Debug/Release/ASan | Eight cases each PASS; 17 engine window lifetimes per run |
| Actual Runtime and Editor bounded windows | Both executables PASS in all three native configurations |
| Windows Runtime-only / Server-only | Configure/build, boundaries and executable smokes PASS; Runtime also opens/closes its bounded window |
| Existing GraphicsProbe shared-GLFW regression | Configure and targeted build PASS; no new GPU run claimed |
| WSL Release Server-only | Configure/build/install, source/header/compiler/link/package closure and installed smoke PASS |
| Separate SDK-free WSL runtime | Fresh `/package-04b`, isolated network and timed Server smoke PASS |

The seven configurations total 191 passing CTests. CPU window fixtures exercise
every attempted startup phase and reverse rollback, 32 owner cycles, second-lease
rejection, wrong-thread methods, all 120 key/eight button mappings, synthetic
native-code failures, focus/minimize release, stale close observations, invalid
window events, 4,096-event diagnostic bursts and extreme/fractional coordinate
conversion. The existing Core/contracts/lifecycle/timing/work regressions pass
across all seven configurations. Family counts overlap where prior tags overlap;
they must not be summed as unique cases.

Native smoke opens owned Windows HWNDs and verifies UTF-8 title conversion,
current-monitor DPI, native client pixel dimensions, resize, real focus transfer,
minimize/restore, close requests, release-before-destroy and repeated RAII teardown.
Targeted synthetic Win32 messages exercise the actual GLFW callback path for key
press/repeat/release, mouse taps, cursor, fractional scroll, BMP/non-BMP Unicode and
text overflow without suppressing releases. The observed current monitor is
**144 DPI, scale 1.5**; after resize its 530x350 content and framebuffer dimensions
match, demonstrating why monitor scale must not replace the actual pixel ratio.
CPU fixtures separately cover unequal/fractional ratios and zero sizes. No display
settings are changed, no system-wide input is injected, and physical keyboard,
multiple monitors or GPU rendering are not claimed.

An initial development smoke passed before native-code diagnostics were added.
An early test assembly expected `invalid_argument` for an absent backend; the
test was corrected to the explicit `capability_unavailable` contract before final
verification. A review also fixed intermediate overflow in cursor conversion by
forming the dimension ratio before multiplication, with an extreme-coordinate
regression. Final suites exercise the completed source. Initial smoke and
dependency-preparation output remain under `out/04b-work`.

Windows uses Windows 11 build 26200 x64, MSVC 14.51.36231, SDK 10.0.28000.0 and
Python 3.13.5. WSL reuses the recorded Ubuntu 24.04 userspaces, GCC 13.3.0,
Clang 18.1.3 and minimal-profile Python 3.12.3 on the same kernel. Pinned tools:
CMake 3.31.10, Ninja 1.13.2 and clang-format 20.1.8. No host package, driver, WSL
resource setting, desktop scaling, hosted workflow or branch policy changed.

Exact top-level verification commands from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/04b-work/verify-windows.py
.tools/venv/Scripts/python.exe out/04b-work/run-wsl-verification.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/04b-work/check-final-repository.py
.tools/venv/Scripts/python.exe out/04b-work/collect-evidence.py
```

The native helper serializes `tools/ci.py` for Windows Debug/Release with jobs 2
and ASan with jobs 1. Each then runs `tools/window_smoke.py` into a fresh
`out/ci/04b-window/<preset>` directory and launches Runtime/Editor with `--window
--window-ms 100`. It configures/builds both standalone presets, runs version/Server
timed checks plus the standalone Runtime window, then configures `graphics-windows`
and builds `GraphicsProbe` with jobs 2. All expanded commands are in the receipt.

The WSL wrapper invokes `run-headless.sh` using private mounts, read-only source
and fresh build/evidence/package paths in retained pinned userspaces. It runs
`tools/headless_ci.py --build-root /work/build-04b --output /evidence --jobs 2`,
the separate SDK-free network-isolated smoke, then the two client CPU presets
under `/work/build-04b-client`. It explicitly checks native-window OFF, absence of
the C language and absence of GLFW/Vulkan/NVRHI targets/dependency directories in
all five configurations. That CPU-only client run does not qualify native Linux
window behavior or an independent machine.

Final `check_format.py`, `validate_contract.py`, its 29 negative self-tests,
`check_repository.py --self-test` (five skills and three negative fixtures), and
`git diff --check` pass. Existing platform-specific tooling fixtures retain
declared internal skips; input/window/Core families do not skip. Native smoke
uses two-second observation deadlines and a 60-second outer process-tree watchdog.

## Limits and next checkpoint

The manifest adds `client.native-window` and keeps current assignment 04 with
full `REQ-04` NOT_RUN. Plans and contributor guidance now point to **04c**. Earlier
handoffs and receipts remain unchanged. `.gitignore` already covers builds,
dependency caches, native smoke output and the local roadmap; no new rule is
needed. Nothing was staged, committed or pushed.

04c must connect these windows and input observations to existing Core clocks and
schedulers, define input delivery across zero/multiple ticks and verify offline
pause/step, long frames and the final current-machine gate. Native callbacks do
not send gameplay messages or alter authoritative server time. Action/UI routing,
relative cursor mode and authoring commands remain later integration work. There
are no outstanding 04b blockers; a usable editor UI and production graphics are
not part of this checkpoint.

Suggested commit: `feat(window): add owned GLFW windows and native input integration`
