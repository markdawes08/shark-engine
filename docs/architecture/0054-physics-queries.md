# ADR 0054: Bounded physics queries

Date: 2026-11-01. Status: accepted for checkpoint 20b.

Expose bounded ray, sphere-overlap and axis-aligned box-overlap queries through
the existing creating-thread PhysicsWorld and active Scene lease. Input coordinates
are bounded Scene-local world axes, never global cell positions. Use the existing
primitive native collision shapes; Jolt types, raw native handles and callbacks
remain private. Return owned scoped body/entity identities with explicit Scene
revision and completed-tick provenance. A query result is an observation, not
authority to mutate a body later.

Translate the existing collision layer/mask rules explicitly for query filters,
with trigger inclusion and an optional checked ignored body. Validate all values
and complete query bounds before native work. Bound collection without allocations
in native callbacks; reject overflow instead of silently returning partial hits.
Sort owned results deterministically, including stable tie handling. Ray hit
normals are absent for fraction-zero hits, where a unique surface normal is not
promised. Queries do not mutate simulation, consume contact events, execute Lua,
or authorize player requests. Actual server authority, cross-region queries and
client prediction remain with their existing owners.

No source, prepared, wire or database format is changed. Lua physics integration,
debug presentation and connected assignment acceptance remain later
[checkpoints](../roadmap/physics-interaction-plan.md).
