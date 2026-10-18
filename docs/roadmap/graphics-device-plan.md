# Assignment 05 checkpoints

Assignment 05 follows the completed
[04c window/frame-loop gate](../handoffs/04c-frame-loop.md) and builds production
graphics ownership on the proven assignment-02 stack. The requirement owner
remains `05`; these checkpoints do not add assignment IDs. Full `REQ-05` is
**NOT_RUN** until all three checkpoints pass. **05a is complete** under the
current-machine scope; the [handoff](../handoffs/05a-graphics-device.md) records
its passing gate. The exact next checkpoint is **05b**.

The accepted [platform/capability contract](../architecture/0003-platforms.md) and
[current-machine scope](../architecture/0010-current-machine-scope.md) apply to
every checkpoint: native Windows on this PC's RTX 4070 Laptop and Intel Raptor
Lake-S Mobile GPUs, with CPU/headless/package regressions in existing WSL Ubuntu.
Mac, native Linux graphical qualification, additional hardware, hosted runners
and independent-machine checks are outside the gate. Retained historical
assignment-02 evidence remains unchanged.

## 05a - Device foundation

Deliver engine-owned adapter metadata, explicit UUID and deterministic supported-
hardware selection, unchanged capability/format admission, owned Window/instance/
surface/device/NVRHI startup and reverse teardown, and bounded validation
diagnostics. Reuse the existing Window lease, error contracts and exact dependency
pins. No borrowed Vulkan/NVRHI handles escape public headers. Keep Core/Server and
device-free CPU builds outside the presentation dependency graph.

Use a pure admission/lifecycle fixture with injected backends for malformed
metadata/selectors, missing individual profile requirements, partial startup,
exceptions, ownership conflicts, thread checks, idempotent cleanup and callback
diagnostic bounds. Provide an explicit machine-readable native fixture for adapter
inventory and 16 repeated create/close lifetimes per local GPU, with required core,
synchronization and NVRHI validation and no warnings/errors/dropped diagnostics.
Record UUID/driver, source/lock/toolchain identity and retained bounded-run output.

Checkpoint gate: both local hardware devices pass native foundation checks;
Windows Debug/Release/ASan and WSL client/server CPU suites pass; Runtime-only and
Server-only boundaries pass; the installed Server runs in the existing SDK-free
WSL runtime. Device-free help/description/invalid-argument and evidence-parser
fixtures must remain noninteractive and cannot substitute for native execution.
The [device guide](../graphics-device.md) describes the actual 05a API and commands.

05a does not create a swapchain, submit commands, render an image or integrate
graphics into the existing empty Editor/Runtime window sample. It establishes no
FPS, resource-retirement, device-recovery or sustained-presentation claim.

## 05b - Presentation lifecycle

Extend the owned device/window infrastructure with swapchain creation/recreation,
command submission, bounded frames in flight, fences and safe retirement after
submitted use completes. Keep these changes together because submission,
presentation and resource ownership depend on one another. Use the existing
frame/timing policies rather than creating another scheduler.

Implement resize and zero-framebuffer/minimize/restore handling without busy
retries or submission to invalid extents. Define concrete bounded outcomes for
out-of-date/suboptimal surfaces, surface loss and fatal device loss as each path
lands. Keep diagnostics and process failure visible. Do not retain future use of
resources beyond proven completion, or hide failures behind automatic adapter
switches or endless recreation.

Checkpoint gate: deterministic lifecycle/failure fixtures and actual native
presentation/resize/minimize/restore on both local GPUs pass required validation,
including repeated teardown. Select and document the exact bounded stress workload
before running it. Existing CPU/server timing and dependency-boundary regressions
must pass; client focus/minimize cannot pause authoritative Server time. Basic
shader/resource rendering remains assignment 06 rather than a premature engine
feature in 05.

## 05c - Failure and stress qualification

Complete recover-or-exit qualification for device/surface failures, sustained
presentation, resize/minimize/restore loops, repeated creation/destruction and
safe resource retirement. Preserve failed runs, exact lifecycle counts/durations,
validation output, hardware/driver/toolchain inputs and measured resource behavior.
Both local devices must execute the same declared correctness workload; Intel is
not required to match RTX frame rate. Fault injection proves policy decisions,
while actual native runs independently prove the exercised hardware paths.

Checkpoint gate: no relevant validation errors, use-after-free or unbounded
resource growth within the recorded workload; unsupported capabilities are rejected
with named requirements; faults recover within the declared policy or exit with
bounded cleanup and a nonzero failure. Finish Windows/WSL regressions and protected
standalone/package checks. An unavailable in-scope device is NOT_RUN and leaves
the gate open; previous probe results cannot close new production lifecycle work.

Only after 05c passes may full `REQ-05` become PASS and assignment **06, shader
pipeline and basic GPU resources**, begin. EngineClient remains presentation-only
for the seamless MMORPG target. Terminal agents and humans receive the same owned
diagnostics and explicit device capabilities; authoring services, shared editor/CLI
transactions and world simulation remain with their planned owners.
