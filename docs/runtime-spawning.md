# Runtime prefab spawning and destruction

21c extends the existing [ScriptRuntime](script-runtime.md),
[physics owner](physics-world.md) and checked Scene lifetimes. Core owns the
RuntimePrefab values and RuntimeSpawnAuthority grant in
[`runtime_prefab.hpp`](../engine/core/include/shark/runtime_prefab.hpp).
Follow [ADR 0058](architecture/0058-runtime-prefab-mutation.md) and the
[prefab checkpoints](roadmap/prefab-plan.md).

## Trusted host setup

RuntimePrefab contains an AssetId, a prefab-local root AuthoredEntityId and
PreparedEntity values. Validation checks the complete rooted hierarchy, local
references, components and composed transforms without JSON parsing, file access
or asset import. The catalog admits at most 16 templates and 4,096 combined
entities under a 16 MiB decoded budget. ScriptRuntime owns validated script
source captures under its existing bounds for subsequent dynamic VM creation.

The trusted host creates RuntimeSpawnAuthority for the exact live Region Scene,
world, authority instance and epoch after establishing actual ownership. This
explicit host grant is not network authentication or proof derived from copied
scope metadata. It retains no Scene pointer. Revocation/destruction, another
Scene incarnation or a different stamp invalidates its use. Revoke before
handoff or Scene replacement. Access belongs to the creating thread.

Before start, call `enable_spawning(scene, physics, authority, prefabs)` on the
existing server-gameplay runtime. Client-presentation, Authored and Replica
execution cannot obtain this capability. [Editor/terminal prefab authoring](prefab-authoring.md)
uses the separate authored owner; ordinary preview remains client presentation. An empty catalog can
authorize self destruction without advertising any spawnable template.

Tools' `prepare_runtime_prefab` validates source into an owned Core value for this
catalog without adding a runtime source parser or granting execution. The host
still supplies captured scripts and admitted mesh assets explicitly. [Connected
acceptance](prefab-acceptance.md) covers repeated contact spawning, replacement
and rendering lifetimes.

## Lua requests and identity

| Entry | Behavior |
| --- | --- |
| `shark.prefab.spawn(sequence, asset_uuid, x, y, z)` | Queue a template at a complete local root translation and return its reserved fresh root WorldEntityId string. |
| `shark.entity.destroy(sequence)` | Queue destruction of checked self and its subtree. |

Only server update, fixed-update and contact callbacks with the live grant and
supplied physics owner may request mutation. Load, init and destroy reject it.
Arguments have exact counts/types; sequence is a positive Lua integer, asset is
a catalog UUID, and finite coordinates must satisfy Core transform/body rules.
A reserved UUID is not a live handle or authority over another entity.

Each script issuer has consecutive request sequences fenced by its complete
EntityReference, Scene incarnation and authority epoch. An identical retained
request returns its original reservation without another queued operation;
changed payloads and unavailable old outcomes reject. Floats match exact bits.
`take_entity_mutations` drains completed owned receipts. Replay does not redeliver
completion notifications. Bounded retention means old/foreign requests need
explicit reconciliation.

Instances receive fresh live WorldEntityIds and internal parent remapping;
mesh IDs and script paths remain content references. Only ephemeral actors are
created. There is no durable world-record, reward, inventory or session operation.
Those effects still require authenticated/durable owners. Old issuers and grants
cannot authorize mutation after reconnect or handoff to a replacement Scene.

## Deferred ordering and failures

Structural requests share one ordered queue with contact physics writes: at
most 64 requests and 4,096 expanded new entities per callback batch. Replay takes
no queue slot. Overflow faults the batch and discards unapplied work. Drain only
after the whole callback batch; newly initialized scripts join the next pass.
Recheck issuer, grant and relevant original contact/body generations at apply.

Spawn creates complete parent-before-child transforms/hierarchy without body
components, attaches native bodies through PhysicsWorld, then loads/initializes
Lua. Init observes complete bodies and hierarchy. Destroy invokes subtree
callbacks in reverse initialization order, removes native bodies, releases VMs
and erases the checked hierarchy. Removal exits stay owned in the physics FIFO;
the host drains them before another step, and stale generations never retarget.
Native contact collection never invokes Lua.

Receipts own issuer/sequence, operation/state, reserved root, created/removed
entity references, completed body/script counts and contextual errors. Body
counts describe successful complete PhysicsWorld calls; a faulted native owner
may have additional incomplete work, marked `native_progress_uncertain`.
`attempted_destroy_callbacks` records callback entry separately from VM releases.
Prepare identities
and receipt storage before effects. Callback/overflow/preflight failures discard
unapplied queued work and fault the runtime. Native or init failure can leave a
partial prefix; receipts describe completed operations and any native uncertainty
without claiming rollback or callback replay.
Faulted native owners need explicit close and host reconciliation/replacement.

Stop attempts eligible teardown callbacks; close accesses no Scene and discards
callbacks. With spawning enabled, stop also removes its owned ephemeral roots
under the original leases, including after grant revocation; its report records
removed entities/bodies and any remaining roots. Initial host entities stay
owned by the host. A cleanup error preserves its explicit failure/remaining-root
outcome. Releasing scripts alone cannot claim deletion of entities/bodies.

M04d's [subtree suspension](gameplay-lifecycle.md) also cleans requester-owned
runtime roots. Bounded ownership records retain the ultimate issuer independently
of drainable receipts, including after an intermediate spawned requester retires.
Destroying or restarting its VM cannot replay a consumed mutation ID.

Private structural writes check both live Script and Physics leases. Ordinary
Scene writes and authoring retain their guards. Core gains no source decoder,
Tools service, native handle exposure or scheduler. Runtime/export still reject
Script-bearing packages until cooked source closure and integrated execution land.
