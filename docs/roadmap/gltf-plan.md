# Assignment 12: static glTF import checkpoints

Date: 2026-10-27. Base: `20b028d`, initially clean `main`. Owner: REQ-12.
The user approved five checkpoints after completing assignment 11. Preserve the
[declared profile](../architecture/0006-v1-profile.md), [MMORPG direction](mmorpg-plan.md)
and [human/agent contract](human-agent-plan.md). **All five checkpoints and full REQ-12 are PASS** under current-machine scope;
see the [12e handoff](../handoffs/12e-gltf-qualification.md).
The per-checkpoint records below preserve their original scope. Rendering follows in 13; material rendering acceptance
belongs to 26, skeletal animation to M06 and final cooked output to 31.

**Testing amendment, 2026-10-27:** the [testing policy](../testing-policy.md)
supersedes earlier per-checkpoint matrix and evidence-process instructions.
12a-12c results remain historical. For 12d, run the new material/resource tests
and affected source/geometry/texture, import/cache/job and FeatureWorld regressions
in the primary Debug build, affected sanitizer tests and an affected WSL
build/smoke. Include stale completion, cancellation and last-valid-output cases
because their owner integration changes. Compile new/affected public examples.
Use relevant dependency boundary checks if targets or locked inputs change;
do not automatically rerun full Server suites or redeploy unrelated packages.
12e owns the broader applicable glTF integration matrix and package checks.
No behavioral workload, fixture, seed, timeout or product boundary is weakened.

## 12a - Import foundation

Pin the private cgltf parser and define concrete input/parser/resource limits.
Implement pure CPU inspection of `.gltf`/`.glb` and explicitly supplied immutable
external resources. Validate containers, source versions, extension policy,
portable URI resolution, buffer/accessor storage and bounded sparse indices.
Return owned counts, resource references/digests and diagnostics. Define shared
reference/texture-role rules before later conversion. No filesystem callback,
public raw-file bypass, cache publication or converted-geometry promise.

Gate: licensed local positive/negative fixtures, malformed arithmetic/containers,
duplicate JSON/mappings, missing resources, contained parent references versus
root escapes, source/JSON/parser bounds, cancellation and allocator failure
cleanup. Extend FeatureWorld and independent coverage. Verify Windows/WSL,
sanitizers, standalone Tools, public examples and protected dependencies/packages.
Pin image decoding when its implementation first lands in 12c.

**Status: PASS under current-machine scope.** The
[handoff](../handoffs/12a-gltf-foundation.md) and
[receipt](../coverage/evidence/12a-gltf-foundation.json) record 486 selected passing
outcomes, 42 public-example compiles, 30 sanitizer objects and protected packages.
Preserve 41 source cases with at least 1,794 assertions and all earlier workloads.
Full REQ-12 remains NOT_RUN pending 12b-12e. Next: 12b.

## 12b - Geometry and hierarchy

Convert the selected/default static scene into engine-owned geometry and node
data. Implement indexed/nonindexed triangles, deterministic strip/fan expansion,
sparse accessors, positions/normals/tangents, UV0/UV1 and COLOR_0. Generate flat
normals and the documented tangent basis when required. Validate referenced UVs,
TRS/decomposable matrices, negative/nonuniform scale and finite vertex data.

Gate: independent expected vertices/indices/transforms/bounds, sparse expansion,
winding/tangent handedness, matrix packing, unsupported modes and malformed data.
Reuse 12a bounds and captured bytes; renderer/Scene mutations remain later owners.

**Status: PASS under current-machine scope.** The
[handoff](../handoffs/12b-gltf-geometry.md) and
[receipt](../coverage/evidence/12b-gltf-geometry.json) record 357 fresh outcomes,
45 public-example compiles, 32 actual sanitizer objects and protected packages.
Preserve 45 geometry cases/988 assertions, all 41 source cases/1,794 assertions
and every earlier workload/limit. The [geometry contract](../gltf-geometry.md)
and [ADR 0029](../architecture/0029-bounded-gltf-geometry.md) fix shared instances,
transform/bounds semantics and the documented per-triangle generated tangent basis.
The completed image/texture checkpoint is recorded below; full REQ-12 remains NOT_RUN.

## 12c - Images and textures

