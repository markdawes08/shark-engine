# ADR 0002: Shark language, dependencies and integration boundaries

Date: 2026-10-17  
Status: Accepted architecture contract; implemented dependency evidence is recorded in owning handoffs, and full graphics compatibility remains unqualified.
Scope: Assignment 00. Assignment 01 implements the build and lock mechanism; later owners integrate their subsystems.

## Decision

16a integrates **Dear ImGui 1.91.9b** privately in native Editor, pinned to commit
`f5befd2d29e66809cd1110a152e375a7f1981f06`. The
[dependency review](../dependencies/imgui.md) records exact archive/source hashes,
MIT and embedded notices, four compiled core sources and reviewed options.
Shark supplies input/graphics adapters; upstream backends and build scripts never
run. Only `SHARK_BUILD_EDITOR AND SHARK_NATIVE_GRAPHICS` creates `Shark::ImGui`.
Runtime, Server and independent CPU Tools do not populate or link it. ImGuizmo
remains assignment 17. The pin does not establish native qualification.

12c integrates **stb_image 2.30** privately in EngineTools, from immutable
upstream commit `003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5`. The exact archive,
complete regular-file inventory and MIT license selection are pinned in the
dependency lock and [decoder review](../dependencies/stb-image.md). Only the
unmodified header's bounded memory PNG/JPEG path is compiled. The private
`Shark::StbImage` SYSTEM interface never enters Core, Client, Runtime or Server;
upstream build files and tests never execute. Offline Tools configurations pass
`SHARK_IMAGE_ARCHIVE`; protected Tools-OFF builds do not populate it. Engine
limits, container validation, allocation cleanup and color/mip behavior remain
required independently of vendor decoding.

M03a adds **SQLite 3.54.1** only to EngineStorage. The official amalgamation,
source ID, exact archive/source/notice hashes, compile options and no-patch policy
are recorded in the lock and [ADR 0015](0015-durable-store.md). The storage helper
verifies all four archive files; only sqlite3.c is compiled. Core, client and
authoring targets do not consume it. Offline Server builds additionally supply
`SHARK_SQLITE_ARCHIVE`, prepared by `tools/storage_dependency.py`. SQLite public-
domain provenance is retained in the Server notice; no database, shell executable,
shared SQLite library or fixture is installed. Existing Core pins remain unchanged.

