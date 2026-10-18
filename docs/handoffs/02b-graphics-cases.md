# 02b executable graphics cases and validation

Date: 2026-10-18 UTC (2026-10-17 America/New_York). Base: clean `main` at
`9049de6` (`feat(graphics): add locked dependencies and terminal probe setup`).
Result: uncommitted worktree updates; staging, commits and pushes remain with the user.
02b implementation and local verification are complete within checkpoint scope.
Full assignment 02 remains NOT_RUN, including the lifecycle limitation below.

## Delivered behavior

The separate opt-in probe now executes analytic NVRHI draws/depth, exact integer
compute, RGBA16F storage-to-graphics, mandatory format blending/filtering, cube
faces/mips and twelve rendered/sampled 2048x2048 D32 layers. Eleven raw readback
artifacts retain bytes, dimensions and hashes. Fourteen comparisons use 21,088
values, fixed ADR0003 tolerances and explicit references/edge exclusions; no
image baseline or tolerance was regenerated to hide a mismatch.

The pinned NVRHI viewport adapter reverses Y bounds to cancel its internal flip,
yielding a positive native Vulkan viewport; HLSL negates projection Y once.
Culling, depth, UV orientation and sRGB decoding are measured, not assumed.
Output attachment encoding is configured but not numerically read back; assignment
25 retains the output-transfer verification.
Textures use valid restored resource states and submitted resources retire before
release. No production renderer or public engine/game-authoring API is introduced.

Schema 2 adds executable case evidence to CPU-safe discovery, inventory and smoke.
The bounded runner checks compiled source/lock hashes, exact device UUID, pinned
shader/tool manifests, named comparisons, artifact layouts and lifecycle counts.
Core and synchronization Vulkan validation plus NVRHI validation must be clean for
case PASS. Diagnostic validation-off runs retain execution results while keeping
case qualification NOT_RUN. Missing prerequisites are unavailable/nonzero; no
fallback GPU, SDK, shader compiler or automatic success retry is selected.

Offline DXC compiles every shader twice, requires byte identity and uses pinned
independent SPIR-V validation. Invalid HLSL and corrupt SPIR-V must fail normally.
A separate development CMake project builds Vulkan-ValidationLayers1.4.352,
SPIRV-Tools and their complete locked header/utility graph. No tool or validation
library enters EngineCore/Server/Runtime or the ordinary engine dependency graph.
All source bytes are verified, with only two explicitly recorded VVL-generated
settings files accepting exact platform newline normalization. Upstream version
metadata is forced to the locked SPIRV-Tools revision rather than the enclosing
Shark Git revision. Licenses and native DLL/tool identities are retained.

The visible lifecycle window exercises actual resize/minimize/restore and optional
bounded presentation/device recreation. Presentation is a serialized Vulkan
attachment clear; draw/compute paths use NVRHI. Host acquire/submission fences and
queue/device waits keep the fixture simple, with no recycled present semaphores.
Full P02-05 still requires 100 resize/minimize/restore cycles, 20 device cycles,
an observed zero framebuffer extent and 600 presentation seconds. Production
frames in flight, robust cross-platform presentation maintenance and packaged
release behavior remain with their later owners.

Explicit missing-feature, missing-shader and wrong-readback fixtures fail with
structured diagnostics. Unknown/absent UUID, unavailable layer, corrupt tool or
shader inputs and a real process deadline are separate failure paths. Windows
CRT assertions are directed to stderr rather than interactive error dialogs.

## Verification and limits

The [durable local receipt](../coverage/evidence/02b-local.json) retains commands,
input/tool/binary hashes, comparison results and evidence identities. Raw logs and
readbacks remain under ignored `out/` directories.

| Local check | Result |
| --- | --- |
| Windows Debug / Release / ASan | PASS, 19/19 each (57 total) |
| Graphics build and CPU CTest | PASS, 19/19 |
| Runtime-only dependency boundary | PASS |
| Pinned Ubuntu24 headless Debug / Clang ASan+UBSan | PASS, 17/17 each; Server package closure and pristine-runtime smoke PASS |
| DXC and independent SPIR-V validation | PASS, four entrypoints compiled twice identically; invalid HLSL and corrupt SPIR-V rejected |
| Vulkan validation tools | PASS, locked Windows build; 15 tooling tests |
| Standard-user Windows launcher | PASS, four actual identity/argument/exit/cleanup cases |
| Evidence and shader tooling | PASS, 41 tests on Windows and WSL |
| RTX4070 Laptop, driver610.62 | PASS, 14 readback comparisons / 21,088 values, core+synchronization+NVRHI validation clean |
| Intel RaptorLake-S Mobile, driver101.6790 | PASS, same cases and clean validation |
| NVIDIA bounded lifecycle | PASS, 100 resize/minimize/restore and observed-zero cycles, 20 device creations, two seconds timed presentation |
| Independent raw-artifact audit | PASS on both GPUs, all 14 checks and 21,088 values reproduced |
| Strict GCC syntax | PASS, supplemental syntax only; no Linux graphics link or GPU claim |
| Seven deliberate failures | PASS, missing feature/shader/readback mismatch exit 1; absent UUID/missing validation receipt/native unavailable layer exit 3; real five-second deadline exit 124 with clean owned-process cleanup |

