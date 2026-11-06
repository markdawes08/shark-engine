# M04a: authoritative character movement

Date: 2026-11-05. Base: `7dc3d9e`, initially clean `main`. The user owns staging,
commits and pushing. Follow [ADR 0059](../architecture/0059-authoritative-character-movement.md),
the [movement contract](../authoritative-movement.md),
[M04 checkpoints](../roadmap/authoritative-gameplay-plan.md) and
[testing policy](../testing-policy.md).

## Delivered

`server::CharacterMovement` connects actual authenticated durable character
ownership to the existing leased PhysicsWorld. The fixed initial profile is
20 Hz, three collision substeps, 6 m/s direction-controlled root spheres and
bounded vertical velocity. Eight queued inputs per actor never accelerate time;
missing input stops horizontal motion. Up to 32 bindings retain owned values
and exact checked references, with no borrowed transport/Scene pointers.

CharacterAuthority supplies a cached, nonblocking ownership read without spending
a sequence or refreshing idle time. Input admission uses its existing exact-next
session command budget. Tick-time checks fence revoked, missing or replaced
sessions and entity/body generations before native work; fresh bindings issue
new stream IDs. Corrections identify the committed tick and last applied input.
Returned contacts go to the existing host dispatch. Native/post-step uncertainty
faults the adapter and cannot replay a completed move.

Core exposes bounded movement protocol 1 values/codecs through authenticated
outer kinds 9/10. Generic rejection remains separate from internal Error context.
The server adapter stays in EngineStorage; Core/Runtime gain no SQLite or source
authoring dependency. Server's bootstrap CLI remains unchanged and opens no Store.
Existing configure/build boundary checks pass on Windows and WSL.

## Verification

| Selection | Result | Evidence under `out/M04a-work/` |
| --- | --- | --- |
| Windows Debug: movement, wire/transport/mTLS/session/replication, physics and durable characters | 10/10 PASS, 9.92 s | `windows-inventory.log`, `windows-tests.log`, `windows-tests.xml` |
| WSL Clang ASan+UBSan: movement, movement wire, outer wire/mTLS/session, physics and durable characters | 7/7 PASS, 19.27 s | `wsl-final/inventory.log`, `tests.log`, `tests.xml` |
| Windows/WSL builds: ServerMovementTests, Tests, StorageCharacterTests, CorePhysicsTests, Server | PASS | `windows-final-build.log`, `wsl-final/build.log` |

Windows uses the existing `windows-debug` wrapper/preset. WSL reuses
`/work/build-05a-final-client/linux-asan-ubsan` with leak detection and
halt-on-error. Inventories were listed before selection; both use seed 1337,
explicit CTest timeouts and `--no-tests=error`. Builds used two jobs without
concurrent writes to the same build directory.

Six movement families cover actual loopback mTLS input/correction and role
rejection, literal 0.3 m/tick movement, global cell carry, neutral stop, malformed
directions/sequence gaps/queue saturation, native wall collision and contacts,
missing/revoked/replaced sessions and fresh streams, entity/body recreation,
durable world/readiness/profile/thread boundaries, idle-read nonrenewal,
replacement physics owners, terminal-speed headroom and a completed native move
whose global conversion fails and permanently rejects replay. Five wire families
use independent golden bytes, exact lengths and malformed/bounded values.

Review caught empty-owner physics replacement and terminal-velocity headroom
before the first behavior run; both were fixed with focused regression cases.
No behavior or compiler failures occurred in the selected runs. Existing compiler
deprecation warnings belong to the unchanged pinned TLS inputs.

The new integration fixture uses real Store and mTLS services with native Jolt
collision in one process. Separate gameplay processes, two-client prediction,
adverse-network combat and graphical world continuity remain later checkpoints.
No GPU/manual UI, package deployment or unrelated whole-project matrix was needed.
No public example changed. Formatting, repository/ignore/link/manifest and diff
checks complete the increment. `.gitignore` already covers all new scratch and
evidence paths; README is unchanged.

## Next

M04b: client prediction/reconciliation, remote interpolation and bounded history.
Keep movement stream/input identities separate from the shared session sequence,
use committed corrections and never assume cross-CPU physics lockstep. Preserve
the explicit primitive movement profile, original native-fault information and
existing UI backlog amendment. Full REQ-M04 remains NOT_RUN until M04e's full
connected gate passes.
