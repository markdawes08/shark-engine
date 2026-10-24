# M03c: Atomic transactions

Date: 2026-10-24. Assignment: M03c, third checkpoint of REQ-M03.
Base: `64ffe0b8b2a00d4bbf75ef54c0562148b0ac69e1` on initially clean `main`.
Result: uncommitted working tree; staging, commits and pushing belong to the user.

**Status: PASS under the current-machine scope. Full REQ-M03 remains NOT_RUN.**
The [qualification receipt](../coverage/evidence/M03c-atomic-transactions.json)
binds the results to tested inputs and retained evidence. Follow the
[six-checkpoint plan](../roadmap/durable-state-plan.md),
[ADR 0017](../architecture/0017-atomic-durable-transactions.md),
[transaction contract](../durable-transactions.md),
[character contract](../durable-characters.md) and [store contract](../durable-store.md).
The exact next checkpoint is **M03d: Runtime failure recovery**.

## Delivered behavior

EngineStorage schema 3 adds economic records, account-wide operation high-water
marks and terminal receipts to the existing real Store. Each owned character
may be initialized once by trusted server setup. Currency and one modeled item
quantity are integers from zero through 1,000,000,000; economic revisions are
independent of character checkpoints and stop at INT64_MAX. There is no later
arbitrary balance setter or implicit grant during character creation.

A one-way transfer checks both participants' expected economic revisions,
debits the actor's selected source character and credits a distinct target.
Both quantities are conserved without negative balances or overflow. Record
writes, operation advancement, terminal receipt and retention pruning share one
SQLite transaction. Candidate state and return values are allocated before
BEGIN. Successful COMMIT precedes acknowledgement and nonthrowing cache
publication. A private fault seam exercises rollback after the source debit;
uncertain commits quiesce Store and CharacterAuthority until destruction,
reopen and explicit reconciliation.

An operation is identified by StoreId, actor AccountId and account-wide sequence.
Only the exact next sequence admits a new outcome. At most eight most recent
terminal outcomes remain replayable: exact retained retries return their original
result before consulting today's target state or revisions. Changed payloads
reject, and evicted IDs remain permanently expired across restart because the
durable high-water mark remains. Retention counts admitted outcomes, without a
clock TTL; sequence 1 expires when sequence 9 is admitted.

Unavailable targets, stale or exhausted revisions, insufficient assets and target
capacity limits commit terminal rejections that consume a sequence and receipt
slot without moving assets. Malformed requests, wrong store/source ownership,
gaps, changed retries and expired IDs reject without durable admission. Callers
check the receipt's terminal status separately from successful Result delivery.
Unavailable targets have no target snapshot.

The existing CharacterAuthority requires the actual current ready mTLS session,
selected character and next live command envelope. Actor and source must match
that session. Admitted queries and retries spend the existing command budget;
subsequent storage conflicts consume the live sequence. Replacement, logout,
expiry, disconnect and revocation still fence retained retries. Restart requires
fresh authentication; durable IDs do not restore sessions or ownership epochs.
Trusted Store audit queries remain available for reconciliation. Economic
snapshots and receipts can contain recipient-private balances and are not player
wire messages. The process fixture sends generic acceptance/denial through
existing session wire version 1; detailed results stay in trusted fixture evidence.

Open validates exact schema, bounded records, canonical requests, contiguous
receipt windows, matching historical snapshots and revision advances explainable
by retained commits plus possible pruned history. Bounds remain 128 economic
records, 512 receipts, 16 columns/bind parameters, a 16 MiB database and a 32 MiB
journal. Independently authored schema-1/2 fixtures reject before writes;
migration remains M03e. Earlier handoffs retain their historical schema meanings.

Production changes extend the existing Store/CharacterAuthority headers and
implementations plus the private write-fault seam. New files are
`tests/storage_transaction_tests.cpp`, `tests/storage_transaction_probe.cpp`,
`tools/tests/test_storage_transactions_process.py`, ADR 0017 and the transaction
contract. Existing storage/service fixtures, CTest registration, coverage,
building/CI guidance, focused build skill and `.gitignore` are updated. SQLite's
pin/options are unchanged and remain private to EngineStorage; Core, Client,
Runtime and standalone Tools do not acquire storage dependencies.

## Qualification

The frozen input set contains **323 source/build/test/fixture/contract inputs and 15 helpers**, unchanged across final qualification.
**All 553 full CPU CTests pass with zero failures or skips**, with **ten additional
standalone Tools passes**. Focused development reruns are separate evidence.

| Profile | Passed CTests |
| --- | ---: |
| Windows Debug | 89 |
| Windows Release | 89 |
| Windows ASan | 89 |
| WSL CPU client GCC Debug | 82 |
| WSL CPU client Clang ASan/UBSan | 82 |
| WSL headless Server GCC Debug | 61 |
| WSL headless Server Clang ASan/UBSan | 61 |
| Standalone Windows Tools authoring families | 5 |
| Standalone WSL Tools authoring families | 5 |

Every full profile verifies the complete named case sets with seed 1337.

| Family | Cases | Assertions in direct reports | Timeout |
| --- | ---: | ---: | ---: |
| `shark.storage.store` | 33 | 3,433 | 60 s |
| `shark.storage.characters` | 21 | 1,368 | 60 s |
| `shark.storage.transactions` | 17 | 4,063 | 90 s |

