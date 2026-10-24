# M03d: Runtime failure recovery

Date: 2026-10-24. Assignment: M03d, fourth checkpoint of REQ-M03.
Base: `9dd0a925981c844f751abe8144a1ff204da404c8` on initially clean `main`.
Result: uncommitted working tree; staging, commits and pushing belong to the user.

**Status: PASS under the current-machine scope. Full REQ-M03 remains NOT_RUN.**
The [qualification receipt](../coverage/evidence/M03d-runtime-recovery.json)
binds this handoff to the tested inputs and retained evidence. Follow the
[six-checkpoint plan](../roadmap/durable-state-plan.md),
[ADR 0018](../architecture/0018-runtime-durable-recovery.md),
[runtime recovery contract](../durable-recovery.md),
[character contract](../durable-characters.md),
[transaction contract](../durable-transactions.md) and [store contract](../durable-store.md).
The exact next checkpoint is **M03e: Database migration and restoration**.

## Delivered behavior

`CharacterAuthority::open_runtime` owns one dedicated persistence worker. Store
opens, executes and closes on that worker; the same CharacterAuthority retains
one creating-thread SessionAuthority. TLS transports and Scene stay on their
owner thread. Typed bounded requests and owned results cross a private executor;
Core's existing CompletionQueue carries trivial slot/sequence tokens. Existing
synchronous construction remains supported. Synchronous storage calls in runtime
mode are explicit blocking maintenance and reject while any result is retained.

`submit_save` and `submit_transfer` validate the actual current ready mTLS session,
its exact next command envelope and a live Scene entity with the character's
immutable durable WorldEntityId in the expected world. Admission copies trusted
server state; player data cannot supply checkpoint authority. Capacity, shape,
clock/deadline and scene-binding failures reject before Heartbeat admission.
Admitted revision or business failures consume the live command sequence.

A runtime has one through sixteen retained slots, default sixteen, and at most
one operation per account. Queued, executing, settled-but-untaken and timed-out
work all count. Tickets combine a fresh RuntimeId with a nonwrapping sequence.
Polling/taking use nonregressing monotonic observations; deadlines are strictly
after now and no more than sixty seconds later, expiring at equality. New
submissions also observe expired retained deadlines before admission.

Completion delivery rechecks current transport, session, account, selected
character, ownership epoch and the exact scoped EntityReference. Later valid
heartbeats do not invalidate the same session. Replacement, logout, revocation,
expiry, disconnect, not-ready state, entity reuse and replacement region/Scene
fence delivery. No completion mutates Scene. Every settled take returns its owned
outcome for trusted reconciliation, including timed-out or fenced delivery;
those delivery states do not authorize publication to a player or live Scene.

Timeout never cancels SQL, implies rollback or frees an unsettled slot. I/O,
uncertain commit and caught worker exceptions quiesce admission; worker failures
also prevent queued commands from executing later writes. Poll/take remain
available. Explicit `recover()` requires all retained outcomes settled and taken,
joins the old worker, reopens the exact StoreId/world/schema and reloads durable
authorization. It rotates runtime identity and sessions. Fresh Login is required;
caller-owned authenticated TLS connections may be reused. Failed reopen leaves
admission closed and can be retried after the underlying issue is repaired.

Recovery does not automatically replay writes. Inspect durable save revision and
state; reconcile a transfer with its identical original operation ID and payload.
The returned transfer request remains available even after uncertain completion.
Economic high-water marks and retained receipts retain their M03c semantics.
Destruction closes admission, joins the worker before releasing retained results
and cancels queued undispatched commands; already-dispatched SQL may commit.
An OS filesystem call cannot be forcibly interrupted by this contract.

Database schema 3, SQLite pin/options and all existing size/record/receipt bounds
remain unchanged. Schema 1, schema 2 and future versions still explicitly reject;
this checkpoint does not implement migration, backup or restore.

