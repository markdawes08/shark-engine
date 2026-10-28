# Component schemas and bounded scene inspection

07c adds CPU discovery and inspection to the existing EngineCore scene owner.
The public contracts are [schema.hpp](../engine/core/include/shark/schema.hpp),
[scene_query.hpp](../engine/core/include/shark/scene_query.hpp) and
[scene.hpp](../engine/core/include/shark/scene.hpp). They use engine-owned values
and the documented public math types; callers need no EnTT access, editor,
viewport, display or GPU. The [scene ownership contract](scene-contracts.md) and
[Name/Transform contract](transforms.md) still govern identity and mutation.

These are shared C++ foundations for human and terminal development. File
persistence, authoring transactions, FeatureWorld, terminal/protocol adapters and
editor integration retain their owners in 08, 09, 10, 15 and 16. Discovery does
not create a gameplay protocol, authorize a player request or establish a
replication policy.

## Schema discovery and validation

`component_schemas()` returns the four supported component schemas in stable ID
order. `component_schema(id, version)` and
`property_schema(component, property, version)` return checked owned copies.
Changing a returned string, constraint or default cannot alter later discovery
or engine validation. Name and Transform are mandatory and not removable;
[Camera and MeshRenderer](render-components.md) are optional and removable under
[ADR 0032](architecture/0032-render-components.md).

| Component | Numeric ID | Stable text ID | Schema version |
| --- | --- | --- | --- |
| Name | 1 | `shark.name` | 1 |
| Transform | 2 | `shark.transform` | 1 |
| Camera | 3 | `shark.camera` | 1 |
| MeshRenderer | 4 | `shark.mesh_renderer` | 1 |

Property IDs are stable across the catalog; each property also has its component
key, display name, typed default, constraints, unit and mutability.

| Property | Numeric ID | Type and default | Unit | Validation |
| --- | --- | --- | --- | --- |
| Name `value` | 1 | UTF-8 text, `Entity` | None | 1..256 bytes; valid scalar encoding; no C0/C1/DEL controls; nonunique; no Unicode normalization |
| Transform `translation` | 2 | Three doubles, `[0,0,0]` | Meters | Finite signed components |
| Transform `rotation` | 3 | Four doubles in `[x,y,z,w]` order, `[0,0,0,1]` | Dimensionless | Finite nonzero input; normalization and deterministic sign |
| Transform `scale` | 4 | Three doubles, `[1,1,1]` | Dimensionless | Finite signed components with absolute magnitude at least `1e-6` |
| Camera `projection` | 5 | Enumeration, perspective (0) | None | Perspective (0) or orthographic (1) |
| Camera `vertical_fov_radians` | 6 | Double, pi / 3 | Radians | Finite, strictly between 0 and pi |
| Camera `orthographic_height` | 7 | Double, 10 | Meters | Finite and positive |
| Camera `near_clip` | 8 | Double, 0.1 | Meters | Finite; positive for perspective, nonnegative for orthographic |
| Camera `far_clip` | 9 | Double, 1000 | Meters | Finite and greater than near |
| MeshRenderer `asset_id` | 10 | Asset ID, no default | None | Required nonnil UUIDv4 |
| MeshRenderer `mesh_index` | 11 | Unsigned 32-bit integer, 0 | None | Full uint32 range; actual asset bounds checked by [prepared asset resolution](prepared-content.md) |
| MeshRenderer `visible` | 12 | Boolean, true | None | Boolean |

Vector/quaternion bounds are inclusive finite-double input limits. Scalar metadata
also states endpoint inclusivity; the complete Camera validator enforces the
projection-dependent near bound and far/near relationship. The scale
floor is an **absolute magnitude**, so negative and nonuniform scales remain
valid. Quaternion input bounds apply before normalization; its canonical sign
is positive W, then the first nonzero X/Y/Z when W is zero. Metadata uses explicit
`xyzw` arrays; constructing `math::Quat` requires the `wxyz` constructor order.
Every current property has `scene_owner_write` mutability, referring to the
existing validated Scene setters. It does not advertise an editor widget, CLI
method or permission to mutate another authority's state.

`validate_component(id, version, ComponentValue)` delegates to the same component
validation used by Scene and returns an owned canonical value. The fixed
alternatives are Name, Transform, Camera and MeshRenderer. Unknown component or
property IDs, unsupported versions and mismatched value types reject explicitly.
Local component validation does not test a candidate hierarchy, reserve a scene
revision or change any scene. Scene mutation still validates all affected world
matrices, depth and authority before committing.

Schema version 1 versions this discovery/validation contract. It is independent
of source, authoring-protocol, gameplay-wire and database versions. Numeric enums
and GLM object memory are not serialized layouts. Camera projection exposes its
named enumerants; the catalog's type, unit, mutability and quaternion-policy
discriminators describe actual behavior.

## Query selection and work limits

`query_entities(options)` starts a query over committed scene state. Results
follow ascending runtime slot order, independent of display names and insertion
into a parent. The order is stable for one revision; it is not a durable authored
sibling order. The selection is one of:

- `all`: all live entities; `parent` must be absent.
- `roots`: entities without a parent; `parent` must be absent.
- `children`: direct children of the required checked parent, not all descendants.

An optional Name prefix filters those selections. It is an exact UTF-8 byte
prefix, with no case folding, locale interpretation or Unicode normalization.
Empty matches every Name. A nonempty prefix must be valid UTF-8 without
C0/C1/DEL controls and fit within 256 bytes. Duplicate names remain distinct
entities in the result.

