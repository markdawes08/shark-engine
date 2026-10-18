# 06b handoff: Owned resources and offscreen rendering

Date: 2026-10-18. Base: `22c5717` on initially clean `main`.
Result: uncommitted worktree changes; the user owns staging, commits and pushes.
Status: **06b PASS under the current-machine scope.** Full `REQ-06` remains
NOT_RUN; 06c owns window integration and complete runtime distribution
qualification. Durable commands, identities, outcomes and retained failures are
in the [06b receipt](../coverage/evidence/06b-gpu-resources.json).

## Delivered behavior

The [resource contract](../gpu-resources.md) defines owned precompiled shaders,
typed vertex/index/constant/texture uploads, a fixed textured pipeline and
color/depth readback. NVRHI/Vulkan stay private in `EngineClient`; Core and Server
remain device-free. No dependency pins or host settings changed.

Shader input is bounded to 4 MiB and 65,536 IDs. The loader checks framing and the
supported typed interface, stage, descriptor slots and matrix layout before
native creation. It rejects missing/corrupt files, malformed Unicode/NUL paths,
oversized diagnostics and invalid upload extents/indices/nonfinite data. This is
interface validation of independently validated offline artifacts, not a full
SPIR-V validator or hostile-code sandbox. Runtime does not invoke a compiler.

`GraphicsDevice::render_offscreen` extends the existing device, queue and timeline
waiter. It reads staging data only after observed completion and retires its
17 top-level NVRHI handles. Partial creation unwinds; incomplete submission
retains owners for explicit drain. Close/destruction accounts for offscreen work
even without swapchain creation. Unresolved recording retains owners and follows
the existing fatal teardown policy. There is no second scheduler or device.

The terminal fixtures render a rotated, textured cube at 96×80 and 128×128,
including a hidden farther cube to expose incorrect depth behavior. The second
draw reverses triangle order and changes the texture. An independent CPU
rasterizer checks fixed geometry, color, depth, coverage and occlusion. Generated
PNGs were visually inspected; representative actual captures are retained at
[96×80](../coverage/evidence/06b-cube-96x80.png) and
[128×128](../coverage/evidence/06b-cube-128x128.png).

## Verification

Windows 11 x64 `10.0.26200`, MSVC 19.51.36248.0 / VC tools 14.51.36231,
SDK 10.0.28000.0, Python 3.13.5, CMake 3.31.10 and Ninja 1.13.2.
The retained WSL environment uses Ubuntu 24.04.5, Python 3.12.3, GCC 13.3 and
Clang 18; native graphics/window options remain OFF.

| Check | Result |
| --- | --- |
| Windows Debug / Release / ASan complete CPU CTest | **50/50 each PASS**, no skipped/disabled tests, final source inputs unchanged. |
| WSL GCC Debug / Clang ASan+UBSan complete CPU CTest | **45/45 each PASS**, no skipped/disabled tests, source bind read-only and final inputs unchanged. |
| Resource and ownership CPU tests | 9 resource cases / 178 Windows assertions; 5 owner cases / 51 assertions. |
| Independent image/protocol tooling | 24 oracle/evidence tests and 4 fault-protocol tests, including 55 negative fault-record mutations; included in CTest above. |
| Offscreen readback | Both GPUs in all three Windows configurations: **12 draw/readbacks PASS**, zero color error and maximum depth error **9.47e-8**, within unchanged 1-byte / 1e-5 tolerances. Each image checks 6,497 or 14,452 pixels and 322 or 818 occluded samples. |
| Native resource faults | **18 children PASS** across both GPUs/configurations: partial creation, retained submission and fatal recording ownership, with expected nonzero child exits and verified cleanup. |
| Existing presentation regression | Both GPUs/configurations: **2,304 frames**, 96 resizes, 48 minimize/restore pairs, 24 device lifetimes; validation and retirement checks pass. |
| Runtime-only / Server-only | Configure, build, ALL dependency-boundary checks and `--version` **PASS**. No authoring/compiler dependency enters Runtime; Server remains Core-only. |

GPU identities: NVIDIA RTX 4070 Laptop, UUID
`863d6f59a51894ab8b4829aa9460c78d`, driver 610.62; Intel Raptor Lake-S Mobile,
UUID `868088a7040000000002000000000000`, driver 101.6790. Required Vulkan core,
synchronization and NVRHI validation reported no unexpected diagnostics.

The first image comparisons exposed a missing framebuffer subpixel rounding
step in the CPU reference: colors matched, but ideal unquantized depth differed
by up to 2.14e-5. The native queried precision is 8 bits on both devices. Modeling
nearest 1/256-pixel X/Y rounding before interpolation corrects the reference;
analytic CPU regression covers that step. Geometry, tolerances and acceptance
budgets were unchanged. The initial failed receipts and diagnostic comparison
remain retained, with hashes in the durable receipt.

An initial ASan launch outside the MSVC environment failed before main with
`0xC0000135` (runtime DLL unavailable). Launching through `tools/windows_dev.ps1`
provided the installed developer runtime; the unchanged binaries passed all
native checks. Initial compile/parser development failures and an earlier CPU
run invalidated by an in-flight source edit also remain retained separately.

Final repository checks pass: pinned formatting for 93 C/C++ files, contract
validation and 29 negative checks, repository/ignore validation and three negative
skill cases, document links and `git diff --check`. These structural checks are
separate from the executed CPU and GPU evidence above.

## Reproduce and continue

Use the [resource guide commands](../gpu-resources.md) for shader generation and
explicit readback. Build/test with matching `windows-debug`, `windows-release`
and `windows-asan` presets through `tools/windows_dev.ps1`; use that environment
for ASan native launches as well. Ordinary CTest remains GPU-free. Exact bounded
native commands and local WSL invocation are retained in the receipt and
`out/06b-work/`.

The new public surface is a bounded diagnostic draw, not a scene/material system,
game-authoring command or editor UI. Next: **06c**, presenting the same resources
through the existing frame loop and qualifying relocated precompiled Runtime
artifacts. FeatureWorld integration follows in assignment 10.

Suggested commit: `feat(graphics): add owned resources and validated offscreen rendering`
