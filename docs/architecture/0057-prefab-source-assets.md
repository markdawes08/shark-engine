# ADR 0057: prefab source assets

Status: accepted, 21a; extended by 21b and 21d. Date: 2026-11-05.

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

## 21b: authored instance commands

[Instance preparation](../prefab-instances.md) assigns fresh authored entity IDs
once and retains an explicit local-to-instance map in one InstantiatePrefabCommand.
Name/local Transform overrides are bounded complete replacements. The existing
authoring candidate, history and retained retry owner validates and publishes the
whole hierarchy, including source extras, in one revision. Preparation is not a
commit token; execution revalidates the complete mutable input. Redo preserves
authored identities and creates fresh handle generations. Existing scene/source
schemas and the source nesting rejection remain unchanged.

## 21d: shared file and adapter workflow

[Prefab authoring](../prefab-authoring.md) connects typed ProjectFiles IO and the
retained project session to both adapters. Source publication requires matching
adjacent metadata; the two files have independent truthful publication receipts.
Opaque exact source tokens and immutable prepared commands preserve file conflicts
and the original ID map across uncertain completion. Instantiation uses the
existing mutation ledger and one history group. Capture is revision-bound and
observational. No new importer, cache format, cooked package or gameplay authority
is implied; source and protocol versions remain unchanged.
