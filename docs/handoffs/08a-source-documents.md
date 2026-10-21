# 08a: Source document formats, validation and migration

Date: 2026-10-21. Assignment: 08a, first checkpoint of REQ-08.
Base: `8463a8434ddd09d885ab99a1b401c5049f35307b` on initially clean `main`.
Result: uncommitted working tree; the user owns staging, committing and pushing.

**Status: PASS under the current-machine scope.** Full REQ-08 remains NOT_RUN.
Follow the [format contract](../source-documents.md),
[four-checkpoint plan](../roadmap/persistence-plan.md) and
[qualification receipt](../coverage/evidence/08a-source-documents.json).

## Delivered behavior

EngineTools now provides bounded, pure project/scene value codecs, exact version-1
fields, identity/reference validation and explicit legacy 0-to-1 migrations.
Both source versions have independently authored fixtures. The legacy shape has
different field names and flat entities; migration inserts component envelopes
and reorders quaternion components. It is a test contract, not a claim of an
earlier shipped file format. Encoders always write readable version 1.

The codec reuses Core IDs, lexical project paths, Name/Transform validation and
global-cell coordinates. It rejects malformed UTF-8/JSON, duplicate decoded keys,
unknown fields/versions, invalid values, missing references and cyclic or excessive
hierarchies. Explicit byte, node, depth, count and extras budgets bound input and
output. Encoders revalidate mutable caller values; rejected input leaves the
caller unchanged. Error operations and JSON-pointer locations are documented.
This is not a filesystem service or active Scene replacement.

`SHARK_BUILD_TOOLS` separates CPU authoring from Editor/Client. The `tools-debug`
preset builds Tools without those targets, a display or GPU. EngineTools publicly
consumes Core values and privately consumes exact nlohmann/json 3.12.0. The
archive's 54 regular files are verified; upstream CMake/tests are not executed.
Protected Runtime/Server builds retain only their prior dependencies.

New production files are `engine_tools/include/shark/tools/source_document.hpp`
and `engine_tools/src/source_document.cpp`. Build changes cover the root/presets,
Tools/tests CMake, private JSON population/verification, the dependency lock and
boundary fixtures. New behavior coverage is `tests/source_document_tests.cpp`,
four source fixtures, and `tools/tests/test_source_document_dependencies.py`.
The format/checkpoint guides, architecture/build/CI guidance, coverage, repository
skills, contributor guidance and `.gitignore` are maintained. The ignored local
roadmap points to the versioned plan; checkout builds do not depend on that file.

## Qualification

All seven full CPU suites pass: **484 CTests, zero failures or skips**. The final
source freeze covers
288 source/build/test/fixture inputs. The new CTest uses seed 1337 and a 60-second
timeout; its 28 public API cases pass 10,604 assertions. The six JSON
dependency cases have a 240-second CTest timeout. Existing boundary fixtures now
cover three allowed and eighteen forbidden closures.

| Profile or check | Result |
| --- | --- |
| Windows Debug | PASS, 78/78 CPU CTests |
| Windows Release | PASS, 78/78 CPU CTests |
| Windows ASan | PASS, 78/78 CPU CTests |
| WSL client GCC Debug | PASS, 71/71 CPU CTests |
| WSL client Clang ASan+UBSan | PASS, 71/71 CPU CTests |
| WSL Server GCC Debug | PASS, 54/54 CPU CTests |
| WSL Server Clang ASan+UBSan | PASS, 54/54 CPU CTests |
| Independent Tools-only build | PASS on Windows and WSL, 1/1 focused CTest each |
| Protected builds and isolated packages | PASS on Windows and WSL |
| Public API example and private dependency audit | PASS, all six native build profiles |
| Formatting, contract, repository and ignore checks | PASS |

The standalone Tools configuration registers 55 CTests but deliberately runs the
one source-document family; those two focused passes are separate from the 484
full-suite total. The isolated WSL
Server package has 11 files and runs in the unchanged 92-package SDK-free
userspace. The Windows Runtime package has 33 files, passes launch after relocation
outside the checkout to a path with spaces and Unicode, and rejects each of ten
missing Core dependency notices. No JSON or authoring payload enters either
protected package.

The published public C++ example compiles with warnings as errors using only
public engine, central GLM and system headers. Actual compiler inputs from all
six native profiles confirm private JSON/EnTT/TLS boundaries and central math
configuration; sanitizer object checks include the codec and tests. WSL audits
also confirm Clang sanitizer instrumentation and private dependencies. All 288
frozen inputs and 14 qualification helpers remain unchanged during the final runs.

Native qualification uses this Windows 11 x64 PC (build 26200, Intel Core
i9-14900HX, 24 cores/32 logical processors), MSVC 19.51 and the installed Windows
SDK. WSL CPU/Server uses the retained Ubuntu userspace, GCC 13 and Clang 18, with
native graphics/window options OFF. No host/WSL resource settings are changed.

The exact local driver commands are:

```text
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08a-work/focused.py')
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08a-work/verify-windows.py')
wsl -d Ubuntu -u root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/08a-work/wsl-run.sh
.tools/venv/Scripts/python.exe out/08a-work/wsl-runtime-audit.py
.tools/venv/Scripts/python.exe out/08a-work/wsl-receipt.py
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08a-work/api-audit.py','--final')
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08a-work/verify-runtime-package.py')
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08a-work/final-checks.py')
.tools/venv/Scripts/python.exe out/08a-work/record-evidence.py
```

These ignored local drivers retain expanded configure/build/CTest/package commands,
exact archive inputs, toolchain reports, logs and hashes under `out/08a-work/`.
The versioned [building guide](../building.md) provides ordinary checkout commands,
including `tools-debug`; the local evidence drivers are not build prerequisites.

The first dependency-test attempt rejected MSVC's valid `-external:I` spelling.
Its assertion now accepts both supported spellings only for the private JSON
SYSTEM include. All six focused cases then passed. That initial output is retained
as an explicitly identified transcription, rather than a fabricated raw log.
An initial PowerShell redirection also treated an upstream CMake deprecation
warning as a shell error; subsequent process-runner configuration succeeded.

An independent public API probe found a real defect during the first full attempt:
moving a `ProjectPath` out of a project entry left an empty path that validation
and encoding accepted, while decoding the output rejected it. Qualification was
stopped, its inputs/results preserved, and every project path is now checked
through Core before copying or encoding. The 28th regression tests the error code,
field pointer and unchanged candidate. Focused attempt 3 and the complete
final matrix pass against the corrected source. No resource or
acceptance limit was relaxed.

## Coverage and limits

Source documents preserve authored content identity; live session/region authority
and player persistence remain separate. FeatureWorld connects in 10. Scene
activation is 08b, safe files/conflicts 08c, full integration 08d, durable live state
M03, shared command history 09, terminal adapters 15 and editor UI 16.

The manifest maps `tools.source-documents` to its executable test, source contract
and independent fixtures. Command/Lua registries remain unchanged. Full REQ-08
stays NOT_RUN until 08b/08c/08d pass. GPU runs are not repeated for this CPU
authoring change. Other machines, native Linux graphics, hosted-runner gates and
production MMO capacity are outside the current qualification scope.

Next checkpoint: **08b - Scene loading and transactional activation**.
