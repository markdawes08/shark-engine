# 08d: Integrated project and scene persistence qualification

Date: 2026-10-22. Assignment: 08d, final checkpoint of REQ-08.
Base: `49cc7b6355a1fff80e774bc5e52e071a319585a4` on initially clean `main`.
Result: uncommitted working tree; staging, commits and pushing belong to the user.

**Status: PASS under the current-machine scope. Full REQ-08 is PASS.**
The [qualification receipt](../coverage/evidence/08d-integrated-qualification.json)
binds the final results to the tested inputs.
Follow the [checkpoint plan](../roadmap/persistence-plan.md),
[source contract](../source-documents.md), [authored owner](../authored-scenes.md)
and [file service contract](../project-files.md).

## Delivered behavior and review

The existing EngineTools implementation and public formats are unchanged. The
integrated public fixture now carries independently authored version-0 project
and scene files through native reads, in-memory migration, activation, Core edits,
explicit saves and reopen as version 1. It verifies source ordering, authored IDs,
hierarchy, world/cell identity, extras, composed transforms and new entity data.
Reading/loading preserves the original legacy bytes; saving one source does not
silently migrate the other. Reopen creates a fresh incarnation without gameplay
authority and rejects old handles.

Nine invalid external scene variants exercise truncated JSON, future schema,
duplicate keys/entities, missing parent, foreign project/world/scene and composed
transform overflow. Both dirty-state policies preserve the active incarnation,
complete metadata, unsaved edits and on-disk bytes on failure. Dirty rejection
precedes composed-transform preparation; explicit discard reaches composition
validation. A valid repair still needs the explicit dirty-state decision.

The external-edit chain retains both authors' owned snapshots, rejects stale file
and scene revisions, explicitly reloads and reapplies the local edit, then saves
and reopens the combined result. Catalog edits have their own revision and are
adopted only by an explicit project reread and owner construction.

The existing native fault family now runs five prepublication faults through
`save_scene`, checking the complete dirty snapshot, original handle, revision,
disk bytes and stage cleanup. A postpublication flush fault returns committed
bytes and an acknowledged clean baseline with limited durability; stale retry
rejects, and reopening recovers the committed scene.

The process fixture now interrupts an actual edited owner's `save_scene` at all
five native publication phases. Another process activates the exact expected
before/after scene, recovers through a Core edit and authored save, and a further
process reopens the exact recovered scene. Whole-file hashes, lock release,
exclusive ownership and untouched orphan stages remain required. There are 55
receipt checks, including recovery hashes and expected inspection arguments.

Changed executable sources are the new `tests/persistence_integration_tests.cpp`,
its registration in `tests/CMakeLists.txt`, the existing file/process fixtures and
`tools/tests/test_project_files_process.py`. No production source, public header,
dependency pin, wire protocol or source format changed. Planning, coverage,
focused skills, `.gitignore`, this handoff and the final receipt are maintained.

## Qualification

The freeze records 299 source, build, test and fixture inputs plus 18
qualification helpers, all unchanged through the final checks. Six focused
families pass on MSVC, GCC and Clang ASan+UBSan before the full matrix. All seven
full CPU profiles pass: **504 CTests, zero failures or skips**. Independent Tools
builds each register 59 tests and select the five authoring families, adding ten
passes separately from the full-suite total.

| Profile or check | Result |
| --- | --- |
| Windows Debug | PASS, 82/82 CPU CTests |
| Windows Release | PASS, 82/82 CPU CTests |
| Windows ASan | PASS, 82/82 CPU CTests |
| WSL client GCC Debug | PASS, 75/75 CPU CTests |
| WSL client Clang ASan+UBSan | PASS, 75/75 CPU CTests |
| WSL Server GCC Debug | PASS, 54/54 CPU CTests |
| WSL Server Clang ASan+UBSan | PASS, 54/54 CPU CTests |
| Independent Tools builds | PASS, 5/5 focused CTests per OS |
| Protected builds and isolated Runtime/Server packages | PASS |
| Three public examples and six native dependency/API profiles | PASS |
| Formatting, contract, repository, ignore and empty-index checks | PASS |

The new integration family uses seed 1337, labels `cpu;tooling;integration` and a
90-second timeout. Its three cases pass 1,543 assertions on both platforms. The
31 file cases pass 2,042 assertions on Windows and 2,040 on Linux; native topology
branches account for the difference. Codec and activation families retain all
28 and 23 cases. Seven process receipts each pass the independent 55-check
validator, including exact before/published/recovered hashes across platforms,
expected activation arguments, exit 73, lock release and untouched orphan stages.
Six deliberately corrupted process receipts and four compiler-input controls
reject as intended. No behavior test failed and no acceptance limits changed.

