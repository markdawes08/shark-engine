# M05e: consistent regional cuts and persistence fencing

Date: 2026-11-06. Base: `f51c235`, initially clean `main`. The user owns staging,
commits and pushing. M05e PASS under its co-located current-machine functional
profile. Full REQ-M05 remains NOT_RUN; M05f is next. Follow
[ADR 0067](../architecture/0067-fenced-region-handoffs.md),
[regional gameplay](../region-world.md), [gameplay cuts](../gameplay-transfer-cuts.md),
[durable ownership](../regional-ownership.md),
[M05 checkpoints](../roadmap/seamless-world-plan.md) and the
[testing policy](../testing-policy.md).

## Delivered

RegionWorld adds explicit persistent ownership alongside M05d's existing functional
mode. Binding requires the actual CharacterAuthority's durable claim. Preparation
allows ordinary gameplay; crossing position alone cannot change the persistent
writer. The source can move within its declared overlap corridor and the world
union. A cut requires the latest committed destination pose, actual current
sessions, healthy equal-tick native/gameplay owners and drained Scene/contact work.
It freezes the whole shared island before capturing final state.

Owned cuts include original movement FIFO and accepted/applied watermarks,
combat health/cooldown/respawn/history, global action order and full original
incoming/outgoing flight requests/decisions. NPC native state/pending intents,
current actual session command sequences, regional binding shadows/counters,
frame/readiness generations and original durable jobs/routes are captured too.
Pending saves now retain their original revision/state even on failure. Canonical
bytes/digest cover these observations without pointers or native ABI layouts.

The native, Lua, session, interest and worker owners remain retained. This preserves
opaque native forces/contacts and mutable VM state that the observations cannot
serialize. Frozen or stale ExecutionPermits fence lower movement/combat mutation
before command admission/native work; only RegionWorld can privately adopt the
new permit on its existing lower owners. Commit performs no actor bind/reconnect,
queue resubmission, rule reevaluation or stream replacement.

The existing persistence worker queues claim/CAS controls. Store schema 4 records
the current regional writer and latest exact transfer, with final tick, input and
shared session watermarks plus cut digest. Actual SQL writer validation occurs
inside the same transaction as checkpoints/economic effects. A job admitted before
CAS can therefore fail as stale at SQL; an earlier committed job keeps its original
result through later delivery. Runtime controls and player work share the bounded
worker while retaining distinct typed take paths. Taking an owned result requires
the current writer and preserves original admission context and OperationId.

Explicit offline 1/2/3-to-4 migration preserves existing schema-3 economics,
watermarks, retained/pruned receipts and revocations without inventing ownership.
Complete backup, restore and migration equality include ownership rows. Existing
source/prepared and player wire formats are unchanged; no command/Lua/component
surface or authoring UI was added.

One active handoff and eight retained receipts bound coordinator work. Canonical
cut encoding is capped at 4 MiB; actor/queue/result limits separately bound its
owned values and duplicate byte blobs. This is not an exact allocation/RSS claim.
Terminal receipts release their internal cut pointer and require explicit release
before slot reuse. The local quiescence budget is 500 ms, with cut/resumption times
and an exceeded flag. Unknown results stay frozen; a known exact late commit can
activate the sole current owner while retaining the exceeded flag. Timeout never
restores source write rights. Reconciliation requires the exact persisted cut.

Destination readiness is rechecked at cut, before SQL submission and before local
activation: exact frame, cell generations/native attachments and complete character
sphere footprint, including dead lives. A changed postcommit attachment leaves the
cut frozen for explicit reconciliation rather than activating stale geometry.

## Connected observations

The actual mTLS/Store/native fixture prepares at tick zero, advances to tick two,
then cuts with accepted input four, applied input two and the current shared
session sequence. Both original FIFO entries survive without spending another
session command. Incoming and outgoing flights retain their evaluated damage
11/12 and emit no launch/outcome at commit. The next rule evaluation returns 13,
independently checking that the mutable Lua VM was retained. NPC intent executes
once after resumption; entity/body, session, movement/combat and replica identities
remain continuous. Normal functional handoffs remain inside the declared clock
budget, while a deliberate unknown-result case crosses it. This is not a
load/latency distribution.

Separate cases retain a dead life and its absolute respawn deadline, original
attacker outcome history and checked fresh life only at the original deadline.
Safe abort preserves queued input and writer identity. Stale lower movement and
combat adapters reject before native work or session sequence consumption. Exact
retry/mismatch, receipt capacity/release and readiness invalidation cases retain
their original effects and failures.

A real worker barrier admits an old-writer economic job behind ownership CAS;
SQL rejects it without an economic receipt. The exact OperationId then commits
under the successor, with independently checked 93/18 and 107/22 balances.
Another barrier holds a pre-CAS save after SQL and verifies its original revision,
state and writer are delivered through the successor without rewriting history.
Post-COMMIT I/O uncertainty requires actual Store reopening; the advanced row
survives while old runtime/session identities remain invalid. A coordinator case
withholds an actual successful completion, exceeds the declared budget and proves
that abort/ticking remain blocked until exact persisted receipt reconciliation.

