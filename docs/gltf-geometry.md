# Static glTF geometry and hierarchy

Owner: 12b / REQ-12. Follow [ADR 0029](architecture/0029-bounded-gltf-geometry.md),
the [five checkpoints](roadmap/gltf-plan.md) and [source contract](gltf-source.md).
`convert_gltf_geometry` is a pure CPU EngineTools operation with error operation
`asset.gltf.convert`. Image decoding, material payloads, cache publication and
rendering retain their owners in 12c/12d/13/26.

## Ownership and selection

Supply the same immutable `GltfSourceInput` bytes, explicit external path/AssetId
snapshots and stop token used by inspection. All borrowed storage stays immutable
and alive for the call. Complete 12a source validation precedes conversion and
retains every original limit, URI policy and allocation cleanup guarantee.

`GltfGeometrySettings::scene` optionally chooses an existing source scene. Without
it, use the declared default, then scene zero. A document without scenes returns
empty geometry. Loose nodes and meshes outside the selected scene are not
implicitly imported. Attribute/transform conversion checks apply to reachable
content; the complete source still passes storage/reference/graph checks.

The result owns all values. `source` retains the original inspection report,
including its source default; `selected_scene` records the effective conversion
choice. No pointer into caller buffers or third-party structures escapes.
No files, Scene, asset catalog/cache, editor, window or GPU are accessed.
Native snapshot capture and later publication stay with their existing owner.

Nodes follow depth-first source root/child order, with parents before children.
`parent`, `children`, `roots`, node `mesh` and mesh `primitives` are checked dense
indices into this result. Explicit `source_node`, `source_mesh`,
`source_primitive` and optional `source_material` preserve source correspondence.
Each reachable mesh is stored once, ordered by source index, even when many
nodes instance it. These indices are not persistent AssetIds or runtime handles.
Names are owned UTF-8 strings bounded to 4096 bytes.

## Vertex and topology profile

Vertices contain finite single-precision arrays, with independent position,
normal, tangent, UV0, UV1 and linear color values. They are CPU values, not a
serialized or GPU-packed layout. Positions/normals/tangents use core floating
accessors. UVs and colors also accept normalized unsigned bytes/shorts. UVs have
two components; colors have three or four, with implicit alpha one. Missing color
is white, missing UV storage is zero, and presence flags distinguish absent data.
Colors must remain in [0,1]. Core direction inputs have unit length within 1e-4,
then normalize; tangent XYZ is orthogonal to the normal within 1e-4 and W is +/-1.
Integer base vertex views require an explicit four-aligned stride; all base
vertex offsets are four-aligned. Sparse values use their separately checked
tightly packed source layout. Unsupported vertex semantics reject; `_` custom
attributes are explicitly diagnosed and omitted.

Decode effective sparse overrides before validating values, including a zero
base when no bufferView exists. Repaired invalid base bytes are never consumed.
All component reads are bytewise little-endian. Interleaved stride/padding,
normalized integer conversion and binary float finiteness are checked.

Triangle lists require a nonzero multiple of three elements. Strips alternate
their winding before expansion; fans keep the first element as their center.
Indices use unsigned scalar values, with the representation's maximum value
reserved. No primitive restart is inferred. Zero-area triangles are discarded
with a diagnostic; original strip parity remains intact. Entirely degenerate
primitives reject. Bounds use the vertices actually referenced by retained faces.

Missing normals split triangle corners and generate flat normals. Supplied
tangents are ignored in that case. A normal map without usable tangents generates
the per-triangle basis described by ADR 0029, using that texture's UV set and
split corners. Constant/degenerate UVs use a deterministic orthogonal fallback
with a diagnostic. `has_tangents` distinguishes an actual basis from default
storage; generation flags identify generated values. Mixed supplied tangent W
signs within one face reject. Local index winding and tangent W never change
because of a particular node's reflection.

Every core material texture link used by a primitive must reference its available
UV0 or UV1; coordinate sets above one reject. This checks geometry requirements
for base color, metallic-roughness, normal, occlusion and emissive usages. Complete
material factors, optional-extension fallback and payload encoding remain 12d.

## Transforms and bounds

Keep right-handed meter coordinates, +Y up, column-major matrices and
`world = parent_world * local`. Source quaternion xyzw is explicitly converted
to Core's constructor order, then canonicalized by its existing validator.
Matrix nodes must decompose into local TRS; local shear, perspective, singular
or scale components below magnitude 1e-6 reject. Negative/nonuniform scale is
preserved. No model-facing-axis guess, world-origin mapping or vertex baking
is performed.

Full double world matrices retain valid hierarchy shear. Core's existing matrix
and inverse checks reject nonfinite, ill-conditioned or unreliable composition.
`normal_matrix` stores nine column-major inverse-transpose linear components.
`mirrored` records the sign of the world basis determinant. Later rendering uses
that flag for culling and tangent handedness, leaving shared local geometry intact.

Primitive/mesh bounds are tight local AABBs over drawn positions. A node's optional
`world_bounds` covers its own mesh, excluding descendants. It is conservative:
transform the eight mesh-bound corners, including shear and reflection. Result
`world_bounds` unions all mesh-instance bounds in the selected scene. Empty
scenes and nodes without meshes have no invented bounds.

## Conversion budgets and errors

| Independent conversion bound | Fixed limit |
| --- | ---: |
| Total output vertices, after corner splitting | 262144 |
| Total expanded triangle indices | 786432 |
| Logical owned output bytes, including metadata | 32 MiB |
| Scalar decode and topology work items | 8388608 |

Precharge the conservative expanded counts before allocation/decoding; degenerate
face removal does not refund the charge. Work charges every primitive's complete
converted vertex attributes, original index elements and expanded index elements.
Many references to the same accessors cannot multiply unbounded conversion work.
Mesh instances do not repeat conversion. Metadata, names and fixed diagnostic
capacity count against output bytes. Logical byte limits are not whole-process
RSS claims: bounded parsing and temporary vertex/index buffers remain separate.
All earlier source/accessor/parser limits stay unchanged. Cache payload limits
remain unchanged and are separately enforced when integration lands in 12d.

Graph walks are iterative. Cancellation is cooperative and polled between phases
and at most 256 vertex/topology/node iterations. Failure returns owned Error
values with the operation, source path and available mesh/primitive/node context.
No partial result or cache artifact is published. Parser failure injection uses
the existing private arena boundary and verifies zero live allocations.

## Public CPU example

```cpp
#include <shark/tools/gltf_geometry.hpp>

shark::Result<shark::tools::GltfGeometry>
convert_static_scene(const shark::ProjectPath& path,
                     std::span<const std::byte> bytes,
                     std::span<const shark::tools::GltfExternalSource> resources,
                     std::optional<std::size_t> scene) {
    return shark::tools::convert_gltf_geometry({path, bytes, resources}, {scene});
}
```

Offline fixtures and independent expected outputs live in
[the geometry corpus](../tests/fixtures/gltf_geometry/README.md). The eleventh
FeatureWorld case uses this same operation without changing its AuthoredScene.