All three published C++ examples pass 27 compile/dependency checks using only
public Core/Tools, locked GLM and system headers. Six native compile-database
profiles preserve the private dependency boundary; nine source/fixture objects
carry Windows ASan instrumentation. WSL confirms Clang ASan+UBSan on 116 TLS C
units per sanitizer profile and the required 16 Server/25 client C++ units. All
fourteen network CTest family contracts and their process receipts pass with
unchanged labels, timeouts and fixtures.

The Windows Runtime package contains 33 files and launches outside the checkout
from a path with spaces and Unicode using a system-only PATH. All ten missing-
license controls reject. The WSL Server package contains 11 files and runs in the
unchanged SDK-free userspace with 92 installed packages. Source readers, JSON,
authoring fixtures and lock/stage files do not enter either package. Final
formatting passes for 165 C++ files; contract self-test passes 37 negative checks,
repository self-test verifies all five skills and ignore eligibility, whitespace
checks pass and the Git index remains empty.

Review corrected one evidence-counting issue without changing tested inputs:
the frozen helper's simple case-name parser required the title on the same line
as `TEST_CASE(`, so it reported 30 file cases. The formatter placed the new long
title on the following line. The test executable runs all 31. Final aggregation
accepts the multiline declaration, requires every case in the retained Windows
JUnit inventories and checks the complete-family totals in WSL CTest output.
Original freeze/driver records remain unchanged with this explicit correction.
The dirty-policy precedence expectation was corrected before the first focused
test execution; no production fix or acceptance-limit change was needed.

The exact local driver commands are:

```text
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08d-work/preflight/run.py','--platform','windows','--attempt','windows-final')
wsl -d Ubuntu -u root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/08d-work/preflight/wsl-run.sh wsl-final
.tools/venv/Scripts/python.exe out/08d-work/audit-negative.py
.tools/venv/Scripts/python.exe out/08d-work/receipt-negative.py
& ./tools/windows_dev.ps1 -CommandArguments @('python','tools/check_format.py')
.tools/venv/Scripts/python.exe out/08d-work/freeze.py
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08d-work/verify-windows.py')
wsl -d Ubuntu -u root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/08d-work/wsl-run.sh
.tools/venv/Scripts/python.exe out/08d-work/wsl-runtime-audit.py
.tools/venv/Scripts/python.exe out/08d-work/wsl-receipt.py
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08d-work/api-audit.py','--final')
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08d-work/verify-runtime-package.py')
.tools/venv/Scripts/python.exe out/08d-work/record-evidence.py
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08d-work/final-checks.py')
.tools/venv/Scripts/python.exe out/08d-work/record-evidence.py
```

Ignored helpers retain expanded commands, source hashes, inventories, JUnit,
toolchain reports, packages and raw receipts under `out/08d-work`. They are local
evidence artifacts; registered tests/builds do not require these helpers or the
ignored roadmap. The [building guide](../building.md) documents the ordinary
focused CTest path.

## Coverage and limits

The integrated family maps to the existing `tools.source-documents`,
`tools.authored-scene`, `tools.project-files`, Name and Transform coverage entries.
FeatureWorld/Persistence attaches in 10. Shared history remains 09, terminal
adapters 15, editor UI 16 and Lua 19. No new command/binding placeholders are added.

Qualification uses this Windows 11 x64 PC, Intel Core i9-14900HX, and its existing
WSL Ubuntu CPU/headless environments. Native builds use MSVC 19.51 and Windows SDK
10.0.28000.0; WSL uses GCC 13 and Clang 18 with native window/graphics OFF. Host and
WSL resource settings remain unchanged. Windows relocated execution retains
development tools installed on the host; the isolated WSL runtime is SDK-free.
Neither is an independent physical machine or broad shipping-platform claim.

The file contract's limits remain unchanged: cooperating writers use a selected
canonical-root lock; external writers must not race final compare/rename and
hostile topology changes must not race final identity-check/syscall. No generic
filesystem compare-and-swap, multi-file transaction, arbitrary metadata
preservation, network-filesystem or hardware power-loss guarantee is added.
Windows confirms file flushing; Linux additionally fsyncs the parent directory.
Process termination tests qualify software recovery and local visibility.

Graphics, audio, hosted runners, other machines and MMO capacity are not run for
this CPU authoring increment. Authored content never grants live authority or
implements character/economy durability. There are no remaining in-scope 08
blockers. The exact next assignment is **M03: Durable characters and transactional
state**.
