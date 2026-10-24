# M03b: Owned durable characters

Date: 2026-10-24. Assignment: M03b, second checkpoint of REQ-M03.
Base: `d2c521ebb66f023bb5e1ff7d9c4936a2300518b3` on initially clean `main`.
Result: uncommitted working tree; staging, commits and pushing belong to the user.

**Status: PASS under the current-machine scope. Full REQ-M03 remains NOT_RUN.**
The [qualification receipt](../coverage/evidence/M03b-owned-characters.json)
binds the final results to tested inputs and retained evidence.
Follow the [six-checkpoint plan](../roadmap/durable-state-plan.md),
[ADR 0016](../architecture/0016-owned-durable-characters.md),
[character contract](../durable-characters.md) and [store contract](../durable-store.md).
The exact next checkpoint is **M03c: Atomic transactions**.

## Delivered behavior

EngineStorage now persists account enrollment, certificate enrollment/revocation,
immutable character ownership, stable CharacterId/WorldEntityId and bounded Name
and canonical WorldPosition checkpoints. Schema 2 validates every table, expected
index and record before activation. A genuine schema-1 fixture rejects without
changing its bytes; there is no automatic migration. M03a's historical evidence
retains its original schema-1 meaning.

One server-only CharacterAuthority owns Store and the existing SessionAuthority.
Startup hydrates the complete bounded registry, then applies revocation
tombstones before admission. New account-only admission checks actual live mTLS
and shares existing authentication budgets so first-character creation does not
require an existing character. It also fences connections older than the latest
successful Login. Selection retains the existing session protocol.

Ready owned sessions, matching incarnations and exact command sequences guard
load/save. Command admission precedes storage access; admitted revision conflicts
consume their sequence/budget. Pure invalid state rejects before admission.
Server-accepted state is never a player-authoritative snapshot. Replacement,
logout, expiry, disconnect and revocation cannot revive stale ownership.

Creation compares a roster revision and commits generated character/entity IDs
with the advanced roster. Save compares a character revision and acknowledges
the complete checkpoint only after commit. Lost replies require explicit
reconciliation; there is no automatic retry or economic deduplication yet.
Uncertain commits or failed rollback quiesce storage; the coordinator also
quiesces on storage failure or unexpected postcommit authorization publication.
Reopen recovers committed state and revocation without restoring live sessions.

Production changes are the Core session admission service and public header,
`server/storage/`, and Server target sources. Tests extend the existing storage
and session families and add `storage_character_tests.cpp`,
`storage_character_probe.cpp` and its Python process supervisor. The new
[ADR](../architecture/0016-owned-durable-characters.md), contracts, coverage,
planning, focused build skill and `.gitignore` record the same boundary.
SQLite remains private to EngineStorage; no dependency pin changes.

## Qualification

The frozen input set contains **318 source/build/test/fixture/contract inputs
and 15 final helpers**, unchanged throughout the amended final matrix.
All **539 full CPU CTests pass, with zero failures or skips**:

| Profile | Passed CTests |
| --- | ---: |
| Windows Debug | 87 |
| Windows Release | 87 |
| Windows ASan | 87 |
| WSL CPU client GCC Debug | 80 |
| WSL CPU client Clang ASan/UBSan | 80 |
| WSL headless Server GCC Debug | 59 |
| WSL headless Server Clang ASan/UBSan | 59 |

Standalone CPU Tools builds register 59 tests each and separately pass all five
authoring families per OS, **ten additional passes**. Each full profile verifies
all 32 named store cases and 16 named character cases, the retained 48-row
initializer process receipt and the seven-phase authenticated character receipt.

Protected Runtime and Server builds pass. Native and WSL Server packages contain
exactly 12 files; WSL's package runs in the retained SDK-free Ubuntu userspace
with an isolated network and a recorded 92-package inventory. Native Runtime
relocation passes with a restricted environment and 33 payload files plus its
manifest; ten missing-license controls reject incomplete packages. These are
isolated local package checks on this PC, not independent-machine evidence.

Both documented public C++ examples compile with MSVC, GCC 13 and Clang 18.
Six native configuration audits verify actual dependencies and the private
SQLite boundary; native ASan object checks cover ten required translation units.
WSL audits verify current sanitizer compilation inventories and isolated runtime
dependencies. API audit, case-report parser and copied process-receipt negative
controls pass. Final formatting, contract, repository, whitespace, source-freeze,
Git index and ignore-boundary checks pass; the index remains empty.

