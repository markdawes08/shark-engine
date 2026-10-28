# 13c: Scene extraction, cameras, bounds and culling

Date: 2026-10-28. Base: `1396d19`, initially clean `main`.
**13c is PASS under current-machine scope.** Full REQ-13 remains NOT_RUN;
next is **13d, depth-tested offscreen rendering and viewport resize** in the
[five-checkpoint plan](../roadmap/scene-rendering-plan.md).
Staging, commits and pushing remain with the user.

## Delivered

Core now extracts one checked committed Scene revision into owned render records
with shared immutable prepared-mesh leases. Explicit camera, viewport and origin
inputs work equally in Runtime and headless Tools. No Scene pointer, source
importer or graphics dependency escapes into the result. Post-load gameplay mesh
edits are resolved again, including hidden references. Failure returns no partial
snapshot and changes no Scene, pending writes or revision. Follow
[ADR 0034](../architecture/0034-scene-extraction.md) and the
[extraction contract](../scene-rendering.md).

Perspective/orthographic matrices follow the existing RH -Z, finite Vulkan depth
[0,1] and one-Y-flip conventions. Camera/model shear and reflection survive;
normal matrices use inverse-transpose and raster parity combines camera/model
reflection. Complete mesh AABBs are cached once per extraction, transformed with
outward rounding and conservatively tested against six frustum planes. Plane
construction uncertainty preserves contact after translated far-plane cancellation.
Numerically collapsed finite projections reject rather than silently changing
their clip distances.

Scene-local render offsets remain separate from the retained global world/cell
anchor. Local mesh positions, relative translations and transformed bounds stay
within +/-65536 meters. Snapshot origin generations are caller-owned. Extraction
admits 4096 entities, scans at most 65536 query slots using precharged 256-slot
pages plus one direct camera lookup, and bounds distinct-mesh vertex work.
Matrices remain doubles; checked GPU packing belongs to 13d. No streaming,
rebasing, LOD, animation, renderer submission or capacity claim is introduced.

FeatureWorld adds its sixteenth native case through a separate prepared Scene
owner: independent camera/bounds/normals, reflection, global/local separation,
culling, hidden invalid references and snapshot lifetime after reload. The four
saved IDs, source scene and original authoring revision are unchanged. Coverage
now maps 55 CPU surfaces and one external GPU command, with 34 process checks.
Existing cases, budgets, seeds and timeouts remain intact.

## Selected verification

Applied the [testing policy](../testing-policy.md), reusing canonical builds,
existing runners and ordinary logs/JUnit under `out/13c-work/`.

| Check | Final result |
| --- | --- |
| Windows MSVC Debug | 8/8 CTests, 20.77 seconds: render-math, scene-render, prepared-assets, prepared-scene, FeatureWorld, FeatureWorld process, scene qualification and scene execution |
| WSL GCC Debug | 3/3, 0.73 seconds: render-math, scene-render and FeatureWorld |
| WSL Clang ASan/UBSan | Same 3/3, 2.09 seconds; leak detection and halt-on-error enabled |
| Protected consumers | Final native `runtime-only` Runtime and `server-only` Server builds/boundaries pass, with Tools/tests OFF and no JSON/cgltf/stb source population |
| Public API / repository | Core-only example compiles without Tools headers; contract/link/manifest, repository ignore/discovery, formatting and diff checks pass |

These are **14 passing final profile/test results**, not 14 unique families.
Two new 60-second CTest families use seed 1337 and contain nine math plus nine
extraction cases. Independent expectations cover clip endpoints/aspect/Y, all
frustum planes/contact, affine shear/mirror normals and bounds, explicit origins,
reference/revision/thread guards, deferred committed reads, retained leases and
4096-entity paging/limit rejection. A translated/rotated camera with a 60000-meter
far plane preserves contact at render zero while still rejecting points 0.01
meters outside; the regression checks the computed uncertainty stays below 1e-6.

Review corrected the initial whole-Scene stats scan/page-budget overrun and added
frustum construction uncertainty before final verification. Earlier WSL tests
had passed; affected selections were rebuilt/rerun after those concrete changes.
No compiler or runtime test failed. No host/WSL settings changed.

Native commands use `tools/windows_dev.ps1`, actual inventory and the explicit
eight-test regex in the logs. Final results are `windows-debug.xml`,
`windows-tests.log`, `windows-build-final.log`, `*-only-build-final.log` and
`public-example-final.log`. `wsl-commands.sh` records the three-test selections
and retained build directories; `wsl/{debug,sanitizer}.xml` and matching logs
contain final portable/sanitizer results. Initial successful build logs are not
additional qualification counts.

No GPU runs, package redeployment or unrelated network/storage/full-project
matrix ran. This checkpoint changes pure CPU extraction with no new dependency,
install, DLL, shader, file format or application launch behavior. Protected builds
exercise the changed Core target; old package/GPU results are not counted fresh.
`.gitignore` was reviewed: existing build/out/cache/local-roadmap rules cover all
generated outputs, with source and fixtures still eligible for version control.
Nothing was staged, committed or pushed.

13d should consume these snapshots through the existing EngineClient
device/resource/shader owners, check GPU matrix/vertex packing and draw budgets,
draw depth-tested static meshes to a resizable offscreen viewport, and qualify
depth/winding/visibility/resize on both current-PC GPUs. Keep temporary unlit
material limitations explicit. Runtime file launch and connected imported-scene
acceptance remain 13e; export/cooking remain 14/31.

Suggested commit: `feat(render): add scene extraction, cameras and frustum culling`.
