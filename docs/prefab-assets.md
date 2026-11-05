# Prefab source assets

21a adds a pure EngineTools codec in
[`prefab_document.hpp`](../engine_tools/include/shark/tools/prefab_document.hpp).
It follows [ADR 0057](architecture/0057-prefab-source-assets.md) and the
[prefab checkpoints](roadmap/prefab-plan.md). The codec returns owned values or
contextual `Result` errors and performs no file I/O or active Scene mutation.

## Format and identity

`.sharkprefab` uses UTF-8 JSON with `format: "shark.prefab"` and integer
`schema_version: 1`. Only version 1 is supported; there is no legacy prefab schema.
Project, scene and component versions remain independent. Unknown fields reject
outside the optional bounded `extras` objects.

| Field | Meaning |
| --- | --- |
| `project_id` | Owning ProjectId, a non-nil UUIDv4. |
| `asset_id` | Stable AssetId for this prefab source. |
| `root_entity` | ID of the one parentless entity in this document. |
| `entities` | Nonempty array using the existing [source entity schema](source-documents.md#version-1-fields). |
| `extras` | Optional opaque JSON object. |

All IDs use the existing validated UUID representation. `entity_id` and `parent`
are local to this prefab's namespace. Parent references may point forward in the
array and must resolve within this document. Duplicate IDs, missing roots,
additional roots, dangling parents, cycles and excessive hierarchy depth reject.
The entity array keeps its input order; the root need not be first.

21b's [instance command](prefab-instances.md) allocates fresh IDs for every entity
and remaps every typed internal reference. Asset IDs and project-relative script source paths are
external content references, not prefab-local entity references. Arbitrary strings
inside extras are preserved and never interpreted or remapped. The current
component set has no general script entity-reference property.

The source's AssetId must match its adjacent `.sharkasset` identity when a later
file/catalog workflow connects it. The pure codec cannot establish that match or
resolve an asset, script file, world, live region or authenticated player.

## Components and limits

The codec shares source entity parsing, canonical encoding, hierarchy checks and
pure component validation with scene documents. Every entity requires Name and
Transform; optional Camera, MeshRenderer, RigidBody and Script retain their
existing versions and constraints. Script references remain inert. A rigid body
must be on the root and have a valid positive uniform local scale; child rigid
bodies reject under the existing [body contract](physics-bodies.md).

Existing source limits apply: 4 MiB input and encoded output, 4,096 entities,
hierarchy depth 256, JSON depth 32, 200,000 JSON nodes, 4,096 bytes per decoded
string and extras object, and 65,536 aggregate extras bytes. Caller-owned value
validation bounds raw extras before parsing or copying their contents. Duplicate
decoded JSON keys, malformed UTF-8, nonfinite numbers and unsupported component
versions reject. Errors include the operation, JSON pointer and known entity ID.

`decode_prefab_document` returns the validated document and original version.
`validate_prefab_document` returns a validated, canonicalized owned copy.
`encode_prefab_document` revalidates caller-owned data and emits deterministic
two-space JSON with a trailing newline. These calls preserve input values.
As with scene source validation, composed world-matrix feasibility belongs to
the later Scene owner before publication.

## Deliberate first-version limits

Nested prefab references, prefab components, inherited variants and stored
instance overrides have no fields in this format and reject as unknown input.
An extras key is inert metadata and cannot enable any of those features.
21b defines simple Name/local Transform overrides in the separate instance
request; source files are not silently rewritten when an instance changes.

The source codec itself performs no instantiation. 21b adds instance preparation
and shared authoring commands. Source-file publication, cache import, terminal
and Editor prefab operations, Lua spawn/destroy and prepared runtime prefab
payloads retain their subsequent owners. Runtime and Server retain their source
parser exclusion; this addition changes no existing scene, cooked, wire or
database format. Prefab identity does not confer authority or create durable
gameplay entities or rewards.
