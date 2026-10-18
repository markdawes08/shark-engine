# Assignment 02 checkpoints

Assignment 02 follows the completed [M01 hosted gate](../handoffs/M01-ci-verification.md).
Its single requirement owner remains `02`; checkpoints do not add numbered
assignments. [ADR 0003](../architecture/0003-platforms.md) defines the active
Windows/Linux gate; [ADR 0009](../architecture/0009-windows-linux-platform-scope.md)
records the user's removal of Mac compatibility and retirement of 02c.

1. **02a - dependency and probe setup:** lock GLFW, NVRHI and matching Vulkan
   headers/loader; add opt-in native build presets, a maintained terminal probe,
   device inventory and explicit-device startup/teardown diagnostics, initial
   offline shader-tool preparation, bounded JSON evidence and CPU failure tests.
   Verify that ordinary CPU and headless builds still exclude graphics discovery,
   downloads and links. This checkpoint does not qualify any complete GPU matrix row.
2. **02b - executable graphics cases:** implement textured/depth draws, compute,
   HDR/storage/sampled formats, texture cubes/depth arrays, shader validation and
   known readbacks using the tolerances and conventions in ADRs 0003/0005. Pin
   independent SPIR-V validation and Vulkan validation layers before use. Add
   bounded lifecycle stress, validation capture and actual failure-path fixtures.
3. **02c - retired by platform scope decision:** the former native Mac feasibility
   and distribution checkpoint has no remaining implementation or verification
   requirement. Its ID is retained for historical references; it is not a PASS.
4. **02d - full matrix and gate closure:** run every required case on all six
   named GPU/OS rows, including validation, lifecycle/soak and clean-machine
   packaged-probe launch. Preserve commands, source/toolchain/driver identities,
   readbacks, licenses, results and outstanding failures in durable records.

**02a is complete within its setup scope**; see its
[handoff](../handoffs/02a-graphics-setup.md). **02b implementation and local verification are complete**;
the [02b handoff](../handoffs/02b-graphics-cases.md) records both supplemental GPU
passes and an earlier unresolved resize timeout. The later user-directed
[scope amendment](../handoffs/windows-linux-scope-amendment.md) supersedes the
earlier Mac deferral and retires **02c**. **02d** implements Release packages and
bounded standalone launchers for Windows and Linux. The
[final local handoff](../handoffs/02d-final-verification.md) records refreshed
Windows lifecycle/relocation evidence and isolated Ubuntu build, CPU and package
checks. Both available Windows GPUs pass the full 100/20/600 workload with required
validation; Ubuntu passes 21 CTests and SDK-free description startup. The
[earlier Windows handoff](../handoffs/02d-windows-validation.md)
retains its original evidence and input hashes. Only the current Windows PC is
available; Ubuntu userspace under WSL does not qualify native Linux graphics.
All six named Windows/Linux GPU rows remain NOT_RUN, and a real clean-machine
package launch remains required. The supplemental Windows results do not
advance the full assignment gate.
REQ-02 remains **NOT_RUN** until the full gate passes.
An unavailable device, driver, display, validation layer or native runner is
reported as unavailable with a nonzero result; it is never converted into PASS.
Supplemental hardware and WSL builds cannot replace the declared native rows.
The hosted CPU workflow at `39110d4` passes all nine jobs and 156 CTests;
the later local Linux packaging/link changes still need a user push and hosted run.
Assignment 03 and dependent renderer architecture have not started.

The [probe guide](../graphics-probe.md) documents the concrete developer commands.
These are shared contributor tools for humans and terminal agents, not public
game-authoring APIs. The probe remains outside EngineCore/Server and does not
introduce presentation dependencies into future CPU authoring services.
