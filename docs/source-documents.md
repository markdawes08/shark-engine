# Project and scene source documents

Assignment 08a supplies in-memory, device-free value documents in EngineTools.
Its [handoff](handoffs/08a-source-documents.md) records qualification status; full
08 follows the [persistence checkpoints](roadmap/persistence-plan.md).
`<shark/tools/source_document.hpp>` exposes engine-owned values and `Result<T>`;
the JSON implementation is private. No function opens a file or changes a Scene.
08b's [authored Scene owner](authored-scenes.md) connects these values to complete
capture and transactional active-scene replacement. 08c owns filesystem
resolution, save/replace and external-edit conflicts.

## Versions and identity

`.sharkproject` and `.sharkscene` are readable UTF-8 JSON objects with exact
`format` strings `shark.project` and `shark.scene`, and integer `schema_version`.
Version 1 is the production source schema. Decoders accept only 0 and 1 and return
`DecodedSource<T>` with the original version. Version 0 is an explicit legacy
test contract; it is not evidence of an earlier shipped format. Encoders always
write version 1. Source, component, gameplay, database and future authoring
protocol versions remain independent.

ProjectId, SceneId, AuthoredEntityId and WorldId are distinct engine-owned types.
JSON uses Core's canonical lowercase non-nil UUIDv4 representation. IDs survive
renaming, hierarchy changes and serialization. Local handles, region authority,
ownership epochs, network sessions and replica IDs are unsupported source fields.
World/cell content identity never grants gameplay authority.

## Version 1 fields

Every object rejects unknown fields except inside an explicit `extras` object.
Required fields must be present even when a C++ type could otherwise supply a
default. Arrays retain their input order; JSON object key order is not semantic.

| Object | Required fields | Optional fields |
| --- | --- | --- |
| Project | `format`, `schema_version`, `project_id`, `world_id`, `name`, `startup_scene`, `scenes` | `extras` |
| Project scene entry | `scene_id`, `path` | None |
| Scene | `format`, `schema_version`, `project_id`, `scene_id`, `entities` | `world_origin`, `extras` |
| Scene entity | `entity_id`, `components` | `parent`, `extras` |
| Components | `shark.name`, `shark.transform` | None |
| Name | `schema_version` (1) | `value` (default `Entity`) |
| Transform | `schema_version` (1) | `translation`, `rotation`, `scale` (Core defaults) |
| World origin | `world_id`, `cell`, `offset` | None |

Project names use the same Name validator as entities. A project contains 1..256
scene entries with distinct scene IDs and exact path strings; `startup_scene`
must identify one of those entries. Paths use the existing `ProjectPath` lexical
contract, including forward slashes and rejection of absolute paths, traversal,
device/ADS names, malformed UTF-8 and control characters. Decoding does not access
the filesystem, establish actual containment, resolve referenced files or prove
portable filesystem case equivalence; those checks retain 08b/08c.

A scene may be empty. Entity IDs are unique within it. `parent` is absent/null
for roots, otherwise it references another entity in this document. Forward
references are allowed; dangling references, self-parenting, cycles and excessive
depth reject. Parent references cannot cross scene boundaries. The fixed current
component set requires Name and Transform; this is not a generic plugin registry.

Translation and scale are numeric three-element arrays. Rotation is explicitly
`[x,y,z,w]`, converted to the Core quaternion constructor's `[w,x,y,z]` order.
Absent fields use the existing schema defaults. Validation uses Core's pure
component validators, including Name UTF-8/control/length rules and finite,
nonzero, normalized/canonically signed rotation and signed-scale magnitude floor.
Local TRS validation does not prove that every composed world matrix is feasible;
08b validates composition through the Scene owner before activation.

`world_origin` is absent/null for an unanchored local scene. Otherwise it declares
the stable world and cell-plus-offset origin of the authored local frame. Cell
coordinates are exact integers within Core's signed 32-bit coordinate range;
offsets are canonical finite doubles in `[0,1024)`. Negative floating zero rejects
under the Core world-position contract; integer JSON `-0` has integer value zero.
No full-world float conversion, origin shifting or region transfer is performed.
Cross-document project/world consistency is checked when references are resolved
in 08b.

## Legacy version 0 migration

Project version 0 uses `id` instead of `project_id`, `start_scene` instead of
`startup_scene`, and `id` instead of `scene_id` in each scene entry. Its other
fields have the same meaning and validation rules.

