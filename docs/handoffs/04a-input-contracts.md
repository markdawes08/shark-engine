# 04a - Input contracts and deterministic tests

Date: 2026-10-18. Base: `f6605b8` on `main`, initially clean.
Result: uncommitted worktree changes; the user owns staging, commits and pushes.

**04a is PASS under the current-machine scope.** The exact next checkpoint is
**04b, native GLFW window integration** in the
[04 plan](../roadmap/window-input-plan.md). Full `REQ-04` remains NOT_RUN pending
04b native window/focus/framebuffer/high-DPI verification and 04c frame-loop,
input consumption and offline pause qualification. Do not advance to assignment 05.

## Delivered behavior and contracts

EngineClient now owns device-free `shark::client::InputState` and value snapshots
in `shark/client/input.hpp`. The [input guide](../input-contracts.md) specifies
the contract. A client/window owner serializes ingestion and capture on one
thread; the state cannot be copied or moved. Snapshots own their arrays and remain
valid independently of subsequent captures or the source owner's lifetime.
There is no allocation, logging, blocking, callback into user code, hidden polling
or scheduler mutation in these methods. All ingestion/capture/query calls are
nonthrowing. Fixed result enums let the future native adapter report failures
outside C callbacks through the existing diagnostics system.

The API exposes 120 engine-owned key locations and eight mouse buttons, with
held/pressed/released state and separate keyboard repeat. Quick taps preserve both
edges; duplicate transitions do not manufacture edges, and repeat requires a held
key. Snapshots coalesce occurrences rather than preserve an ordered key-event log.
Cursor content coordinates and fractional scroll are finite doubles. Scroll
overflow rejects both new components atomically. Text preserves up to 64 Unicode
scalars in order, including U+0000, rejecting surrogates and values above U+10FFFF.
Overflow retains the accepted prefix and cannot block releases.

The state starts unfocused. Focus loss releases every held control, cancels
unconsumed pressed/repeated/text/scroll data and invalidates the cursor. Existing
release edges and overflow diagnostics survive. Valid late input is ignored while
unfocused; regain restores no held controls. These raw local rules neither pause
the shared server nor implement an authenticated online release message.

Only EngineClient compiles the new source. Tests add `client_input_tests.cpp` and
link EngineClient conditionally; Server-only configurations exclude the input
source/header/test. No dependency, lock, persistent format or public authoring
schema changed. Native mapping/window ownership, frame/tick latching and action/UI
routing remain explicit integration work. Core/Server stay presentation-free.

## Verification and evidence

The [durable receipt](../coverage/evidence/04a-local.json) records the full base
SHA, final source/CMake/test hashes, prior Core receipt hash, exact commands,
toolchains, raw artifact digests and limitations. Raw logs and helper scripts
remain ignored under `out/04a-work` and `out/ci/04a-headless`.

| Configuration/check | Result |
| --- | --- |
| Windows Debug, Release, AddressSanitizer | 26/26 CTests each PASS |
| WSL client-enabled GCC Debug | 26/26 CTests PASS |
| WSL client-enabled Clang ASan+UBSan | 25/26 initially; existing process fixture repaired and focused check PASS, closing all 26 |
| WSL headless GCC Debug, Clang ASan+UBSan | 23/23 CTests each PASS; client input excluded |
| Input family, all five client-enabled configurations | 17 cases, 34,172 assertions each; seed 1337; no skips |
| Prior Core/contracts/lifecycle/timing/work regressions | PASS in all seven configurations |
| Windows Runtime-only / Server-only | Configure/build, dependency boundaries and executable smokes PASS |
| WSL Release Server-only | Configure/build/boundary/install, source/compiler/header/link/package closure and installed smoke PASS |
| Separate SDK-free WSL runtime | Fresh `/package-04a`, isolated network and timed Server startup/shutdown PASS |

The seven configurations close 176 CTests, including the focused correction below.
Input tests cover every key/button,
capture boundaries, rapid transitions, duplicate/repeat behavior, malformed enums,
nonfinite values, scalar boundaries, full buffers, focus loss/regain and repeated
independent lifetimes. A 4,096-event deterministic reference trace uses seed 1337.
The input family has the `cpu;unit;client` labels and a 15-second CTest timeout;
fixtures use injected observations without a real display, input device or sleeps.
All C++/CMake/input test hashes remained unchanged during verification. No engine
implementation failure or test tolerance change was needed.

