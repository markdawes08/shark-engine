# Authoritative combat and interactions

M04c adds server-only `server::CharacterCombat` and Core's bounded `CombatRules`
under [ADR 0061](architecture/0061-authoritative-combat.md). Follow the
[movement contract](authoritative-movement.md), [M04 plan](roadmap/authoritative-gameplay-plan.md)
and [testing policy](testing-policy.md). The host supplies trusted captured Lua
source; players cannot submit scripts, component writes, damage, poses or rewards.

M05d adds [bounded authoritative point projectiles](authoritative-projectiles.md)
through this same owner and lifecycle. The original melee/interaction behavior
and version-1 bytes remain compatible; projectile actions/outcomes use combat
payload version 2.

## Initial combat profile

| Setting | Selected behavior |
| --- | --- |
| Time | 20 Hz, three physics substeps; combat resolves after committed movement |
| Participants | At most 32 explicitly registered, combat-enabled player characters in one authoritative Region Scene |
| Health | 100 per life; strike/projectile damage chosen by server Lua in 1..25 |
| Strike | Current 3D center distance at most 2.5 m; first compatible non-trigger ray hit must be the target |
| Interaction | Current range at most 3 m and the same line-of-sight rule; confirmed non-economic interaction outcome, zero damage |
| Projectile (M05d) | Launch range 10 m; straight point flight at 10 m/s for at most 20 ticks, current native segment collision |
| Cooldown | Shared per actor; strike/projectile 10..200 ticks, interaction 5..200, chosen by bounded server rules |
| Respawn | 60 ticks after death, at the captured trusted spawn transform |
| Lag compensation | No rewind, client timestamp or historical target pose; window zero |
| Admission/history | One pending action or retained flight per actor; global admission order; eight retained outcomes per actor/life |

The reference rules return strike damage 25/cooldown 10 and interaction cooldown
5. Registered actors are attackable under this profile. M05d's explicit projectile
extension has its own bounds; factions, wider ability content and other combat
models are not inferred from these values.
LOCAL-MMO-1 load and network correction/latency distributions retain the full
connected acceptance owners. Revisit this explicit profile using those measurements.

## Admission and fixed order

`bind` uses actual CharacterAuthority ownership of a checked authoritative entity,
then the existing movement bind. It captures the body settings/spawn transform and
issues fresh opaque combat-stream and target-life UUIDs. Region metadata or token
possession alone grants no authority. No duplicate account/entity/connection may
bind. Combat/life target fields carry no Scene/native handle or durable entity
ID; the existing SessionEnvelope retains the caller's own CharacterId.

`submit` requires its live stream, a consecutive nonwrapping action ID, one free
pending/flight slot, actual ready mTLS/session ownership and checked actor/target body
generations. Stale, self, dead or unknown targets reject before shared session
admission. Existing Heartbeat admission then validates the SessionEnvelope and
spends the same exact-next sequence/rate budget used by movement and replication.
The combat action ID, movement input ID and shared session sequence are distinct.
No client field chooses damage, elapsed time, cooldown, collider or target pose.

One `step` fences live connections/sessions before fixed callbacks, calls the
retained CharacterMovement exactly once, then rechecks source/target generations.
Optional entity-script/contact ordering follows the
[lifecycle contract](gameplay-lifecycle.md). It resolves pending actions
in admission order using committed current poses. For strike/interaction,
cooldown, current 3D range and complete bounded ray checks precede Lua. Overflow
or an unavailable ray rejects line of sight; no partial result authorizes a hit.
Triggers are ignored and body
layer/mask compatibility remains bilateral. Previously launched projectiles
advance before new pending actions, using current native segments and their
captured participant fences; launch evaluates Lua once and its final outcome
arrives later. The [projectile contract](authoritative-projectiles.md) defines that
ordering and backpressure. An earlier death prevents a later
queued action from that actor; the profile does not implement simultaneous trades.

Range/cooldown denials are owned fixed-tick outcomes for the admitted action, not
another request. Exact or altered replay of an accepted action ID rejects without
another effect or shared sequence admission. Retained outcomes support observation;
they do not implement authoring-style exact original-request retry. Full uncertain
network/effect reconciliation follows [M04d](gameplay-lifecycle.md). A typed interaction outcome confirms
the admitted interaction only and grants no item, currency, quest or durable effect.

## Server Lua rules

Create `CombatRules` with a bounded captured `ScriptSourceSnapshot`, explicit
server gameplay profile and optional limits. Client presentation profile rejects.
The value-only VM has no `shark.entity`, physics, file, network or durable service
bridge. It reuses the existing private LuaVm, allocator, traceback and instruction
hook. No ScriptRuntime phase, Scene lease or native callback capability is widened.

Required entry:

