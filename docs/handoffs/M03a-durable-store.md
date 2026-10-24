# M03a: Server-owned durable store foundation

Qualification: 2026-10-22. Handoff finalized: 2026-10-24.
Assignment: M03a, first checkpoint of REQ-M03.
Base: `01968d56cfae7e5faa999fa414dbf73e2b3c75c2` on initially clean `main`.
Result: uncommitted working tree; staging, commits and pushing belong to the user.

**Status: PASS under the current-machine scope. Full REQ-M03 remains NOT_RUN.**
The [receipt](../coverage/evidence/M03a-durable-store.json) binds the results to
the tested inputs. Follow the [six-checkpoint plan](../roadmap/durable-state-plan.md),
[ADR 0015](../architecture/0015-durable-store.md) and
[store contract](../durable-store.md). The exact next checkpoint is **M03b:
Owned durable characters**; it has not been implemented by this increment.

## Delivered behavior

EngineStorage provides explicit creation and reopening of a real local SQLite
database, immutable typed store/world identity and independent version-1 database
metadata. It owns one creating-thread connection and returns existing owned
`Result`/`Error` values. Exclusive local ownership rejects another opener promptly;
normal destruction and process termination release ownership. A native exclusive
file reservation prevents create from overwriting an existing path. Schema and
metadata creation commit in one real transaction.

Open rejects missing, malformed, foreign, future-version and mismatched-world
stores. Raw header admission precedes SQLite recovery; recognized version-1 files
may undergo hot-journal recovery before deeper schema/world validation. The
contract distinguishes those cases and documents failed-create artifacts and
explicit retry responsibilities. Validation checks exact schema, one metadata
row, identity, configured limits and database integrity. Files, paths, SQL inputs,
cache suggestions and sidecars have explicit bounds; main files are capped at
16 MiB and admitted rollback journals at 32 MiB.

SQLite 3.54.1 is pinned by immutable archive, source and license hashes. Only the
amalgamation is compiled, with extension loading disabled and fixed private
options. The selected rollback-journal policy is DELETE with synchronous EXTRA
and exclusive locking. The upstream header's public-domain notice is preserved
as the installed SQLite notice. The offline Server path adds a separately
prepared `SHARK_SQLITE_ARCHIVE`; the existing three Core archives are unchanged.

EngineStorage is available only with Server and privately links SQLite. Core,
Client, Runtime, Tools and Editor reject storage dependency leakage. Public
storage headers expose neither SQLite types nor SQL. Server bootstrap links the
new library but remains idle: it does not automatically open a store or expose
database flags. No character, economy, player-protocol or generic SQL API is added.

Production changes are `server/storage/`, `cmake/StorageDependencies.cmake`,
target/boundary configuration and the dependency lock. New unit and separate-
process fixtures live in `tests/storage_tests.cpp`, `tests/storage_process.cpp`
and `tools/tests/test_storage_process.py`. Dependency preparation, source/link/
package inspectors and their fixtures now enforce the storage boundary. Planning,
coverage, focused skills and `.gitignore` are maintained. Generated databases and
journals remain under ignored build/output directories; source database fixtures
are not globally ignored.

## Qualification

The freeze records 310 source/build/test/fixture/contract inputs and 15 helpers,
unchanged through final qualification. All seven full CPU profiles pass:
**525 CTests, zero failures or skips**. Independent Tools configurations register
59 tests each and run five focused authoring families, adding ten passes counted
separately from the full-suite total.

| Profile or check | Result |
| --- | --- |
| Windows Debug | PASS, 85/85 CPU CTests |
| Windows Release | PASS, 85/85 CPU CTests |
| Windows ASan | PASS, 85/85 CPU CTests |
| WSL client GCC Debug | PASS, 78/78 CPU CTests |
| WSL client Clang ASan+UBSan | PASS, 78/78 CPU CTests |
| WSL Server GCC Debug | PASS, 57/57 CPU CTests |
| WSL Server Clang ASan+UBSan | PASS, 57/57 CPU CTests |
| Independent Tools configurations | PASS, 5/5 focused CTests per OS |
| Protected Runtime/Server builds and local packages | PASS |
| Public storage example and private dependency/API audits | PASS |
| Formatting, contract, repository, ignore and empty-index checks | PASS |

The storage unit family contains 18 cases with seed 1337 and a 60-second timeout.
The 90-second process family produces 48 validated rows per profile. It covers
Unicode paths, create/reopen identity, competing owners, release, abrupt exit73,
interrupted creation at schema-written/before-commit/after-commit phases and an
actual supported-version hot journal. The hot-journal fixture flushes uncommitted
pages, abruptly exits, then reopens through the public store API and verifies
rollback to the original metadata, journal cleanup and a second exact reopen.
These are real file/process checks, not an in-memory database substitute.

Eight embedded supervisor negative controls exercise the actual entry point,
including malformed identity/metadata, child failure, deadline/cleanup failure
and an empty `AssertionError`. Ten copied-receipt corruptions and six API-audit
controls reject. The three new CTest families preserve explicit labels/timeouts;
the dependency family has a 240-second timeout. Existing fourteen network
families and their contracts/receipts remain part of every applicable full suite.

The documented public example compiles using MSVC, GCC 13 and Clang 18 with only
public headers and approved GLM/system includes. Native compile/dependency audits
cover six profiles, verify SQLite compile options and inspect actual ASan object
instrumentation. WSL verifies source, header, target, link and ELF closure as well
as sanitizer instrumentation. Protected Runtime and standalone Tools never
populate or compile SQLite.

