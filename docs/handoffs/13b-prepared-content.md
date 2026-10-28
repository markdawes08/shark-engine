# 13b: Prepared runtime content and built-in geometry

Date: 2026-10-27. Base: `e5fe852`, initially clean `main`.
**13b is PASS under current-machine scope.** Full REQ-13 remains NOT_RUN;
next is **13c, scene extraction, cameras, bounds and frustum culling** in the
[five-checkpoint plan](../roadmap/scene-rendering-plan.md).
The user owns staging, commits and pushing.

## Delivered

Core now provides bounded prepared mesh/scene codecs, immutable asset snapshots
and complete scene loading using its existing Scene owner. The new independently
versioned `SHKMESH1` and `SHKSCNE1` formats have explicit little-endian fields,
body digests and validated counts/references. They contain engine values rather
than raw glTF, source JSON, handles or authoring history. Existing import/cache
and source formats remain unchanged. Follow [ADR 0033](../architecture/0033-prepared-runtime-content.md)
and the [prepared content contract](../prepared-content.md).

Asset snapshots contain deterministic unit cube/plane geometry at stable reserved
IDs, with no files or downloads. Additional mesh payloads are validated completely
before immutable publication; duplicate/reserved/mismatched IDs and resource bounds
reject. Scene references share const asset ownership. Existing references survive
entity destruction and explicit replacement; failed candidates leave old owners
usable. Loaded scenes preserve authored IDs, hierarchy, component values and the
separate world/cell anchor, but always receive a fresh runtime incarnation.
Every referenced mesh is checked, including invisible ones.

Three pure Tools bridges prepare cached glTF meshes, captured authored scenes and
imported hierarchies with explicit caller-provided node IDs. Trusted callers select
fresh, correctly identified and audience-permitted cache artifacts through the
existing services. Preparation does not imply cache freshness or permission from
a payload span. Imported data cannot replace built-ins. The temporary unlit profile
retains all six vertex attribute arrays, base-color factor and double-sided state;
textures, PBR and alpha-mode rendering remain outside this profile. Full import
data stays in the original cache for later material/cooking owners.

FeatureWorld retains four entity IDs and its global anchor. Its saved renderer now
references the built-in cube instead of the earlier unresolved placeholder. The
fifteenth native case independently checks cube/plane topology, prepared saved
scene loading, an imported shared/mirrored triangle hierarchy, factor/culling
values, lifetime/reload and missing/corrupt content rejection. The authoring owner
is unchanged by that case. Coverage maps 54 CPU surfaces and one external GPU
command; the process supervisor now has 33 checks. Earlier workloads remain intact.

## Selected verification

Applied the [testing policy](../testing-policy.md), using existing runners and
ordinary logs/JUnit under `out/13b-work/`.

| Check | Result |
| --- | --- |
| Windows MSVC Debug | 8/8 CTests, 18.98 seconds: prepared assets/scenes/Tools bridge, existing glTF asset codec, FeatureWorld, process discovery, scene qualification and scene execution |
| WSL GCC Debug | 4/4, 0.66 seconds: the three prepared-content families and FeatureWorld |
| WSL Clang ASan/UBSan | Same 4/4, 2.05 seconds; leak detection and halt-on-error enabled |
| Protected consumers | Native `runtime-only` Runtime and `server-only` Server configure/build/boundaries pass; Tools/tests are OFF and no JSON/cgltf/stb source population exists in either protected build |
| API / repository | New Core-only public example compiles without a Tools include path; contract/link/repository checks, C++ formatting and diff checks pass |

These are **16 passing profile/test results**. Six new asset, six new scene and
five new preparation cases use three CTest families, each with seed 1337 and a
60-second timeout. Tests include independently authored scene bytes, digest-valid
malformed records, finite/value/topology/reference rejection, input/decoded limits,
4096-entity/256-depth bounds, literal built-in IDs/bounds/winding and shared lifetime.
Snapshot byte charges are logical owned values, not allocator/transient overhead
or global process memory; caller-held old snapshots have separate lifetimes.

Native commands use `tools/windows_dev.ps1`, the recorded `windows-debug`
inventory and explicit selections. `windows-debug.xml`, `windows-tests.log`,
`windows-build.log`, `tests-build-final.log`, both protected `*-only-build.log`
files and `public-example.log` retain results. WSL reuses the retained Debug and
Clang sanitizer client build directories; `wsl-commands.sh` records the four-test
selections and `wsl/{debug,sanitizer}.xml` retains their results.

The first GCC compile stopped on copied structured-binding loops in new tests.
Reference bindings fixed those warnings without flag suppression; original logs
are retained as `wsl/*range-loop-failure.log`. Both test files were rebuilt before
the final native selection. No runtime test failed. No host/WSL settings changed.

No GPU tests, full network/storage matrix or package redeployment ran. This is
pure CPU loading/preparation with no install, Runtime launch, shader, lock or DLL
changes. Protected builds validate absence of source importers; application file
loading and graphical acceptance remain 13e, export/relocation remains 14, and
final cooking remains 31. No historical package result is counted fresh.
`.gitignore` was reviewed: existing build/out/cache/local-roadmap rules cover all
generated prepared payloads and verification outputs. Nothing was staged.

13c should consume these immutable assets and the existing Core Scene to derive
scene-local render records, camera view/projection, mesh/world bounds and frustum
culling. Resolve references introduced by post-load Core mutations again; loading
does not permanently validate future edits. Keep the global anchor distinct from
local matrices. Actual draw/resource lifetimes and resize belong to 13d/13e.

Suggested commit: `feat(content): add prepared runtime assets and built-in geometry`.
