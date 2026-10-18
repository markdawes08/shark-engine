# ADR 0003: Client and headless-server platform qualification

- Date: 2026-10-17
- Status: accepted planning contract; M01 server foundation **PASS**, every shipping release target **UNVERIFIED**
- Owners: 01 (build qualification), M01 (headless-server foundation), 02 (client graphics feasibility), 05-06 (production graphics integration), later MMO milestones (server behavior/load), 35 (client release qualification)

## Decision and present support status

Shark targets Windows, Ubuntu Linux, Apple Silicon Macs and Intel Macs for clients,
and Ubuntu Linux for the planned dedicated server. **No shipping client or server
release is qualified.** The [01 handoff](../handoffs/01-build-foundation.md) records
passing local Windows bootstrap builds/tests and supplemental WSL Ubuntu 26.04
CPU checks. The [CI verification handoff](../handoffs/01-ci-verification.md) records
the completed 01 hosted CPU matrix and deliberate-failure/red-to-green gate. These
results do not qualify client graphics, a headless server or a released game.
M01's separate foundation gate is **PASS**. Local results are recorded below and
in its [handoff](../handoffs/M01-headless-server.md); the
[hosted follow-up](../handoffs/M01-ci-verification.md) records the native CPU
matrix, minimal Docker Server gate and deliberate-failure/restored-success proof.
Full graphics and multiplayer qualification remain **NOT RUN**. The
[graphics probe](../graphics-probe.md) adds executable local cases in 02b after
02a's setup diagnostics. Individual case execution is separate from the required
nine-row hardware gate; no complete mandatory row is qualified yet. The
[MMORPG plan](../roadmap/mmorpg-plan.md) adds future server milestones without
removing the native client commitments.

Use GLFW, NVRHI and Vulkan on every client target. Server excludes that stack at
configuration, build, link and runtime. NVRHI's upstream README currently identifies Windows and Linux on x64/ARM64, and describes its Vulkan backend as Vulkan 1.3. It does not establish macOS support. A compiler accepting NVRHI, or a Vulkan triangle without NVRHI, does not prove this stack works. [NVRHI README](https://github.com/NVIDIA-RTX/NVRHI/blob/main/README.md)

Assignment 02 owns the unresolved macOS feasibility decision, including NVRHI source portability, device creation, shader translation, formats, limits, maintenance and redistribution. It records the exact NVRHI and graphics-toolchain revisions before probe execution through the dependency lock mechanism established in 01. Patches must be versioned with an upstream issue, rationale and maintenance owner. A failure requires a minimal reproducer and a concrete decision request. Removing either Mac CPU family, lowering the baseline, or replacing NVRHI requires an explicit recorded scope/architecture decision; it cannot be reported as a passing gate.

## Intended v1 OS and CPU boundaries

These are minimum deployment candidates. New major OS versions or GPU families require qualification; newer does not automatically mean tested.

| Target | OS and window system | Native CPU/ABI | Planned graphics route | State |
| --- | --- | --- | --- | --- |
| Windows client | Windows 11 25H2, Win32 desktop | x86-64, SSE4.2 floor; no mandatory AVX/AVX2 | Native vendor Vulkan driver | UNVERIFIED |
| Linux client | Ubuntu 24.04 LTS, glibc 2.39 baseline, X11 desktop session | x86-64, SSE4.2 floor; no mandatory AVX/AVX2 | Native vendor/Mesa Vulkan driver | UNVERIFIED |
| macOS Apple Silicon | macOS 26, Cocoa | arm64, Apple M1 floor | KosmicKrisp Vulkan-on-Metal candidate | UNVERIFIED |
| macOS Intel | macOS 15, Cocoa | x86-64, SSE4.2 floor; no mandatory AVX/AVX2 | MoltenVK Vulkan-on-Metal candidate | UNVERIFIED |
| Linux dedicated Server | Ubuntu 24.04 LTS, glibc 2.39 baseline, no display session | x86-64, SSE4.2 floor; no mandatory AVX/AVX2 | None; no GPU, window, graphics or audio-device requirement | M01 idle foundation PASS; shipping release UNVERIFIED |