Production changes extend `server/storage/include/shark/storage/characters.hpp`
and `server/storage/src/characters.cpp`, with private `durable_worker.hpp/.cpp`
and `durable_test_hooks.hpp`. New executable coverage uses
`tests/storage_recovery_tests.cpp`, `storage_recovery_probe.cpp`,
`storage_recovery_vfs.hpp/.cpp` and
`tools/tests/test_storage_recovery_process.py`. CMake registration, contracts,
coverage, planning/build/CI guidance, the build skill and `.gitignore` are updated.
EngineStorage stays Server-only; Core, Client, Runtime and standalone Tools do
not acquire SQLite or runtime storage dependencies.

## Qualification

The final qualification below is complete.
The final matrix uses the amended frozen set, containing **333 source/build/test/fixture/contract inputs
and 16 helpers**, unchanged across final technical qualification.
The recorded seven full CPU profiles total **567 CTests with zero failures
or skips**, with **10 additional standalone Tools passes**.
Focused development reruns and direct named-case reports are separate evidence.

| Profile | Recorded successful CTests |
| --- | ---: |
| Windows Debug | 91 |
| Windows Release | 91 |
| Windows ASan | 91 |
| WSL CPU client GCC Debug | 84 |
| WSL CPU client Clang ASan/UBSan | 84 |
| WSL headless Server GCC Debug | 63 |
| WSL headless Server Clang ASan/UBSan | 63 |
| Standalone Windows Tools authoring families | 5 |
| Standalone WSL Tools authoring families | 5 |

Every full profile verifies the complete named case sets with seed 1337.
Distinct observed direct-report assertion counts are listed if profiles differ.

| Family | Cases | Assertions in direct reports | Timeout |
| --- | ---: | ---: | ---: |
| `shark.storage.store` | 33 | 3,433 | 60 s |
| `shark.storage.characters` | 21 | 1,368 | 60 s |
| `shark.storage.transactions` | 17 | 4,063 | 90 s |
| `shark.storage.recovery` | 21 | 724 | 120 s |

Recovery unit cases use actual mTLS and Scene references with controlled clocks
and worker barriers. They cover successful owned delivery, exact admission,
capacity including untaken work, timeouts before execution and after real commit,
all current session/Scene fences, uncertain commit, failed reopen and retry,
queued-work quiescing, worker exceptions and deterministic shutdown ordering.
The recovery families use `cpu;unit;server;integration` (unit, 120 seconds) and
`cpu;integration;server` (process, 240 seconds).

The recovery process matrix contains **69 independent rows and 207 child processes per profile**:
45 process-interruption rows and 24 native-VFS outage rows. Each has a fresh seeded
Store, a separate mutation process and a separate reopen/reconciliation process.
Each of the eight record/economy mutation kinds is interrupted before/after commit and after its successful
reply; transfer also covers partial source debit, terminal rejection and receipt
pruning. Delegating native SQLite VFS fixtures inject actual write, sync and disk-full
errors. Exact complete seeded, reopened, retried and reopened-again snapshots
prove atomicity, stable IDs, conservation, retained receipt behavior and no repeated
same-ID effect. These are trusted Store process tests; authenticated runtime
completion fencing is separately exercised by the recovery unit family.

Each recovery supervisor runs 18 controls through its actual failure exit and JSON
path, including incomplete or contradictory state, false acknowledgement,
duplicate retry effect and empty exceptions. Earlier initializer coverage retains
48 checks/8 controls; character coverage retains 7 phases,
17 children/14 mTLS connections/12 controls;
transaction coverage retains 2 phases/6 children/
4 mTLS connections/16 controls. All fourteen network families remain required.

Evidence validation records 20 named-case parser controls,
25 API parser/dependency controls,
10 copied initializer-receipt controls,
12 copied recovery-receipt controls and
24 aggregation controls and
21 source-amendment/history controls. These include omitted worker sources,
incomplete public examples, changed compiler bindings and absent sanitizer flags.

Protected native Runtime/Server and WSL Server builds are qualified. Native Server
has 12 package files; the WSL Server inventory has
12. The WSL package runs in the retained SDK-free Ubuntu
userspace. Native Runtime relocation has 33 payload files plus
1 manifest; 10 missing-license controls reject incomplete packages.