The two local GPUs are supplemental devices, not the named GTX1660/IrisXe baseline
rows. P02-01/02/03/04/06 passed locally. P02-05 remains NOT_RUN because the final
timed presentation was two seconds, not 600; P02-07/08 remain separate aggregate
qualification. Native Mac/Linux graphics, the nine-row matrix and clean-machine
packaged launch are not qualified. No 02b hosted CI result is claimed.

Initial diagnostic readbacks also passed before Vulkan validation was available.
The first state-tracking assertion exposed an invalid restore-to-Undefined policy;
the fix uses meaningful persistent resource states. Windows minimum window sizes
also exposed an exact-resize timeout; lifecycle requests now use 256/320-pixel
sizes above native tracking minima. Error-report aggregation was fixed to retain
case records without reading a moved-from object. The first required-validation
run exposed missing `dynamicRendering` enablement; capability detection and explicit
device enablement now match the pinned NVRHI backend. The runner isolates implicit
layer discovery with an empty `VK_IMPLICIT_LAYER_PATH`, instead of a disabling
filter that itself generates loader warnings. No third-party patch, warning
suppression or tolerance change was used.

One earlier 100-cycle run timed out waiting for a resize with no validation
messages. Its exact dimensions were not captured, so the cause remains unresolved.
The instrumented build passed, and the final build also passed after requiring
restored logical dimensions and a positive framebuffer before recreation. These
passes do not erase the initial failure or establish its root cause. Future failures
include cycle/requested/observed dimensions; 02d must investigate any recurrence
and complete the full soak. No deadline or required cycle count was relaxed.

Windows high-integrity processes cannot use the loader's local layer path. The
optional launcher verifies a same-user/session standard desktop token, creates a
bounded child with atomic job containment and verifies its actual medium integrity.
It never changes ACLs, registry entries or loader safeguards. Python3.13's private
temporary-directory ACL also exposed an elevated-only published DXC cache. Persistent
tool-cache staging now inherits the workspace's existing permissions; the old cache
is preserved separately. Validation-tool source publication uses the same fix.
The original validation build receipt remains accurate build-time provenance; its
preparer source hash predates that permissions-only change, while lock and binary
hashes remain identical.
All five existing validation source trees were also republished with inherited
permissions after verifying all 3,027 files; bytes, modes and timestamps remain
identical. A real standard-user process verified the complete cached inputs offline.
The original trees, tool binaries and build receipt were preserved.

An existing POSIX process-cleanup test exposed asynchronous SIGKILL retirement
under the loaded WSL chroot: immediate observation saw state D. The test now
observes actual termination within the existing one-second cleanup budget;
only disappearance or zombie state passes. Persistent D/running/sleeping still
fails. Production cleanup behavior and all command budgets are unchanged. Initial
failing evidence is retained separately from the corrected rerun.

The negative-evidence assessor initially expected a success-only teardown stage on
exception paths. That unsupported assertion was removed from the assessor after
checking the actual contract: native RAII unwinds before error reporting, and the
proof verifies nonzero bounded exits, diagnostics and process absence. The initial
assessment and all identical raw evidence are preserved; no GPU retry was used to
change that conclusion. A documentation check also caught a link to the not-yet-
generated durable receipt; generation preceded the final passing contract run.

## Reproduction

See [the probe guide](../graphics-probe.md) for exact native setup, case, lifecycle
and failure commands. Prefix Windows configure/build/tool commands with
`powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1`.
Use fresh output directories; retained evidence is never overwritten.
The final NVIDIA command uses `--lifecycle-cycles 100 --device-cycles 20
--present-seconds 2 --timeout 300`; the Intel command uses the default two resize
cycles. Both use `--mode cases`, the exact UUID, pinned validation receipt and
prepared shader directory. The durable receipt contains complete native argv and
the standard-user launcher receipts; the guide explains the compiler-wrapper array
needed when forwarding the helper's `--` separator.

The headless rerun uses the unchanged pinned Ubuntu24 build userspace created by
`tools/ci/headless/chroot.sh`, fresh build directories and a separate pristine
runtime userspace. It has no display/GPU/audio stack. The shared WSL kernel and
writable chroot root are explicit limits; this is not hosted Docker evidence.
The original fresh userspace preparation and corrected rerun commands are retained
in the receipt. A new host can reproduce with the standard chroot command using
fresh root/evidence paths.

## Coverage and next checkpoint

`graphics.probe-cases` maps executable external GPU fixtures and actual CPU
contract/failure tests. Component, game-authoring command and Lua arrays remain
empty; FeatureWorld begins in 10. Server authority and human/terminal parity plans
remain intact. `.gitignore` covers source/tool caches, generated SPIR-V, layers,
readbacks and reports under `.tools/`, `build/` and `out/`; the local roadmap stays
ignored. No source or shared evidence document is hidden.

Next is **02c native Mac feasibility and distribution**. Both native CPU families,
both Intel GPU rows, full Linux graphics builds, the nine-row matrix, ten-minute
presentation qualification and clean-machine packaged probe remain open. 02d
closes the full gate. Assignment 03 and production renderer work have not begun.
No hosted CI, staging, commits, pushes or external messages were performed.

Suggested commit: `feat(graphics): add validated GPU cases and lifecycle probes`
