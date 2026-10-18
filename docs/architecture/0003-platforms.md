# ADR 0003: Client and headless-server platform qualification

- Date: 2026-10-17
- Status: accepted planning contract, narrowed by ADR 0010; M01 foundation and local 02 feasibility **PASS**, shipping release **UNVERIFIED**
- Owners: 01 (build qualification), M01 (headless-server foundation), 02 (client graphics feasibility), 05-06 (production graphics integration), later MMO milestones (server behavior/load), 35 (client release qualification)

## Decision and present support status

Shark targets the currently available Windows 11 x64 PC for client/editor
development and uses its existing WSL2 Ubuntu environments for CPU, headless
Server and local package work. [ADR 0010](0010-current-machine-scope.md) records
the user's machine-bounded scope amendment and retires the earlier broader
platform matrix. Native Linux client/GPU shipping and additional physical machines
are outside the active plan. [ADR 0009](0009-windows-linux-platform-scope.md)
continues to exclude Mac compatibility. **No shipping client or server
release is qualified.** The [01 handoff](../handoffs/01-build-foundation.md) records
passing local Windows bootstrap builds/tests and supplemental WSL Ubuntu 26.04
CPU checks. The [CI verification handoff](../handoffs/01-ci-verification.md) records
the completed 01 hosted CPU matrix and deliberate-failure/red-to-green gate. These
results do not qualify client graphics, a headless server or a released game.
M01's separate foundation gate is **PASS**. Local results are recorded below and
in its [handoff](../handoffs/M01-headless-server.md); the
[hosted follow-up](../handoffs/M01-ci-verification.md) records the native CPU
matrix, minimal Docker Server gate and deliberate-failure/restored-success proof.
The revised assignment 02 feasibility gate is **PASS** for this machine; multiplayer
and final release qualification remain **NOT RUN**. The
[graphics probe](../graphics-probe.md) adds executable local cases in 02b after
02a's setup diagnostics. The [02d evidence](../handoffs/02d-final-verification.md)
supplies full lifecycle runs on both available GPUs plus isolated package checks.
The [MMORPG plan](../roadmap/mmorpg-plan.md) retains server correctness and seamless
world goals, with capacity measured within the same machine's resources.

Use GLFW, NVRHI and Vulkan on every client target. Server excludes that stack at
configuration, build, link and runtime. NVRHI's upstream README identifies Windows and Linux on x64/ARM64, and describes its Vulkan backend as Vulkan 1.3. A compiler accepting NVRHI, or a Vulkan triangle without NVRHI, does not prove this stack works. [NVRHI README](https://github.com/NVIDIA-RTX/NVRHI/blob/main/README.md)

Assignment 02 owns local Windows graphics feasibility, including NVRHI device creation, shader execution, formats, limits, maintenance and package dependency isolation. It records the exact NVRHI and graphics-toolchain revisions before probe execution through the dependency lock mechanism established in 01. Patches must be versioned with an upstream issue, rationale and maintenance owner. A failure requires a minimal reproducer and a concrete decision request. Further baseline changes or replacing NVRHI require an explicit recorded scope/architecture decision. ADR 0010 changes the required environments, not observed outcomes, readback tolerances or lifecycle workload. Mac checkpoint 02c remains retired by ADR 0009.

## Current-machine OS and CPU boundaries

These are the active development and local acceptance environments. No result extends to a different machine, OS build or GPU family automatically.

| Target | OS and window system | Native CPU/ABI | Planned graphics route | State |
| --- | --- | --- | --- | --- |
| Windows client/editor | Current Windows 11 build 26200, Win32 desktop | x86-64; retain existing SSE4.2 code-generation policy | This PC's NVIDIA and Intel Vulkan drivers | Local 02 feasibility PASS; final application unimplemented |
| WSL2 CPU and headless Server | Existing Ubuntu environments; pinned Ubuntu 24.04 userspace/glibc 2.39 for isolated qualification, no display session | x86-64 under this PC's shared WSL kernel | None; no GPU, window, graphics or audio-device requirement | M01 idle foundation PASS; MMO behavior planned |
| WSL2 graphics package fixture | Existing isolated Ubuntu 24.04 build/runtime userspaces | x86-64, SDK-free runtime closure | Description and unavailable-display paths only | CPU/package checks PASS; native Linux GPU work outside scope |

