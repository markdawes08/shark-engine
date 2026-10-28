# 13d: Depth-tested offscreen scene viewport

Date: 2026-10-28. Base: `a638780`, initially clean `main`.
**13d is PASS under current-machine scope.** Full REQ-13 remains NOT_RUN;
next is **13e, connected Runtime scene acceptance** in the
[rendering plan](../roadmap/scene-rendering-plan.md).
Staging, commits and pushing remain with the user.

## Delivered

EngineClient now packs immutable Core render snapshots into bounded shared
geometry and ordered primitive draws, then renders them through the existing
GraphicsDevice/Vulkan/NVRHI owner. One persistent RGBA8/D32 viewport reuses its
five target handles at the same size. Resize constructs a complete replacement
before retiring the old completed target. Draws share one clear and depth surface;
model/camera parity and double-sided material values select raster state. Follow
[ADR 0035](../architecture/0035-offscreen-scene-viewport.md) and the
[viewport contract](../scene-viewport.md).

The separate precompiled scene shader uses position/color vertices and an 80-byte
MVP/factor block. The strict bounded native reader selects that interface
explicitly; the original cube recipe, bundle and interface defaults are unchanged.
There is one projection Y flip. Material output is temporary opaque unlit vertex
color times base-color factor; alpha is written without blending/alpha testing.
Textures, lighting, PBR and final transparency are not claimed.

Requests admit 2048x2048 targets, 4096 draws, 262144 unique vertices, 786432 unique
indices and 3,145,728 submitted indices including repeated draws. Float conversion
rejects overflow, denormals, collapsed clipping planes and clip-distance drift
beyond the fixed 1e-6 absolute plus 1e-3 relative budget. A conservative product/sum
bound prevents shader clip arithmetic overflow. Mutable requests are revalidated.
Scene/origin metadata is retained; no Scene, global cells or authoring state is
mutated. Core/Tools/Server dependency direction is unchanged.

Successful synchronous readback follows the existing one-second completion wait.
Timeout/readback failure retains all submitted owners until explicit drain;
unresolved recording follows existing fatal retention. Resize creation failure
preserves the old target. Current geometry/pipelines are rebuilt per call; this
is a bounded development/readback path, not an interactive performance or MMO
capacity claim. Application file launch/presentation remains 13e; editor/terminal
adapters and export/cooking retain their owners.

FeatureWorld retains sixteen native CPU cases, four saved IDs and 55 CPU surfaces.
One additional external viewport command brings the external GPU surface count to
two. The new fixture consumes actual Core extraction/prepared assets without
making CPU Tools link Client. Independent manifest qualification preserves the
old SceneRunner GPU catalog and explicitly checks the new separate fixture.

## Selected verification

The [testing policy](../testing-policy.md) selected affected CPU owners, one
portable sanitizer profile and actual native graphics/lifetime checks. Logs and
existing-format reports are under `out/13d-work/`; no new receipt framework.

| Check | Final result |
| --- | --- |
| Windows MSVC Debug | 8/8 CTests, 5.50 seconds: graphics, presentation, GPU resources, offscreen, scene viewport, shader bundle, scene qualification and new device-free CLI |
| WSL GCC Debug | 4/4, 0.19 seconds: GPU resources, offscreen, scene viewport and shader bundle |
| WSL Clang ASan/UBSan | Same 4/4, 0.93 seconds, leak detection/halt-on-error enabled |
| New Python oracle/evidence tests | 6/6 standalone unittest cases; reused final results rather than rerunning the same family in CTest |
| Native scene rendering | Five frames on each GPU: orthographic, perspective, resized, restored and empty; all ten color/depth pairs pass |
| Native scene failures | Three children on each GPU: failed resize with old-target recovery, timeout/drain/recovery, unresolved recording with expected fatal retention |
| Existing native regressions | Two original offscreen frames on each GPU plus the original three offscreen fault children on RTX; all pass |
| Protected/API checks | Native `runtime-only` Runtime build/boundary and public Client/Core-only example compile pass; Tools/tests OFF, no source importer population |
| Repository | Contract/link/manifest, ignore/discovery, C++ formatting and diff checks pass |

These are **16 selected CTest profile outcomes**, six standalone Python cases,
fourteen native color/depth pairs and nine controlled fault children. The new
60-second `shark.client.scene-viewport` family has eight Catch cases, seed 1337;
three shader-profile cases extend the existing resource family. The new tooling
and CLI families keep 30-second limits. Existing cases and limits are unchanged.

| Device | Observed driver | New readbacks / fault controls |
| --- | --- | --- |
| NVIDIA RTX 4070 Laptop, `863d6f59a51894ab8b4829aa9460c78d` | NVIDIA 616.56 | 5/5 and 3/3 PASS |
| Intel RaptorLake-S Mobile, `868088a7040000000002000000000000` | Intel 101.6790 | 5/5 and 3/3 PASS |

Required Vulkan core/synchronization and NVRHI validation remained enabled. Both
new render runs finish with zero warnings/errors/dropped messages, no pending
frame and zero live targets. Generations follow 1,1,2,3,3; close retires generation
three. Nonempty frames create/retire twenty transient handles; empty clears one.
The fixed independent reference retains one-byte color and 1e-5 depth tolerances,
one-pixel edge exclusion and the queried 8-bit subpixel model. Maximum observed
new color error is one byte and depth error is below 1.2e-16; no checked pixel
fails. Orthographic RTX and perspective Intel images were also visually inspected.

The shader generation is
`257c648de10734db42af482c80a863aacb60a001864cd0d0d30d71e24d2f2ae9`;
it was built with the existing pinned DXC/spirv-val receipt at
`out/12c-work/validation-tools-pinned/summary.json`. The dependency lock remains
`89e7761c68b0b5566cec3e763f2dbc48eef0cc4e92686168b98dfdd1625bf764`.
GPU reports retain actual executable/source/shader/tool identities and raw
readbacks. `rtx-render`, `intel-render`, `rtx-faults`, `intel-faults`, both
`*-offscreen` directories and `rtx-offscreen-faults` contain their summaries.
Paired `*-token.json` files record standard-token launches.

Native commands use `tools/windows_dev.ps1 -CommandArguments @(...)` and the
existing unelevated launcher; wrappers/arguments are recorded in reports.
An initial positional PowerShell wrapper invocation failed before launching a
GPU child; its log is preserved as `rtx-render.log`. The explicit argument-array
invocation fixed that launch error. No engine/compiler/readback test failed.
`windows-debug.xml`, `windows-tests.log`, `wsl/{debug,sanitizer}.xml`, build logs,
`runtime-only-build.log` and `public-example.log` retain CPU/build results.

No full-project/network/storage suite, Release matrix, native ASan GPU run or
package redeployment ran. WSL sanitizer results cover portable packing/profile/
frontend code, not the Windows native backend; real validated GPU fault controls
cover that backend's new ownership paths. Core/Server implementation, locks,
install rules and application launch are unchanged; their earlier evidence is
not counted fresh. No host/driver/WSL settings changed. Existing `.gitignore`
build/out/cache/local-roadmap rules cover generated shaders, readbacks and logs.
Nothing was staged, committed or pushed.

13e should add connected Runtime loading of FeatureWorld and imported prepared
content, fresh built-in scenes, camera/clipping/winding/visibility/culling/resize,
and visible-entity destruction/reload through these owners. Reuse applicable
13d evidence and close the affected rendering subsystem; do not repeat unrelated
matrices or weaken the temporary-material limitations.

Suggested commit: `feat(render): add depth-tested offscreen scene viewport`.
