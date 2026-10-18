# Presentation failure and stress qualification

Assignment 05c closes the [graphics-device checkpoint plan](roadmap/graphics-device-plan.md).
This document declares the workload and acceptance limits before execution.
The current checkpoint result is **FAIL; qualification remains open**. The
[05c handoff](handoffs/05c-presentation-qualification.md) records observed resource
growth and an ASan child exit timeout; full `REQ-05` remains **NOT_RUN**. The
[follow-up investigation](handoffs/05c-failure-investigation.md) isolates a
desktop DC retained by NVIDIA 610.62. The
[latest investigation](handoffs/05c-intel-asan-investigation.md) reproduces Intel
USER growth outside Shark and fixes a separate Windows queue-thread exit
deadline defect. The original ASan fault-run timeout remains unexplained after
64 capture-ready diagnostics. These investigations do not replace the workload
or acceptance limits below. The
[presentation lifecycle contract](presentation-lifecycle.md)
defines the production behavior; the [05b handoff](handoffs/05b-presentation-lifecycle.md)
preserves the earlier short-run evidence.

## Fixed native workload

Run separately on this PC's RTX 4070 Laptop and Intel Raptor Lake-S Mobile GPUs.
Select each exact UUID, require the pinned Vulkan core/synchronization and NVRHI
validation, and retain every failure and partial trace. Use Windows Debug,
Release and AddressSanitizer builds. Both GPUs execute the same workload for each
configuration; this is a correctness gate without an equal-frame-rate requirement.

| Phase | Debug / ASan | Release |
| --- | --- | --- |
| Initial active-owner warmup | 24 frames, one minimize/restore, eight frames | 24 frames, one minimize/restore, eight frames |
| Sustained owner, measured after warmup | 60 seconds | 600 seconds |
| Resizes spread through that interval | 100 | 100 |
| Minimize/restore, after every fifth resize | 20 | 20 |
| Subsequent complete device lifetimes | 20, with 16 presentations each | 20, with 16 presentations each |

Normalize initial content to 640x400 before creating the first swapchain; that
setup resize is not a measured transition. Cycle explicit content sizes through
700x420, 530x350, 800x450 and 640x400. Observe each requested size, present after
each resize and prove the prior generation retired. Minimize must suspend new
submission; restore must resume presentation at a valid extent. An unexpected
close, failed observation, exhausted bounded retry, missing transition or shortened
duration fails the run. Record monotonically increasing submission/completion and
presentation counters, at most the selected one/two frame limit, and continued
progress between resource samples. Do not convert a user-interrupted run to PASS.

Sample resources after the initial warmup, after drained resize/restore points,
at five-second intervals, at the final drained sustained state, and after each complete owner closes. Record process
private bytes and handle counts, along with a private read-only snapshot of live
engine-owned native resources. Do not expose native handles in a public API.

The initial active warmup sample is the active-owner baseline. The fifth closed
short lifetime is the repeated-lifetime baseline; the remaining 15 must stay
within the same fixed allowances:

| Measured resource | Maximum increase above its baseline |
| --- | --- |
| Process handles | 8 |
| GDI objects / USER objects | 0 each |
| Process private bytes, Debug / Release | 64 MiB |
| Process private bytes, ASan | 256 MiB |

The fixed warmup was amended before final qualification after a retained
nonqualifying diagnostic run: the first restore loaded `CoreUIComponents.dll`
before rendering resumed and added 21 process handles. Over the remaining 19
restores and 95 resizes, counts stayed within three handles of that initialized
state. The original 32-frame baseline produced 127 budget failures, all retained.
One explicit warmup minimize/restore now exercises this lazy Windows path before
the baseline; total warmup presentation remains 32 frames. The measured workload
still includes all 100 resizes and 20 minimize/restore pairs, and the eight-handle
and memory allowances are unchanged. The baseline is never reset during a run.
This amendment does not resolve or excuse other resource growth.
The investigation also exposed per-lifetime GDI growth that process kernel-handle
counts did not capture. Normal qualification now explicitly records GDI and USER
counts and rejects growth beyond either fixed initialized baseline. This adds a
check; it does not replace or relax the original process/native-resource limits.
These are whole-process GUI counts. In particular, USER objects can vary during
Win32/IME/animation cleanup after GPU work completes; a failed zero-growth check
alone does not prove a USER-object leak. Retain the trace and investigate UI
quiescence instead of treating GPU drain as proof that all Windows UI work ended.

The larger ASan allowance accounts for instrumentation and allocator quarantine;
it does not suppress sanitizer findings. Report sampled high-water marks and final deltas, not only
the last sample. Retain raw values to expose trends. A failed limit requires an
investigation; no automatic baseline replacement or widened allowance is allowed.
Engine-owned resources have exact structural limits: one swapchain, two command
slots, at most eight swapchain images and corresponding image views/wrappers,
with synchronization objects bounded by that strategy. Every owned count must
be zero after close. Process measurements include driver/validation allocations;
they are not a complete device-local VRAM accounting or a production MMO budget.

Each native process has an external process-tree deadline, longer than its
declared workload but finite. Ordinary engine waits retain their existing
deadlines. Zero warnings, errors and dropped diagnostics are required through
teardown; validation-off runs cannot qualify.

The native deadline is 780 seconds for Release and 240 seconds for Debug/ASan.
Window observations have a two-second deadline; each frame has at most 16 bounded
attempts. Periodic samples require progress, a five-to-ten-second observation gap,
and a final sustained duration between the declared duration and ten seconds later.
Record the last observed window state on a failed transition. These deadlines
bound failure; they do not grant permission to skip transitions.

