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
| M05d | Adjacent authoritative regions in one process, overlapping interest and continuous movement, targeting, combat/projectiles and durable-operation boundary fixtures; connect shared cell-geometry preparation to retained human/terminal authoring adapters. |
| M05e | Consistent prepare/cut/commit transfer, final tick/input state, pending operations and stale-owner fencing at mutation and persistence boundaries. |
| M05f | The same transfer across actual local processes, bounded routing of post-cut inputs/completions and recovery from source/destination loss and partitions before/after commit. |
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
