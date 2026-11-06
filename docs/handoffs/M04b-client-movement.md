# M04b: client movement prediction and interpolation

Date: 2026-11-05. Base: `168f845`, initially clean `main`. The user owns staging,
commits and pushing. Follow [ADR 0060](../architecture/0060-client-movement-presentation.md),
the [client contract](../client-movement.md),
[M04 checkpoints](../roadmap/authoritative-gameplay-plan.md) and
[testing policy](../testing-policy.md).

## Delivered

Core's MovementPredictor retains at most eight unacknowledged directions and
replays them from validated authoritative corrections. The nominal profile shares
M04a's constants; host-configured gravity and native collision differences remain
explicit. Immutable prepared input bytes, owner/revision fences and atomic
FIFO/history publication preserve exact retry outcomes without sending twice.
Rejection/uncertainty suspends new prediction and retains history. A valid server
correction whose approximate replay exceeds local bounds is accepted and suspends
prediction rather than being vetoed. Replay invokes no effect callbacks.

Timed application extends the existing ReplicaClient's atomic cache publication
with eight complete frames. Current topology prevents delayed despawn resurrection;
duplicates never retime anchors, equal times coalesce and sampling clamps without
extrapolation. TRS/shortest-rotation interpolation uses a complete current-pose
fallback for shear/reflection/invalid blends. Display samples never mutate cache,
Scene, source or ACK revision.

CharacterAuthority forwards publication/control through its existing private
SessionAuthority. Durable-world publication checks precede publisher admission.
The native movement fixture connects a second actual mTLS account/client to this
publisher and validates delayed midpoint samples from committed moving-character
poses and real ACKs. No second session registry or mutable borrow is exposed.

## Verification

Ten unique selected Windows Debug CTests and seven WSL Clang ASan+UBSan CTests
pass on their final relevant inputs. Windows first ran nine selected checks;
after closing the durable-owner publication integration, the expanded movement
fixture and durable character regression both passed. The eight unchanged Core
results remain applicable without rerunning them.

| Selection | Result | Evidence under `out/M04b-work/` |
| --- | --- | --- |
| Windows prediction, interpolation, movement wire, mTLS, replica wire/cache, interest and real replication process; initial connected movement | 9/9 PASS, 9.19 s | `windows-inventory.log`, `windows-tests.log`, `windows-tests.xml` |
| Final Windows expanded connected movement and durable characters | 2/2 PASS, 5.99 s | `windows-connected-inventory.log`, `windows-connected.log`, `windows-connected.xml` |
| WSL prediction/interpolation, connected movement, durable characters, replica cache/interest and actual replication processes | 7/7 PASS, 22.92 s | `wsl-final/inventory.log`, `tests.log`, `tests.xml` |

The new six-family predictor and six-family interpolation fixtures cover exact
wire admission/retry, history saturation, correction progression, stale/foreign
preparations, duplicate receipts, suspension, replay-bound fallback, literal
gravity/headroom values, midpoint/shortest rotation, affine fallback, equal-time
coalescing, eight-frame clamping, lifecycle/baseline loss and creating-thread rules.

Two new connected movement families extend M04a's six retained families. One
uses actual TLS input/corrections and native wall collision for 40 server ticks,
then applies seeded decoded-message delay/jitter/loss/reorder/duplicates and a
500 ms partition. It observes predictor backpressure, continued native ticks and
final exact convergence without Scene mutation during correction. These faults
are semantic message tests after verified TLS delivery, not encrypted TCP packet
loss or real WAN evidence. The other exercises actual two-client/account motion
publication, interpolation, ACKs and wrong-world nonpublication.

The existing separate-process replication fixture now applies timed updates and
checks presentation endpoint parity. Its original session, resync, duplicate,
privacy and reconnect workload is unchanged. CPU sampling and native correction
evidence do not qualify graphical movement smoothing or full combat acceptance.

Windows uses `windows-debug` through `tools/windows_dev.ps1`; WSL reuses
`/work/build-05a-final-client/linux-asan-ubsan` with leak detection/halt-on-error.
Both listed inventories, used seed 1337, explicit timeouts, `--no-tests=error`
and two build jobs. Server/Core protected dependency checks pass in both builds.
No behavioral/compiler failures occurred. Pinned TLS CMake deprecation warnings
remain unrelated. No dependency, wire/schema version or installation rule changed.

Formatting, diff and repository/link/manifest checks complete the increment.
Existing `.gitignore` rules cover scratch/evidence; README remains unchanged.
No public example changed. Applicable M04a physics/session results remain valid
for unchanged owners; no unrelated Release/package/GPU/manual UI matrix was needed.

## Next

M04c: authoritative combat/interactions, cooldowns, range, damage and death/respawn
with server Lua rules. Select the explicit combat/lag-compensation profile first.
Keep prediction/interpolation presentation-only and route durable operations
through their existing owners. M04d retains full connected effect deduplication
and reconnect/spawn lifecycle; M04e retains the complete acceptance gate. Full
REQ-M04 remains NOT_RUN under the existing UI backlog amendment.