## Failure and capability policy

Use deterministic backend/process fixtures for surface/device errors, unavailable
capabilities, timeout retention, repeated calls after a fault and safe destruction.
Exercise actual production fail-fast destruction in a child process with an
external deadline and an explicit nonzero exit. Verify that the primary diagnostic
is retained and unsafe owners are not destroyed. Existing pure capability tests
must identify each missing requirement by name.

The native recording fixture injects a failure immediately after the real pinned
NVRHI command list opens. This deliberately reaches the unresolved recording
ownership state identified in 05b. It must retain live owners, report the error
and exit through the production destructor policy. Test hooks are private,
explicitly constructed by test executables, and absent from Runtime-only builds;
no environment flag or public gameplay API activates them.

Named surface/device-error injections into a real backend must still use real
healthy-driver completion for cleanup. Never pretend the hardware is lost to
justify destroying pending work. These cases prove error propagation and policy,
not recovery from physical GPU removal, a driver reset or an actual device fault.
No test changes drivers, host settings or WSL resource settings.

## Terminal commands

Configure and build the desired Windows preset using the developer wrapper in
[building](building.md). The fixture's profile is compiled into its binary; a
Debug/Release executable cannot select ASan's resource allowance. Help,
description and invalid-argument checks access no device. Use a fresh output
directory for every actual run, the explicit UUID and the locked validation-tool
receipt prepared by [the graphics probe guide](graphics-probe.md).

```powershell
& ./tools/windows_dev.ps1 -CommandArguments @('python','tools/windows_unelevated.py','--timeout','270','--evidence','out/presentation/stress-debug-nv-token.json','--','python','tools/presentation_stress.py','--executable','build/windows-debug/bin/PresentationStress.exe','--build-dir','build/windows-debug','--mode','stress','--profile','debug','--device','863d6f59a51894ab8b4829aa9460c78d','--validation-manifest','out/05a-work/validation-tools/summary.json','--output','out/presentation/stress-debug-nv')
& ./tools/windows_dev.ps1 -CommandArguments @('python','tools/windows_unelevated.py','--timeout','150','--evidence','out/presentation/faults-debug-nv-token.json','--','python','tools/presentation_faults.py','--executable','build/windows-debug/bin/PresentationFaults.exe','--build-dir','build/windows-debug','--mode','native','--device','863d6f59a51894ab8b4829aa9460c78d','--validation-manifest','out/05a-work/validation-tools/summary.json','--output','out/presentation/faults-debug-nv')
```

Repeat with Intel UUID `868088a7040000000002000000000000` and distinct outputs,
then the Release and ASan presets/profiles. Release needs the 810-second outer
standard-user deadline. Every native child has its own bounded process-tree
runner. Expected native recording-fault exits are 86; safe surface/device-error
exits are 1. The fault wrapper reports PASS only after checking the specific
expected error, primary diagnostic, owner-retention or zero-resource cleanup,
and zero actual validation diagnostics. An arbitrary nonzero exit cannot pass.

## Final regression and package gate

Run the Windows Debug/Release/ASan and existing WSL client/headless CPU suites,
the short native device/presentation/window regressions, Runtime-only and
Server-only boundaries, and the isolated SDK-free WSL Server package launch.
Qualify a relocated Release Runtime package on both local GPUs with inspected
DLL closure, redistribution notices and a restricted development environment.
Its clear-window launch is a dependency/relocation check; required-validation
native stress remains separate. It is not a game exporter or an assignment-35 release.

Build the production-only Release preset before packaging. The builder rejects
stale compilation/link inputs, authoring/test-enabled configurations and unexpected
PE imports. It packages `Runtime.exe`, the pinned Vulkan loader, the three locked
Release CRT DLLs, their dependency/license metadata and notices. It publishes into
a fresh directory and records the package manifest hash.

```powershell
& ./tools/windows_dev.ps1 -CommandArguments @('cmake','--preset','runtime-only')
& ./tools/windows_dev.ps1 -CommandArguments @('cmake','--build','--preset','runtime-only')
& ./tools/windows_dev.ps1 -CommandArguments @('python','tools/package_runtime.py','build','--build-dir','build/runtime-only','--crt-dir','C:/Program Files/Microsoft Visual Studio/18/Community/VC/Redist/MSVC/14.51.36231/x64/Microsoft.VC145.CRT','--output','out/runtime-package/manual')
```

Copy that package to a fresh directory outside the checkout with spaces and
Unicode in its path. Run `tools/package_runtime.py check --package <relocation>
--manifest-sha256 <builder-reported-hash> --device <UUID> --device <other-UUID>
--output <fresh-evidence-directory>` through `tools/windows_unelevated.py` in the
same manner as the native fixtures above. The checker inspects the exact
inventory and loaded modules, launches from the relocation with a system-only
PATH and cleared development overrides, and requires actual presentations on
both devices. Python, the compiler and inspection tools are verifier dependencies;
they are absent from the Runtime payload. Development tools remain installed on
this PC; this is the accepted local dependency-isolation test under ADR 0010.

Record source, lock, compiler, binary, tool and device/driver identity, exact
commands and outcomes in the final handoff. Full `REQ-05` remains NOT_RUN until
all in-scope gates pass. Only then may assignment 06 begin; shader pipelines,
scene rendering, authoring tools and MMO world behavior remain outside 05c.