The new transaction process family has a 180-second timeout and
`cpu;integration;server` labels. Two phases use six children and four mTLS
connections, with at most three children active together. Two clients present
their initial intents before the single owner processes either. One transfer
commits and the other is stale: a gift of 7 currency/2 items changes A's 100/20
to 93/18 and B's 100/20 to 107/22, both at economic revision 2. Totals remain
200/40. Both streams advance to sequence 9, retaining sequences 2–9. A fresh
server and clients then recover exact records/receipts; retained retries,
mismatches and expired IDs leave database bytes unchanged.

Sixteen transaction supervisor controls reject altered/missing evidence,
nonzero exits, timeouts, cleanup failures and an empty exception. The original
initializer process retains 48 checks/eight controls; the character process
retains seven phases, 17 children, 14 mTLS connections and 12 controls. All
fourteen network families remain registered. Fifteen named-case parser controls,
17 API parser/dependency controls, ten copied initializer-receipt controls and
**22 distinct aggregation-validator controls** pass. The latter reject missing
public-example checks, leaked private headers, incomplete sanitizer source sets,
changed compile-database bindings and absent instrumentation.

Protected native Runtime/Server and WSL Server builds pass. Native Server has
12 package files; WSL Server has 12. The WSL package runs in the
retained SDK-free Ubuntu userspace with an unchanged 92-package inventory.
Native Runtime relocation passes with 33 payload files plus 1 manifest;
10 missing-license controls reject incomplete packages.

All three public C++ examples compile with MSVC, GCC 13 and Clang 18.
6 native audits verify actual private-dependency boundaries, and ASan object checks
cover 12 required units. WSL audits confirm the current sanitizer compiler/source
closures and the isolated runtime dependencies. These are local CPU/package
checks, not independent-machine evidence.

Final formatting passes 177 files; contract validation's 37 negative controls and repository checks for 5 skills pass.
Whitespace, ignore boundaries, source/helper freeze and the empty Git index also pass.

## Commands, evidence and closed findings

From the repository root, completed focused commands are:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03c-work/preflight/native-focused.py native-r2
python out/M03c-work/launch-wsl.py preflight --attempt focused-r1
```

The completed WSL final commands exit 0:

```text
python out/M03c-work/launch-wsl.py final
python out/M03c-work/wsl-runtime-audit.py
python out/M03c-work/wsl-receipt.py
```

Completed native technical and evidence-control commands:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03c-work/verify-windows.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03c-work/verify-runtime-package.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03c-work/api-audit.py --final
python out/M03c-work/storage-receipt-negative.py out/M03c-work/windows-final/windows-debug/storage-process.json
python out/M03c-work/aggregation-negative-controls.py
python out/M03c-work/record-evidence.py --draft
python out/M03c-work/publish-handoff.py
```

Final repository and receipt closure commands pass:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03c-work/final-checks.py
python out/M03c-work/record-evidence.py
python out/M03c-work/publish-handoff.py
```

Exact command arrays, exits, logs, inventories, JUnit and process receipts are
retained under `out/M03c-work/`. `wsl-verification.json` records the completed WSL
gate; `source-freeze.json` binds inputs and helpers. The ignored drivers preserve
local evidence; checkout builds and CI use versioned fixtures/presets and do not
depend on these drivers or the ignored roadmap.

Preflight exposed a real receipt-validation defect: changing committed outcomes
to stale outcomes could preserve locally plausible snapshots while failing to
account for current revisions. Reopen now checks that retained commits and
possible pruned operations explain every advance; complete retained history must
account for all advances. The failure remains in `native-preflight/storage.xml`
and `storage.log`. Exhaustion fixtures now include consistent pruned history.

Two compatibility fixture assumptions were also corrected: schema-2 values and
the old three-column account insertion in `initial-storage.xml`, and a raw
hot-journal header check still requiring version 2 in
`native-preflight/storage-process-schema3-failed.json`. Supported recovery and
legacy rejection retain separate coverage. Helper preparation also corrected
adjacent C++ string-literal case-name extraction and rejects optimized Python
qualification. Final source/helper freeze occurred after these fixes; no bound,
required family or workload was relaxed.

## Scope, coverage and next checkpoint

The current hardware record identifies Lenovo 83DG, Windows 11 Home build 26200,
Intel Core i9-14900HX (24 cores/32 logical processors), approximately 32 GiB RAM
and the existing WSL CPU/headless environments. M03c requires no GPU, additional
machine, external service or host/WSL setting change. Process restart and fault
injection do not qualify physical power loss, independent-host availability,
production MMO capacity or a live 20 Hz simulation latency budget.

`server.durable-transactions` maps to executable transaction unit/process and
authenticated service cases; earlier store/character coverage remains. Human and
terminal developers use the same bounded C++ services and noninteractive tests.
FeatureWorld/MMOWorld remain assigned to 10/M09. Server bootstrap remains idle;
no editor UI, authoring CLI, player economy codec or LLM SDK is introduced.

This checkpoint implements one modeled item and one-way gifts. Item catalogs,
bilateral trading, loot and progression remain M07. The complete crash/outage/
lost-acknowledgement matrix, bounded pending work and fenced runtime completion
belong to **M03d: Runtime failure recovery**. Migration and backup/restore remain
M03e; full REQ-M03 integrated qualification remains M03f. Schema-1/2 rejection is
current behavior, not migration support.

Suggested commit: `feat(storage): add atomic economic transactions`.
