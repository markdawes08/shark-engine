# M04: authoritative movement and gameplay checkpoints

Follow the [MMORPG plan](mmorpg-plan.md), [ADR 0007](../architecture/0007-mmorpg-architecture.md)
and [testing policy](../testing-policy.md). Extend the existing character/session,
physics, replication and durable-operation owners. The user owns staging, commits
and pushing. Manual UI testing remains paused.

| Checkpoint | Scope |
| --- | --- |
| M04a | Server-owned character movement/collision, authenticated sequenced directions, bounded input queues and committed movement corrections. Complete under the initial primitive movement profile; see the [handoff](../handoffs/M04a-authoritative-movement.md). |
| M04b | Owned-character prediction/reconciliation, remote interpolation and bounded history, using actual authoritative corrections. Complete; the [handoff](../handoffs/M04b-client-movement.md) records native correction and second-client publication/interpolation checks. |
| M04c | Server-validated combat/interactions, cooldowns, range, damage, death/respawn and server Lua rules. Complete under the explicit targeted-melee/current-pose profile; see the [handoff](../handoffs/M04c-authoritative-combat.md). |
| M04d | Connected spawn/despawn, disconnect/reconnect and replay lifecycle, including explicit owned-contact/entity-script host ordering. Complete; the [handoff](../handoffs/M04d-gameplay-lifecycle.md) records preserved health/deadlines, fresh bindings, once-only presentation and original durable-operation replay. |
| M04e | Connected two-client acceptance under declared network adversity, client pause/disconnect and headless world continuity. Cover the full M04 behavioral gate and reuse valid earlier results. |

M04a uses an explicit 20 Hz, three-substep root-sphere profile rather than
introducing a separate character-controller dependency. Its speed and local
collision fixtures are functional qualification, not the LOCAL-MMO-1 active-load
or terrain/navigation gate. M05 owns origin-safe streaming and region transfer;
M06 owns the measured presentation/content profile. Preserve those requirements.

Each checkpoint includes focused checks for its changed owners and failure paths.
M04e connects the retained pieces; it does not defer input authority, sequencing,
collision or lifetime checks until the end. Keep REQ-M04 NOT_RUN until its full
gate passes. The current UI backlog amendment permits continued coding while
unperformed manual evidence remains NOT_RUN.
