# Assignment 06: Shader and GPU resource checkpoints

Date: 2026-10-18. Starting revision: `db4117f` on `main`.
Owner: `REQ-06`; full assignment verification remains NOT_RUN.
**06a is complete for offline tooling.** The
[handoff](../handoffs/06a-offline-shaders.md) records its scoped verification.

## Sequencing decision

The user's instruction to proceed with the next subchunk starts **06a, offline
shader tooling**, while the [05c qualification gate](../presentation-qualification.md)
remains open. This is a limited change to execution order: CPU shader preparation
does not depend on swapchain stress or native process-exit qualification. It does
not pass assignment 05, remove either GPU, change resource budgets, or qualify a
release. The [latest investigation](../handoffs/05c-intel-asan-investigation.md)
and all original failed receipts remain intact.

The NVIDIA desktop-DC retention, attributed Intel hook retention and unexplained
original Intel ASan exit timeout remain outstanding. **06b GPU implementation
still requires 05c closure or a subsequent explicit sequencing decision.** Neither
offline compilation nor a future cube image can close the native lifetime gate.
Original assignment IDs and the final acceptance criteria remain unchanged.

## 06a - Offline shader inputs and artifacts

Provide a noninteractive, GPU-free developer command for a bounded shader recipe,
using the existing locked DXC and SPIRV-Tools inputs. Build from cached tools only;
tool preparation stays an explicit separate operation. Define versioned shader,
binding and matrix conventions; track source/include content and tool identities;
compile and independently validate SPIR-V; publish a complete artifact generation
atomically. Reuse verified output for unchanged inputs and preserve the previous
generation if a build fails. Distribution verification must not need sources,
DXC, validation layers, a Vulkan device, Editor or EngineTools.

Gate: exercise real offline compilation, deterministic repeated output,
dependency edits, missing includes, malformed HLSL, invalid recipes and corrupt
artifacts. Exercise bounded process failures with CPU fixtures. Verify that normal
Runtime/Core/Server builds do not discover or invoke a shader compiler. Record
Windows evidence and device-free WSL tooling regression separately. Define all
unsupported input forms explicitly; this is a shader builder, not the asset cooker
or public game-authoring interface. See the [contract](../shader-pipeline.md) and
[handoff](../handoffs/06a-offline-shaders.md).

## 06b - Owned resources and offscreen rendering

After the sequencing prerequisite above, extend the existing EngineClient device,
submission and retirement owners with validated shader artifact loading, typed
buffer/texture uploads, binding layouts and graphics pipelines. Do not duplicate
the device, scheduler or queue-waiting implementation. Keep NVRHI/Vulkan private;
Core/Server continue to have no shader or GPU dependencies.

Gate: render a textured, depth-tested cube to an offscreen target on both current
Windows GPUs, with required validation and known-pixel tolerances fixed before
recording results. Test missing/corrupt artifacts, shader stage and layout mismatch,
upload bounds, partial creation failures and retirement after completion. Use
column-major matrices and the existing winding, depth, texture-origin and color
conventions in [ADR 0005](../architecture/0005-simulation-and-rendering.md).

## 06c - Window integration and complete qualification

Present the same cube through the existing offline frame/presentation loop and
compare the offscreen/readback result. Keep compilation out of the Runtime and
package precompiled artifacts with a complete verified dependency closure.
Integrate reproducible build inputs into the development workflow without making
distribution or headless configuration depend on shader tools.

Gate: both-GPU window/offscreen readbacks, resize/minimize/restore and resource
retirement, malformed shader/layout/artifact failures, dependency rebuild checks,
and relocated Runtime packages with no source shader compiler. Run the relevant
Windows and WSL CPU/sanitizer/boundary regression. Full `REQ-06` becomes PASS only
after its complete acceptance criteria and the outstanding 05c prerequisite are
met. FeatureWorld integration follows in 10; these earlier isolated fixtures have
explicit coverage until then. No MMO world, material editor or authoring UI is
implied by this milestone.
