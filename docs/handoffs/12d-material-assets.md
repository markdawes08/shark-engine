# 12d: Materials and asset integration

Date: 2026-10-27. **12d is PASS under the current-machine scope.** Base: `5afe06e`,
initially clean `main`. Changes are unstaged/uncommitted; the user owns staging,
commits and pushing. **Next: 12e, integrated glTF qualification.** Full REQ-12
remains NOT_RUN until that checkpoint closes.

Follow [ADR 0031](../architecture/0031-gltf-material-assets.md), the
[asset contract](../gltf-assets.md), [checkpoint plan](../roadmap/gltf-plan.md) and
[selected test results](../coverage/evidence/12d-material-assets.json).

## Delivered

Core metallic-roughness conversion preserves factors, five texture/UV roles,
normal scale, occlusion strength, emissive, alpha mode/cutoff and double-sided
state. Selected explicit materials retain source indices; omitted materials use
core defaults. The existing source owner supplies reachability and UV checks.
Usable PNG/JPEG core fallback survives ignored optional compressed-image sources,
while all core image references retain their original validation.

The version-1 engine-owned payload contains converted geometry, hierarchy,
transforms, material values, semantic mip bytes and source correspondence.
Bounded binary readers/writers validate versions, lengths, digests, finite values
and cross-references. Existing 2 MiB artifact / 16 MiB closure limits remain;
pure CPU conversion can legitimately exceed the smaller serializable limit and
then fail import without replacing last-valid output.

`GltfAssetImporter` and identity-only `GltfResourceImporter` register with the
existing AssetImports owner. Dependency views expose captured descriptors and
original source digests. Typed resource identity/digest validation rejects the
older potentially transformed byte envelopes. Normalized settings explicitly map
logical URI paths to direct dependency AssetIds. These are aliases, not native
file requests; actual bytes still come from catalog sources through ProjectFiles.
Coordinated physical moves retain IDs and keys when logical settings stay equal.

Preparation, synchronous publication and worker jobs retain existing source,
metadata, catalog/cache revisions, audience restrictions, cancellation and
last-valid-output behavior. No new worker, file resolver, Scene mutation,
Core/Server dependency or runtime renderer is introduced. The dependency lock,
shader inputs and package rules are unchanged. `.gitignore` continues to contain
generated payloads/scratch under the existing build/out boundaries.

The thirteenth native FeatureWorld case independently verifies material factors,
triangle/node values and deterministic codec behavior without changing its four
saved entities. Coverage maps 47 CPU surfaces plus the existing external GPU
command. The three new Catch families contain 14 material, 12 codec/importer and
15 owner-integration cases, with seed 1337 and 60/90/90-second limits.

## Verification

Selection follows the [testing policy](../testing-policy.md), rather than the
older repeated full-profile matrices:

| Profile | Passing selected results |
| --- | ---: |
| Windows MSVC Debug | 15 |
| Retained WSL GCC standalone Tools Debug | 4 |
| Retained WSL Clang ASan/UBSan | 9 |

The 28 passing results are profile executions, not 28 new unique tests. Windows
covers the six glTF families, import and job behavior, synchronous/asynchronous
interruption fixtures, FeatureWorld/native-process/coverage qualification and two
dependency-boundary checks. WSL Debug runs the three new families and FeatureWorld;
the sanitizer selection additionally includes source/geometry/textures and the
existing import/job families. All keep original limits. Source-document dependency
qualification passed in 185.87 seconds at its unchanged 240-second limit.

Windows' first 14-test batch had thirteen passes and one fixture failure: the
FeatureWorld process test still expected twelve final revisions. Extending that
independent list with the thirteenth case's unchanged revision fixed it. Only
that process test was rerun, and it passed all 31 checks. The failed batch remains
in `out/12d-work/native-tests.xml` and `.log`; the passing retry is
`out/12d-work/featureworld-process-fixed.xml` and `.log`. The other Windows result
is `out/12d-work/dependency-boundary.xml` and `.log`.

Existing import-process checks retain 59 synchronous and 113 asynchronous checks.
Scene qualification retains 19 checks and 15 rejection controls. The new public
example compiles under MSVC using public headers only. Changed C++ formatting,
repository guidance/ignore boundaries and contract/link validation pass.

The existing native development wrapper builds selected targets and runs CTest
in `build/windows-debug`; commands/results are in `out/12d-work/native-build-final.log`
and `native-tests.log`. WSL reuses `/work/build-09a-tools` and
`/work/build-05a-final-client/linux-asan-ubsan` in the retained userspace.
`out/12d-work/wsl-commands.sh` records the selection, `wsl/driver.log` the build/run
output, and `wsl/debug.xml` / `wsl/sanitizer.xml` the results. No host/WSL settings
changed. No bespoke source-freeze or qualification framework was created.

Unrelated full client/Server suites, all-example compiles, package deployments
and GPU runs were not repeated: their implementation/lock/install/graphics inputs
are unchanged, and the affected dependency boundary passes. These omissions are
not fresh passes. 12e still owns broader integrated glTF qualification; rendering,
adapters, cooked distribution and animation retain their later owners.

Suggested commit: `feat(tools): integrate glTF materials and asset payloads`.
