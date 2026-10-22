# 08c: Safe project files and conflicts

Date: 2026-10-21. Assignment: 08c, third checkpoint of REQ-08.
Base: `789a7826496444b2072b0cf439528550aa1a8911` on initially clean `main`.
Result: uncommitted working tree; staging, commits and pushing belong to the user.

**Status: PASS under the current-machine scope.** Full REQ-08 remains NOT_RUN.
Follow the [file contract](../project-files.md),
[persistence checkpoints](../roadmap/persistence-plan.md) and
[qualification receipt](../coverage/evidence/08c-project-files.json).

## Delivered behavior

EngineTools `ProjectFiles` reads and writes typed project/scene documents through
the existing strict codec and Scene validators. It retains the canonical project
root and an exclusive native cooperating-writer lock. Real paths reject links,
reparse points, hard-linked files, case aliases, overlapping catalog paths and
reserved service names. Reads, path depth and directory inspection have fixed
bounds; missing catalog references are permitted while actual save parents must
already exist.

Opaque revisions bind root, relative path and exact bounded original bytes.
Same-size/same-timestamp changes and formatting changes conflict; returning to
identical bytes is accepted as an explicit content-revision policy. New-file
writes require absence. Candidates validate before sibling staging, file flush,
final expected-content comparison and one native rename. Ordinary errors mean
no publication by this operation. Post-publication durability failure returns
a committed result with limited durability, never a false rollback claim.
Normal cleanup removes only the operation's verified stage; process interruption
can leave an orphan that reopening neither activates nor globally deletes.

Load/reload reuse AuthoredScene activation and its dirty/revision/pending/visit
guards. Save captures one committed revision and marks it clean only if that
revision remains current with no pending work. A later edit returns committed
file information with `current_scene_clean=false`; it is preserved in memory.
The source format and schemas are unchanged. Catalogs remain explicit snapshots,
and publication is per file, not a multi-file project transaction.

New production files are `engine_tools/include/shark/tools/project_files.hpp`,
`engine_tools/src/project_files.cpp` and private `project_files_native.hpp/.cpp`.
AuthoredScene adds only a private save acknowledgement. EngineTools CMake and the
exact source-dependency fixture cover the five implementation files. No Core,
Runtime, Server, renderer, network implementation or dependency lock changed.
New public tests and the native process probe/supervisor cover file behavior and
interruptions; tests are separate EngineTools consumers. Documentation, coverage,
focused skills and `.gitignore` are maintained. Generated helpers/evidence stay
under ignored `out/08c-work/`; reserved lock/stage names are ignored without
ignoring authored source fixtures.

## Qualification

The final freeze contains 298 source/build/test inputs and 19 qualification
helpers. Five focused families pass on Windows MSVC, WSL GCC 13 and WSL Clang 18
ASan+UBSan against those same inputs. The file family has 30 cases: 1,832
assertions on Windows and 1,830 on Linux because native topology behavior differs.
All 44 process checks pass in preflight. Formatting passes for 164 files; the
complete public C++ example passes ten dependency/compile checks, and four
compiler-input plus four process-receipt corruption controls reject as intended.

All seven full CPU suites pass: **499 CTests, zero failures or skips**. The
standalone Tools configurations each register 58 tests and deliberately run the
four authoring families, adding eight passes separately from the full-suite total.

| Profile or check | Result |
| --- | --- |
| Windows Debug | PASS, 81/81 CPU CTests |
| Windows Release | PASS, 81/81 CPU CTests |
| Windows ASan | PASS, 81/81 CPU CTests |
| WSL client GCC Debug | PASS, 74/74 CPU CTests |
| WSL client Clang ASan+UBSan | PASS, 74/74 CPU CTests |
| WSL Server GCC Debug | PASS, 54/54 CPU CTests |
| WSL Server Clang ASan+UBSan | PASS, 54/54 CPU CTests |
| Independent Tools builds | PASS, 4/4 focused CTests per OS |
| Protected builds and isolated packages | PASS on Windows and WSL |
| Public example/private dependency audit | PASS, six native profiles plus WSL compiler audit |
| Formatting, contract, repository, ignore and index checks | PASS |

