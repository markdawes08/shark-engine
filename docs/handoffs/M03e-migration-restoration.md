# M03e: Database migration and restoration

Date: 2026-10-25. Assignment: M03e, fifth checkpoint of REQ-M03.
Base: `dabed278f8617109db6dbc6508df032534ffdcbf` on initially clean `main`.
Result: uncommitted working tree; staging, commits and pushing belong to the user.

**Status: PASS under the current-machine scope. Full REQ-M03 remains NOT_RUN.**
The [qualification receipt](../coverage/evidence/M03e-migration-restoration.json)
binds this handoff to the tested inputs and retained evidence. Follow the
[six-checkpoint plan](../roadmap/durable-state-plan.md),
[ADR 0019](../architecture/0019-durable-maintenance.md) and
[maintenance contract](../durable-maintenance.md). Existing
[store](../durable-store.md), [characters](../durable-characters.md),
[transactions](../durable-transactions.md) and [runtime recovery](../durable-recovery.md)
contracts remain in force. The exact next checkpoint is **M03f: Integrated qualification**.

## Delivered behavior

The existing Server-only Store exposes four typed offline operations:
`inspect_snapshot`, `backup`, `migrate` and `restore`. Each requires the expected
StoreId and WorldId. Snapshot summaries return metadata, bytes and record counts;
they are not authorization or freshness tokens. Ordinary `Store::open` remains
schema-3-only and never creates or migrates an existing database.

Maintenance callers drain/take pending runtime work, reconcile uncertain outcomes
and destroy the active CharacterAuthority/Store first. Backup and migration acquire
exclusive source ownership. Inspection and restore use read-only snapshots;
they accept inert journals but refuse hot-journal recovery without changing the
source or journal. Writable backup/migration may recover recognized interruptions.
Foreign/future headers still reject before SQLite can write or recover them.

Backup uses the pinned SQLite page-copy API outside a source write transaction.
The copy is bounded by the existing 4096-page/16 MiB profile and is independently
reopened and compared with the complete source state before acknowledgement.
Destination parents must already exist in different canonical directories.
Exclusive file reservation prevents overwrite; failed or interrupted copies may
leave incomplete or complete files requiring inspection and a fresh retry path.
Existing path/link/sidecar and local-directory trust boundaries remain unchanged.

Migration requires an existing independently validated backup of the exact
pre-upgrade logical state. Exact schema 1 upgrades through schema 2 to schema 3;
schema 2 upgrades to schema 3. Sequential transforms and final candidate validation
share one COMMIT. Store/world IDs, character/entity identity, ownership, checkpoints,
revisions and revocations survive unchanged. New operation high-water marks start
at zero, and economic/receipt tables start empty: migration grants no resources.

After interruption, explicit migration recovers the recognized source and retries
with the original backup. A schema-3 source returns `already_current` only when
all logical state equals that backup's deterministic transformation. Subsequent
writes conflict; a lost reply does not justify replacing the original backup or
silently treating a different state as the same upgrade. There is no downgrade.
M03a builds accept schema 1, M03b schema 2, and M03c/M03d/M03e schema 3.

Restore produces a new isolated candidate at the snapshot's schema and never
activates it or replaces the active database. Same-point restart preserves
revocations, economic high-water marks, terminal outcomes and replay/expiry rules;
it requires the original authority stopped and no acknowledged post-backup writes.
Legacy candidates require explicit migration. Existing CharacterAuthority performs
fresh actual mTLS Login after restart; sessions and live handles are not restored.

Production changes extend `server/storage/include/shark/storage/store.hpp` and
`server/storage/src/store.cpp`, with the private `maintenance_test_hooks.hpp`.
New coverage uses `tests/storage_migration_tests.cpp`, `storage_migration_probe.cpp`,
`tests/fixtures/storage/fixture.hpp`, three independently authored historical/current
SQL fixtures and `tools/tests/test_storage_migration_process.py`. The existing
test-only recovery VFS gains an optional native main-write observer, inactive in
its prior outage matrix. CMake registration, contracts, coverage/status documents,
repository build/release skills and `.gitignore` are maintained. SQLite remains
private to EngineStorage and absent from Core, Client, Runtime and standalone Tools.