```lua
function on_combat(kind, tick, actor_health, target_health, distance)
  if kind == 1 or kind == 3 then return true, 25, 10 end
  return true, 0, 5
end
```

Kinds are strike 1, interaction 2 and projectile 3. Inputs are bounded owned server observations;
tick fits a signed Lua integer and both participants are alive. Exactly three
returns are required: boolean allowed, integer damage and integer cooldown ticks.
Allowed strike/projectile damage is 1..25; interaction damage is zero. Projectile
rules evaluate at launch and charge cooldown then. Cooldowns use the
profile table. Denial requires zero damage/cooldown. Wrong types/counts or bounds
fault the rule owner; a failed evaluation can retain prior Lua global changes.

Memory is bounded to 64 KiB..4 MiB, instructions to 100,000 per evaluation and
source text to 32 KiB. At most 32 actor actions can evaluate in a tick. Instruction
limits do not measure native library work or promise a wall-clock deadline.
Input validation errors do not execute Lua; actual evaluation failures permanently
fault the owner. Native contact entry, reentry and foreign-thread access reject.
Player input and client prediction never execute these rules locally for outcomes.

## Death, respawn and partial effects

Death commits its damage/outcome before later native cleanup. Checked movement
`release` stops the old body and discards pending movement without marking those
inputs applied. Physics removes its body through the active lease and preserves
the visual entity. Dead actors have no movement/collider admission; the session
readiness flag is not repurposed as a death flag. Ordinary Scene writes remain
blocked by the existing physics lease.

At the respawn tick the same host checks current session/entity ownership,
recreates captured body settings, teleports through PhysicsWorld, and rebinds
movement. Combat stream, target life token, movement stream and body generation
are fresh; action/input counters restart only in those fresh domains. The durable
WorldEntityId and checked Scene entity reference remain unchanged. Old targets,
actions and body/input handles cannot address the new life.

`CombatTick` owns committed movement, action results, rebindings, deaths/respawns,
fences and native teardown/recreation milestone counts. An error inside that
report means earlier effects remain and the owner is permanently faulted. A failed
movement call can already have completed native work: the combat stats retain the
observed completed movement tick and the original error is returned. Never infer
rollback from outer Result failure or repeat an uncertain tick. Scoped teardown
remains available through the owning services.

Movement's contact batch is followed by body-removal exits in the returned owned
observations; the native FIFO is drained before another movement tick. M04d adds
optional same-owner entity-script dispatch of these owned batches, suspension on
death/disconnect and fresh initialization after body/movement restoration. Its
[lifecycle contract](gameplay-lifecycle.md) defines original reports, replay fences
and reconnect preservation. Combat rule Lua stays outside native collection.

The movement prefix precedes combat resolution: a character may have moved and
then died in that tick. Use the final combat view for life state. A view returns
only an explicit bounded trusted visibility selection, current own life state and
retained action outcomes. Views may expire sessions and advance the monotonic
clock fence; a regressed subsequent tick rejects before native work. Views and
prediction never authorize later mutations or persist health/rewards.

## Combat wire protocols 1 and 2

Authenticated clients send outer kind 11 (`combat_action`); servers send kind 12
(`combat_state`). Diagnostic TLS and reversed roles reject. The independent
payload prefix is version u16 BE, kind u8, reserved-zero u8. UUIDs are canonical
16-byte values; integers are big-endian, with no native layouts or trailing bytes.

Action kind 1 is exactly 93 bytes: SessionEnvelope (48), combat stream UUID,
nonzero action u64, target-life UUID and action kind u8. State kind 1 follows the
public CombatState field order: session/epoch/stream/tick/self-life, own health,
cooldown/respawn ticks, optional exact 108-byte movement state, then u8-counted
target and outcome lists. Live state requires matching movement session/epoch/tick;
dead state has a respawn tick and no movement. Targets are sorted unique nonself
tokens with health/respawn observations. Up to 32 targets and eight consecutive
ordered outcomes fit the 2,048-byte codec bound (current maximum 1,253 bytes).
State kind 2 is a four-byte generic rejection without internal diagnostics.

Version 2 uses these same field layouts for projectile action kind 3 and its
final outcomes. Decoders accept versions 1/2 but reject projectile values under
version 1. Encoders keep exact version-1 bytes when no projectile value is
present, and emit version 2 for a projectile action or a state whose retained
outcomes include one. The original generic rejection stays version 1. Unknown
versions/kinds remain errors; this does not add capability negotiation. See the
[projectile compatibility contract](authoritative-projectiles.md#combat-wire-compatibility).

The [handoff](handoffs/M04c-authoritative-combat.md) records actual two-client TLS,
native death/respawn, rule and codec evidence. [M04e](handoffs/M04e-gameplay-acceptance.md)
records passing connected network-adversity and continuity gates; full REQ-M04
PASS under its declared current-machine functional scope.
