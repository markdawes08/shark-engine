---
name: shark-renderer-validation
description: Validate Shark graphics changes against Vulkan/NVRHI requirements, readback and visual tolerances, and both GPUs on the current Windows PC.
---

# Shark renderer validation

Apply the [testing policy](../../../docs/testing-policy.md) and read [AGENTS.md](../../../AGENTS.md)
plus the assigned increment. Select relevant graphics regressions on both local
GPUs when graphics behavior changes. CPU import/documentation changes do not
trigger GPU runs; profile breadth follows actual compiler/build impact. Run GPU
fixtures serially. Artifact integrity remains required when artifacts are rebuilt.

Read [space/color conventions](../../../docs/architecture/0005-simulation-and-rendering.md),
[the rendering profile](../../../docs/architecture/0006-v1-profile.md) and
[current-machine scope](../../../docs/architecture/0010-current-machine-scope.md).
Required devices are this Windows PC's RTX 4070 Laptop and Intel GPU. RTX is the
primary performance target; Intel is the correctness/fallback target without an
equal-FPS promise. Native Linux graphics, other machines and Mac are outside scope.
Existing WSL supports CPU/headless/package checks only.

## Choose the existing fixture

Use the [probe guide](../../../docs/graphics-probe.md) for draw/compute/readback and
lifecycle cases; [device](../../../docs/graphics-device.md),
[presentation](../../../docs/presentation-lifecycle.md) and
[GPU resources](../../../docs/gpu-resources.md) for production ownership changes;
and the [test supervisor](../../../docs/scene-execution.md) for explicit GPU-mode
scene execution. CPU Tools/SceneRunner retain their protected independence.

Use explicit device UUIDs and the locked validation-tools/shader inputs. Required
core, synchronization and NVRHI validation, independent raw-evidence checks and
fixed readback tolerances remain mandatory for qualifying runs. Validation-off
execution is diagnostic only. Synthetic/CPU-safe CLI checks qualify no GPU, and
incomplete GPU work cannot claim device-free initialization.

Preserve each selected fixture's workload and failure controls. Short lifecycle
runs do not close the full 100/20/600 gate. Close requests, partial soaks and missing
prerequisites are incomplete/NOT_RUN, not passing qualification. Historical retired
hardware rows remain NOT_RUN. Preserve unsuccessful attempts and their diagnostics.
Do not retry away failures, silently remove a target or replace NVRHI.

## Validate behavior

Keep controlled input/time, native validation and known readbacks. Test affected
resize, minimize/restore, submission, retirement and teardown paths alongside
images. A triangle/software renderer cannot replace required compute, depth, HDR,
texture-array or lifecycle behavior. Investigate differences before proposing
reviewed baseline changes; never automatically replace baselines or widen tolerances.

For rendering changes record named hardware/driver, lock and shader/compiler
identities, capabilities/formats, validation settings, commands, exits and readback
evidence. Apply the declared per-case tolerances. Add PBR/IBL/shadows/AO/tonemapping
cases only as those owners land; temporary unlit results do not prove PBR quality.

The [MMORPG plan](../../../docs/roadmap/mmorpg-plan.md) keeps production presentation
in EngineClient. Origin-shift, streaming, LOD and animated-crowd behavior belong to
their owning increments. Rendering/minimize must not advance or pause server time;
a static reference sphere cannot qualify world population or production capacity.

Package work follows the separate release-packaging skill and existing builders.
A relocated SDK-free description startup or CPU launcher test does not establish
native GPU correctness. Preserve Windows job containment versus POSIX process-group
limitations, actual package identity and readbacks. Use focused shared-code tests
when affected; document/contract validation alone proves no graphics behavior.