## Qualification

The technical matrix contains **344 frozen source/build/test/fixture/contract
inputs and 17 helpers**, unchanged throughout qualification.
Seven full CPU profiles record **581 passing CTests with zero failures or skips**,
plus **10 standalone Tools checks**. Focused reruns and direct case reports
remain separate evidence and are not added to those totals.

| Profile | Recorded successful CTests |
| --- | ---: |
| Windows Debug | 93 |
| Windows Release | 93 |
| Windows ASan | 93 |
| WSL CPU client GCC Debug | 86 |
| WSL CPU client Clang ASan/UBSan | 86 |
| WSL headless Server GCC Debug | 65 |
| WSL headless Server Clang ASan/UBSan | 65 |
| Standalone Windows Tools authoring families | 5 |
| Standalone WSL Tools authoring families | 5 |

Every full profile verifies the complete named case sets with seed 1337. Distinct
observed direct-report assertion counts are listed if profiles differ.

| Family | Cases | Assertions in direct reports | Timeout |
| --- | ---: | ---: | ---: |
| `shark.storage.store` | 33 | 3,433 | 60 s |
| `shark.storage.characters` | 21 | 1,368 | 60 s |
| `shark.storage.transactions` | 17 | 4,063 | 90 s |
| `shark.storage.recovery` | 21 | 724 | 120 s |
| `shark.storage.migration` | 25 | 742 | 120 s |

Migration unit cases exercise independently prepared schemas 1/2/3, exact backup
requirements, sequential upgrades, immutable IDs/state, malformed/version/path
rejection, copy/commit failures and readonly cold-journal preservation. Actual
mTLS tests verify migrated revocations, restored fresh Login and original-ID
economic reconciliation. The migration families use `cpu;unit;server;integration`
(unit, 120 seconds) and `cpu;integration;server` (process, 240 seconds).

The process matrix contains **66 rows and 200 child processes per profile**:
35 abrupt-process-exit rows, 25 injected maintenance failures and
6 invalid-input rejections. Separate seed/operation/reconciliation processes compare
the exact schema and every typed record, high-water mark and retained receipt.
Migration interruptions cover both sequential steps, precommit, postcommit and
successful reply; backup/restore cover partial page copy, completed copy,
independent validation and reply. Retries never overwrite an allocated destination.

Two additional inspection children within the 2 native-write crash rows verify
actual successful main-database writes and hot rollback-journal headers. Main-file
bytes must change before the crash; read-only inspection must reject while source
and journal SHA-256 remain identical. Production migration then recovers the
original version before completing the upgrade. Completed/validated/acknowledged
copies must reopen with exact state; only an interrupted partial step may reject.
Current restores preserve committed and rejected retries and permanently expired IDs.

Each maintenance supervisor exercises 24 controls through its actual failure
exit and JSON path. The prior recovery VFS matrix remains 69 rows/207 children/
18 controls per profile. Earlier initializer coverage retains 48 checks/
8 controls; character coverage retains 7 phases/17 children/
14 mTLS connections/12 controls; transactions retain
2 phases/6 children/4 mTLS connections/16 controls.
All fourteen network families remain required.

Evidence validation separately records 25 named-case parser controls,
31 public API/dependency controls, 10 copied initializer-receipt
controls, 12 copied recovery-receipt controls,
16 copied migration-receipt controls and
29 aggregation controls. Omitted sources/examples, malformed process
evidence and missing instrumentation must fail those validators.

