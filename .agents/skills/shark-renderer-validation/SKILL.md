---
name: shark-renderer-validation
description: Validate Shark graphics changes against Vulkan/NVRHI requirements, readback and visual tolerances, and both GPUs on the current Windows PC.
---

# Shark renderer validation

Apply the [testing policy](../../../docs/testing-policy.md) before choosing checks.
Its 2026-10-27 cadence supersedes older recurring full-matrix instructions while
preserving required GPU behaviors, validation and image tolerances. Run relevant
graphics regressions on both local GPUs when graphics behavior changes; select
configuration breadth by actual compiler/build impact and assignment closure.
CPU-only import or documentation changes do not trigger GPU runs. Existing shader
and package artifact validation remains required when those artifacts are rebuilt.

10c's [test supervisor](../../../docs/scene-execution.md) connects the existing
native offscreen/window cube fixtures through explicit GPU mode. Preserve exact
UUID selection, required core/synchronization/NVRHI validation, fixed readback
tolerances and independent raw-evidence checks. The completed 10c matrix ran both
GPUs serially in Windows Debug/Release/ASan; future selections follow the testing
policy and keep GPU runs serial. CPU SceneRunner/Tools retain their protected
dependencies. Synthetic evidence tests qualify no GPU; incomplete GPU work has
unknown initialization rather than a false device-free result. Keep fixed shader
payload preparation explicit and preserve failed/unavailable attempts.

Read [AGENTS.md](../../../AGENTS.md), the assigned increment and
[platform evidence](../../../docs/architecture/0003-platforms.md),
[space/color conventions](../../../docs/architecture/0005-simulation-and-rendering.md)
and [v1 lighting profile](../../../docs/architecture/0006-v1-profile.md).
The user-directed [current-machine scope](../../../docs/architecture/0010-current-machine-scope.md)
selects this Windows PC's RTX 4070 Laptop and Intel Raptor Lake-S Mobile GPUs.
Existing WSL environments provide Linux CPU/headless/package checks; native Linux
GPU shipping and other physical hardware are outside the active plan.

The opt-in probe has executable 02b draw/compute/readback and lifecycle cases; see
the native presets and bounded JSON commands in the [probe guide](../../../docs/graphics-probe.md).
CPU-safe `shark.graphics-probe.cli` checks run only in graphics configurations.
Prepare the locked standalone validation tools and shader manifest, then use
`--mode cases`, an explicit device UUID, its validation receipt and a fresh output
directory. Core and synchronization validation are required for case PASS;
`--validation off` retains diagnostic execution only. Short lifecycle runs do not
close the 100/20/600-second gate. Both active local rows have passed that workload
in the retained 02d receipts; later graphics changes require relevant regression
evidence on these devices.
Run the native CPU build/CTest commands in [building](../../../docs/building.md)
for relevant shared code. Contract validation checks documentation only.
02d provides Release packages with a bounded PowerShell launcher on Windows and
a Python 3.12+ standard-library launcher on Ubuntu 24.04 x86-64, documented in the
probe guide. Use `graphics-windows-release` or `graphics-linux-release` and their
respective package builders. Linux uses the selected X11 backend, explicit
app-local loader selection and an unprivileged account; no native Wayland or
GPU claim follows from a successful ELF build or SDK-free description startup.
Preserve failed/incomplete soaks, partial lifecycle progress,
package identity and relocated raw readbacks. A close request is an incomplete
run, not a passing soak. The user removed Mac compatibility from scope under
[ADR 0009](../../../docs/architecture/0009-windows-linux-platform-scope.md);
02c is retired and no Mac hardware qualification is required.
Never treat an empty selection or unavailable prerequisite as passing coverage.
Linux package CPU tests exercise real deadline/output/cancellation cleanup with
synthetic children. They do not replace native GPU runs. ADR 0010 accepts Windows
dependency-isolated relocation and the SDK-free WSL runtime as local package
evidence; an independent physical clean target is no longer required. Retain
process-group limitations separately from Windows job containment; neither allows
failed evidence to be discarded.

Assignment 02 is complete under the narrowed local scope, using the same pinned
GLFW/NVRHI/Vulkan cases on both available GPUs. Preserve historical NOT_RUN
outcomes for retired hardware rows and raw aggregate qualification fields;
do not rewrite them as native passes. For future graphics changes capture
OS/CPU/GPU/driver, lock digest, compiler/shader versions, capabilities/formats,
validation settings and logs, commands, exit status and readback evidence. A bare
triangle or software renderer cannot replace required compute, depth, HDR,
texture-array, lifecycle and portability checks. A failing required path needs a
reproducer and explicit decision; never silently remove a target or swap NVRHI.

Use Vulkan and NVRHI validation, explicit timeouts, controlled input/time and
known readbacks. Apply the ADR's per-case tolerances and approved platform image
baselines. Investigate a difference before proposing a reviewed baseline change;
never automatically replace baselines, widen tolerances or retry away failures.
Test resize, minimize/restore, teardown and resource retirement as well as images.

As lighting lands, exercise PBR, IBL, shadows, AO and tonemapping separately and
combined in FeatureWorld and isolated fixtures. Preserve explicit color/depth/
winding conventions and measure cost on named hardware. Update coverage and the
handoff with PASS/FAIL/NOT RUN; an unavailable active local GPU leaves its check
open. Other machines are outside the current contract. RTX is the primary
performance target; Intel is the correctness/fallback target without an equal-FPS
promise. Hosted CI remains supplemental to local progression.

For the [seamless MMORPG profile](../../../docs/architecture/0007-mmorpg-architecture.md),
keep production presentation in EngineClient; M01's split already exists. The
02 probe has its own private presentation dependencies. Add origin-shift,
streaming, animated-crowd and LOD-cost fixtures in their owners; a static reference
sphere cannot qualify a populated world. Rendering/minimize must not advance or
pause server time. Existing depth/lighting choices require measured amendments if
large-world or crowd evidence shows a problem, not a speculative replacement.
