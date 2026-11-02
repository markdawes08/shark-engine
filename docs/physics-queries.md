# Filtered physics queries

20b extends the existing [PhysicsWorld](physics-world.md) with bounded raycast,
sphere-overlap and axis-aligned box-overlap queries under
[ADR 0054](architecture/0054-physics-queries.md). They use the same owner thread,
active Scene writer lease and native bodies as fixed-step simulation. The caller
must own the actual simulation/region authority; a Scene scope or returned hit
is not itself authorization. Replica Scenes do not acquire a physics lease.
These CPU observations add no player protocol, Lua binding or global cell query.

## Geometry and filters

`raycast(scene, from, to, filter, max_hits)` tests a finite segment and returns
all admitted body hits. Each hit carries a fraction from zero to one, a Scene-axis
position and, for a nonzero fraction, an optional outward surface normal. Fraction
and position use the float-packed native segment and shape; the normal comes from
the native surface at that point. A ray
starting inside a solid reports fraction zero; any fraction-zero hit, including
a surface start, has no normal.
`overlap_sphere(scene, center, radius, filter, max_hits)` and
`overlap_box(scene, center, half_extents, filter, max_hits)` test native body
shapes, not merely their broadphase bounds; the box uses Scene axes and has no
rotation. Native shapes, single-precision conversion and collision tolerances
define near-boundary results.

Coordinates and the complete query bounds must fit the existing [-8192,8192]
Scene-local meter range. Query radius and each box half extent must be finite in [0.01,256] meters;
degenerate rays,
invalid floating-point inputs and values not representable by the native query
are rejected. These coordinates are not global cell positions or entity-local
axes. Query geometry is evaluated against committed native poses: an explicit
teleport is observable immediately, while an unconsumed kinematic target is not.

`PhysicsQueryFilter` supplies a query layer [0,15], a 16-bit mask, trigger
inclusion and an optional ignored checked body. As in simulation, both the query
mask must include the body's layer and the body's mask must include the query
layer. Trigger exclusion applies independently. A stale or foreign ignored handle
rejects rather than silently changing the filter.

## Results and lifetime

A query accepts `max_hits` from 1 through 256, default 64. More distinct hits than
the requested capacity returns an error, never an apparently complete partial
list. Native collection is bounded and does not allocate or invoke application
code in a native callback. Ray hits are ordered by fraction then stable entity
identity; overlaps by stable identity. Results own their body/entity references,
world identity, Scene revision and completed tick. The caller must resolve those
references again before using them after later Scene or world changes.

Queries do not advance the tick, apply pending forces or targets, consume contact
events, change the Scene/history, or provide a render-interpolated picking
authority. The observed physics Scene may be an isolated local development copy
or a trusted authoritative region; neither implies production capacity or remote
player access. Later [checkpoints](roadmap/physics-interaction-plan.md) add Lua
bindings, contact callbacks, debug visualization and connected acceptance.