The initial WSL client sanitizer suite exposed an existing Python fixture race in
`tools/tests/test_graphics_package_linux.py`: a successfully retired process could
disappear between `Path.exists()` and reading `/proc/<pid>/stat`. The resulting
`FileNotFoundError` was the sole failed CTest; all input and Core cases passed.
The fixture now observes process state with a single read, treating disappearance
as retirement while preserving live-process/deadline assertions and other errors.
Both descendant-retirement checks use that observation. The repaired package
module passes 21 Python tests in WSL with no skips; Windows passes with 10 declared
Linux-specific skips. A focused CTest rerun in the same WSL sanitizer build closes
the failed test. Original failed JUnit,
phase summaries and wrapper receipt remain unchanged. The corrective receipt is
`out/04a-work/package-fixture-correction.json`, embedded in the durable receipt.
Other full suites were not repeated after this Python-only fixture repair.

Windows: Windows 11 build 26200 x64, MSVC 14.51.36231, SDK 10.0.28000.0,
Python 3.13.5. WSL: existing kernel 6.18.33.2 and retained pinned Ubuntu 24.04
userspaces, GCC 13.3.0, Clang 18.1.3 and Python 3.12.3. Pinned tools: CMake 3.31.10,
Ninja 1.13.2 and clang-format 20.1.8. The WSL client checks are CPU-only with display
variables absent; they do not claim native Linux window/GPU support. The SDK-free
runtime shares this PC and its WSL kernel, not an independent clean machine.

Exact top-level commands from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/04a-work/verify-windows.py
.tools/venv/Scripts/python.exe out/04a-work/run-wsl-verification.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/04a-work/check-final-repository.py
.tools/venv/Scripts/python.exe out/04a-work/collect-evidence.py
```

The Windows helper serializes existing entry points followed by both standalone
builds and smokes; the receipt retains every expanded command and phase:

```text
python tools/ci.py --preset windows-debug --jobs 2
python tools/ci.py --preset windows-release --jobs 2
python tools/ci.py --preset windows-asan --jobs 1
cmake --preset runtime-only
cmake --build --preset runtime-only --parallel 1
cmake --preset server-only
cmake --build --preset server-only --parallel 1
build/runtime-only/bin/Runtime.exe --version
build/server-only/bin/Server.exe --run-for-ms 20
```

The WSL wrapper invokes `wsl --user root --exec bash
/mnt/c/Users/mrkda/Projects/shark/out/04a-work/run-headless.sh`. It reuses the
recorded pinned userspaces with read-only source, private mounts and fresh build,
package and evidence paths. Inside, `python /source/tools/headless_ci.py
--build-root /work/build-04a --output /evidence --jobs 2` checks the headless build
and package. A separate network namespace runs the SDK-free smoke. Then
`python /source/out/04a-work/verify-client-linux.py` runs `linux-debug` and
`linux-asan-ubsan` in fresh `/work/build-04a-client` directories, including all
client-enabled CTests. The declared minimal WSL Python 3.12.3 profile is retained.

Final repository checks: `python tools/check_format.py`,
`python tools/validate_contract.py`, its `--self-test`,
`python tools/check_repository.py --self-test` and `git diff --check` all PASS.
These cover 50 C++ files, 29 negative contract cases, five skills and three
malformed-skill cases. Their logs and commands are hashed in the receipt. An
initial documentation check found the newly linked receipt before it was created;
the final check passes with the receipt present. That intermediate log is retained
under `out/04a-work/repository-checks-before-receipt`. Existing platform-specific
tooling fixtures retain declared internal skips; actual POSIX signal and
descriptor-failure process checks run in WSL. Input and Core families have no skips.

## Plan, limits and next task

The manifest adds the `client.raw-input` feature and its real CPU fixture. Its
current assignment stays 04 and full `REQ-04` stays NOT_RUN. Plans, contributor
guidance and the ignored local roadmap now name **04b** as the next checkpoint.
Historical 03a/03b/03c receipts and handoffs retain their original results.

04b must connect the existing input contract to owned native GLFW window
creation/teardown, callbacks, focus/close/resize/minimize, content/framebuffer sizes
and high-DPI behavior, with startup rollback and native Windows lifecycle smoke.
04c then integrates the existing Core schedulers, input across zero/multiple ticks
and offline pause/step before the full assignment gate can pass. No game-authoring
editor UI, gameplay protocol, production renderer or MMO capacity lands in 04a.
No GPU or hosted run was needed for these CPU-only changes.

There are no outstanding 04a blockers. `.gitignore` was reviewed and already covers
the roadmap, builds, caches and generated evidence; no new rule is needed. All new
source/tests/docs remain eligible for version control. No host package, driver,
WSL resource setting, hosted workflow or branch policy changed. Nothing was staged,
committed or pushed.

Suggested commit: `feat(input): add injectable client input state and focus handling`