Protected native Runtime/Server and WSL Server builds pass. Native Server contains
12 package files and WSL Server contains 12; the latter runs in the retained
SDK-free Ubuntu userspace. Relocated native Runtime contains 33 payload files plus
1 manifest; 10 missing-license controls reject incomplete packages.
All 5 public C++ examples compile with MSVC, GCC 13, Clang 18, including the complete
maintenance example without private SQLite/TLS headers. 6 native dependency audits and
19 required ASan units are verified; WSL audits check actual sanitizer sources,
compiler flags and runtime dependencies.

Final formatting checks 188 files; contract validation records 37 negative controls and repository checks cover 5 discoverable skills. Whitespace, ignore boundaries, unchanged frozen inputs/helpers and the empty Git index pass.

## Commands, evidence and corrected findings

Run from the repository root. Exact command arrays, compiler inputs, exit codes,
inventories, logs, JUnit, case reports and process receipts are retained under
`out/M03e-work/` and bound by the versioned receipt. These local helpers do not
replace checkout presets or introduce a dependency on the ignored roadmap.

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03e-work/verify-windows.py
python out/M03e-work/launch-wsl.py final
python out/M03e-work/wsl-runtime-audit.py
python out/M03e-work/wsl-receipt.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03e-work/verify-runtime-package.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03e-work/api-audit.py --final
python out/M03e-work/migration-receipt-negative.py --positive out/M03e-work/preflight/native-r2/storage-migration-process.json
python out/M03e-work/aggregation-negative-controls.py
python out/M03e-work/record-evidence.py --draft
```

Closure updates the handoff from the receipt, records final repository checks,
then regenerates the receipt and handoff with the resulting final status:

```text
python out/M03e-work/publish-handoff.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03e-work/final-checks.py
python out/M03e-work/record-evidence.py
python out/M03e-work/publish-handoff.py
```

Initial native focused build rejected the private migration probe helper named copy because filesystem path arguments also found std::filesystem::copy through argument-dependent lookup. The helper is renamed; production and unit sources compiled, but no CTest was executed in this attempt.

The corrected focused `native-r2` run passes all eleven storage/session families,
including the new migration unit/process families and original recovery-VFS
regressions. The retained `native-r1` compile failure executed no CTests and is
excluded from final passing-profile totals. Required tests and limits were not
relaxed. No source-freeze amendment from an earlier checkpoint is reused here.

## Scope, coverage and next checkpoint

The recorded machine is LENOVO 83DG, Microsoft Windows 11 Home
build 26200, Intel(R) Core(TM) i9-14900HX (24 cores/
32 logical processors), approximately 32 GiB RAM, with the existing
WSL CPU/headless environments. This is current-machine software qualification;
no GPU work, additional machine, external account or host/WSL setting change is
required or claimed.

`server.durable-maintenance` maps the new public operations to executable unit,
authentication and process evidence. Existing durable surfaces remain covered.
Human and terminal developers use the same CPU C++ services and noninteractive
receipts without an editor or LLM SDK. FeatureWorld/MMOWorld remain 10/M09;
operational administration and terminal/editor adapters retain M08 and 15/16.
The Server bootstrap remains idle, with no new gameplay protocol or live simulation.

Acknowledged character and economic effects have zero permitted authoritative
rollback. An old backup can omit later writes, restore revoked access and lower
operation high-water marks, making previously consumed IDs appear new. Such a
restore is an isolated inspection/reconciliation candidate, not permission to
resume gameplay or run two authorities sharing one StoreId. There is no automatic
promotion, downgrade, replay, scheduled backup cadence or elapsed-time loss budget.
Filesystem calls have no live-tick latency guarantee. These tests do not establish
physical power-loss, disk/host-loss survival, independent disaster recovery,
distributed fencing or production MMO capacity.

The exact next checkpoint is **M03f: Integrated qualification**. It connects the completed durable
capabilities and closes full REQ-M03 only after its integrated gate passes.
Earlier handoffs retain their historical evidence.

Suggested commit: `feat(storage): add validated migrations and isolated restore`.
