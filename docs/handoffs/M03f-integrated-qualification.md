# M03f: Integrated durable-state qualification

Date: 2026-10-25. Assignment: M03f, sixth checkpoint of REQ-M03.
Base: `de9ffac92d82001fabb049eeef08450411aeb75a` on initially clean `main`.
Result: uncommitted working tree; staging, commits and pushing belong to the user.

**Status: PASS under the current-machine scope. Full REQ-M03 is PASS.**
The [qualification receipt](../coverage/evidence/M03f-integrated-qualification.json)
binds this handoff to the tested source, helpers and 658 retained artifact hashes.
Follow the [checkpoint plan](../roadmap/durable-state-plan.md) and
[integrated qualification contract](../durable-qualification.md). The exact next
assignment is **09: Shared authoring commands and undo/redo**.

## Delivered behavior

The new connected fixture carries one actual store through independent legacy
creation, validated backup and migration, authenticated checkpointing, concurrent
economic requests, interrupted work, explicit recovery and isolated restoration.
It exercises the existing public Store and CharacterAuthority services, actual
mTLS peers, the private Store worker and real Scene references. Schema 3, gameplay
wire version 1 and the five existing storage contracts remain unchanged.

The starting schema-2 fixture fixes Store 700 / World 900, accounts 1/2/3,
characters 101/102/103 and entities 201/202/203. Migration preserves identity,
ownership, checkpoints and revocations; trusted one-time initialization separately
supplies each of the two active economies with 100 currency and 20 item units.
The revoked third account and existing certificate tombstone remain revoked.

| Phase | Children | Connected behavior |
| --- | ---: | --- |
| prepare | 1 | Exact pre-upgrade backup, production migration and explicit initial resources |
| runtime | 3 | Two real mTLS clients; checkpoint save and concurrently retained economic requests |
| commit-crash | 2 | Transfer COMMIT followed by server exit 73 before acknowledgement |
| outage | 3 | Original-ID committed replay, observed native SQLite write failure, drained work and explicit recovery |
| recover | 3 | Fresh authentication, original failed-request retry, receipt eviction and durable revocation |
| restore | 1 | Stopped-owner same-point backup and isolated candidate; old backup remains untouched |
| restored | 3 | Fresh owner, exact replay, expiry/mismatch rejection and revoked Login denial |

Account 1's character checkpoint advances from revision 7 to 8 with the name
`Integrated checkpoint`, cell `[55, -8, 9]` and offset `[0.5, 1.25, 777.75]`.
Other character revisions remain 11 and 3. Account 1 operation 1 transfers 7/2;
account 2's competing operation 1 durably records stale revisions. Operation 2
transfers 3/1 before the crash and replays without another effect. Operation 3's
4/1 transfer fails at actual I/O, remains uncommitted and succeeds only on an
explicit fresh-owner retry. Seven stale operations 4 through 10 consume outcomes
without moving resources; operation 11 transfers 2/1.

The exact final economies are **84 currency / 15 items** and **116 / 25**, both
at revision 5. Currency and items conserve totals **200 / 40**. Account 1's
high-water mark is 11 with outcomes 4 through 11 retained; account 2's mark is 1
with its original stale result. Account 2 is durably revoked. After restore,
operation 11 replays, expired operations 1 and 3 stay expired, and changed payload
under retained ID 11 rejects. All old session envelopes reject and permitted
access requires fresh Login. No final read or retry changes durable state.

The supervisor computes these complete expected records independently of child
PASS flags and compares identities, DDL, headers, rows, revisions, receipts and
file hashes. The original pre-upgrade backup is never activated after later
acknowledged writes. Only the latest verified same-point snapshot is activated
after the original owner stops; original and restored file hashes remain stable
through inspection/replay. There is no authoritative rollback or second live owner.

Changed source centers on `tests/storage_integration_probe.cpp` and
`tools/tests/test_storage_integration_process.py`, with CMake registration,
the qualification contract, coverage/status mappings, build/release guidance,
repository skills and `.gitignore`. Existing migration SQL and private VFS
fixtures are reused. No new public API, dependency or production protocol is added.

## Qualification

The receipt binds **347 source/build/test/fixture/contract
inputs and 18 helpers**, unchanged for the final matrix.
Seven complete CPU profiles pass **588 CTests**, with zero failures or
skips, plus **10 standalone Tools checks**. Direct named-case reports,
focused preflights and supervisor controls are separate evidence, not extra CTests.

| Profile | Passing full-suite CTests |
| --- | ---: |
| Windows Debug | 94 |
| Windows Release | 94 |
| Windows ASan | 94 |
| WSL CPU client GCC Debug | 87 |
| WSL CPU client Clang ASan/UBSan | 87 |
| WSL headless Server GCC Debug | 66 |
| WSL headless Server Clang ASan/UBSan | 66 |
| Standalone Windows Tools authoring families | 5 |
| Standalone WSL Tools authoring families | 5 |

Every profile also passes all five named case groups with seed 1337. Distinct
direct-report assertion counts are shown when profiles differ.

| Family | Cases | Direct-report assertions | Timeout |
| --- | ---: | ---: | ---: |
| `shark.storage.store` | 33 | 3,433 | 60 s |
| `shark.storage.characters` | 21 | 1,368 | 60 s |
| `shark.storage.transactions` | 17 | 4,063 | 90 s |
| `shark.storage.recovery` | 21 | 724 | 120 s |
| `shark.storage.migration` | 25 | 742 | 120 s |

