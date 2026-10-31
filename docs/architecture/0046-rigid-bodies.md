# ADR 0046: Authored rigid bodies and checked native snapshots

Date: 2026-10-31. Status: accepted. Owner: 18b.

Add one optional engine-owned `RigidBody` component containing motion, primitive
shape, dimensions, mass, friction and restitution. Atomic settings avoid a
temporarily incomplete body/collider pair. Preserve every inactive field across
validation, schema discovery, source, shared commands/history, exact retained
matching and terminal authoring. Physics/native handles are not authored values.

Use the existing Scene owner for root, positive uniform scale and bounded local
pose checks. Replica scenes cannot acquire physical authority. Authored edits
remain editable; no body appears implicitly. Existing render-only hierarchies
keep their broader affine support. The [body contract](../physics-bodies.md)
defines exact dimensions, units, limits and float conversion.

Extend the existing PhysicsWorld with explicit native body snapshots and scoped
generational handles. Validate the caller-supplied Scene afresh on creation and
observation; retain only owned source values/references, no borrowed Scene pointer.
Changed source state requires explicit remove/recreate. Cleanup remains possible
after the entity or Scene dies. Native body/shape owners release before the world
registration lease. Do not add another scheduler or Scene mutation path.

Retain source schema 1 with a new optional component version 1. Old readers reject
the unknown component. The Core prepared codec reads versions 1 and 2; version 2
preserves body settings, and scenes without bodies retain byte-identical version-1
encoding. This explicit format decision prevents silent component loss in export.

Nonempty stepping retains 18c because temporary-work admission and simulation
pose ownership must land together. Reject it before native work in 18b; do not
pretend an empty-update qualification proves collision or arena capacity.
Layer/trigger events retain 18d and native editor controls/connected acceptance
18e. Human and terminal paths share the implemented schemas/services; no future
Lua API, player protocol or unimplemented capability is advertised.
