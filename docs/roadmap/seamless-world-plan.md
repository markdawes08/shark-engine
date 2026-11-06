# M05: seamless world checkpoints

Follow the [MMORPG plan](mmorpg-plan.md), [ADR 0007](../architecture/0007-mmorpg-architecture.md)
and [testing policy](../testing-policy.md). Extend the existing coordinate, content,
physics, session, gameplay, interest and durable-operation owners. The user owns
staging, commits and pushing. Manual UI testing remains paused.

| Checkpoint | Scope |
| --- | --- |
| M05a | Checked world/local frames, generation-bound float conversion, validated static development cell payloads, Tools preparation and bounded synchronous Core loading. Complete; see the [handoff](../handoffs/M05a-world-cell-foundation.md). |
| M05b | Asynchronous content loading, prefetch, cancellation, retry, last-valid replacement, eviction and explicit resident memory/IO limits. Complete; see the [handoff](../handoffs/M05b-world-cell-residency.md). |
| M05c | Static-world collision and tiled navigation streaming, traversal readiness and bounded NPC pathfinding/pursuit across cell edges. Complete under the explicit authored-surface profile; see the [handoff](../handoffs/M05c-world-traversal.md). |
| M05d | Complete under the co-located functional profile: adjacent regional grants, overlapping interest, continuous player/NPC movement, targeting/projectiles and original durable-operation identity; shared cell preparation/export through retained human/terminal adapters. See the [handoff](../handoffs/M05d-colocated-regions.md) and [regional contract](../region-world.md). Native panel interaction remains deferred. |
| M05e | Complete under the co-located persistent profile: final tick/input cuts, original pending operations, execution fencing and transactional durable writer CAS. See the [handoff](../handoffs/M05e-fenced-region-handoffs.md), [cut contract](../gameplay-transfer-cuts.md) and [ownership contract](../regional-ownership.md). The exact native/Lua/session owners remain retained; this is not a portable simulation image. |
| M05f | Complete under the declared whole-island functional profile: real Server child execution, explicit portable native/rule state, all-writer CAS, bounded original input/completion routing and source/destination loss/partition recovery. See the [handoff](../handoffs/M05f-process-region-execution.md) and [process contract](../region-process-execution.md). |
| M05g | Authored continuous-world connected acceptance with actual separate region processes, network faults, precision, readiness and resident-budget evidence. |

Each implementation checkpoint tests its changed behavior and failure paths as it
lands. M05g connects those results and covers the complete affected subsystem;
it does not defer authority or recovery correctness until closure. Reuse valid
unchanged evidence under the testing policy.

Cells identify content, not simulation regions or authority. M05a's loaded static
Scene is a content observation, not a second gameplay Scene or a traversal-ready
region. It prepares the owned values later checkpoints activate through existing
runtime owners. M04's targeted melee and primitive collision do not already supply
world geometry, navigation, NPC pursuit or projectile continuity; M05c/d implement
those bounded profiles deliberately.

Development payloads are prepared through EngineTools without bringing source
importers into Core/Server. Final cooked cell/dependency manifests remain with
31/M08. Actual party/trade continuity follows M07/M09. M09/M10 retain the declared
LOCAL-MMO-1 crossing/load/endurance qualification. Local streaming alone cannot
close full REQ-M05; its status remains NOT_RUN until the complete gate passes.
