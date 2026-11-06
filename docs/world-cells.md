# Static world-cell content and local frames

M05a extends [WorldPosition](scene-contracts.md#global-cell-coordinates) and
[prepared content](prepared-content.md) under [ADR 0063](architecture/0063-world-cell-content.md).
The [M05 plan](roadmap/seamless-world-plan.md) retains streaming, navigation and
authority-transfer acceptance.

## Coordinates

`world_position_relative_to(position, origin, maximum_abs_meters)` validates
canonical positions in the same world and returns bounded local doubles.
`world_position_from_local(origin, local, maximum_abs_meters)` reverses that
conversion through the existing precision-preserving translation. Bounds are
positive, finite and at most `world_local_coordinate_limit` (65,536 meters per
axis). Both require round-to-nearest. Integer cell differences precede local
offset arithmetic, preserving nearby detail at the representation extremes.
Zero outputs are positive zero; nonfinite/out-of-bound values reject.

`WorldLocalFrame` pairs a canonical origin with a nonzero caller-owned generation.
`pack_world_position` produces bounded float meters and that generation;
`unpack_world_position` requires the same generation before reconstructing a
canonical position. Float quantization is explicit, not lossless or a global
position format. The caller changes generation whenever its origin changes and
checks it before applying packed values. The helpers do not mutate Scene/native
owners, rebase bodies, reinterpret existing handles or select an origin.
Packing also checks the quantized reconstruction: float rounding that would carry
past the global representation edge rejects before a packed value is returned.

## Development payload

`WorldCellKey` is WorldId plus WorldCell. Cell axes keep the version-1 signed
32-bit representation bounds even though public fields use int64. Cells and
simulation regions are independent identities and can have different boundaries.

`PreparedWorldCell` contains the key, positive uint64 content revision and one
PreparedScene whose world_origin must be exactly `{world, cell, {0,0,0}}`.
Missing, mismatched or nonzero-offset anchors reject; preparation never relocates
content silently. Authored entity IDs/hierarchy, Name/Transform, MeshRenderer and
optional static non-trigger RigidBody settings use the existing validators.
Camera, Script (including disabled), dynamic/kinematic bodies and triggers reject
in this static-content profile. Static body settings are inert until a later
trusted native owner activates them. Mesh geometry, collision and navigation
readiness remain distinct; a visible mesh is not a collision surface.

`encode_world_cell` / `decode_world_cell` use this little-endian format:

| Field | Encoding |
| --- | --- |
| Envelope | 8-byte `SHKCELL1`, uint32 version 1, 32-byte SHA-256 of the complete body |
| World | 16-byte UUID |
| Cell | Three signed int32 axes, x/y/z |
| Content revision | uint64, nonzero |
| Scene size / bytes | uint32 byte count and one complete `SHKSCNE1` payload |

The total encoded bound is `prepared_scene_maximum_payload_bytes + 128`;
the nested scene retains its 4 MiB encoded, 16 MiB logical decoded, 4,096-entity
and 256-depth limits. Truncation, extra bytes, unknown versions, bad digests,
invalid source values and oversized fields reject before publication. The digest
is integrity checking, not a signature. Content revision does not prove freshness,
audience authorization, a monotonic update or ownership; the trusted content host
must select the intended key/revision and the permitted asset closure.

## Preparation and activation

`tools::prepare_world_cell(owner, expected_revision, key, content_revision)` uses
the existing exact committed scene capture. Existing owner-thread/revision/pending
guards apply. Dirty source remains dirty; history, source extras and files are
untouched. Runtime bytes exclude authoring metadata, project paths, credentials,
session state and region authority. Both human and terminal authoring consumers
can use this shared service; no new editor panel or terminal method is advertised.

`LoadedWorldCell::load(bytes, assets)` validates the complete cell and uses
LoadedPreparedScene to check every mesh reference, including hidden meshes,
against the supplied immutable PreparedAssets. Missing assets, wrong mesh indices
and null asset snapshots reject. Only a complete candidate is returned. It owns
its private fresh Scene plus the retained asset snapshot; input bytes may be
released immediately. Access to the Scene is const and existing creating-thread
checks remain. Independent successful loads have distinct incarnations.
Construct, query and destroy an activated owner on its creating thread. Later
async workers can return owned payload bytes; they cannot hand a worker-created
Scene to another thread for activation or teardown.

This synchronous content activation does not publish a region, acquire a physics
lease or make a cell traversable. There is no implicit IO, residency, update,
eviction or latest-revision choice. A host retains/replaces owners explicitly;
failure leaves the old owner intact. During loading, validation can temporarily
hold more than one decoded Scene candidate; the existing scene and asset limits
bound logical work per candidate, not allocator overhead, peak RSS or total
resident memory. M05b must bound concurrent/resident owners before async work.

M05b now supplies [asynchronous residency](world-cell-residency.md) around this
same loader: one reader worker returns owned bytes, and explicit creating-thread
drain publishes complete candidates under pending/resident reservation limits.
The synchronous loader remains available with its original ownership contract.
