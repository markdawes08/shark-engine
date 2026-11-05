# ADR 0057: prefab source assets

Status: accepted, 21a. Date: 2026-11-05.

## Decision

Use an independently versioned `shark.prefab` schema-1 source envelope with
ProjectId, AssetId, an explicit root entity ID and the existing SourceEntity
values. Keep the codec in CPU EngineTools and share source component validation,
bounded JSON parsing and encoding with the project/scene codecs.

A prefab contains one nonempty rooted hierarchy. Entity IDs and parent links are
stable within that asset; they never become live instance identities without
remapping. Preserve existing component constraints, including root-only rigid
bodies. Reject nesting and unsupported fields explicitly. Extras remain opaque.

The source format does not contain a SceneId, global world origin, live authority,
native handles, inherited variants or instance override records. Decode and
validation perform no I/O, Lua execution or Scene publication. Source files and
runtime entities have separate ownership and compatibility boundaries.

## Consequences

The [prefab contract](../prefab-assets.md) defines exact fields, budgets and error
behavior. Existing project/scene schemas and legacy migration retain their
meanings. Prefab schema 0 and future versions reject. No source parser enters
Core, Runtime or Server.

The [checkpoint plan](../roadmap/prefab-plan.md) connects remapping, shared
authoring/history, scoped files, runtime initialization, Lua mutation queues and
Editor/terminal parity in subsequent increments. No importer or authoring method
is advertised before that implementation exists. Final cooking retains its own
dependency-closure and dynamically selected prefab requirements.
