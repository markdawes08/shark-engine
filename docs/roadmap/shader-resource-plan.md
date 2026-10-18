# Assignment 06: Shader and GPU resource checkpoints

Date: 2026-10-18. Starting revision: `db4117f` on `main`.
Owner: `REQ-06`; **full assignment verification is PASS** under the current-machine scope.
The [06c handoff](../handoffs/06c-window-distribution.md) closes window and distribution qualification.
**06a is complete for offline tooling.** The
[handoff](../handoffs/06a-offline-shaders.md) records its scoped verification.
**06b is complete for owned resources and offscreen rendering.** See the
[resource contract](../gpu-resources.md) and [06b handoff](../handoffs/06b-gpu-resources.md)
for both-GPU and CPU verification. **06c and the subsequent assignment 07 are complete.**
The [network plan](network-plan.md) tracks the current assignment M02.
[M02b owned sessions](../handoffs/M02b-owned-sessions.md) is **PASS** under the
current-machine scope; M02c replication is next.

## Sequencing decision

On 2026-10-18, the user placed the remaining 05c work in the
[nonblocking backlog](../backlog.md) and authorized **06b and later development**.
This supersedes the earlier CPU-only 06a exception. Historical evidence and fixed
acceptance limits are preserved; original assignment IDs remain unchanged.
Each subsequent checkpoint must satisfy its own acceptance criteria.

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

Extend the existing EngineClient device,
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
after its complete acceptance criteria are met. FeatureWorld integration follows
in 10; these earlier isolated fixtures have
explicit coverage until then. No MMO world, material editor or authoring UI is
implied by this milestone.
