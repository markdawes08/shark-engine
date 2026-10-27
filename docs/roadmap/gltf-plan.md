# Assignment 12: static glTF import checkpoints

Date: 2026-10-27. Base: `20b028d`, initially clean `main`. Owner: REQ-12.
The user approved five checkpoints after completing assignment 11. Preserve the
[declared profile](../architecture/0006-v1-profile.md), [MMORPG direction](mmorpg-plan.md)
and [human/agent contract](human-agent-plan.md). Full REQ-12 remains NOT_RUN until
12e closes all gates. Rendering follows in 13; material rendering acceptance
belongs to 26, skeletal animation to M06 and final cooked output to 31.

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

## 12c - Images and textures

Pin and privately integrate the selected PNG/JPEG decoder with checked encoded,
dimension, pixel and decoded-byte limits. Decode embedded/external images;
generate bounded mip chains and preserve filtering/wrap. Color interpretation
belongs to texture usage: one image may serve both sRGB color and linear data.
Normal/data mips need their documented semantic treatment.

Gate: independent expected pixel/color/mip results, sampler behavior, one image
in different roles, malformed/truncated images, missing images and decoder
allocation failures. Keep CPU work independent of windows, editors and GPUs.

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