All seven process receipts pass the independent 44-check validator. Each proves
before/after whole-file hashes, five explicit interruption phases, expected exit
73, bounded child cleanup, writer exclusion and unchanged orphan-stage hashes
after recovery. Four deliberate receipt corruptions reject. The compiler audit
checks eight codec/activation/file implementation and fixture objects under
Windows ASan; WSL separately confirms required Clang ASan+UBSan instrumentation,
including the unchanged private TLS graph. All fourteen network CTest contracts
and their regression receipts remain validated.

The Windows Runtime package contains 33 files, launches outside the checkout from
a path with spaces and Unicode using a system-only PATH, and rejects each of ten
missing dependency notices. The WSL Server package contains exactly 11 files and
runs in the unchanged SDK-free userspace with 92 installed packages. Neither
package acquires JSON, authoring code, source fixtures or service lock/stage files.
Windows development tools remain installed; this is isolated execution on the
current PC, not an independent clean-machine claim.

Hardware is this Windows 11 x64 PC, build 26200, Intel Core i9-14900HX with 24
cores/32 logical processors. Native builds use MSVC 19.51 and Windows SDK
10.0.28000.0. Retained WSL Ubuntu uses GCC 13 and Clang 18 with native window and
graphics disabled. No host/WSL resource settings are changed.

The initial 23-case and expanded 29-case Windows runs both passed. A final review
requested one direct ancestor-relocation case before freezing inputs: Windows
blocks moving the held parent and publishes at the original path; Linux detects
the completed move, rejects publication, preserves original bytes and removes
only its own stage. Both preserve a foreign stage. The earlier three-compiler
preflight passed; its logs and exact test source were retained before adding this
case, then all five focused families were rerun against the final source. No
production failure or acceptance-limit change prompted the addition.

The exact final driver commands are:

```text
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08c-work/preflight/run.py','--platform','windows','--attempt','windows-final')
wsl -d Ubuntu -u root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/08c-work/preflight/wsl-run.sh wsl-final
.tools/venv/Scripts/python.exe out/08c-work/audit-negative.py
.tools/venv/Scripts/python.exe out/08c-work/receipt-negative.py
& ./tools/windows_dev.ps1 -CommandArguments @('python','tools/check_format.py')
.tools/venv/Scripts/python.exe out/08c-work/freeze.py
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08c-work/verify-windows.py')
wsl -d Ubuntu -u root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/08c-work/wsl-run.sh
.tools/venv/Scripts/python.exe out/08c-work/wsl-runtime-audit.py
.tools/venv/Scripts/python.exe out/08c-work/wsl-receipt.py
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08c-work/api-audit.py','--final')
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08c-work/verify-runtime-package.py')
.tools/venv/Scripts/python.exe out/08c-work/record-evidence.py
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08c-work/final-checks.py')
.tools/venv/Scripts/python.exe out/08c-work/record-evidence.py
```

Ignored helpers retain the expanded commands, source hashes, inventories, JUnit,
toolchain reports, packages and raw receipts. They are local evidence artifacts;
ordinary builds and registered tests do not depend on their presence. The
[building guide](../building.md) documents the reproducible focused CTest path.
The final record verifies all frozen inputs unchanged and the Git index empty.

## Coverage and limits

`tools.project-files` maps to `shark.tools.project-files` and
`shark.tools.project-files-process`, their source/probe/supervisor and the public
contract. Both families have 90-second timeouts; Catch2 uses seed 1337.
FeatureWorld/Persistence remains assignment 10; no command, terminal/editor or
Lua binding is invented. Shared history remains 09, adapters 15/16 and live MMO
durable state M03.

The service lock coordinates Shark owners. External editors must not race the
final comparison/rename interval, and hostile topology changes must not race the
final identity check/syscall. Native handles and rechecks reject observed changes
but do not claim a portable OS compare-and-swap or adversarial sandbox. Windows
confirms file flushing, while Linux additionally fsyncs the directory; neither
scope establishes hardware power-loss or arbitrary network-filesystem behavior.
New-file publication does not preserve inode identity or arbitrary prior file
metadata. These limits are explicit in the contract and unchanged by test results.

Graphics, audio, other machines, hosted runners, native Linux graphics and MMO
capacity are not run for this CPU authoring increment. No host/WSL settings are
changed. Integrated persistence qualification remains 08d, so full REQ-08 remains
NOT_RUN even after 08c passes.

Next checkpoint: **08d: Integrated qualification**.