Focused native evidence passes 32 store cases (3,421 assertions), 16 character
service cases (890 assertions), the session family and initializer process
fixture. GCC and Clang ASan/UBSan WSL preflight each pass all five selected
storage/session families. Formatting passes 175 files; contract validation's
37 negative controls and repository skill/ignore checks pass.

The authenticated process fixture has seven fresh-server phases, 17 total child
processes and 14 mTLS connections, with at most three child processes at once.
Two accounts own two characters. Create/save, full restart/read, replacement,
logout/expiry, certificate revocation, account revocation and restart denial
preserve exact identities/state/revisions and expected database hashes.
Twelve supervisor negative controls reject incomplete/altered receipts and
nonzero/timeout/cleanup failures. A supplemental optimized-Python run passes
the same fixture; its retention report distinguishes preflight provenance from
the final frozen runs.

The current machine is Windows 11 build 26200 on Intel Core i9-14900HX with
approximately 32 GiB RAM and the retained WSL Ubuntu environment. This increment
needs CPU/network/local-storage evidence, not GPU execution. It does not qualify
the full 16-player LOCAL-MMO-1 workload, a live simulation tick, a graphical
client, physical power loss or distributed ownership. No host/WSL settings,
software installation or remote service changes were made.

## Commands and retained findings

From the repository root, native commands use the existing wrapper:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03b-work/verify-windows.py
wsl.exe -d Ubuntu -u root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/M03b-work/wsl-run.sh
python out/M03b-work/wsl-runtime-audit.py
python out/M03b-work/wsl-receipt.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03b-work/verify-runtime-package.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03b-work/api-audit.py --final
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/M03b-work/final-checks.py
python out/M03b-work/record-evidence.py
```

These ignored local helpers preserve exact configure/build/test/package commands
and source hashes. Portable behavior tests remain registered in CTest; checkout
builds do not require the helpers or ignored local roadmap.

Preflight found and corrected two fixture assumptions: a supported hot-journal
header still expected version 1, and malformed ownership setup was blocked by
foreign-key enforcement before it could reach the reader. The latter now
explicitly disables constraints only in the independent corruption fixture.
The probe uses C++20 UTF-8 filesystem conversion and closed-sibling publication
for readiness/discovery. An intermediate configure observed a test file before
its parallel author finished creating it; PowerShell stderr teeing also treated
a dependency deprecation warning as a terminating native error. Logs are
retained; final drivers capture actual subprocess exits without those races.
No acceptance limit was relaxed.

The first full WSL attempt passed protected Server Release and all 59 Server
Debug CTests. A separate direct Catch report command then inherited the
read-only source working directory and could not create its fixture databases.
The preserved attempt is `out/M03b-work/wsl-qualification-r1`; the helper-only
amendment makes direct case reports run in writable `build/tests` on both
platforms. Original helper/freeze bytes are retained under
`out/M03b-work/history/initial-wsl`. All 318 engine/test/contract input hashes
remain identical. The final matrix reran successfully with the amended helpers;
the receipt validates both freezes, the exact two helper-only diffs and the
preserved failed attempt. That initial attempt is not counted in the final total.

## Coverage and remaining scope

`server.durable-characters` maps to executable store/service/session/process
tests; `server.durable-store` retains schema/initialization/dependency coverage.
Both human and terminal developers use the same device-free C++ services.
Fixture discovery is local test metadata, not an advertised roster protocol,
authoring command or administration endpoint. FeatureWorld and MMOWorld remain
10/M09. Server bootstrap remains idle with no automatic database/listener flags.

Migration from schema 1, inventory/currency transactions, durable operation
deduplication, full outage/crash recovery and backup/restore were not implemented
or qualified here; they retain M03c-e. Synchronous store operations must not be
inserted unbounded into a live tick. Off-thread persistence must reuse Core's
bounded completion machinery and explicit owner/operation fencing. Full REQ-M03
requires M03c-f before it can pass. There are no open M03b blockers under the
current-machine scope. The next checkpoint is **M03c: Atomic transactions**.
