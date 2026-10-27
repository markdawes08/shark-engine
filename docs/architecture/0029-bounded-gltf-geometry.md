# ADR 0029: bounded static glTF geometry and hierarchy

Date: 2026-10-27. Status: accepted for checkpoint 12b; execution evidence is
recorded separately in the checkpoint handoff and receipt.
Follow the [static profile](0006-v1-profile.md), [source boundary](0028-bounded-gltf-sources.md),
[five checkpoints](../roadmap/gltf-plan.md) and [geometry contract](../gltf-geometry.md).

## Decision

Extend the existing validated source owner with pure CPU geometry conversion.
The same bounded JSON, resource, accessor and parser checks run before conversion.
Read binary components bytewise, applying effective sparse replacements and
implicit zero bases. Never bind cgltf buffer pointers or use its data readers.
One controlled parser allocation/exception boundary remains in `gltf_source.cpp`.
Existing source inspection and all its bounds keep their semantics.

Return engine-owned vertices, triangle indices, shared meshes, selected nodes,
local TRS, full world matrices, inverse-transpose normals and computed bounds.
Indices into this result are dense output references; explicit source indices
remain separate from persistent AssetIds and future runtime handles. No Scene,
cache, filesystem or device is consulted or changed. Human/terminal adapters
later call this same operation; neither has a separate conversion policy.

Only the selected scene is converted. An explicit scene index overrides the
source default; otherwise use that default, then scene zero. No scenes means
an empty result. Reachable nodes keep root/child source order, with parents before
children. Each reachable mesh is converted once in source-index order and shared
by its instances. Source validation still checks the complete input inventory.

Reuse Core's canonical Transform, matrix decomposition, composition and inverse
validation. Local shear and singular transforms reject. Hierarchies may retain
world shear; mirrored determinant metadata belongs to the node. Preserve local
vertex positions, triangle winding and tangent sign so differently transformed
instances never rewrite shared geometry. Rendering must apply its established
mirror-aware culling and normal/tangent rules when that owner lands.

## Generated attributes

Expand triangle lists, strips and fans into an ordered triangle list. Remove
zero-area faces while preserving original strip parity, and reject an entirely
degenerate primitive. Missing normals generate flat face normals and split
corners. Source tangents without source normals are ignored.

For a normal-mapped primitive without usable tangents, split corners and generate
per-triangle UV derivatives. Project the tangent against each corner normal,
normalize it and derive its sign from the bitangent. Use the normal texture's
UV set. A degenerate UV basis uses the least-aligned coordinate axis and emits
a bounded diagnostic. This is Shark's documented per-triangle tangent basis;
MikkTSpace equivalence is not a compatibility promise. Baker-specific smooth
tangent bases can be supplied explicitly in `TANGENT`. Final PBR image acceptance
remains assignment 26.

## Bounds and compatibility

Charge output vertices, indices, metadata bytes and scalar/work items before
their associated allocations or loops. Repeated primitive references consume
the same budgets; repeated node instances share mesh storage. Cached local bounds
are tight over drawn vertices. Transform their eight corners for conservative
per-instance bounds, then union those bounds for the selected scene.

The contract fixes independent conversion limits without changing inherited
source limits. It defines an owned C++ result, not a serialized/GPU layout or
cooked format. Material payload serialization and cache publication remain 12d;
their existing payload limits still apply. No source decoder dependency, Core
implementation, server authority or protected dependency policy changes here.

Qualification uses independently expected vertices/indices/transforms/bounds,
licensed offline fixtures, real allocation failures, FeatureWorld, Windows/WSL
CPU/sanitizer profiles and protected packages. Full REQ-12 remains NOT_RUN until
12e. The [Khronos specification](https://raw.githubusercontent.com/KhronosGroup/glTF/main/specification/2.0/Specification.adoc)
is the source-format reference; explicit product policies above define this
bounded conversion profile.