Each integrated process run contains **7 phases / 16 children /
9 authenticated connections**, with **38 actual-main failure controls**.
It proves 1 postcommit crash and 1 observed native-write outage phase per profile.
`shark.storage.integration-process` uses `cpu;integration;server` and a 300-second
CTest timeout; children are limited to 30 seconds, readiness to 8 seconds and
concurrency to three children. Accepted JSON is capped at 1 MiB after capture,
and stderr rejects; this is not an OS pipe/disk quota. Failures, missing rows,
changed state, weak evidence, nonzero exits and empty exceptions cannot report PASS.

All prior process regressions remain required in every full profile:

| Prior coverage | Per-profile evidence |
| --- | --- |
| Store initialization and ownership | 48 checks / 8 controls |
| Owned characters and authentication | 7 phases / 17 children / 14 mTLS connections / 12 controls |
| Concurrent economic transactions | 2 phases / 6 children / 4 mTLS connections / 16 controls |
| Runtime mutation crash/outage matrix | 69 scenarios / 207 children / 18 controls |
| Migration/backup/restore matrix | 66 scenarios / 200 children / 24 controls; 2 actual main-write crashes |

The recovery matrix retains detailed precommit, postcommit, lost-reply and native
I/O evidence. All fourteen network families retain their original limits.
Separate validators pass 25 named-case parser controls,
34 API/dependency controls,
10 copied initializer-receipt controls,
12 copied recovery controls,
16 copied migration controls,
24 copied integrated controls and
31 aggregation controls.

Protected native Runtime/Server and WSL Server builds pass. Server packages
contain 12 native files and 12 WSL files;
the WSL package runs in the retained SDK-free Ubuntu userspace. Relocated native
Runtime has 33 payload files plus 1 manifest;
10 missing-license controls reject incomplete packages.
All 5 public C++ examples compile with MSVC, GCC 13, Clang 18 using public
headers. The 6 native dependency audits and 21 required native
ASan units pass; WSL audits verify the actual sanitizer source/flag inventory and
runtime dependencies. SQLite stays private to Server storage and approved test fixtures.

Final checks pass for 189 formatted sources, 37 contract negative controls and 5 repository skills. Whitespace, ignored-output/source eligibility, the unchanged freeze and empty Git index pass.

## Commands and retained findings

Run from the repository root. The receipt retains exact command arrays, exit
codes, source/compiler hashes, inventories, logs, JUnit, case and process reports
under `out/M03f-work/`. Generated helpers do not replace normal checkout presets
or make builds depend on the ignored roadmap.

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03f-work/verify-windows.py
python out/M03f-work/launch-wsl.py final
python out/M03f-work/wsl-runtime-audit.py
python out/M03f-work/wsl-receipt.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03f-work/verify-runtime-package.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03f-work/api-audit.py --final
python out/M03f-work/integration-receipt-negative.py --positive out/M03f-work/preflight/native-r1/storage-integration-process.json
python out/M03f-work/aggregation-negative-controls.py
python out/M03f-work/record-evidence.py --draft
python out/M03f-work/publish-handoff.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03f-work/final-checks.py
python out/M03f-work/record-evidence.py
python out/M03f-work/publish-handoff.py
```

No failed current-increment build or CTest run is recorded. The helper-preparation correction below is retained separately.

Initial native and WSL focused preflights passed. Read-only review requested an explicit reconciliation_required assertion on the injected runtime outage; the probe-only assertion was added and native-r2 plus WSL focused-r2 passed all twelve focused families. No production behavior changed.

New copied-receipt helper preparation initially referenced economy instead of the actual economies snapshot key. Corrected before final qualification; the incomplete preparation output remains isolated and is not accepted gate evidence.

Any recorded development failure is excluded from final passing-profile totals;
its original artifacts remain available. Required test inventories and acceptance
limits are preserved throughout qualification.

## Scope, coverage and next assignment

The recorded machine is LENOVO 83DG, Microsoft Windows 11 Home
build 26200, Intel(R) Core(TM) i9-14900HX (24 cores /
32 logical processors), approximately 32 GiB RAM,
with its existing WSL CPU/headless environments. No GPU gate, other machine,
external account or host/WSL resource change is required or claimed.

REQ-M03 maps each required durable capability to its retained unit, process,
connected scenario and public API evidence. Human and terminal developers use
the same CPU services, noninteractive commands and machine-readable results.
FeatureWorld and MMOWorld retain owners 10 and M09; operational administration
and terminal/editor adapters remain M08 and 15/16. The Server bootstrap remains
idle; these test executables do not add a production gameplay/storage protocol.

This qualification covers a bounded local development workload, not the full
LOCAL-MMO-1 population, 20 Hz simulation latency, seamless-travel capacity,
distributed authority or production readiness. Acknowledged character/economic
effects permit zero authoritative rollback. Old snapshots may omit later writes,
revocations and high-water marks; they remain isolated inspection/reconciliation
candidates and cannot authorize gameplay resumption. No backup cadence, elapsed-time
loss allowance, automatic promotion/replay or downgrade is supplied. Filesystem
calls have no live-tick latency guarantee. Software failures and process restarts
do not qualify physical power/disk/host loss or independent disaster recovery.

The exact next assignment is **09: Shared authoring commands and undo/redo**. Earlier handoffs preserve
their historical scope and evidence.

Suggested commit: `test(storage): qualify integrated durable state`.