The retained Linux graphics fixture uses X11 build dependencies. Its existence
does not create a Linux desktop, Wayland, WSL GPU or shipping compatibility gate.
Server has no window-system gate. Retain exact image, package and WSL kernel
identities in each Linux result; WSL userspace is not a native Linux hardware run.

Mac on every CPU/GPU family, Windows/Linux ARM64, 32-bit builds, mobile, consoles, browser deployment and native Metal are outside this platform profile. Mac has no compatibility, CI, packaging, signing or hardware qualification commitment. Incidental upstream portability code does not establish support.

## Headless Server qualification in M01

[ADR 0001](0001-targets-and-boundaries.md) separates EngineCore from EngineClient
and adds Server. `server-only` builds the headless process; `runtime-only` remains
a client bootstrap with authoring/tests/server disabled.
M01 follows closure of the 01 CPU gate. It can qualify independently of 02's
client graphics feasibility; it neither bypasses nor closes any client GPU row.
M01 starts one idle process. Simulation writers for internal world partitions
are future behavior; M05 later qualifies multi-process ownership transfer.
Windows server deployment and Linux ARM64 server deployment are not selected
by this amendment.

| M01 evidence | Required acceptance | Result |
| --- | --- | --- |
| Dependency isolation | Fresh server-only configure/build with locked Core inputs; no client/authoring package discovery, download or target creation; direct/transitive boundary rejection fixtures pass | PASS locally and in hosted Docker, including actual source/header/link/package closure |
| Native CPU build | Ubuntu 24.04 x86-64 Debug/Release with GCC and Clang ASan/UBSan; record exact compiler, standard library, SDK, OS/image and dependency lock digest | PASS in pinned Ubuntu userspace under WSL and hosted native x86-64 Docker; shared kernels, not bare-metal qualification |
| Headless startup | Actual Server runs with DISPLAY and WAYLAND_DISPLAY unset, without X11/Wayland, Vulkan/GLFW, GPU or audio-device services; package and dynamic-library inspection agree with the Core-only closure | PASS locally and hosted; pristine pinned Docker runtime has networking disabled and read-only root/package |
| CLI and configuration | Help/version, valid minimal configuration, unknown flags and invalid/missing configuration have tested output and exit status; configuration failure occurs before work is accepted | PASS locally and hosted |
| Lifecycle and smoke | Bounded process-level smoke test, partial-initialization failure and graceful stop signal all terminate within recorded deadlines; owned workers/resources are released and repeated starts/stops remain clean | PASS for the implemented idle lifecycle in build/test environments; pristine Docker runtime runs a separate finite smoke; no simulation workers yet |
| CI evidence | Dedicated headless job exercises the real Server tests with nonempty CTest selection and archives configuration/build/test logs, JUnit, package inventory and toolchain metadata | PASS: hosted Debug and ASan/UBSan each 13/13, package smoke, deliberate sentinel failure and restored success |

Use an isolated Ubuntu 24.04 runtime userspace under the existing WSL environment
with no client libraries or mapped GPU/audio devices for the local deployment
smoke test. Unsetting display variables on a rich desktop runner is insufficient
on its own. Record the isolated userspace's host, CPU allocation, memory/IO limits
and image identity so load results describe
the tested environment. The local native Windows GPU checks remain separate.

These checks qualify the server foundation, not multiplayer capacity, durable
world state or live operations. Later MMO milestones add actual simulation,
protocol, persistence, recovery and seamless partition handoff checks. Performance
qualification must record the named CPU/RAM and deployment limits, clients and
entities per partition, interest density, tick duration/debt, CPU/RAM use,
bandwidth, queue bounds and soak duration. Client GPU/FPS measurements cannot
stand in for those results. Provisional capacity goals in ADR 0007 require load
evidence before being advertised; no current player-count claim is made.

## Active local GPU rows and retired hardware matrix

