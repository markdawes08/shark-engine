# ADR 0061: authoritative combat and interactions

Status: accepted for M04c. Follow [ADR 0007](0007-mmorpg-architecture.md),
[ADR 0059](0059-authoritative-character-movement.md), the
[combat contract](../authoritative-combat.md) and [M04 checkpoints](../roadmap/authoritative-gameplay-plan.md).

## Selected profile

Use targeted melee in the current local development profile: 20 Hz, three native
collision substeps, 100 health, at most 25 damage per strike, minimum ten-tick
strike cooldown, 2.5 m center range and current native line of sight. Interactions
use 3 m range, minimum five-tick cooldown and a confirmed non-economic outcome.
Both actions share a cooldown. Trusted server Lua chooses bounded decisions.
Death removes movement admission and the collider; respawn after 60 ticks uses
the captured server spawn transform and fresh native/combat/movement identities.

There is no historical rewind or client timestamp in this profile: the
lag-compensation history/window is zero. Validate against committed current poses
after movement. This is a selected functional combat foundation; broader combat
styles, ability content, latency tuning and LOCAL-MMO-1 capacity need their own
measured decisions. No performance or final game-content claim follows from a duel.

## Authority and execution

Add server-only CharacterCombat over the existing CharacterAuthority, retained
CharacterMovement and PhysicsWorld. Bounded actions use actual ready mTLS/session
admission and its existing shared sequence/rate budget. Combat action IDs are a
separate consecutive domain; replay rejects. Opaque life tokens map to checked
entity/body references without serializing their durable entity IDs or native
handles. The existing session envelope still carries the caller's CharacterId.

Run one movement/native tick, then resolve admitted actions in global admission
order against live sessions, health, cooldown, current 3D range and complete
filtered rays. A killed actor cannot apply a later queued action in that tick.
Health and outcomes are ephemeral server gameplay values, not new serialized
Scene components or authoring setters. No inventory/reward endpoint is added.

Core CombatRules reuses the private LuaVm through one protected value-returning
entry, `on_combat`. Its VM has no Scene or engine service bindings. Preserve the
existing allocator, instruction hook, traceback, native-contact and thread guards;
client presentation profile creation rejects. Strict bounded decisions cannot
extend range, speed, damage ceilings or choose another target. Fault after a rule
execution failure because earlier Lua state changes cannot roll back.

## Lifetimes, outcomes and compatibility

Death records committed damage before checked movement release/body teardown.
Retain original action outcomes, completed movement and native milestone counts
if a later operation fails. Faulted combat owners never replay a tick. Native
movement publication failure may already include a completed tick; retain that
observed counter and the original error. Observation also advances the owner's
monotonic clock fence before another tick can be admitted.

Respawn recreates the body, teleports through the physics owner, rebinds movement
and rotates the combat stream/life token. Durable and Scene entity identity stay
separate from these ephemeral generations. Return ordered native contact values
and removal exits without invoking Lua inside native callbacks. Entity
ScriptRuntime contact/death/spawn orchestration remains explicit host integration;
this value-rule adapter does not deliver those callbacks automatically.

Outer wire version 1 gains authenticated combat action/state kinds 11/12. Their
independent combat protocol 1 has exact bounded fields and generic rejection.
Older peers reject unknown kinds. Source, database, prepared-content and authoring
versions do not change; no Lua/native type enters a public header. M04d retains
full reconnect/spawn/effect integration and M04e retains connected acceptance.
