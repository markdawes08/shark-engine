# Assignment 02 checkpoints

Assignment 02 follows the completed [M01 hosted gate](../handoffs/M01-ci-verification.md).
Its single requirement owner remains `02`; checkpoints do not add numbered
assignments. [ADR 0010](../architecture/0010-current-machine-scope.md) narrows the
active gate to this Windows PC and its existing WSL environments;
[ADR 0003](../architecture/0003-platforms.md) retains the graphics capability
profile. [ADR 0009](../architecture/0009-windows-linux-platform-scope.md) records
the removal of Mac compatibility and retirement of 02c.

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
4. **02d - current-machine gate closure:** run every graphics case on this PC's
   RTX 4070 Laptop and Intel Raptor Lake-S Mobile GPUs, including validation and
   full 100/20/600 lifecycle/soak. Validate Windows package relocation with
   isolated dependencies and the existing SDK-free WSL CPU/package environment.
   Preserve commands, source/toolchain/driver identities, readbacks, licenses,
   results and outstanding failures in durable records.

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
retains its original evidence and input hashes. The user's current-machine
amendment explicitly makes those two available GPUs the active rows
`LOCAL-W-NV` and `LOCAL-W-INTEL`. Native Linux graphics, the six former hardware
rows and an independent physical clean machine are outside the active plan.
Their earlier NOT_RUN evidence remains unchanged. Dependency-isolated Windows
relocation and the SDK-free WSL runtime are the accepted local package checks;
they establish neither independent clean-machine execution nor broad compatibility.
**REQ-02 is PASS under the revised local scope.** Its aggregation uses the
existing receipts; native JSON still leaves overall qualification and P02-07/08
as NOT_RUN because package/failure evidence is assessed separately.
An unavailable device, driver, display, validation layer or native runner is
reported as unavailable with a nonzero result; it is never converted into PASS.
WSL builds cannot establish native Linux graphics. The earlier unexplained resize
timeout remains preserved; later successful full runs do not establish its cause.
The hosted CPU workflow at `39110d4` passes all nine jobs and 156 CTests;
that result is revision-specific. Existing hosted automation remains supplemental
and does not block local roadmap progression or imply a GitHub policy change.
**Assignment 03 is next and unstarted.** Dependent renderer architecture has not
started. RTX 4070 Laptop is the primary future performance target; Intel remains
a correctness/fallback target without an equal-frame-rate guarantee.

The [probe guide](../graphics-probe.md) documents the concrete developer commands.
These are shared contributor tools for humans and terminal agents, not public
game-authoring APIs. The probe remains outside EngineCore/Server and does not
introduce presentation dependencies into future CPU authoring services.
