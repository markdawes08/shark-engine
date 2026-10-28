# ADR 0032: Shared Camera and MeshRenderer components

Date: 2026-10-27. Status: accepted for assignment 13a.

Camera and MeshRenderer are optional, version-1 Core values on the existing Scene
owner. Their stable schema IDs are 3 / `shark.camera` and 4 / `shark.mesh_renderer`.
Name and Transform remain mandatory. The [component contract](../render-components.md)
defines fields, validation, queries and source representation.

These fields describe authored presentation. They grant no gameplay authority and
are not added to network replication or durable character records. A headless
Core/Server may store the values without creating a renderer or loading assets.
The entity Transform remains the only pose; world/cell identity stays separate.
Camera aspect follows the eventual viewport rather than duplicating a mutable
viewport size in persistent state.

MeshRenderer references a stable asset ID and a mesh index within that asset.
Move the existing asset identity type into Core, retaining `shark::tools::AssetId`
as an alias to the same type. No asset importer, catalog or loader enters Core.
The value validator cannot establish asset availability or mesh-index resolution;
13b owns those checks. A required asset has no fabricated default or nil sentinel.

One optional setter per component handles add, update and removal. All immediate,
deferred, candidate and authored operations use the same validation and revision
rules. Extend source capture, activation, history restoration and exact retained
retry matching together. No second component owner or arbitrary plugin registry
is introduced. Human/agent adapters will consume these services in 15/16.

Compatibility: source envelope 1 and existing component versions remain unchanged.
The strict known-component catalog gains two optional version-1 entries. Existing
0/1 sources decode without synthesizing them; files without them encode unchanged.
Older readers explicitly reject either unknown component instead of silently
discarding it. Unsupported new component versions still reject. The legacy-0
shape is unchanged. This is an additive catalog extension, not a reinterpretation
of existing fields. C++ consumers must rebuild; no stable binary ABI is promised.

13a proves CPU ownership/authoring/persistence only. Projection math, resource
resolution and rendering retain the [later 13 checkpoints](../roadmap/scene-rendering-plan.md).
