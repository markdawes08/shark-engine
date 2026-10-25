# 09e: Integrated qualification

Date: 2026-10-25. **09e and full REQ-09 are PASS under the current-machine scope.**
Assignment: the fifth of the [authoring checkpoints](../roadmap/authoring-plan.md).
Base: `e8fb808cafa71574b48f050ed76cead8de6451a0` on initially clean `main`.
Result: uncommitted working tree; staging, commits and pushing belong to the user.
The [final receipt](../coverage/evidence/09e-integrated-qualification.json) records
all executed gates. **Next: 10, FeatureWorld and automated scene runner.**

## Delivered behavior

The new [qualification contract](../authoring-qualification.md) and eight-case
`AuthoringIntegrationTests` fixture connect the existing services. No production
implementation, dependency or public format changed. One real project-file lineage
and one AuthoredScene carry alternating human-equivalent and agent-equivalent
edits through pure preview, atomic commit/rejection, common undo/redo, retained
success/noop/failure replies, save, external text changes, explicit reload and
reconciliation. Complete expected source documents come from independently
authored legacy/current fixtures, not snapshots of a second mutable scene model.

The main lineage explicitly migrates a version-0 project, preserves original
scene bytes until save, then creates/reparents/transforms through a batch. It
retains survivor handles, source order, extras, Unicode and global cells. A lost
reply replay after another edit/save returns the immutable original without
reapplying. Undo/redo share one history across both callers. Invalid intermediate
hierarchy rolls back the entire batch, and its terminal error remains retained.

A real external text edit conflicts with local unsaved work. Save and ordinary
reload preserve full owner state and external bytes on failure. Explicit discard
reload replaces Scene incarnation and clears history while retaining the same
mutation ledger. Old handles expire; old mutation replies remain read-only. A
reconciled new intent is saved, both owners are destroyed, and reopening verifies
exact source values plus a fresh mutation scope. Old IDs report unknown outcome
without writing. The existing real three-process restart fixture also remains
required in every applicable final profile.

Additional connected cases preserve redo across preview/noops/rejections and
invalidate it only on a changed branch; restore deleted hierarchy and source
metadata with fresh handle generations; preserve history/retries through five
observed native I/O failures and explicit successful recovery; and permanently
fence expired edit/history identities across reload. A postpublication callback
submits another retained edit, proving a valid saved snapshot cannot acknowledge
a newer dirty scene. An injected directory-flush failure is a committed durability
limitation on both OS targets, never a rollback.

Only the private native file-fault seam is used by the new fixture. All edits,
queries, capture, preview, history, retry and persistence behavior goes through
public services. No editor/terminal adapter, gameplay RPC, mutable registry bypass
or generic job system is added. Future import/export jobs retain 11-14; project
ownership/terminal and editor integration retain 15/16. Gameplay still uses Core
safe mutation without authoring history, and M03 owns live economic durability.

## Qualification evidence

All seven full profiles pass with no failures or skips: Windows Debug, Release
and ASan each execute 101 CTests; WSL GCC Debug and Clang ASan/UBSan each execute
94; headless Server Debug and ASan/UBSan each execute 67. Total: **625 full-suite
CTest executions**. Standalone Tools executes all eleven authoring families per
OS: **22 additional checks**. Both Server suites are refreshed here.

`shark.tools.authoring-integration` has a 90-second timeout and seed 1337. Preserve
all eight base cases. Every earlier CTest name, label, timeout, seed and source
workload remains, including the 4096-entity/4095-delete fixture, all six retry
allocation sweeps with their 512 ceilings, the 25 retry cases, the 31 file cases,
the 55-check interrupted-save fixture and separate-process restart evidence.

Eight complete public Tools examples compile on MSVC, GCC and Clang: **24
compiles**. Compiler dependency reports exclude private headers/dependencies.
Actual ASan flags/symbols are audited in 14 native objects, ASan/UBSan in 15 WSL
client/authoring objects and two Server history objects. The new connected fixture
is included. All protected Runtime/Server builds and closures, relocated native
Runtime deployment/license rejection checks, and the isolated SDK-free WSL Server
package pass. The latter retains 92 runtime packages and 12 installed files.

The frozen set contains **361 source/contract inputs and 16 helpers**, with SHA-256
`0fbf6043406bddf310115002defaa9ea34b61767e2e5435fe66a4ad08a57febe`. The receipt retains exact commands, inventories, JUnit, every base
Catch case, actual process receipts, object audits, package records and artifact
hashes. Inventory/JUnit/Catch parser controls reject 43 malformed records; final
metadata validation rejects seven generic failures and five native-audit inventory
failures. All helpers are frozen before final execution.

## Preparatory outcomes and reproduction

Initial new-fixture compilation corrected the actual public member/constant
names. Its first fault expectations were corrected to existing `io_failure` and
injected `directory_flush_failed` behavior, and Unicode stage enumeration now uses
UTF-8 paths. An optional callback member initializer is explicit for GCC. The
first WSL preflight passed all connected/authoring cases but hit the unchanged
10-second boundary limit during overlapping native work; serial preflight passes
both WSL profiles. A variable-shadowing mistake in the evidence helper was fixed
and its controls passed before the freeze. All attempts are retained and excluded
from the final gate. No previous workload, acceptance limit, host setting or
production behavior was changed to pass qualification.

Representative final commands from the repository root:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09e-work/verify-windows.py final
.tools/venv/Scripts/python.exe out/09e-work/launch-wsl.py final
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09e-work/native-audit.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09e-work/verify-runtime-package.py
```

Ignored evidence destinations are create-only; preserve a completed attempt
before preparing a fresh run. These local helpers are not checkout/CI inputs.
Versioned focused tests run through CTest with
`-R ^shark.tools.authoring-integration$`. Source persistence and local C++ authoring
are qualified on this Windows PC and retained WSL environments; no new GPU,
other-machine or production MMO-capacity claim is made. Human/terminal adapters
are still future work. FeatureWorld connects supported components and commands
in assignment 10; its fixtures are not retroactively claimed here.