The capability profile below is unchanged. ADR 0010 explicitly selects the two
GPUs already present in this PC as the complete active native graphics matrix.
The [Windows receipt](../coverage/evidence/02d-windows-refresh.json) records the
same final package passing the full 100/20/600 workload on each GPU. These results
are local feasibility evidence, not a minimum GPU specification for other users.

| Active row | GPU | Driver / Vulkan | Local 02 result |
| --- | --- | --- | --- |
| LOCAL-W-NV | NVIDIA GeForce RTX 4070 Laptop | 610.62 / 1.4.341 | PASS |
| LOCAL-W-INTEL | Intel Raptor Lake-S Mobile integrated GPU | 101.6790 / 1.4.311 | PASS |

The six previously required rows below are retired by the user's scope change.
Their recorded NOT RUN outcomes are preserved; no newer GPU is treated as proof
of an older GPU's compatibility.

| Evidence row | OS/CPU from above | Required representative GPU | Vulkan implementation to identify and pin in evidence | Result |
| --- | --- | --- | --- | --- |
| W-NV | Windows x86-64 | NVIDIA GeForce GTX 1660 | NVIDIA driver | NOT RUN |
| W-AMD | Windows x86-64 | AMD Radeon RX 5500 XT | AMD driver | NOT RUN |
| W-INTEL | Windows x86-64 | Intel Iris Xe, Tiger Lake | Intel driver | NOT RUN |
| L-NV | Ubuntu x86-64 | NVIDIA GeForce GTX 1660 | NVIDIA driver | NOT RUN |
| L-AMD | Ubuntu x86-64 | AMD Radeon RX 5500 XT | Mesa RADV | NOT RUN |
| L-INTEL | Ubuntu x86-64 | Intel Iris Xe, Tiger Lake | Mesa ANV | NOT RUN |

None of the six retired rows blocks local progression. The former Mac rows remain
retired under ADR 0009. Future performance work in 34 and local acceptance in 35
measure the two active GPUs, including integrated/UMA behavior, under the recorded
CPU/RAM contention of this machine: Intel Core i9-14900HX, 24 cores/32 logical
processors and approximately 32 GiB RAM. The RTX 4070 Laptop (8 GiB VRAM) is the
primary performance target; Intel is the secondary correctness/fallback target,
without an equal-FPS promise. No FPS, scene-size or maximum-resolution
guarantee follows from the feasibility probe.

## Mandatory graphics capability profile

The initial profile is Vulkan API 1.3 with the functionality exercised below. An implementation advertising that version still needs per-feature and per-format queries. A failure on either active local GPU must be surfaced.

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

## Portability and redistribution work in 02

Evaluate both active local GPU rows using the same pinned NVRHI and probe sources. Record every compatibility patch, build option, Vulkan loader/ICD version, shader compiler version, SDK, OS build and GPU selection. Test actual bindings, texture arrays/cubes, FP16 image storage, readback and resource barriers used by the profile; a bare triangle is insufficient. Query extensions before enabling them and keep workarounds intentional and testable.

The gate must produce a redistribution inventory: loader, applicable ICD manifests,
licenses/notices, transitive binaries, loader search paths and any fork maintainer.
Accept dependency-isolated Windows package relocation outside the checkout with
system-only PATH, cleared developer overrides and explicit DLL closure inspection;
accept the existing separate SDK-free WSL runtime userspace for Linux CPU/package
checks. An independent physical clean machine is no longer required. These checks
establish local package behavior, not absence of installed tools on the Windows
host or broad redistribution compatibility. 31/35 apply the same machine-bounded
policy to their future actual artifacts. Mac redistribution remains outside scope.

## Build support is a separate claim

01 provides C++20 target-based CMake and native CPU CI configuration. The current
CPU execution environments are this Windows PC and its existing Ubuntu userspaces.
Hosted Windows/Linux CI remains supplemental automation and is not a prerequisite
for local roadmap progression; no GitHub branch policy changes follow. The historical required hosted
CPU execution gate passed on its recorded matrix, including the now-retired Mac
jobs; those records do not require ongoing Mac CI. Use MSVC on Windows and GCC
plus a Clang sanitizer configuration on Ubuntu. The project declares CMake
3.28 as its floor and pins 3.31.10 for qualification; the
[CI verification handoff](../handoffs/01-ci-verification.md) identifies the versions
actually tested. Record exact CMake, compiler, SDK, standard-library
and runner versions with each result. M01 adds server-only qualification without
relabeling the existing CPU jobs as server tests.

