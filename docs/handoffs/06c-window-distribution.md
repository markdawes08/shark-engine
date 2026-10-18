# 06c handoff: Window cube and shader distribution

Date: 2026-10-18. Base: `2a349c6` on initially clean `main`.
Result: uncommitted worktree changes; the user owns staging, commits and pushes.
Status: **06c and full REQ-06 PASS under the current-machine scope.** The
[durable receipt](../coverage/evidence/06c-window-distribution.json) records
commands, hashes, observed hardware, outcomes and retained failures.

## Delivered behavior

Runtime and Editor expose `--cube`, optional `--shader-bundle PATH`, bounded
`--graphics-ms`, explicit device selection and validation through the existing
offline frame/input loop. F6/F7 retain their offline pause/step behavior; the
static cube does not implement world simulation or an authoring editor.
`--graphics` remains the clear presentation sample.

The same typed shaders, uploads, bindings and depth-tested pipeline now draw into
the actual window attachment. Per-image resources survive frame reuse and retire
with their swapchain generation. Optional color/depth capture waits on the existing
queue timeline. Window RGB is sRGB encoded once, alpha remains linear, and BGRA
surfaces are normalized to RGBA. No second device or scheduler was introduced.
The [resource contract](../gpu-resources.md) defines installation, capture and
failure ownership; public types remain engine-owned and client-only.

The [shader workflow](../shader-pipeline.md) adds explicit `prepare` and `export`
commands. A bounded, versioned `.sharkshader` bundle carries both precompiled
stages and verifies payload SHA-256 plus typed interfaces before device creation.
The default path is `assets/cube.sharkshader` beside the executable, independent
of the working directory. Hashes provide integrity, not producer authentication.
Normal builds and distributed Runtime do not discover or execute a shader compiler.

Runtime packaging verifies the offline generation, includes the exact bundle,
records provenance and rejects incomplete/corrupt closure. The isolated launcher
also preserves UTF-8 diagnostics. CMake registers device-free bundle, launch,
readback-oracle and fault-evidence tests; GPU fixtures remain explicit opt-in tools.
`.gitignore` retains generated bundles, captures, packages and receipts under
`build/`, `out/` or `dist/`, while source recipes and tests remain eligible.

## Verification

All **261 CTests passed without skips**: Windows MSVC Debug, Release and ASan each
55; retained WSL Ubuntu GCC Debug and Clang ASan+UBSan each 48. Protected Windows
Runtime-only and Server-only builds passed. WSL native window/graphics options
remain OFF. Core/Server contain no shader or GPU dependency; Runtime excludes
authoring libraries and offline compilers.

Both RTX 4070 Laptop (driver 610.62) and Intel Raptor Lake-S Mobile (101.6790)
passed all native checks in Debug, Release and ASan, with required Vulkan core,
synchronization and NVRHI validation and no unexpected diagnostics:

- Six window suites: three fixed sizes, nine frames, actual window and independent
  offscreen color/depth captures, resize, minimize/restore and complete retirement.
- Six existing offscreen suites, including alternate texture/triangle order.
- Eighteen window fault children and eighteen offscreen fault children, proving
  partial cleanup, retained submitted owners and expected fatal recording behavior.
- Six clear-presentation regression suites: 2,304 frames across 24 device lifetimes,
  with the existing resize/minimize/restore workload.

The fixed readback limits remain one byte per color channel and 0.00001 depth;
no baseline was regenerated or tolerance widened. Actual PNGs were inspected.
The native window fixture uses 192x128, 256x192 and 192x192 after explicitly
normalizing initial DPI scaling. Windows' decorated minimum width prevented the
original smaller sizes; their failed setup receipts remain retained.

Real locked offline tooling passed 11 checks across 17 commands: reproducible
compilation, dependency changes, malformed source/recipes, failed publication,
corrupt/missing artifacts and relocated source/tool-free verification. Repeated
`prepare` returned CACHED and exported the same 1,932-byte bundle. Runtime and
Editor production `--cube` launches passed on both GPUs with required validation.

The relocated Release Runtime package passed on both GPUs from a Unicode/space
path outside the checkout, with system-only PATH, cleared development overrides,
verified app-local DLL closure and no compiler/source payload. Missing/corrupt
bundles failed before GPU creation with correct Unicode error paths. Package
launches use validation off and are deployment evidence; separate native fixtures
provide validated image correctness. Development tools remain installed on this
PC, so this is dependency isolation, not a separate clean physical machine.

Retained development failures also include malformed UTF-8 escaping the original
MSVC argument exception handler (fixed), a fault executable's stale configure-time
identity (rebuilt), an initial package publication rename denied by Windows
(fresh attempt succeeded without code/host changes), and redirected Unicode
decoding in the verification helper (fixed and rechecked). Historical receipts
are preserved rather than overwritten as passing evidence.

Final structural checks pass: pinned formatting for 103 C/C++ files, contract
validation with 29 negative checks, repository/ignore validation with three
negative skill cases, document links and `git diff --check`. These checks are
separate from executed CPU/GPU qualification; their local logs are under
`out/06c-work/final-checks/` (formatting was reported directly by the build wrapper).

## Reproduce and continue

Use the [build commands](../building.md#cube-sample-and-runtime-package),
[window/readback commands](../gpu-resources.md#window-cube-and-capture) and receipt
for exact qualification invocations. Run Windows presets through
`tools/windows_dev.ps1`, including ASan native launches. Standard-user native
validation uses `tools/windows_unelevated.py` and the locked validation receipt.
The retained WSL entry point is `out/06c-work/run-wsl-cpu.sh`; its commands and
source identities are recorded in the durable receipt.

To view the prepared sample from this workspace:

```powershell
.\build\windows-debug\bin\Editor.exe --cube
```

Next: **assignment 07, ECS, component schema and transform hierarchy**. FeatureWorld
integration follows in 10. Scene editing, public authoring commands, networking,
persistence and MMO capacity remain future work; this gate establishes only the
shader/resource and window/distribution foundation on the available machine.

Suggested commit: `feat(graphics): present textured cube and package verified shaders`