All 4 published C++ examples compile with MSVC, GCC 13, Clang 18.
6 native audits inspect actual dependency inputs; ASan object checks cover
16 required units. WSL audits verify the exact sanitizer compiler/source
closures and isolated runtime dependencies. These are current-machine CPU/package checks.

Final formatting checks 184 files; contract validation records 37 negative controls and repository checks cover 5 discoverable skills. Whitespace, ignore boundaries, unchanged frozen inputs/helpers and the empty Git index pass.

## Commands, evidence and corrected findings

Qualification entrypoints from the repository root are listed below. Exact child
command arrays, exit codes, logs, inventories, direct named-case reports, CTest
JUnit and process receipts are retained under `out/M03d-work/` and bound by the
versioned receipt. The local drivers do not replace checkout presets or fixtures.

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03d-work/verify-windows.py
python out/M03d-work/launch-wsl.py final
python out/M03d-work/wsl-runtime-audit.py
python out/M03d-work/wsl-receipt.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03d-work/verify-runtime-package.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03d-work/api-audit.py --final
python out/M03d-work/aggregation-negative-controls.py
python out/M03d-work/record-evidence.py --draft
```

The final closure sequence is complete:

```text
python out/M03d-work/publish-handoff.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03d-work/final-checks.py
python out/M03d-work/record-evidence.py
python out/M03d-work/publish-handoff.py
```

The initial timed-out completion discarded its settled durable outcome; the owner now preserves that outcome for explicit reconciliation while retaining timed-out/fenced delivery.

Protected Server compiled and installed, but the strict closure filter rejected the private runtime_worker.cpp filename. Renamed three private files and their include/build references; retained the checker unchanged and reconfigured/recompacted Ninja metadata before fresh protected and focused verification.

The retained amendment binds the original freeze and failed protected-build
attempt to 6 renamed/updated source inputs,
327 unchanged inputs and 3 updated helpers.
Both freeze references, the complete original source/helper snapshot and its
negative controls are validated by the receipt.

Recorded failed attempts remain explicit development evidence and are excluded
from final profile totals. Fixed limits and required families were not relaxed.
Ignored helpers, generated databases and local receipts stay under `out/` or
`build/`; builds and CI do not depend on the local roadmap or these helpers.

## Scope, coverage and next checkpoint

The hardware record identifies LENOVO 83DG, Microsoft Windows 11 Home
build 26200, Intel(R) Core(TM) i9-14900HX (24 cores/
32 logical processors), approximately 32 GiB RAM, and the existing
WSL CPU/headless environments. No additional machine, GPU qualification, external
service or host/WSL setting change is required or claimed by M03d.

`server.durable-recovery` maps to the executable runtime and process families;
earlier durable store, character and transaction coverage remains. Human and
terminal developers use the same device-free C++ service and noninteractive
machine-readable fixtures. FeatureWorld/MMOWorld remain 10/M09. Server bootstrap
remains idle; no live simulation loop, editor UI, authoring CLI, new player economy
codec, arbitrary SQL interface or LLM SDK is introduced.

Acknowledged character checkpoints and economic effects have zero permitted
rollback under the existing SQLite commit policy. Unacknowledged writes require
inspection because either old or committed state may exist. There is no scheduled
checkpoint cadence or elapsed-time rollback budget before simulation owns it.
Startup, maintenance, recovery and teardown make no live-tick latency guarantee.
Software interruption/VFS injection does not establish physical power-loss
survival, disk/host-loss recovery, production MMO capacity or distributed region
fencing. The modeled economy remains bounded currency and one item with one-way
gifts; game rules and catalogs retain their later owners.

The exact next checkpoint is **M03e: Database migration and restoration**.
Migration/backup/restore are M03e; final integrated qualification and full
REQ-M03 closure remain M03f. Earlier handoffs retain their historical evidence.

Suggested commit: `feat(storage): add bounded runtime recovery and completion fencing`.
