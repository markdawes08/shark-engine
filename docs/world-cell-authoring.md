# World-cell authoring

M05d connects [static cell geometry](world-traversal.md) to the retained terminal
project owner and Editor. Both use the same exact Scene capture, explicit asset
catalog/cache and [ProjectFiles](project-files.md) publication. Preparing or
inspecting content does not edit the Scene, change history or acknowledge a save.

## Source and asset closure

The entire current Scene becomes the cell's prepared Scene. Its world anchor must
equal the requested world's cell corner with zero offset. Preparation rejects
Camera, Script, dynamic/kinematic bodies and triggers throughout the Scene; it
does not silently remove unsupported components. Source extras and project paths
are excluded from runtime bytes. Cells remain separate from region authority.

Collision and navigation are explicit authored-entity selections. Collision must
contain 1..256 unique mesh entities; navigation is a unique subset, at most 256.
Visibility does not select collision or exempt a mesh reference from validation.
The existing geometry profile applies: at most 4,096 vertices, 2,048 collision
triangles and 256 navigation triangles, on the exact cell-local 1/1024-meter grid.
Navigation retains the single-layer centerline profile; this is not a clearance
bake or automatic terrain generation.

The caller supplies an explicit import target ID and `client` or `server` side.
Every external mesh reference, including hidden and unselected meshes, requires
a fresh typed glTF cache artifact and a complete permitted dependency closure.
Each record must belong to this project and permit the requested side or be
shared. Built-in cube/plane meshes use Core's existing prepared assets. Preparation
does not scan for sidecars or import stale content. Inventory, import, drain and
take remain explicit operations on the existing catalog/job owner.

The closure retains the existing bounds: 126 external meshes, 128 source records,
512 dependency visits, 16 MiB of imported payload and 32 MiB of prepared mesh
payload. Exact Scene, catalog and cache revisions are checked, and source-file
and per-root freshness checks repeat before publication.

## Terminal commands

`protocol.discover` advertises `world_cells` and all three CPU methods. They require
an open project and use the existing request/response bounds. No binary payload
or additional preparation/file token is returned.

| Method | Parameters and result |
| --- | --- |
| `scene.cell.prepare` | Supply `expected_revision`, `expected_catalog`, `expected_cache`, `world_id`, `cell:{x,y,z}`, `content_revision`, `target`, `collision_entities` and `navigation_entities`. Return the prepared payload digest/counts, source revision and dirty observation without publishing a file. |
| `scene.cell.export` | Supply the identical preparation fields, a project-relative `.sharkcell` `path` and required `expected_digest`. Prepare again under those fences, reject a digest mismatch, then publish create-only. Return the actual `publication` receipt with its durability result. |
| `cell.inspect` | Supply a project-relative `.sharkcell` `path`. Bounded-read and validate the complete owned cell payload; return its digest, key, content revision and counts. This is a current-file observation. |

`expected_revision` is the Scene's `{scene_instance,revision}` pair. Catalog/cache
revisions use their existing `{scope,value}` objects. `content_revision` is a
positive canonical uint64 decimal string; it labels content rather than proving
freshness or ownership. Cell axes retain signed int32 bounds. Digests are exactly
64 lowercase SHA-256 hex digits of the complete encoded payload.

Obtain revisions from `project.status`, `asset.catalog.status` and
`asset.cache.status`. Call `scene.cell.prepare`, retain its `payload_digest`, and
send that digest with the exact original preparation fields to
`scene.cell.export`. A source or dependency change requires explicit reconciliation
and a new preparation. A correlation ID does not deduplicate publication.

Prepare/export reports include `payload_bytes`, `payload_version`, entity/geometry
counts and `mesh_closure_checked:true`. The artifact is the existing cell v2
development format. **It does not embed external runtime mesh assets**;
`runtime_assets_embedded:false` makes that boundary explicit. The consuming trusted
host must supply matching immutable PreparedAssets when loading the cell. This
operation does not publish a cooked asset bundle, standalone package or residency
request.

## Publication and uncertainty

The destination's parent directories must already exist. An existing destination
rejects, even if its bytes match. Select a fresh path for a deliberate replacement
candidate; selection/adoption of resident content belongs to the runtime host.
Publication reuses the root lock, contained-path checks, reserved sibling stage,
native create-only rename and FileCommit durability receipt. A directory-flush
warning still reports a successful publication. Scene source and saved baseline
remain unchanged.

Retain the prepared digest before export. After a lost export reply, inspect the
destination and compare its digest to that known intent. Equality establishes
that the observed current bytes match; it does not recover the original
publication/durability receipt or prove which operation wrote the file. Matching
only the cell key or content revision is insufficient. Without a known expected
digest or equivalent prepared bytes, the original intent remains unresolved.

Inspection reports `mesh_closure_checked:false`: decoding a file does not establish
that its external assets are present, permitted or fresh. It returns no
`publication` field and never converts a failed retry into an original success.
Typed binary cells are outside `text.*`'s script-only path domain.

## Editor

The World cells tab selects meshes from the existing hierarchy, marks the chosen
subset walkable and accepts the cell coordinates, content revision, target and
new output path. Selections bind to one exact Scene revision; source changes
require clearing and selecting again. Explicit Export prepares and publishes
through the same commands. Drawing the tab and reading status never publish.
Preview blocks the human export action.

The adapter retains the prepared digest/report before publication so a later
Inspect can compare current bytes after uncertain completion. The UI distinguishes
Published from Observed and keeps durability warnings. Matching observed bytes
does not imply an original publication receipt.

Actual native interaction is **NOT_RUN** under the current
[manual UI pause](testing-policy.md#manual-ui-testing-pause-2026-11-01). CPU adapter
and terminal-process checks cover shared behavior, not desktop input qualification.
