---
name: shark-renderer-validation
description: Validate Shark graphics changes against Vulkan/NVRHI requirements, readback and visual tolerances, and the separate Windows/Linux/Mac hardware gates.
---

# Shark renderer validation

Read [AGENTS.md](../../../AGENTS.md), the assigned increment and
[platform evidence](../../../docs/architecture/0003-platforms.md),
[space/color conventions](../../../docs/architecture/0005-simulation-and-rendering.md)
and [v1 lighting profile](../../../docs/architecture/0006-v1-profile.md).

At assignment 01 there is no renderer, graphics probe or GPU test command.
`python tools/validate_contract.py` checks contracts only. Run the native CPU
build/CTest commands in [building](../../../docs/building.md) for relevant shared
code, but record graphics validation as NOT RUN until its owner implements it.
Add actual runnable graphics commands and fixture IDs when those layers land;
do not invent test labels or treat an empty selection as coverage.

Assignment 02 must qualify pinned GLFW/NVRHI/Vulkan on the exact real-hardware
matrix, treating Apple Silicon and both Intel-Mac GPU rows independently. Capture
OS/CPU/GPU/driver, lock digest, compiler/shader versions, capabilities/formats,
validation settings and logs, commands, exit status and readback evidence. A bare
triangle or software renderer cannot replace required compute, depth, HDR,
texture-array, lifecycle and portability checks. A failing Mac path needs a
reproducer and explicit decision; never silently remove the target or swap NVRHI.

Use Vulkan and NVRHI validation, explicit timeouts, controlled input/time and
known readbacks. Apply the ADR's per-case tolerances and approved platform image
baselines. Investigate a difference before proposing a reviewed baseline change;
never automatically replace baselines, widen tolerances or retry away failures.
Test resize, minimize/restore, teardown and resource retirement as well as images.

As lighting lands, exercise PBR, IBL, shadows, AO and tonemapping separately and
combined in FeatureWorld and isolated fixtures. Preserve explicit color/depth/
winding conventions and measure cost on named hardware. Update coverage and the
handoff with PASS/FAIL/NOT RUN; absent hardware remains an open gate.

For the [seamless MMORPG profile](../../../docs/architecture/0007-mmorpg-architecture.md),
keep presentation in EngineClient once M01 introduces the split. Add origin-shift,
streaming, animated-crowd and LOD-cost fixtures in their owners; a static reference
sphere cannot qualify a populated world. Rendering/minimize must not advance or
pause server time. Existing depth/lighting choices require measured amendments if
large-world or crowd evidence shows a problem, not a speculative replacement.