Windows 25H2 avoids beginning a new qualification program on Windows 11 24H2 Home/Pro after its October 2026 end of updates. This is a project baseline choice, not a claim of tested compatibility. [Microsoft lifecycle](https://learn.microsoft.com/en-us/lifecycle/products/windows-11-home-and-pro)

The Linux client v1 window-system commitment is X11; native Wayland is a future qualification target. XWayland may be investigated but cannot substitute for the named X11 gate. Server has no window-system gate. Ubuntu 24.04 supplies the selected glibc 2.39 baseline. [Ubuntu release notes](https://discourse.ubuntu.com/t/ubuntu-24-04-lts-noble-numbat-release-notes/39890)

Windows/Linux ARM64, 32-bit builds, Rosetta-only qualification, mobile, consoles, browser deployment and native Metal are outside this v1 platform profile. Separate native arm64 and x86-64 Mac artifacts are sufficient; a universal binary is optional and must preserve both qualification results.

The selected Apple route deliberately has different OS floors. Mesa documents KosmicKrisp as an Apple Silicon implementation using Metal 4 on macOS 26+. Khronos' MoltenVK notice recommends KosmicKrisp for Apple Silicon and community-maintained MoltenVK for Intel Macs. These upstream statements establish candidates, not Shark compatibility. [Mesa KosmicKrisp](https://docs.mesa3d.org/drivers/kosmickrisp.html), [MoltenVK README](https://github.com/KhronosGroup/MoltenVK/blob/main/README.md)

## Headless Server qualification in M01

[ADR 0001](0001-targets-and-boundaries.md) separates EngineCore from EngineClient
and adds Server. `server-only` builds the headless process; `runtime-only` remains
a client bootstrap with authoring/tests/server disabled.
M01 follows closure of the 01 CPU gate. It can qualify independently of 02's
client graphics feasibility; it neither bypasses nor closes any client GPU row.
M01 starts one idle process. Simulation writers for internal world partitions
are future behavior; M05 later qualifies multi-process ownership transfer.
Windows/macOS server deployment and Linux ARM64 server deployment are not selected
by this amendment.

| M01 evidence | Required acceptance | Result |
| --- | --- | --- |
| Dependency isolation | Fresh server-only configure/build with locked Core inputs; no client/authoring package discovery, download or target creation; direct/transitive boundary rejection fixtures pass | PASS locally and in hosted Docker, including actual source/header/link/package closure |
| Native CPU build | Ubuntu 24.04 x86-64 Debug/Release with GCC and Clang ASan/UBSan; record exact compiler, standard library, SDK, OS/image and dependency lock digest | PASS in pinned Ubuntu userspace under WSL and hosted native x86-64 Docker; shared kernels, not bare-metal qualification |
| Headless startup | Actual Server runs with DISPLAY and WAYLAND_DISPLAY unset, without X11/Wayland, Vulkan/GLFW, GPU or audio-device services; package and dynamic-library inspection agree with the Core-only closure | PASS locally and hosted; pristine pinned Docker runtime has networking disabled and read-only root/package |
| CLI and configuration | Help/version, valid minimal configuration, unknown flags and invalid/missing configuration have tested output and exit status; configuration failure occurs before work is accepted | PASS locally and hosted |
| Lifecycle and smoke | Bounded process-level smoke test, partial-initialization failure and graceful stop signal all terminate within recorded deadlines; owned workers/resources are released and repeated starts/stops remain clean | PASS for the implemented idle lifecycle in build/test environments; pristine Docker runtime runs a separate finite smoke; no simulation workers yet |
| CI evidence | Dedicated headless job exercises the real Server tests with nonempty CTest selection and archives configuration/build/test logs, JUnit, package inventory and toolchain metadata | PASS: hosted Debug and ASan/UBSan each 13/13, package smoke, deliberate sentinel failure and restored success |

Use a clean Ubuntu 24.04 server environment or minimal Ubuntu 24.04 runtime
container with no client libraries or mapped GPU/audio devices for the deployment
smoke test. Unsetting display variables on a rich desktop runner is insufficient
on its own. Server qualification may use a native x86-64 VM/container; record its
host, CPU allocation, memory/IO limits and image identity so load results describe
the tested environment. This does not relax the separate real-GPU client gates.

These checks qualify the server foundation, not multiplayer capacity, durable
world state or live operations. Later MMO milestones add actual simulation,
protocol, persistence, recovery and seamless partition handoff checks. Performance
qualification must record the named CPU/RAM and deployment limits, clients and
entities per partition, interest density, tick duration/debt, CPU/RAM use,
bandwidth, queue bounds and soak duration. Client GPU/FPS measurements cannot
stand in for those results. Provisional capacity goals in ADR 0007 require load
evidence before being advertised; no current player-count claim is made.

## GPU baseline and mandatory representative hardware

The functional GPU floor is the capability profile below, together with a passing native-hardware result. Marketing model names alone do not establish support. The following named candidates bound the initial qualification workload. Each vendor/OS pair is a separate result; a Windows result does not qualify its Linux counterpart.

| Evidence row | OS/CPU from above | Required representative GPU | Vulkan implementation to identify and pin in evidence | Result |
| --- | --- | --- | --- | --- |
| W-NV | Windows x86-64 | NVIDIA GeForce GTX 1660 | NVIDIA driver | NOT RUN |
| W-AMD | Windows x86-64 | AMD Radeon RX 5500 XT | AMD driver | NOT RUN |
| W-INTEL | Windows x86-64 | Intel Iris Xe, Tiger Lake | Intel driver | NOT RUN |
| L-NV | Ubuntu x86-64 | NVIDIA GeForce GTX 1660 | NVIDIA driver | NOT RUN |
| L-AMD | Ubuntu x86-64 | AMD Radeon RX 5500 XT | Mesa RADV | NOT RUN |
| L-INTEL | Ubuntu x86-64 | Intel Iris Xe, Tiger Lake | Mesa ANV | NOT RUN |
| M-APPLE | macOS arm64 | Apple M1 integrated GPU | Pinned KosmicKrisp build and loader | NOT RUN |
| M-INTEL-AMD | macOS x86-64 | AMD Radeon Pro 5300M, 2019 16-inch MacBook Pro | Pinned MoltenVK build and loader | NOT RUN |
| M-INTEL-IGPU | macOS x86-64 | Intel Iris Plus, 2020 13-inch MacBook Pro with four Thunderbolt 3 ports | Pinned MoltenVK build and loader | NOT RUN |

The two Intel-Mac rows distinguish CPU architecture from GPU vendor. Apple lists the Radeon Pro 5300M and Intel UHD 630 in the 2019 model, and Iris Plus in the specified 2020 model; the test must log and select the intended GPU rather than accidentally qualifying automatic switching. UHD 630 is not an initial qualification claim. [Apple 2019 specifications](https://support.apple.com/en-us/111932), [Apple 2020 specifications](https://support.apple.com/en-us/111339)

All nine rows are required for the planned graphics gate. Substituting hardware is a recorded baseline amendment with a reason; a newer GPU cannot silently prove the named floor. Integrated/UMA GPUs remain in scope. Memory use and performance budgets must be measured against these candidates in 34 and rechecked in 35; chunk 00 makes no FPS, scene-size or maximum-resolution guarantee.

## Mandatory graphics capability profile

The initial profile is Vulkan API 1.3 with the functionality exercised below. A portability implementation advertising that version still needs per-feature and per-format queries. No assumption that all Intel Macs meet this profile is permitted; a failure is precisely the decision that 02 must surface.

- GLFW surface creation, a graphics/compute-capable queue, presentation support, `VK_KHR_swapchain`, FIFO presentation, transfer uploads and readbacks. A single queue is sufficient; dedicated compute/transfer queues are not mandatory.
- Vertex, fragment and compute shaders using FP32 arithmetic, 16/32-bit index buffers, uniform/storage buffers, sampled 2D/cube textures, mipmaps, linear filtering, alpha blending, depth testing and storage-image writes. Offline shaders target the selected Vulkan/SPIR-V environment; no runtime source-shader compiler dependency ships with Shark.
- `timelineSemaphore`, `synchronization2` and `dynamicRendering` supported and explicitly enabled where used. The pinned NVRHI graphics pipeline uses dynamic rendering; 02b validation exposed and corrected its missing feature enablement. Continue auditing the pinned revision for any additional required features/extensions and actual synchronization paths. Do not enable an entire feature struct or treat successful extension enumeration as successful execution.
- At least 4096-pixel 2D images, 2048-pixel cube faces, 12 layers of a 2048-pixel D32 depth array with per-layer attachment views and a sampled-array view, four color attachments, four bound descriptor sets, 16 sampled images and 16 samplers per shader stage, 16 KiB uniform-buffer range, 128 MiB storage-buffer range and 128 compute invocations per workgroup. Descriptor layouts and compute dispatches must stay inside the measured limits. The shared shadow array occupies one sampled-image binding; separately bound shadow maps must not exceed this budget.
- No required ray tracing, mesh/geometry/tessellation shaders, bindless descriptors, descriptor indexing, sparse residency, subgroup-width assumption, float64, vendor-specific feature or dedicated GPU. If the chosen NVRHI revision imposes a conflicting requirement, 02 must expose it before later renderer work.

Mandatory optimal-tiling image usages are queried and exercised, not inferred from Vulkan version:

| Format | Required usages and purpose |
| --- | --- |
| `VK_FORMAT_R8G8B8A8_SRGB` | Sampled texture, linear filtering, transfer destination/source; base color and emissive inputs |
| `VK_FORMAT_R8G8B8A8_UNORM` | Sampled texture, linear filtering, color attachment, alpha blending, transfer destination/source; linear material inputs and LDR intermediates |
| `VK_FORMAT_R16G16B16A16_SFLOAT` | Color attachment, alpha blending, sampled image, linear filtering, storage image, transfer destination/source; HDR lighting, IBL and compute intermediates |
| `VK_FORMAT_D32_SFLOAT` | Depth attachment, sampled image, transfer source; scene depth and shadow maps |
| Presentation format | `B8G8R8A8_SRGB` or `R8G8B8A8_SRGB` surface format with `SRGB_NONLINEAR` color space; an equivalent UNORM surface requires explicit shader sRGB encoding and the same readback/color tests |

Use conventional 0-to-1 depth. Shadow filtering can use explicit depth taps, so linearly filtered depth, stencil and comparison-filter support are not mandatory. Uncompressed textures are the mandatory interchange path; BC/ASTC compression is optional and cannot make an otherwise valid source asset unexportable. HDR here means scene-referred floating-point rendering with tonemapped SDR presentation; an HDR monitor/output path is not required.

02 may propose a documented equivalent-precision format fallback after testing the complete sampled/rendered/storage/readback path. It must update this ADR, capability selection and tests together; it cannot quietly reduce HDR precision or disable SSAO, IBL or shadows. 05 implements clear rejection diagnostics for missing mandatory capabilities, and 06 verifies shader layouts and resource behavior against the accepted profile.

## macOS-specific feasibility work in 02

Evaluate KosmicKrisp on native Apple Silicon and MoltenVK on both Intel-Mac GPU candidates using the same pinned NVRHI and probe sources. Record every compatibility patch, build option, Vulkan loader/ICD version, shader compiler version, SDK, OS build and GPU selection. Test shader translation for actual bindings, texture arrays/cubes, FP16 image storage, readback and resource barriers used by the profile; a bare triangle is insufficient.

Query extensions before enabling them. Enable portability enumeration with its instance flag when using a portability implementation; enable `VK_KHR_portability_subset` when advertised and inspect its feature/property restrictions. MoltenVK documents the loader enumeration requirement, and the Vulkan specification requires the subset extension when present. GLFW owns surface construction; audit that the selected path uses the modern Metal surface mechanism. [MoltenVK runtime guide](https://github.com/KhronosGroup/MoltenVK/blob/main/Docs/MoltenVK_Runtime_UserGuide.md), [Vulkan portability subset](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_portability_subset.html)

Record restrictions on image views/swizzles, sampler behavior, vertex stride/alignment, event/barrier usage and shader translation wherever they affect NVRHI. Keep workarounds intentional and testable. Do not require private Metal APIs or experimental driver features in the release baseline. Compare Apple-Silicon MoltenVK only as an explicitly documented fallback experiment if KosmicKrisp fails; it does not automatically become the shipping path.

The gate must also produce a redistribution inventory: loader, ICD manifests, translation libraries, licenses/notices, transitive binaries, install names/rpaths, signing requirements and an assigned maintainer for any fork. A packaged probe must launch on a clean target machine without a developer SDK or environment variables. 31/35 own final packaging and release validation, but infeasible redistribution is a blocker discovered in 02.

## Build support is a separate claim

01 provides C++20 target-based CMake and native CPU CI configuration for Windows
x86-64, Ubuntu 24.04 x86-64, macOS arm64 and macOS x86-64. The required hosted
CPU execution gate has passed. Use MSVC on Windows, GCC plus a Clang sanitizer
configuration on Ubuntu, and Apple Clang on macOS. The project declares CMake
3.28 as its floor and pins 3.31.10 for qualification; the
[CI verification handoff](../handoffs/01-ci-verification.md) identifies the versions
actually tested. Record exact CMake, compiler, SDK, standard-library
and runner versions with each result. M01 adds server-only qualification without
relabeling the existing CPU jobs as server tests.

Configure dependency SIMD options to preserve the CPU floor. Do not compile every binary for the host's CPU or require AVX2 through Jolt defaults. Native arm64 tests do not qualify x86-64 through cross-compilation. Build host OS/toolchain requirements may be newer than the deployment OS; record both and validate the actual deployment artifact. Building on a GPU-less CI runner is CPU/build evidence only. Software Vulkan, virtual machines and remote desktop runs may supplement diagnostics, but do not satisfy the real-GPU rows.

## Observable gate evidence and current results

02 must produce a machine-readable result and linked artifacts for every client
graphics matrix row. Each record contains repository commit, dependency lock digest, full configure/build/run commands, timestamp, host/OS/CPU/GPU IDs, driver and ICD versions, shader hashes, selected queues/features/formats, limits, exit code and PASS/FAIL/NOT RUN for each item below. A missing device/runner is NOT RUN and leaves its required gate open.

| Check | Observable acceptance evidence | Present result |
| --- | --- | --- |
| P02-01 Stack/device | Native GLFW surface, Vulkan device and swapchain created through the pinned NVRHI route; capability report matches mandatory profile | NOT RUN |
| P02-02 Graphics/depth | Textured primitive and overlapping depth-tested draws; known foreground/background pixels read back correctly | NOT RUN |
| P02-03 Compute | Representative buffer transform plus RGBA16F storage-image write, graphics consumption and readback; known integer outputs exact | NOT RUN |
| P02-04 Formats/shaders | Every mandatory format and usage exercised; sampled sRGB/linear, HDR values above 1, cube mip selection, each layer of the depth array and depth readback verified; shader compile/validation logs saved | NOT RUN |
| P02-05 Lifecycle | At least 100 resize/minimize/restore cycles and 20 create/destroy cycles, including zero framebuffer extent, followed by clean exit and a 10-minute presentation run | NOT RUN |
| P02-06 Validation | Vulkan validation and NVRHI validation enabled; no relevant errors or unexplained warnings; logs and enabled validation configuration retained | NOT RUN |
| P02-07 Portability/packaging | Both Mac CPU families and both Intel GPU rows evaluated; patch/feature/format/license report and clean-machine packaged-probe launch recorded | NOT RUN |
| P02-08 Failure paths | Missing feature, unavailable device and shader failure produce actionable diagnostics and nonzero failure, with bounded timeout/cleanup | NOT RUN |

For readback comparisons, use exact integer results and channel tolerances of 1/255 for simple UNORM tests, 2e-3 absolute or 2e-3 relative for HDR test values, and 1e-5 absolute for controlled depth values. State reference calculations, tested values and edge exclusions in the probe. Image baselines are platform-specific, reviewed artifacts; never approve a changed baseline merely to turn a failure green. These are probe tolerances, not a promise of bit-identical rendering or physics across platforms.

The [01 handoff](../handoffs/01-build-foundation.md) records historical local
build/CTest passes; the [CI verification handoff](../handoffs/01-ci-verification.md)
closes the historical hosted CPU matrix/red-green gate; the M01 follow-up closes
the server foundation. 02 hardware and MMO gameplay/load results remain NOT RUN.
Assignment 00 originally assigned these uncertainties;
this amendment adds server ownership without waiving a later gate. Dependent
renderer architecture cannot advance on an assumed macOS PASS. Assignment 35 may
call a client target supported only after the final packaged Editor/Runtime,
input, audio, physics, scripting and graphics suite passes on its declared matrix.
Server release qualification is separate and additionally requires the MMO
behavior, recovery, security and load gates in ADR 0007 and the MMORPG plan.
