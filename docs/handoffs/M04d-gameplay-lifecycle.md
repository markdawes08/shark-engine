# M04d: connected gameplay lifecycles

Date: 2026-11-05. Base: `d3d7605`, initially clean `main`. The user owns staging,
commits and pushing. Follow [ADR 0062](../architecture/0062-gameplay-lifecycles.md),
the [lifecycle contract](../gameplay-lifecycle.md), [M04 plan](../roadmap/authoritative-gameplay-plan.md)
and [testing policy](../testing-policy.md).

## Delivered

CharacterCombat composes the same retained movement, physics and optional entity
script owners. Session detach precedes fixed/native work; fixed callbacks, owned
contacts, ordered combat, death/respawn and removal exits have explicit host order.
Reports retain original script/contact/mutation receipts and native milestones,
including teardown before a later movement publication failure. Faulted work is
never blindly replayed.

ScriptRuntime now accepts already-owned bounded native contact batches under the
same weak-world and sequence fences as FIFO delivery. Checked subtree suspension
destroys in reverse order and cleans transitive requester-owned runtime roots,
even after grant revocation or an intermediate issuer's retirement. Resume loads
fresh VMs from original source captures while preserving mutation watermarks.
Nonzero runtime incarnation observations let combat fix its optional script owner
on first admitted use; later omission, opt-in or replacement rejects before work.

Disconnect retains the same checked Scene entity, health, cooldown, original death
deadline and last living pose in the existing 32-record actor bound. Fresh actual
ready session/transport reconnect rotates ephemeral bindings without healing or
delaying death. Living body/movement initialization precedes fresh scripts; dead
reconnect stays colliderless until the original respawn gate. Explicit active
selection produces a replica despawn and later fresh replica identity without
duplicating the authoritative entity or releasing its physics lease.

Core CombatClient publishes complete owned presentation values once, checks
binding/life/tick/movement-ack progression and preserves original overlapping
outcomes. Gaps freeze effects while retaining valid state and the first gap;
explicit resync skips historical effects. New lives require new caches and the
host publishes one canonical state per recipient/tick. Prediction, effects and
interactions grant no durable reward authority.

The existing real durable worker/Store fixture now connects an admitted
zero-reward interaction, a conserved transfer whose SQL commit precedes held reply
publication, continued native world stepping and gameplay/session replacement.
The host retains the original TransferRequest/OperationId; exact retry returns its
committed receipt once and altered payload rejects. Literal balances/items remain
conserved. No new reward API or health persistence schema is added.

## Verification

Eleven unique selected Windows Debug CTests and seven WSL Clang ASan+UBSan CTests
PASS on final inputs. Earlier successful selections are retained but not counted
again. Final review added script-owner omission/replacement and movement-ack
regression guards; the affected selection was rebuilt and rerun after those edits.

| Selection | Result | Evidence under `out/M04d-work/` |
| --- | --- | --- |
| Windows script-runtime, runtime-spawning, server movement/combat/gameplay-lifecycle, storage recovery, combat wire/client, replication, CPU Editor physics adapter and FeatureWorld | 11/11 PASS, 10.62 s | `windows-review-build.log`, `windows-review-inventory.log`, `windows-review-tests.log`, `windows-review-tests.xml` |
| WSL script-runtime, runtime-spawning, server movement/combat/gameplay-lifecycle, storage recovery and combat client | 7/7 PASS, 17.22 s | `wsl-review/build.log`, `inventory.log`, `tests.log`, `tests.xml`, `driver.log` |

Five connected gameplay families use actual local mTLS/session/Store owners and
native physics. They cover preserved living health/cooldown, fresh generations,
replica absence/reappearance, old action/input/cache rejection and once-only
effects; explicit contact replay rejection; death teardown, bodyless reconnect,
original respawn timing and fresh initialization; contact callback partial failure;
pre-native teardown retained after completed movement publication failure; and
the optional script-owner mode/incarnation fence. Core tests cover owned batch
validation, stale participants, complete subtree/transitive-spawn cleanup, original
mutation receipts, client history gaps/resync, thread/life fences and atomic
rejection. The existing FeatureWorld catalog is unchanged.

Windows used the existing `windows-debug` wrapper/preset, building `Tests`,
`ServerMovementTests`, `StorageRecoveryTests`, `EditorPhysicsTests`,
`FeatureWorldTests` and `Server` with two jobs. Listed inventory was selected by
the exact names above, using `--output-on-failure --no-tests=error` and JUnit.
WSL reused `/work/build-05a-final-client/linux-asan-ubsan`, two jobs, leak detection
and halt-on-error. Both builds passed protected Server/Core dependency checks.
Seeds remain 1337 and existing explicit timeouts remain binding.

The initial Windows build FAILED because including the combat header made an old
test-only unqualified `detail` namespace ambiguous. Storage hooks now use the
explicit `storage_detail` alias; the corrected and final builds pass. Preserve
`windows-build.log` for that failure and `windows-build-corrected.log` for the
first correction. No selected behavior test failed. Initial successful logs,
`windows-final`/`wsl-prefix` partial-report regressions and
`windows-durable`/`wsl-durable` strengthened recovery checks remain historical;
the review reports above cover their final inputs.

Pinned formatting, whitespace, contract/manifest and repository checks complete
the increment. `.gitignore` already covers builds, caches and managed evidence;
README is unchanged. No public example, dependency, install/package rule, source
format, database schema, wire version, Lua API or serialized component changed.
No GPU, manual UI, package, Release, WAN, separate-process gameplay or capacity
qualification is claimed by these CPU/same-process authenticated fixtures.

## Next

M04e: complete connected two-client adversity, pause/disconnect and headless
continuity acceptance, reusing valid earlier results. Full REQ-M04 stays NOT_RUN.
Keep the existing manual UI pause and backlog amendment.

Retained reconnect state is not durable health across process restart. Detached
records consume capacity until owner teardown. Combat-managed bodies cannot be
silently adopted after Lua remove/recreate. Keep exact original durable requests
through uncertain completion; fresh transport/session envelopes do not create new
durable intents. The lifecycle contract records these supported boundaries.