Scene version 0 uses `id` instead of `scene_id` and `origin` instead of
`world_origin`. Each entity has `id`, optional `parent`, optional `name` string,
optional `transform` and optional `extras`. There is no `components` object.
Legacy Transform uses optional `position`, `rotation_wxyz` and `scale`; absent
values use Core defaults. Migration explicitly changes the field names, inserts
the Name/Transform component envelopes and versions, and changes quaternion
ordering. It preserves IDs, array order, references, world/cell data and extras.

Each source version is validated against its own exact field set. Mixing legacy
and current fields, unsupported old/future versions or a failed migration rejects
the entire value. Migration is in memory, does not alter the input buffer, and
never saves automatically. Independently authored legacy fixtures live under
`tests/fixtures/source_documents/`.

## Fixed resource and validation limits

| Resource | Maximum |
| --- | ---: |
| Input document / encoded output | 4 MiB each |
| Entities per scene | 4,096 |
| Scene entries per project | 256 |
| JSON container depth, root container counted as 1 | 32 |
| JSON nodes, including containers, scalar values and object keys | 200,000 |
| Decoded JSON string or key | 4,096 UTF-8 bytes |
| Compact encoded extras object | 4,096 bytes each |
| Sum of compact extras objects in a document | 65,536 bytes |
| Sum of raw caller-supplied extras strings in a typed candidate | 4 MiB |
| Authored hierarchy depth, root counted as 1 | 256 |

Duplicate decoded JSON keys reject before a DOM can overwrite them, including
escaped spellings of the same key. The bounded preflight precedes DOM creation.
Strict parsing rejects trailing documents, comments, truncated input, invalid
UTF-8/surrogates, NaN/infinity, nonfinite numeric overflow and integer tokens
outside signed/unsigned 64-bit representation. Boolean and floating-point values
are not accepted as integer schema versions or cell coordinates.
Integer values use signed/unsigned 64-bit representation; other JSON numbers use
binary64 floating point, including inside extras. Arbitrary decimal precision is
not preserved.

`extras_json` is an owned opaque JSON object string in the C++ value API. Only
explicit extras are preserved; unsupported source fields are never silently
dropped. Validation canonicalizes extras using compact JSON while retaining
their values. Caller-supplied raw extras strings share a 4 MiB aggregate budget
checked before parsing each object; compact per-object and aggregate limits still apply. These are bounded
work/input rules, not a process RSS limit or a hostile-code sandbox.
Default empty `{}` objects count toward the raw and compact aggregate budgets.

Encoders revalidate caller-owned documents and apply the same limits. They emit
deterministic object ordering, two-space indentation and a trailing newline,
while preserving array order. Formatting and quaternion canonicalization may
change source bytes. Output is bounded during serialization; over-budget output
fails without publishing a partial result. Typed validation checks field values,
references and extras limits; successful encoding additionally proves that
escaping and indentation fit the output budget. Source value validation/copying is
distinct from live-scene mutation and disk commit.

Errors use owned Core `Result<T>`/`Error`, stable operations
`source.project.decode`, `source.scene.decode`, their `.validate` and `.encode`
counterparts, and JSON-pointer `context.path` (empty for the root). Syntax,
version, shape and reference errors use `invalid_configuration`; ID/path/component
errors retain the relevant Core code. Allocation/execution failures are distinct.
Known entity IDs can be included in context. A field pointer is a diagnostic
location, not a filesystem path or an authoring command endpoint.

## Build and usage boundaries

`SHARK_BUILD_TOOLS` can build EngineTools independently of Editor and Client.
The `tools-debug` preset provides CPU authoring tests with those executables and
native graphics disabled. The source codec links privately to the exact pinned
nlohmann/json input; JSON headers and definitions do not escape its implementation.
Core, Runtime and Server do not fetch or compile it. Protected runtime-only and
server-only configurations keep authoring disabled. The standalone
`SourceDocumentTests` executable uses the existing Catch2/CTest foundation.

```cpp
#include <shark/tools/source_document.hpp>

shark::Result<std::string> migrate_scene_text(std::string_view input) {
    auto decoded = shark::tools::decode_scene_document(input);
    if (!decoded) {
        return decoded.error();
    }
    return shark::tools::encode_scene_document(decoded.value().document);
}
```

This CPU API is shared groundwork for human and terminal development. It is not
a CLI/editor adapter, undo service, active-scene loader, save command, cooked
runtime format or live player database. Those layers retain their assigned owners.