The Windows Runtime package contains 33 payload files plus its manifest. It
launches outside the checkout from a path containing spaces and Unicode with a
system-only PATH, and rejects
all ten missing-license controls. Native and WSL Server packages contain 12 files,
including the locked SQLite notice. The installed WSL Server launches in the
unchanged SDK-free userspace with 92 packages and identical installed-file hashes.
No database, journal, test probe, source reader, SQLite shell or shared SQLite
library enters these packages.

Final formatting covers 170 C++ files. Contract self-test passes 37 negative
checks; repository self-test verifies all five skills and source eligibility.
Whitespace and ignore checks pass and the Git index remains empty.

## Findings and retained attempts

The initial Windows focused run exposed two fixture defects: VACUUM could not
construct the unsupported-page-size case because ATTACH is intentionally disabled,
and narrow argv lost Unicode path characters. The fixture now edits the unsupported
header explicitly and the Windows probe uses native wide arguments. Initial GCC
compilation rejected three copied structured bindings under warnings-as-errors;
they now bind const references. Failed attempts remain in the raw evidence and
were followed by passing focused and full runs.

Independent review corrected the compound-SELECT limit from zero (SQLite's
unlimited value) to one, added limit readback, preserved I/O/SQLite error categories
and fixed a supervisor failure-state check that could accept an empty
`AssertionError`. Actual-entry-point negative controls now require nonzero failure
and a nonempty diagnostic. All corrections preceded the final freeze; no gate or
acceptance limit was weakened.

The first full Windows Release attempt timed out in `shark.tooling.processes`
across a host sleep interval. Windows Power-Troubleshooter records sleep at
12:06:53 UTC and wake at 13:04:40 UTC; the test recorded 3,463 seconds across
that gap. The failed attempt remains unchanged under `windows-final`. A separately
hashed continuation driver carries forward the completed Debug result with
explicit hash-verified provenance, reruns the entire Release suite and completes
ASan/protected profiles under `windows-r2`. Sources and all original 15 frozen
helpers remain unchanged; no power settings or timeout limits were altered.
The final 525-test total counts the selected passing profiles once, excluding
the retained failed Release attempt.

## Commands and evidence

The retained drivers record expanded commands, versions, hashes, inventories,
JUnit, package lists and process outcomes under `out/M03a-work`. Principal exact
commands from the repository root are:

```text
.tools/venv/Scripts/python.exe tools/storage_dependency.py --output out/M03a-work/dependencies
& ./tools/windows_dev.ps1 -CommandArguments @('cmake','--preset','windows-debug','-DSHARK_SQLITE_ARCHIVE=C:/Users/mrkda/Projects/shark/out/M03a-work/dependencies/sqlite-amalgamation-3540100.zip')
& ./tools/windows_dev.ps1 -CommandArguments @('cmake','--build','--preset','windows-debug','--target','StorageTests','StorageProcess','Server','--parallel','2')
& ./tools/windows_dev.ps1 -CommandArguments @('ctest','--preset','windows-debug','--show-only')
& ./tools/windows_dev.ps1 -CommandArguments @('ctest','--preset','windows-debug','-R','^shark.storage.','--output-on-failure','--output-junit','C:/Users/mrkda/Projects/shark/out/M03a-work/preflight-windows-r3.xml')
& ./tools/windows_dev.ps1 -CommandArguments @('ctest','--preset','windows-debug','-R','^shark\.(boundary|tooling\.(storage-dependencies|scene-dependencies|source-document-dependencies|graphics-dependencies|server-closure|runtime-package))$','--output-on-failure','--output-junit','C:/Users/mrkda/Projects/shark/out/M03a-work/preflight-boundaries.xml')
.tools/venv/Scripts/python.exe out/M03a-work/freeze.py
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/M03a-work/verify-windows.py')
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/M03a-work/resume-windows.py')
wsl.exe -d Ubuntu -u root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/M03a-work/wsl-run.sh
.tools/venv/Scripts/python.exe out/M03a-work/wsl-runtime-audit.py
.tools/venv/Scripts/python.exe out/M03a-work/wsl-receipt.py
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/M03a-work/api-audit.py','--final')
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/M03a-work/verify-runtime-package.py')
.tools/venv/Scripts/python.exe out/M03a-work/record-evidence.py
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/M03a-work/final-checks.py')
.tools/venv/Scripts/python.exe out/M03a-work/record-evidence.py
```

Ignored qualification helpers are local evidence artifacts; registered builds and
tests do not depend on them or the ignored roadmap. The [building guide](../building.md)
documents ordinary configure/build/CTest and offline dependency commands.

## Coverage and limits

`server.durable-store` maps to the three executable storage/dependency families
and a justified external real-file/process fixture. FeatureWorld begins in 10;
MMOWorld acceptance remains M09. No command/Lua placeholders or premature editor
database controls are added. Both human and terminal developers can use the same
documented CPU C++ services and noninteractive machine-readable test results.

Qualification uses this Windows 11 x64 PC with Intel Core i9-14900HX and its
existing WSL Ubuntu environments. Windows uses MSVC 19.51 and SDK 10.0.28000.0;
WSL uses GCC 13 and Clang 18 with native presentation disabled. No host/WSL resource
settings changed. Windows retains installed developer tools; the isolated WSL
runtime is SDK-free. Neither establishes independent-machine or broad release
qualification.

This local exclusive owner is not distributed region fencing. External SQL and
concurrent path/topology replacement require caller coordination. Bounds are not
total heap/disk quotas. Process interruption and hot-journal recovery do not
simulate physical power loss or disk loss. Character/session durability, economic
deduplication, complete mutation/outage recovery, migrations and backup/restore
remain M03b-f. No GPU/audio, hosted-runner, additional-machine or MMO-capacity
tests were required or claimed. There are no remaining M03a blockers.
