# Assignment 02 checkpoints

Assignment 02 follows the completed [M01 hosted gate](../handoffs/M01-ci-verification.md).
Its single requirement owner remains `02`; checkpoints do not add numbered
assignments or weaken [ADR 0003](../architecture/0003-platforms.md).

1. **02a - dependency and probe setup:** lock GLFW, NVRHI and matching Vulkan
   headers/loader; add opt-in native build presets, a maintained terminal probe,
   device inventory and explicit-device startup/teardown diagnostics, initial
   offline shader-tool preparation, bounded JSON evidence and CPU failure tests.
   Verify that ordinary CPU and headless builds still exclude graphics discovery,
   downloads and links. Record Mac tool/driver prerequisites before claiming
   portability. This checkpoint does not qualify any complete GPU matrix row.
2. **02b - executable graphics cases:** implement textured/depth draws, compute,
   HDR/storage/sampled formats, texture cubes/depth arrays, shader validation and
   known readbacks using the tolerances and conventions in ADRs 0003/0005. Pin
   independent SPIR-V validation and Vulkan validation layers before use. Add
   bounded lifecycle stress, validation capture and actual failure-path fixtures.
3. **02c - native Mac feasibility and distribution:** build the locked probe
   natively on both CPU families. Pin the complete tested KosmicKrisp and
   MoltenVK input graphs, native shader-tool route and redistribution inputs;
   maintain any required source patches with reproducer and rationale. Test both
   Intel GPU rows. An incompatible stack requires an explicit decision supported
   by evidence before dependent renderer work.
4. **02d - full matrix and gate closure:** run every required case on all nine
   named GPU/OS rows, including validation, lifecycle/soak and clean-machine
   packaged-probe launch. Preserve commands, source/toolchain/driver identities,
   readbacks, licenses, results and outstanding failures in durable records.

**02a is complete within its setup scope**; see its
[handoff](../handoffs/02a-graphics-setup.md). **02b implementation and local verification are complete**;
the [02b handoff](../handoffs/02b-graphics-cases.md) records both supplemental GPU
passes and an earlier unresolved resize timeout. Next is **02c**, without advancing
the full assignment gate. 02d retains the full soak and any lifecycle recurrence investigation.
REQ-02 remains **NOT_RUN** until the full gate passes.
An unavailable device, driver, display, validation layer or native runner is
reported as unavailable with a nonzero result; it is never converted into PASS.
Supplemental hardware and WSL builds cannot replace the declared native rows.
Assignment 03 and dependent renderer architecture have not started.

The [probe guide](../graphics-probe.md) documents the concrete developer commands.
These are shared contributor tools for humans and terminal agents, not public
game-authoring APIs. The probe remains outside EngineCore/Server and does not
introduce presentation dependencies into future CPU authoring services.