| Bound | Default | Accepted range |
| --- | --- | --- |
| `page_size` | 64 rows | 1..256 |
| `scan_budget` | 256 slots | 1..4096 |

Each page stops when its row limit, scanned-slot budget or the end of the slot
table is reached. Scanned slots include vacant and retired slots as well as
nonmatching live entities. A sparse scene or restrictive filter can produce an
empty page with a continuation cursor. **An empty page does not mean the query
is complete.** Only the absence of `next_cursor()` means the remaining slot range
has been exhausted. `scanned_slots()` reports the actual bounded work.

Identity, scene scope and authority metadata are always returned. Five field
groups select optional payloads:

- `name`: the owned Name value.
- `transform`: local TRS and the cached world matrix in the scene's local frame.
- `hierarchy`: checked parent, direct `child_count` and depth.
- `camera`: the owned optional Camera value.
- `mesh_renderer`: the owned optional MeshRenderer value.

Name, transform and hierarchy default true; the two new flags default false, so
existing `{false, false, false}` selections still give identity-only rows. Select
Camera/MeshRenderer explicitly: an empty optional means absent component when
selected, or omitted data when unselected. No default component is fabricated.
Queries do not copy complete child
vectors. Use a separately paged direct-children selection to enumerate a large
family. Each row therefore has bounded text and fixed-size transform/hierarchy
data, and the page never retains a full-scene snapshot.

`inspect_entity(handle, fields, expected_revision)` performs a checked one-row
inspection with the same field groups and consistency rules. It has no
continuation. The earlier `entity_state` API remains an owned direct C++ state
copy with a complete child vector; it is not the bounded semantic query path.

## Revisions, cursors and lifetime

`SceneRevision` contains both scene-instance token and revision. An optional
`expected_revision` on an initial query or direct inspection rejects an old
observation instead of silently reading newer state. A successful page records
the current revision and scope.

`continue_query(cursor)` resumes the original options at the original revision.
The opaque process-local cursor owns its options and next slot; it contains no
Scene pointer, native entity or retained scene snapshot. It cannot be default
constructed, edited through the API or serialized as a protocol token. Copying
and replaying it at the same revision produces the same page. Changing the
caller's original options after a query cannot change its continuation.
Cursor construction and assignment from rvalues also copy: the source token
retains its original filter, fields, revision and position for safe replay.

An actual create, subtree delete, component change/removal or reparent invalidates
older cursors and expected revisions. Failed operations and exact no-ops do not
advance the revision. Enqueueing deferred work does not change the observed
committed state; applying a successful mutation does. A cursor from another scene
instance, a reload or a closed scene cannot resume. Recreate-with-the-same-ID and
slot reuse cannot revive it. A conflict requires an explicit fresh query; the
engine never silently restarts or merges different revisions.

All Scene query calls check the owner thread and live scene before accessing
mutable state. Reads are permitted during visits and while deferred work is
pending. They never apply queued work, change hierarchy, advance revisions or
invoke authoring transactions.

`SceneQueryPage` owns its data and exposes const views. A page or a copied entity
value survives scene mutation, close and destruction; embedded references may
become stale for future Scene access. Its scope, entity span and cursor views
are valid only while that page remains alive and unchanged. Those view getters
are deleted on rvalues to reject views into a temporary page. Copy a page or
value before retaining it or handing it to another thread, with normal caller
synchronization. Reading that independent immutable copy does not grant
cross-thread Scene access.

## C++ examples

This complete helper counts matching roots without accumulating a full-scene
result. It keeps following the cursor even when a page contains zero rows.

```cpp
#include <shark/scene.hpp>

#include <cstddef>
#include <utility>

shark::Result<std::size_t> count_spawn_roots(const shark::Scene& scene) {
    shark::SceneQueryOptions options;
    options.selection = shark::SceneQuerySelection::roots;
    options.name_prefix = "Spawn";
    options.fields = {false, false, false};
    options.page_size = 32;
    options.scan_budget = 128;

    auto result = scene.query_entities(options);
    if (!result) {
        return result.error();
    }
    auto page = std::move(result).value();
    std::size_t count = 0;
    for (;;) {
        count += page.entities().size();
        if (!page.next_cursor()) {
            return count;
        }
        auto next = scene.continue_query(*page.next_cursor());
        if (!next) {
            return next.error();
        }
        page = std::move(next).value();
    }
}
```

To enumerate a known parent's direct children, select `children` and pass its
checked handle in `options.parent`. To combine several initial queries at one
revision, copy `page.revision()` into each query's `expected_revision`. For one
known entity, pass that same revision to `inspect_entity`. Handle a returned
conflict before retrying from a newly observed revision; do not reuse stale
selection or mutation assumptions.

Schema discovery is separate from query execution. Use
`component_schema(ComponentSchemaId::transform)` to obtain the supported version
and properties. A later adapter can present those values without reading
private registry storage or inferring types from a rendered viewport.

## Qualification scope

`shark.core.schema` checks catalog identity, ownership and agreement with existing
validation. `shark.core.scene-query` checks bounded work/output, filtering,
pagination, revision conflicts and snapshot lifetime. Earlier scene, hierarchy,
transform and coordinate fixtures remain required. These are CPU contracts;
FeatureWorld connects in 10 and human/terminal adapters in 15/16. Full REQ-07
passes the current-machine regression and protected dependency/package gates:
see the [07c handoff](handoffs/07c-schema-inspection.md) and
[qualification receipt](coverage/evidence/07c-schema-inspection.json).