Configure dependency SIMD options to preserve the CPU floor. Do not compile every binary for the host's CPU or require AVX2 through Jolt defaults. Cross-compilation does not qualify native execution. Build host OS/toolchain requirements may be newer than the deployment OS; record both and validate the actual deployment artifact. Building on a GPU-less CI runner is CPU/build evidence only. Software Vulkan, virtual machines and remote desktop runs may supplement diagnostics, but do not satisfy the real-GPU rows.

## Observable gate evidence and current results

02 must produce a machine-readable result and linked artifacts for both active client
graphics matrix rows. Each record contains repository commit, dependency lock digest, full configure/build/run commands, timestamp, host/OS/CPU/GPU IDs, driver and ICD versions, shader hashes, selected queues/features/formats, limits, exit code and PASS/FAIL/NOT RUN for each item below. A missing active device/runner is NOT RUN and leaves its required check open.

| Check | Observable acceptance evidence | Present result |
| --- | --- | --- |
| P02-01 Stack/device | Native GLFW surface, Vulkan device and swapchain created through the pinned NVRHI route; capability report matches mandatory profile | PASS, both local rows |
| P02-02 Graphics/depth | Textured primitive and overlapping depth-tested draws; known foreground/background pixels read back correctly | PASS, both local rows |
| P02-03 Compute | Representative buffer transform plus RGBA16F storage-image write, graphics consumption and readback; known integer outputs exact | PASS, both local rows |
| P02-04 Formats/shaders | Every mandatory format and usage exercised; sampled sRGB/linear, HDR values above 1, cube mip selection, each layer of the depth array and depth readback verified; shader compile/validation logs saved | PASS, both local rows |
| P02-05 Lifecycle | At least 100 resize/minimize/restore cycles and 20 create/destroy cycles, including zero framebuffer extent, followed by clean exit and a 10-minute presentation run | PASS, both local rows |
| P02-06 Validation | Vulkan validation and NVRHI validation enabled; no relevant errors or unexplained warnings; logs and enabled validation configuration retained | PASS, both local rows |
| P02-07 Portability/packaging | Exact local artifacts, patch/feature/format/license reports, Windows dependency-isolated graphical relocation and SDK-free WSL CPU/package launch | PASS, revised local scope |
| P02-08 Failure paths | Missing feature, unavailable device and shader failure produce actionable diagnostics and nonzero failure, with bounded timeout/cleanup | PASS, retained 02b/02d failure fixtures |

The assignment-level assessment combines the retained 02b/02d receipts under
ADR 0010. Native case JSON deliberately leaves aggregate P02-07/P02-08 and
`qualification` as NOT_RUN; those historical reports are not rewritten. The
original unexplained resize failure also remains in the evidence history.

For readback comparisons, use exact integer results and channel tolerances of 1/255 for simple UNORM tests, 2e-3 absolute or 2e-3 relative for HDR test values, and 1e-5 absolute for controlled depth values. State reference calculations, tested values and edge exclusions in the probe. Image baselines are platform-specific, reviewed artifacts; never approve a changed baseline merely to turn a failure green. These are probe tolerances, not a promise of bit-identical rendering or physics across platforms.

The [01 handoff](../handoffs/01-build-foundation.md) records historical local
build/CTest passes; the [CI verification handoff](../handoffs/01-ci-verification.md)
closes the historical hosted CPU matrix/red-green gate; the M01 follow-up closes
the server foundation. ADR 0010 closes 02 for the explicitly narrowed local scope;
assignment 03 is next and unstarted. MMO gameplay/load results remain NOT RUN.
Assignment 35 may call this machine's application profile accepted only after the final packaged Editor/Runtime,
input, audio, physics, scripting and graphics suite passes on its declared matrix.
It cannot claim other machines or broad shipping support. Server local acceptance
is separate and additionally requires the MMO
behavior, recovery, security and load gates in ADR 0007 and the MMORPG plan.