Pin and privately integrate the selected PNG/JPEG decoder with checked encoded,
dimension, pixel and decoded-byte limits. Decode embedded/external images;
generate bounded mip chains and preserve filtering/wrap. Color interpretation
belongs to texture usage: one image may serve both sRGB color and linear data.
Normal/data mips need their documented semantic treatment.

Gate: independent expected pixel/color/mip results, sampler behavior, one image
in different roles, malformed/truncated images, missing images and decoder
allocation failures. Keep CPU work independent of windows, editors and GPUs.

**Status: PASS under current-machine scope.** The
[handoff](../handoffs/12c-gltf-textures.md) and
[receipt](../coverage/evidence/12c-gltf-textures.json) record 507 fresh outcomes,
48 public-example compiles, 36 actual sanitizer objects and protected packages.
The 231 full client Debug, 140 Tools selections and 136 fresh full Server
outcomes are distinct; standalone Tools 56 is included in 140. Follow
[ADR 0030](../architecture/0030-bounded-gltf-textures.md), the
[texture contract](../gltf-textures.md) and [decoder review](../dependencies/stb-image.md).
Preserve 43 texture cases/9,022 assertions, source 41/1,794, geometry 45/988,
all independent budgets and every earlier workload/limit. FeatureWorld retains
twelve native cases, 30 process checks, 43 CPU surfaces and one external GPU command.
Full REQ-12 remains NOT_RUN; 12d is complete below and 12e is next.

## 12d - Materials and asset integration

Assemble core metallic-roughness materials, factors, texture/UV references,
normal scale, occlusion strength and emissive data. Preserve OPAQUE/MASK/BLEND,
alpha cutoff and double-sided fields in independently versioned engine payloads.
Connect accepted source resources through the existing import/cache/job owner,
with exact settings/dependency identities and original publication fences.

External resource payloads need an explicit identity-only contract. Existing
ByteAssetImporter bodies may be transformed; stripping their header cannot
establish original source truth. Choose and test the typed resource boundary
before registering a glTF importer. Callbacks never open files or mutate scenes.

Gate: material defaults/links/serialization, dependency edits/reuse, audience
privacy, bounded async work, stale completion, cancellation and last-valid output.
Qualify optional-extension core fallback, including a usable core PNG/JPEG texture
whose unsupported extension references a separate extension-only compressed image.
Such ignored extension content cannot defeat an otherwise valid core fallback.

**Status: PASS under current-machine scope.** The
[handoff](../handoffs/12d-material-assets.md) and
[selected results](../coverage/evidence/12d-material-assets.json) record 28 passing
profile results under the testing policy, plus the new public example. Follow
[ADR 0031](../architecture/0031-gltf-material-assets.md) and the
[asset contract](../gltf-assets.md). The new 14/12/15 Catch cases preserve material
values, bounded engine payloads and real typed-resource cache/job integration.
Thirteen native cases map 47 CPU surfaces and one external GPU command.
Full REQ-12 remains NOT_RUN. Next: 12e.

## 12e - Integrated qualification

Exercise the complete declared static profile through licensed offline fixtures,
FeatureWorld and existing owner/job/cache paths. Refresh relevant Windows/WSL
CPU/sanitizer and package evidence, public examples and actual coverage mappings.
Preserve earlier workloads, seeds and timeouts. Independent local validation is
an additional oracle; it never replaces engine checks or downloads during tests.

Gate: accepted embedded/external scenes, missing resources, required-extension
errors/core fallback, dependency reimport/moves/reopen, interrupted/failing work
and retained valid output. Full REQ-12 passes only after every checkpoint. Reuse
canonical builds/archive caches and clean successful temporary deployments.

**Status: PASS under current-machine scope.** The
[handoff](../handoffs/12e-gltf-qualification.md) and
[results](../coverage/evidence/12e-gltf-qualification.json) close full REQ-12.
262 passing profile-test results cover full Windows/WSL Debug inventories,
selected Release/sanitizer and standalone Tools paths. Protected packages and
12 positive/three negative independent oracle checks pass. The existing import
family grows to nineteen cases; the new 90-second process family retains fifty
checks/sixteen children/three interruption phases. All earlier cases/limits remain.
Corrected fixture bounds and the stale FeatureWorld supervisor count are recorded.
Next: assignment 13, basic scene rendering and cameras; plan checkpoints first.
