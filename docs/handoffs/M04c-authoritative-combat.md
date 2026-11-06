# M04c: authoritative combat and interactions

Date: 2026-11-05. Base: `7ec523f`, initially clean `main`. The user owns staging,
commits and pushing. Follow [ADR 0061](../architecture/0061-authoritative-combat.md),
the [combat contract](../authoritative-combat.md), [M04 plan](../roadmap/authoritative-gameplay-plan.md)
and [testing policy](../testing-policy.md).

## Delivered

The selected targeted-melee profile uses 20 Hz/three substeps, current 3D range
and native line of sight, tick cooldowns, bounded server Lua decisions and zero
historical rewind. CharacterCombat composes the existing actual durable session,
movement and physics owners. Opaque life/stream tokens retain checked references;
one pending action per actor resolves in global admission order after movement.
Strikes, non-economic interactions, cooldown/range/occlusion and stale/replayed
actions have explicit outcomes. No client damage, position, script or reward
payload is admitted.

Core CombatRules reuses private LuaVm with a protected value-only `on_combat`
entry, exact bounded returns, 4 MiB/100,000-instruction ceilings and existing
thread/native-contact guards. It binds no Scene or engine services and rejects
client presentation profile creation. Failed evaluation faults the owner without
pretending to roll back earlier Lua state or combat effects.

Death records applied damage, releases movement and discards queued input,
removes the collider and returns removal exits. Sixty ticks later the host
recreates/teleports the body and rotates combat/movement/life identities while
retaining durable and Scene entity identity. Reports preserve completed movement,
ordered outcomes and teardown/recreation milestones on partial failure. A native
movement publication failure can already have completed a tick; stats retain that
counter and later combat attempts reject. Observations also fence regressed time.

Authenticated combat kinds 11/12 carry independent protocol-1 bounded values.
Explicit trusted visibility lists publish only selected target health/life values
and eight retained outcomes. Source/database/prepared/authoring versions and
Server's bootstrap CLI are unchanged; no Store is opened by that CLI.

## Verification

Twelve unique selected Windows Debug CTests and seven WSL Clang ASan+UBSan
CTests PASS on final relevant inputs. Final review added a clock-observation
regression; only the affected server combat case was rebuilt/rechecked in each
profile. Unchanged selected results remain valid and are not counted twice.

| Selection | Result | Evidence under `out/M04c-work/` |
| --- | --- | --- |
| Windows combat rules/wire, server combat/movement, Lua VM/runtime/spawning, outer wire/transport/mTLS, movement wire/prediction | 12/12 PASS, 4.00 s | `windows-inventory.log`, `windows-tests.log`, `windows-tests.xml` |
| WSL rules/wire, server combat/movement, Lua VM/runtime/spawning | 7/7 PASS, 6.04 s | `wsl-final/inventory.log`, `tests.log`, `tests.xml` |
| Final server combat clock-fence regression and retained families | Windows 1/1 PASS, 1.06 s; WSL 1/1 PASS, 2.86 s | `windows-final.log`, `.xml`; `wsl-clock/tests.log`, `tests.xml` |

Five connected combat families use two actual mTLS accounts/clients, real local
Store and native bodies. They cover literal health/damage/cooldown/interaction
outcomes and actual packets; queued/replayed/foreign/stale admission; 3D range and
native wall rejection before Lua; death discarding seven future movement inputs,
owned body-removal exits and exact respawn timing; fresh action/input/body life
boundaries; revoked targets; earlier damage retained when a later Lua rule fails;
regressed observation time; and native completion followed by publication failure.
Ordinary Name writes remain blocked under the physics lease.

Five rule families cover exact inputs/returns, denial, mutable-state failures,
profile/resource/instruction limits, thread and native-callback entry. Five wire
families use independent literal bytes, malformed/count/lifecycle constraints,
maximum list sizes and outer framing. Existing Lua/runtime/spawning and network
checks qualify the affected shared private VM/transport consumers.

Windows uses the existing `windows-debug` wrapper/preset. WSL reuses
`/work/build-05a-final-client/linux-asan-ubsan`, leak detection and halt-on-error.
Inventories were listed, seed is 1337, timeouts are explicit and empty selections
fail. Builds used two jobs with no same-directory concurrency. Server/Core
protected dependency checks pass in both builds. No behavior/compiler failure
occurred; unchanged pinned TLS CMake deprecation warnings remain unrelated.

Formatting, link/manifest/repository and whitespace checks complete the increment.
`.gitignore` already covers managed scratch/evidence. README remains unchanged;
no public example, dependency lock or install rule changed. No unrelated Release,
package, GPU, manual UI, full active-load or WAN qualification is claimed.

## Next

M04d: connected spawn/despawn, reconnect and effect/replay lifecycle. Combat submit
currently rejects replayed action IDs; its outcome history is not an exact
original-request retry service. Preserve original durable-operation identity and
explicit uncertainty rather than repeating rewards/events.

Combat rule Lua is value-only. Returned movement contacts precede combat outcomes
and appended removal exits; the adapter does not automatically deliver entity
ScriptRuntime contact/death/spawn callbacks. Its existing dispatch API drains the
native FIFO, so it cannot consume an already-owned CombatTick batch. Preserve that
batch and connect the host's owned-event/entity-script lifecycle deliberately in
M04d. Final life state comes from the post-combat view, not the earlier movement
prefix. M04e retains complete two-client adversity/continuity acceptance. Full
REQ-M04 remains NOT_RUN; the manual UI pause and existing backlog amendment persist.
