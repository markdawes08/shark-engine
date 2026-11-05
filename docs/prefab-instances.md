# Prefab instances in authored scenes

21b connects [prefab source values](prefab-assets.md) to the existing
[authoring command owner](authoring-commands.md). Public types and preparation
live in [`prefab_instance.hpp`](../engine_tools/include/shark/tools/prefab_instance.hpp).
The [checkpoint plan](roadmap/prefab-plan.md) retains runtime/Lua spawning in
21c and Editor/terminal prefab workflows in 21d.

## Prepare once, execute through the owner

`prepare_prefab_instance` accepts a PrefabDocument and optional
PrefabInstanceOptions, returning an owned InstantiatePrefabCommand. Production
uses the existing OS UUID generator. The explicit IdEntropy overload supports
deterministic fixtures. Preparation changes no Scene, file, history or revision.

The command retains the source prefab, an explicit ordered `remap` of
prefab-local `source` IDs to fresh `instance` IDs, its mapped root `id`, an
optional external root `parent` and the requested overrides. Generated IDs are
distinct from all source IDs and from one another. Entropy failure or repeated
collisions reject; allocation/generation failure never publishes an instance.

Submit it as an AuthoringCommand to the existing execute, batch, preview or
retained mutate APIs with the expected SceneRevision. The owner revalidates
caller-owned command values, project identity, mapping completeness/uniqueness,
root correspondence, destination collisions, components and hierarchy before
publication. A command cannot use an old mapping to overwrite an existing entity.
Every typed internal parent reference maps within its instance. External mesh
asset IDs, script source paths and opaque extras retain their values.

The existing private Scene candidate creates parents before children even when
source array order places a child first. Source capture retains prefab entity
order and per-entity extras. Source metadata, complete history snapshots and the
receipt are prepared before one Core publication. Committed instances are
ordinary authored entities with independent components; there is no automatic
propagation from later asset edits or persistent prefab inheritance relationship.

## Simple overrides

Each PrefabEntityOverride targets a prefab-local `entity` ID and supplies an
optional complete Name and/or local Transform replacement. A transform replaces
the authored local TRS; it is not an additional offset or world transform.
Unknown targets, duplicate targets and empty overrides reject. Omitted values
retain the source. Component addition/removal and arbitrary property patches are
outside this initial override request.

The root may attach to an existing authored parent through the command's `parent`.
Its overridden transform remains local to that parent. Existing hierarchy depth,
world-matrix feasibility and root-only rigid-body constraints still apply. A
prefab with a root body cannot attach below another entity. Overrides cannot
relax body scale, script path or other component constraints.

## History, retries and bounds

An instance command publishes one Scene revision and one history command. A
batch containing instances remains one atomic history group. Preview validates
the same candidate and history capacity without changing source, history or
live handles. Failure preserves the entire previous state. Undo removes the
whole instance; redo restores its authored IDs and values with fresh entity
generations. Old handles remain invalid. Save/reopen uses existing ordinary
scene persistence; prefab source files are not rewritten.

One command supports the source bound of 4,096 entities, subject to remaining
Scene capacity, hierarchy/extras and history budgets. A batch remains limited to
256 commands and at most 4,096 new entities across prefab and ordinary creates. No partial prefix is
published when a bound is exceeded.

Retain the exact prepared command and mutation identity if a completion is
uncertain. Replaying that request returns the original retained outcome without
creating entities. Preparing again generates a new mapping and is a new intent.
Matching includes the project/asset/root, source entity order and every component,
raw extras, mapping, parent and ordered override field; floating-point inputs
compare exact bits. Mutation admission retains its existing 512 KiB payload
bound and capacity accounting, so a large valid ordinary command may be rejected
as `not_admitted` by the retained API. Foreign/expired identities still require
explicit state reconciliation under the [retry contract](authoring-retries.md).

This API owns authored content only. It starts no native physics body or Lua
callback and grants no gameplay, replica, region or durable economy authority.
The existing Scene write leases, owner-thread, visit, pending-work and revision
fences remain binding. Human and terminal prefab adapters will consume this same
command owner when their checkpoint lands.
