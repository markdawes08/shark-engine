# Authoritative point projectiles

M05d extends the existing [combat owner](authoritative-combat.md),
[movement](authoritative-movement.md) and [lifecycle order](gameplay-lifecycle.md)
with `CombatActionKind::projectile` (3). The host still uses actual ready mTLS
session admission, checked combat streams, consecutive action IDs and the shared
session command sequence. No client supplies a trajectory, speed, damage or
timestamp. Follow the [M05 checkpoints](roadmap/seamless-world-plan.md) and
[testing policy](testing-policy.md).

## Profile and admission

| Setting | Behavior |
| --- | --- |
| Launch | Current actor/target center distance is nonzero and at most 10 m |
| Travel | Straight, nonhoming point at 10 m/s; one 0.5 m segment per 20 Hz tick |
| Lifetime | At most 20 flight ticks after launch |
| Damage / cooldown | Server rules choose damage 1..25 and cooldown 10..200 ticks |
| Capacity | One retained flight per actor; at most 32 actors/flights |
| Collision | First compatible current native ray hit, triggers excluded and owner body ignored |

`CharacterCombat::submit` uses the existing actual-session/body/target admission.
A pending action or flight occupies the actor's one combat slot. Another combat
action rejects before shared sequence admission; movement remains independent.
This backpressure preserves consecutive outcome identities and at most one new
outcome per actor/tick. Replaying the original action cannot launch another flight.

After committed movement and owned contact delivery, an admitted launch checks
current range, participant generations and cooldown. Server Lua kind 3 evaluates
once with the launch tick, living health values and launch distance. An allowed
decision charges cooldown immediately and captures only owned values: original
action, decision, source/target life/body identities, target entity, launch
position, direction, remaining ticks and global admission order. A denied or
invalid launch returns the ordinary final denial; an allowed launch has no
presentation outcome yet.

Rules supporting projectiles must handle kind 3 explicitly, for example:

```lua
function on_combat(kind, tick, actor_health, target_health, distance)
  if kind == 1 or kind == 3 then return true, 25, 10 end
  return true, 0, 5
end
```

The existing value-only VM limits and phase restrictions apply. A script that
returns zero allowed projectile damage or an invalid cooldown faults its rule
owner; no implicit formula or old interaction branch substitutes for that error.

## Fixed-tick resolution and lifetime

Each later tick first advances previously launched flights in original admission
order, then resolves newly pending actions. Queries observe committed current
native geometry. A successful ray's first hit must be the exact captured target
body; another body/surface blocks it. A moved target is not homed toward or hit at
its historical pose. An unavailable or overflowed ray grants no damage. No hit
after the twentieth segment produces `out_of_range` and releases the flight.

Every flight rechecks its source stream/life/body and actual live session, plus
the captured target life, entity and body generation. Source death/detach yields
`actor_unavailable`; target death/detach/replacement yields `target_unavailable`.
Fresh lives and reconnects do not inherit old flights. These final results carry
the original admitted action ID and zero damage. They are retained observations,
not original-request retry receipts or new admission events.

A valid target hit uses the same damage/death helper as melee. The original
outcome and health/death prefix are committed before fallible script suspension,
movement release and native body removal. Cleanup errors preserve that prefix
and fault the owner; the tick must not be replayed. Existing 60-tick respawn,
fresh life/body/movement identities, contact watermarks and partial-effect reports
remain in force. No durable reward, economy write or health persistence is added.

`CombatTick.projectile_launches` counts accepted launches in that tick;
`projectile_impacts` counts ray collisions, including blocking surfaces.
`CombatStats.pending_projectiles` observes retained flights. A final hit or miss
enters the existing eight-outcome history exactly once. CombatClient applies the
same binding/history/gap rules and emits each owned final presentation effect
once; duplicates cannot repeat gameplay or presentation effects.

There is no projectile Scene entity, native rigid body, radius sweep, ballistics,
area damage, client hit claim, historical rewind or replicated flight-animation
surface. The authoritative point trajectory and its final outcome are the
implemented profile. Shared-island movement can cross a logical region boundary
without changing the retained flight. M05e/f must explicitly capture/reconcile
this state when introducing transfer cuts and separate native/process owners.

## Combat wire compatibility

Combat payload version 2 adds action kind 3 using the existing 93-byte action
and outcome field layouts. Both decoders accept versions 1 and 2; version-1
payloads containing a projectile action/outcome reject. Unknown versions/kinds,
invalid damage and malformed lengths still reject. Outer message kinds 11/12,
gameplay framing, movement protocol and list/byte limits are unchanged.

Encoders preserve exact version-1 bytes for strike/interaction actions and states
without retained projectile outcomes. Projectile actions and states containing
projectile outcomes emit version 2. Generic rejection remains the original
four-byte version-1 payload. Version 2 can also decode legacy action/state values;
there is no implicit capability negotiation or claim that old readers support it.
Older readers reject version 2 rather than reinterpret its new action kind.

Independent wire fixtures, current-native flight cases, actual-session rejection,
death/session fencing and once-only presentation checks map this profile to
external Core/server fixtures. They do not qualify graphical flight presentation,
production capacity or the full seamless multi-process M05 gate.