## Verification

Seventeen unique selected CTests are covered on Windows Debug and on retained WSL
Clang ASan+UBSan, with corrected final relevant selections passing. The selection
covers the complete affected Storage behavior/process inventory and existing
movement/combat/lifecycle/region consumers, plus the new transfer family. It is
not a whole-project run. Evidence is under `out/M05e-work/`:

| Selection | Result | Evidence |
| --- | --- | --- |
| Initial Windows full affected selection | 13 PASS / 4 FAIL, 124.78 s | `windows-final-build.log`, `windows-final-inventory.log`, `windows-tests.log`, `windows-tests.xml` |
| Initial WSL full affected selection | 13 PASS / 4 FAIL, 133.78 s | `wsl-final/build.log`, `wsl-final/inventory.log`, `wsl-final/tests.log`, `wsl-final/tests.xml` |
| Final Windows transfer/recovery and corrected process checks | 5/5 PASS, 18.80 s | `windows-corrected-build.log`, `windows-corrected-inventory.log`, `windows-corrected-tests.log`, `windows-corrected.xml` |
| Final WSL same corrected owners | 5/5 PASS, 20.17 s | `wsl-corrected/build.log`, `wsl-corrected/inventory.log`, `wsl-corrected/tests.log`, `wsl-corrected/tests.xml` |

The final five repeat one previously passing owner to cover added readiness checks;
they are not five additional unique tests. The other twelve initial results remain
valid on unchanged relevant inputs. Earlier implementation checks are retained in
`windows-focused*` (4/4 PASS, 6.11 s), not counted as extra coverage.

Full affected selection:

```text
^shark\.(storage\.(store|region-ownership|characters|transactions|recovery|migration|process|characters-process|transactions-process|recovery-process|migration-process|integration-process)|server\.(movement|combat|gameplay-lifecycle|regions|transfer))$
```

Final corrected selection:

```text
^shark\.(storage\.(recovery|process|characters-process|transactions-process)|server\.transfer)$
```

Inventory precedes selections; CTest uses `--output-on-failure --no-tests=error`
and JUnit. Windows uses matching `windows-debug` presets through
`tools/windows_dev.ps1`. WSL reuses
`/work/build-05a-final-client/linux-asan-ubsan` in the retained isolated userspace,
with leak detection and ASan/UBSan halt-on-error. Both use two build jobs and build
StorageTests, StorageRecoveryTests, StorageMigrationTests, StorageTransactionTests,
StorageCharacterTests, StorageProcess, the Character/Transaction/Recovery/Migration/
Integration probes, ServerMovementTests and Server. GameplayProbe also compiles
against the final interfaces. Protected headless dependency checks pass. No host
or WSL settings changed. New families reuse seed 1337 and 60-second timeouts;
controlled clocks/barriers retain existing process/crash workloads.

Failures were corrected at their causes. Initial compilation found a shadowed
regional authority member, a fixture using `.x` on an array, and an unused test
helper rejected by Clang; logs are in `windows-build*` and `wsl/build.log`.
The full behavior run found three process supervisors still expecting active
schema 3. Their current-version expectations are corrected to 4, preserving
historical schemas, SQLite's format-3 signature and future-version rejection at 5.
The new postcommit-recovery fixture had reinstalled its one-shot failure on the
replacement worker; it now disarms that original fault before reopening. The
final affected checks pass without tolerance/workload changes or blind retries.

Contract/link/manifest, repository/ignore and diff checks pass. No GPU, manual UI,
Release/package or whole-project run was selected: this increment changes CPU
authority/persistence and Server interfaces. M04e's unchanged shaped transport
qualification and M05d's cell authoring remain their original evidence. The user’s
manual UI pause remains in force.

## Continuation

M05f must carry this ownership contract across actual separate local processes:
portable state/import, staged non-simulating destination/native state, source
retirement, routing of post-cut inputs/completions and source/destination loss or
partition before/after commit. Define supported mutable script/rule/native-state
transfer explicitly; reloading source is not state preservation. SQL cut metadata
is not a durable restartable simulation image. The retained local owner proof does
not qualify separate region processes or split-host availability.

Keep cell identity separate from regional, durable, session and execution epochs.
Use the persistent mode for the next transfer work; the default M05d functional
mode remains explicit and has no SQL writer claim. NPCs retain their real native
identities under the shared executor; do not invent player/economic identities for
them. M05g owns the complete connected gate, so full REQ-M05 stays NOT_RUN.

`.gitignore` was reviewed: build/output/cache, local environments and the optional
roadmap are already excluded; new independent SQL fixtures remain versionable.
No ignore change was needed. Root README is unchanged. No staging, commit or push
was performed.