08a integrates **nlohmann/json 3.12.0** privately in EngineTools. The official
release is pinned to commit `55f93686c01528224f448c19128836e7df245f72` and
`json.tar.xz` SHA-256
`42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa`.
The MIT `LICENSE.MIT`, release archive and CMake integration were reviewed on
2026-10-21. [Official release](https://github.com/nlohmann/json/releases/tag/v3.12.0),
[official CMake guidance](https://json.nlohmann.me/integration/cmake/).
`SourceDocumentDependencies.cmake` verifies all 54 regular archive files and
creates the SYSTEM header interface without executing upstream CMake or fetching
transitive dependencies. Implicit conversions and legacy discarded-value
comparison are disabled. No patch is applied. JSON is authoring-only and is
absent from Core, Runtime and Server configure, compile and package closures.
`SHARK_JSON_ARCHIVE` supplies the exact offline archive when Tools is enabled;
protected builds continue to require only their existing Core archives. Source
parsing and serialization use [explicit bounded rules](../source-documents.md).

07a integrates **EnTT 4.0.0** in EngineCore's scene implementation, pinned to
commit `85c6bba014049b5de8fad49d25424df2f1f6a8c1` and archive SHA-256
`1160aaa50e9f28dc0e250079d6bc507e1566a9ee6db5f4f968a29d357ef17a00` in the
single dependency lock. The official release, exact commit archive, MIT `LICENSE`
and upstream build configuration were reviewed on 2026-10-18.
[Release notes](https://github.com/skypjack/entt/releases/tag/v4.0.0),
[pinned license](https://github.com/skypjack/entt/blob/85c6bba014049b5de8fad49d25424df2f1f6a8c1/LICENSE),
[pinned CMake](https://github.com/skypjack/entt/blob/85c6bba014049b5de8fad49d25424df2f1f6a8c1/CMakeLists.txt).

07b integrates **GLM 1.0.3**, pinned to commit
`8d1fd52e5ab5590e2c81768ace50c72bae28f2ed` and archive SHA-256
`36eefbae41503e6822e8d9e1b4cc85c6a6621f17676f6258aaf0a64c2de0a09c` in the
same lock. The official release, exact archive, license and upstream build
configuration were reviewed on 2026-10-18. Shark selects the MIT option from the
dual Happy Bunny/MIT license and retains the complete upstream `copying.txt`.
[Release notes](https://github.com/g-truc/glm/releases/tag/1.0.3),
[pinned license](https://github.com/g-truc/glm/blob/8d1fd52e5ab5590e2c81768ace50c72bae28f2ed/copying.txt),
[pinned CMake](https://github.com/g-truc/glm/blob/8d1fd52e5ab5590e2c81768ace50c72bae28f2ed/CMakeLists.txt).

`SceneDependencies.cmake` and `MathDependencies.cmake` share the fixed
`CoreHeaderDependencies.cmake` helper. FetchContent only populates these archives;
Shark creates their SYSTEM C++20 header interfaces. EngineCore links `EnTT::EnTT`
privately, keeping registry and component storage details out of public contracts.
Neither header dependency's upstream CMake project executes, so no C language setup, upstream library,
tests, testbed/GUI, examples, documentation, global flags or downloads enter
Shark's graph. Neither integration has patches or transitive dependencies. This
helper implements the existing lock policy for these two dependencies.

GLM is the public math exception. EngineCore publicly links `SharkMath`; consumers
include `<shark/math.hpp>` as the single GLM include point. The interface exports
`GLM_FORCE_RADIANS`, `GLM_FORCE_DEPTH_ZERO_TO_ONE`, `GLM_FORCE_EXPLICIT_CTOR`,
`GLM_FORCE_CTOR_INIT` and `GLM_FORCE_PURE`. The wrapper supplies matching defaults
for direct header consumers and rejects prior GLM setup or incompatible
handedness, layout, precision, swizzle and SIMD definitions. Public math uses
packed scalar doubles, right-handed coordinates and zero-to-one depth. See the
[transform contract](../transforms.md) for quaternion, matrix and numerical rules.
GLM object memory is never a serialized format or network protocol. This exception
does not expose EnTT or authorize per-target math configuration.

Disconnected `server-only` builds require the exact local `SHARK_ENTT_ARCHIVE`,
`SHARK_GLM_ARCHIVE` and `SHARK_MBEDTLS_ARCHIVE`, or their previously populated
locked archive caches. Explicit
preparation with `tools/core_dependencies.py` occurs before configure; local
extraction permits neither network fetching nor arbitrary source overrides. The
headless closure gate verifies every populated regular file against its archive
and checks all installed notices. Server components and Runtime packages retain
`licenses/EnTT/LICENSE`, `licenses/GLM/copying.txt` and eight notices under
`licenses/MbedTLS/`. See
[building](../building.md#headless-server), [CI](../ci.md), the
[07a handoff](../handoffs/07a-entity-ownership.md) and
[07b handoff](../handoffs/07b-transform-hierarchy.md) for commands and qualification;
pin review alone does not claim a passing build.

06a reuses the exact locked DXC and SPIRV-Tools inputs for the standalone
[offline shader builder](../shader-pipeline.md). Compilation is an explicit
developer command using cached tools; it is absent from the Runtime/Core/Server
target and package graphs. The lock retains the original dependency introduction
owners and unchanged bytes; this adds a consumer, not a second toolchain pin.
The versioned recipe and artifact convention is defined by the shader contract.

04b reuses the existing exact GLFW pin through `SharkWindowDependencies` for
Windows EngineClient windows. Shared locked source handling also serves the
opt-in graphics probe. `SHARK_NATIVE_WINDOW` gates all native window population;
Core/Server plus WSL CPU-only clients never discover or download GLFW.
05a defaults `SHARK_NATIVE_GRAPHICS` ON for Windows native-window clients and
shares the same exact Vulkan/NVRHI inputs with the probe through
`SharkGraphicsDependencies`. Those links remain private to EngineClient.
The lock changes only owner metadata; no dependency revision or patch changes.
See the [window guide](../window-lifecycle.md) and
[graphics device contract](../graphics-device.md).
The pinned loader's Windows SDK `cfgmgr32` dependency is an explicit
client-classified imported target. Boundary checks allow only the exact
`LINKER:/guard:cf` and `LINKER:/guard:ehcont` protection flags; lookalike library
injection remains rejected. The client propagates a UTF-8 Windows manifest for
loader path handling. Headless builds acquire none of these dependencies.

03a uses native OS entropy for UUIDv4 creation: Windows SDK `bcrypt` through
an explicitly classified Core-only imported target, and Linux `getrandom`
through libc. This adds no fetched dependency. Installed compiler/SDK and
headless userspace identities are recorded with local test evidence. It does
not supply the network cryptography implementation; M02a separately selects
Mbed TLS under ADR 0011.

MMORPG amendment: the product now follows [ADR 0007](0007-mmorpg-architecture.md).
The existing dependency lock and working bootstrap are retained. M01 introduces
EngineCore/EngineClient/Server before client subsystem integration: GLFW, NVRHI,
Vulkan, miniaudio and platform input are EngineClient-only; math, ECS, authoritative
physics, simulation Lua and bounded runtime data belong to EngineCore. EngineTools
uses Core for authoring; optional previews depend explicitly on Client. Runtime
uses the client composition, while Server links Core and the server-only
EngineStorage module introduced in M03a. Rows below naming
generic Engine describe these owning implementations under that split, not
permission to put presentation dependencies into Core. Headless configurations
must not discover/download graphics/audio/window packages merely to disable them.

MMO additions require deliberate selections at their owners: maintained transport,
session authentication and cryptography in M02; transactional persistence and
migration tooling in M03; terrain/collision/navigation and bounded streaming in
M05; skeletal-animation processing in M06; deployment/content authenticity in
M08. M02a selects Mbed TLS below and M03a selects SQLite above; navmesh and
hosting providers remain unselected.
Use the existing single dependency lock before first integration. License, patch,
CPU/platform and offline/server package tests apply to these dependencies too.
Single-precision Jolt is confined to bounded local origins; it does not select
single-precision global world coordinates or deterministic lockstep simulation.

Use C++20 for engine-owned code and target-based CMake, with CMake 3.28 as the minimum project version. Require the standard without compiler extensions. Express includes, definitions, warnings and link dependencies on targets rather than directory-wide settings. Build third-party C code in its supported language. Pin the exact compiler, CMake and other build-tool versions used by CI in assignment 01; a minimum version is not a reproducible toolchain lock. CMake's target model supports scoped usage requirements, and current EnTT upstream requires C++20. These are upstream facts, not evidence that this project's build passes. [CMake build-system manual](https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html), [EnTT integration requirements](https://github.com/skypjack/entt#integration).

Retain the roadmap's requested stack. The following selections authorize integration only in their owning assignments. Unless a release is explicitly given below, select an immutable revision at first integration after checking upstream release notes, toolchain compatibility and licensing; never build a floating branch while waiting for that selection.

| Purpose | Selected dependency/configuration | Allowed consumer | Integration owner and evidence required |
| --- | --- | --- | --- |
| Windows and raw input | GLFW; Vulkan windows, no OpenGL context | Engine platform implementation | 02 probe, 04 lifecycle; surface creation and input/focus/resize checks on target systems |
| Graphics | NVRHI Vulkan backend; Vulkan-Headers and loader from one recorded compatible toolchain | Engine graphics implementation | 02 pins and qualifies the combination; 05 owns production lifecycle |
| Math | GLM 1.0.3, centrally configured `SharkMath` interface and `<shark/math.hpp>` | EngineCore and public math consumers, the documented third-party type exception | 07b; convention, configuration and transform tests; never serialize GLM object memory |
| ECS | EnTT 4.0.0 entity/registry facilities behind the engine's scene interface | EngineCore scene implementation only, private link | 07a pins entity ownership and safe mutation; 07b adds hierarchy tests |
| Transport security | Mbed TLS 4.1.1 LTS with its bundled pinned TF-PSA-Crypto; TLS 1.3 over nonblocking TCP | EngineCore transport implementation only, private static link | [M02a foundation qualified](../handoffs/M02a-transport-wire.md); M02b owns player authentication and sessions |
| Rigid-body physics | Jolt, single-precision world, CPU options matching the platform baseline | Engine physics implementation only | 18/20; lifecycle, fixed-step and event/query checks |
| Audio playback/mixing | miniaudio, one implementation translation unit, source decoding/encoding disabled | Engine audio implementation only | 24; offline mix and actual playback/device checks |
| Offline audio import | dr_wav for WAV; Xiph libvorbisfile/libvorbis plus libogg for Ogg Vorbis | EngineTools audio importer only | 24; exact decoder pins, malformed-input/length/loop tests and validated PCM output |
| Gameplay | Lua 5.5.1, official C API with small engine-owned bindings | Engine scripting implementation only | 19; binding/lifetime/error/limit tests on each CPU family |
| Editor GUI | Dear ImGui docking configuration | Editor only | 16; pin core and backend sources from the same commit; test docking and viewport sizing |
| Transform editing | ImGuizmo transform widget, using that same ImGui checkout | Editor only | 16 selects the pair; 17 proves gizmo/undo/matrix compatibility |
| glTF parsing | cgltf | EngineTools importer only | 12; bounded parse, accessor/profile validation and fixture tests |
| Image decoding | stb_image limited to PNG/JPEG; Radiance HDR enabled for environment import | EngineTools image importer only | 12 and 27; malformed/oversized input and color/orientation checks |
| Source JSON/configuration/protocol | nlohmann/json with engine-owned typed validation | Engine configuration/serialization implementation; EngineTools and local tool services | 03/08/15; bounded parsing, version rejection and transactional-load tests |
| Tests | Catch2 v3, the sole primary C++ test framework, registered with CTest | Tests only | 01; a real starter test and evidence that test failures fail CI |
| glTF fixture validation | Khronos glTF-Validator, local CLI/package | Development/CI only | 12; pin tool and its runtime; keep validation reports with fixture provenance |
| Offline shader compilation | HLSL through DXC to SPIR-V; SPIR-V validation tools | Build/cook tooling only | 02 probe pins the initial toolchain; 06 owns reproducible shader pipeline |

GLFW supplies the selected Windows windowing route; the retained Linux probe is a CPU/package fixture, not a native Linux graphical target under [ADR 0010](0010-current-machine-scope.md). GLM supplies the selected math types. Jolt supplies the selected physics implementation. miniaudio provides playback, mixing and spatialization. Their broader advertised platform support and features do not expand the v1 profile. [GLFW](https://github.com/glfw/glfw), [GLM](https://github.com/g-truc/glm), [Jolt platform requirements](https://github.com/jrouwe/JoltPhysics#supported-platforms), [miniaudio](https://github.com/mackron/miniaudio).

## Immutable dependency inputs

M02a adds privately integrated Mbed TLS 4.1.1 and its bundled TF-PSA-Crypto from
the exact official release archive. [ADR 0011](0011-network-foundation.md) records
the transport/threat assessment and [network contract](../network-transport.md)
records the TLS/wire limits. This CPU-only dependency requires C alongside C++,
uses three static libraries and seven reviewed object targets, and adds eight
redistribution notices. All public network interfaces remain engine-owned.
The single lock records the complete archive/commit/options/license graph; no
installed TLS library, provider account, floating source or custom crypto is used.
Shark selects the Apache-2.0 license option and preserves the complete bundled
license/notice files. The portable population verifies all 3,927 regular files
and omits only the 147 pinned symlinks in unused PQC tests/examples. Upstream
tests, programs and generation are disabled; its documentation utility targets
are not part of the default or protected dependency build. Both C and C++ use
matching compiler families; sanitizer configurations instrument the private
TLS libraries and object targets as well as engine code.

Assignment 01 qualification amendment: source dependencies, development-tool
wheels and GitHub actions have immutable hash pins. Hosted CI selects explicit
compiler families/vendor bundles and records their full observed versions,
binary hashes, SDK and runner-image revision. GitHub's rolling images cannot be
made immutable by choosing a runner label. This is an explicit limitation on
the original exact-toolchain-pin intention, not a claim of reproducible hosted
VMs. Release reproduction in 35 requires archived toolchain/build inputs or a
controlled immutable runner image. See [CI configuration and evidence](../ci.md).
The initial integrated dependency is Catch2. Checkpoint 02a adds GLFW, NVRHI,
matching Vulkan headers/loader and offline DXC inputs in the same lock, consumed
only by the opt-in probe/tool preparation. See the [probe guide](../graphics-probe.md).
Checkpoint 02b adds a separate, explicitly prepared SPIRV-Tools/Vulkan-ValidationLayers
source graph with its transitive headers/utilities. It is never added to the engine
CMake graph. Checkpoint 07a adds the private Core EnTT pin and its MIT notice;
07b adds the approved public GLM math interface and its MIT notice. Ordinary
Core/Server builds now consume those header inputs and M02a's reviewed CPU-only
TLS source dependency.
[ADR 0009](0009-windows-linux-platform-scope.md) removes Mac driver/tool
qualification and retires checkpoint 02c.

Assignment 01 must establish one authoritative, versioned dependency manifest consumed by the CMake dependency setup. Use CMake FetchContent or locally populated source trees from that manifest; do not introduce competing package-manager resolution paths. Every dependency must record upstream URL, release label when available, full commit SHA or archive URL plus SHA-256, applicable patches, build options, license files, owning target and integration assignment. Include transitives, vendored headers, shader tools and CI tool downloads. Locally populated trees must match the same locked inputs.

A branch name, `latest`, a version range, an unverified mutable tag, or the host's arbitrary installed package is not an exact pin. CMake also recommends commit hashes for fetched Git content. Disable upstream automatic downloads or override every transitive revision centrally. No configure-time update may silently change the selected inputs. A normal build can populate a missing locked dependency; repeat builds and tests use the populated cache, and tests do not download assets. Dependency changes require a manifest diff, license review and the affected owner tests. [CMake FetchContent guidance](https://cmake.org/cmake/help/latest/module/FetchContent.html).

The initial manifest in 01 locks all dependencies actually consumed by that increment. Later assignments must add their dependency pins before integrating them. Do not populate future subsystems or create fake compile checks in 00 merely to make an untested lock look complete. Toolchain/platform/package qualification remains separate from knowing a source checksum.

## Lua release and binding contract

Select the maintained Lua 5.5 line, initially **Lua 5.5.1**. Upstream lists 5.5.1 as current and 5.4.9 as the final 5.4 release. The published archive is `https://www.lua.org/ftp/lua-5.5.1.tar.gz`, with published SHA-256 `1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce`. This is a documented source pin; the archive has not been downloaded or built here. Its integration owner must verify the digest and review intervening fixes before adding it to the build. A later update must be explicit. [Lua version history](https://www.lua.org/versions.html), [Lua archive checksums](https://www.lua.org/ftp/).

Use the C API documented for this exact Lua language version, with engine-owned registration and checked userdata. This avoids an unverified third-party binding compatibility assumption. No LuaJIT, sol2 or general C++ reflection/binding framework is selected. The official 5.5 manual supplies the embedding API, protected calls, allocator and debug-hook interfaces. Compatibility is therefore an API selection supported by documentation; actual engine bindings remain untested until 19. [Lua 5.5 manual](https://www.lua.org/manual/5.5/manual.html).

The versioned gameplay API exposes engine values and checked entity handles, never borrowed component pointers or EnTT/Jolt/NVRHI types. Script calls are protected; the C API boundary must not let Lua error unwinding bypass live C++ objects requiring destruction, and C++ exceptions must not escape C callbacks. Compile Lua as C and keep error-raising trampolines narrow. Assignment 19 must test stack restoration, stale handles, allocator failure, instruction limits and teardown. The VM's existence is not a security sandbox. Persistent scripts use source text; do not promise portable precompiled Lua bytecode across versions or CPU configurations.

## Graphics and editor compatibility contract

Upstream NVRHI describes Vulkan 1.3 on Windows/Linux. Pin NVRHI, its Vulkan inputs and offline compiler together in 02, and identify tested native drivers in hardware evidence. Enable only the Vulkan backend; disable DX11, DX12, NVAPI and optional ray-tracing integrations unless a later approved scope change requires them. The platform ADR and assignment 02 qualify the local Windows path using actual evidence from both GPUs under ADR 0010; this does not qualify other machines. Choosing Vulkan or GLFW alone cannot satisfy this gate. Mac compatibility patches and Vulkan-on-Metal drivers have no current integration or maintenance obligation under ADR 0009. [NVRHI requirements and CMake options](https://github.com/NVIDIA-RTX/NVRHI/blob/main/README.md).

DXC documents HLSL-to-SPIR-V support, which justifies the toolchain candidate. It does not prove that particular shader features or layouts work on the selected local Windows GPU targets. Assignment 02 verifies representative graphics/compute shaders and 06 fixes layout/compilation conventions. Shipped games consume precompiled shader assets and do not require a compiler or Vulkan SDK installation. [DXC SPIR-V mapping](https://github.com/microsoft/DirectXShaderCompiler/blob/main/docs/SPIR-V.rst).

Select Dear ImGui docking in one host window. Multi-viewport OS windows are disabled in v1. Use a release's docking tag resolved to an immutable commit, or a documented docking-branch commit, rather than following the branch. Pin ImGuizmo and that exact ImGui revision as a compatibility pair in 16; test transform behavior in 17 and update the pair together. Upstream offers docking release tags, while ImGuizmo uses Dear ImGui and manipulates matrices. No particular pair has been compiled here. [Dear ImGui docking](https://github.com/ocornut/imgui/wiki/Docking), [ImGuizmo](https://github.com/CedricGuillemet/ImGuizmo).

Editor owns ImGui platform/backend integration and translates GUI draw data through engine rendering services. Engine headers must not expose ImGui types. Do not import ImGuizmo's example application, renderer, unrelated widgets or bundled dependency downloads. The runtime HUD is engine-owned and does not link these authoring libraries.

## Import, validation and serialization boundaries

cgltf parses `.gltf`/`.glb` and provides `cgltf_validate`; it does not itself establish the engine's supported rendering, decompression or image profile. The importer combines parser checks with checked arithmetic, accessor bounds, finite-value checks, resource budgets, project-contained URI resolution and an explicit required-extension allowlist. Unknown required extensions fail. Khronos Validator supplies an additional local fixture-validation oracle; it is not shipped or treated as a replacement for engine checks. [cgltf API](https://raw.githubusercontent.com/jkuhlmann/cgltf/master/cgltf.h), [Khronos glTF-Validator](https://github.com/KhronosGroup/glTF-Validator).

Use stb_image's memory decoding paths behind the bounded importer, with only the image formats in the v1 profile enabled. Enforce file, dimension, pixel-count and decoded-byte limits before allocating; recheck decoder results. Configure a finite `STBI_MAX_DIMENSIONS` consistent with those limits. Color-texture sRGB conversion and linear data-map treatment are engine import decisions, not a decoder side effect. Radiance `.hdr` covers the initial HDRI source path; EXR and compressed texture formats require an explicit future decision. The selected header provides PNG/JPEG/HDR decoding and configurable dimension limits. [stb_image](https://raw.githubusercontent.com/nothings/stb/master/stb_image.h).

nlohmann/json is the selected parser/writer, not the source of schema semantics. Engine-owned code defines document versions, required fields, numeric ranges, duplicate-key policy, references and migration/rejection behavior. Its upstream CMake interface target supports target-scoped integration. Keep authored source readers and external asset parsers/decoders in EngineTools; Engine's JSON use covers runtime configuration and manifests. Final Runtime consumes validated cooked assets. The early export spine consumes validated engine-owned development payloads prepared by EngineTools, never glTF or project-source imports at launch. It does not authorize linking EngineTools into Runtime. [nlohmann/json CMake integration](https://github.com/nlohmann/json#cmake).

Catch2 v3 integrates with CMake/CTest. Other upstream projects may use their own test tools internally; do not adopt them as additional engine test frameworks or ship them. [Catch2 CMake integration](https://catch2-temp.readthedocs.io/en/latest/cmake-integration.html).

## Audio import and runtime boundary

Keep miniaudio for runtime device I/O, mixing and spatialization, fed through engine-owned PCM data sources. Compile its sole implementation with `MA_NO_DECODING` and `MA_NO_ENCODING`; configure all miniaudio consumers consistently. It must not pull a source decoder into Runtime through a resource-manager convenience path. These switches disable the documented decoder/encoder interfaces; assignment 24 must verify the selected playback configuration with cooked PCM. [miniaudio build configuration](https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h).

EngineTools imports WAV through standalone dr_wav and Ogg Vorbis through Xiph libvorbisfile/libvorbis with its libogg dependency. These are private offline importer targets, pinned in 24 under the same immutable-input policy. dr_wav supplies memory/callback decoding; Vorbisfile supports application-controlled I/O callbacks and explicit setup/teardown. Decode through bounded input and output buffers, validate channel/rate/frame counts and checked decoded sizes, reject malformed/truncated streams, and publish engine-owned PCM only after complete validation. No source codec or container is needed by exported Runtime. [dr_wav API](https://raw.githubusercontent.com/mackron/dr_libs/master/dr_wav.h), [Vorbisfile callback API](https://xiph.org/vorbis/doc/vorbisfile/ov_open_callbacks.html), [Xiph Vorbis](https://github.com/xiph/vorbis), [Xiph Ogg](https://github.com/xiph/ogg).

Assignment 24 fixes and tests concrete source limits, channel conversion, sample rates, decoded sample counts and loop boundaries before claiming import support. Library format recognition does not expand Shark's WAV/Ogg Vorbis profile. Do not substitute stb_vorbis silently: its documented floor-0, concatenation and beginning-truncation limitations would require an explicit narrower source contract. [stb_vorbis limitations](https://raw.githubusercontent.com/nothings/stb/master/stb_vorbis.c).

## Consequences and evidence

Third-party types stay inside the owning implementation except centrally configured GLM types permitted by the engine math contract. Shared scene/command schemas use engine-owned types. M01 implements EngineTools -> EngineCore, Editor -> EngineTools/EngineClient, Runtime -> Engine -> EngineClient -> EngineCore and Server -> EngineCore. Client previews in Tools remain future optional integrations; subsystem integration status is recorded by each owning handoff. Test-only and build-only libraries must not enter packaged runtime dependency closures. Assignment 01 establishes boundary checks, and 14/31 inspect exported packages and licenses.

The original assignment 00 selections were reviewed on 2026-10-17; later pin reviews are dated with their integration decisions above. Architecture selection alone does not establish tested combinations, performance, audio playback or platform support. No dependencies were installed and no builds or hardware checks ran in assignment 00. Unknown compatibility has explicit owning assignments; a failing gate requires evidence and a concrete decision, never a silent stack replacement.
