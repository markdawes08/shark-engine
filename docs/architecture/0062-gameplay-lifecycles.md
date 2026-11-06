# ADR 0062: connected gameplay lifecycles

Status: accepted for M04d. Extends [ADR 0061](0061-authoritative-combat.md),
[ADR 0051](0051-script-runtime.md) and [ADR 0058](0058-runtime-prefab-mutation.md).
The [lifecycle contract](../gameplay-lifecycle.md) specifies the host order and
current profile; the [M04 plan](../roadmap/authoritative-gameplay-plan.md) retains
full assignment acceptance.

## Decision

Compose the existing CharacterCombat, CharacterMovement, PhysicsWorld,
ScriptRuntime, CharacterAuthority and replication owners. Add owned contact
delivery to ScriptRuntime with the same lease/watermark as native FIFO dispatch.
Validate each complete bounded batch and consume its watermark before callbacks;
partial callback effects cannot be blindly replayed. Native collection never
invokes Lua.
The first admitted gameplay step fixes the optional ScriptRuntime incarnation;
later omission, opt-in or replacement rejects before callbacks/native work.

Suspend checked entity subtrees and their requester-owned runtime roots in reverse
order; resume fresh VMs from original source captures after complete body/movement
initialization. Keep mutation watermarks and original receipts independent of VM
lifetime. Retain bounded spawn ownership separately from evictable receipts so
cleanup remains possible after requester retirement or grant revocation.

Retain disconnected combat records in the existing bounded actor inventory.
Detach before another fixed/native tick. Reconnect needs a fresh actual session
and transport for the exact retained entity/account/character. Preserve health,
cooldown, dead state/deadline and last living pose; rotate only ephemeral bindings.
Dead reconnect does not heal. Exclude absent actors through explicit replication
selection without replacing the authoritative Scene or releasing its write lease.

Add a Core CPU combat-presentation cache with checked binding/tick/outcome fences,
once-only owned effects, explicit gap suspension and deliberate resynchronization.
One canonical snapshot per recipient/tick avoids ambiguous same-tick changes.
Fresh lives construct fresh owners. Presentation is never durable authority.

Retain the original durable request/OperationId across uncertain completion and
session replacement, using existing storage retry semantics. No new economic
endpoint, health component, scheduler, native dependency or player protocol is
needed. Source, database, prepared-content, Lua API and wire versions stay unchanged.

## Consequences

Partial reports preserve earlier native, script and combat effects; failures fault
the relevant owner rather than claiming rollback. Combat-managed body generations
are not silently rebound after arbitrary Lua remove/recreate. Dormant records count
toward the 32-actor limit and permanent retirement requires owner teardown.
Reconnect state is retained-realm state, not durable health across process restart.
The final adversity/continuity acceptance remains M04e, with full REQ-M04 NOT_RUN.
