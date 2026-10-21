# 08b: Scene loading and transactional activation

Date: 2026-10-21. Assignment: 08b, second checkpoint of REQ-08.
Base: `a67fcc5705939ab433360b573d26047d625c837a` on initially clean `main`.
Result: uncommitted working tree; staging, commits and pushing belong to the user.

**Status: PASS under the current-machine scope.** Full REQ-08 remains NOT_RUN.
Follow the [capture/activation contract](../authored-scenes.md),
[persistence checkpoints](../roadmap/persistence-plan.md) and
[qualification receipt](../coverage/evidence/08b-scene-activation.json).

## Delivered behavior

EngineTools now owns an authored Scene through `AuthoredScene`. Creation validates
the project and selected scene, checks project/catalog/world identity, then
constructs an inactive `AuthoredSceneScope` using Core's existing entity, hierarchy
and composed-transform validation. Source order remains independent of parent-
first construction order. No Core, network, Runtime or Server implementation
changes were required, and no dependency was added.

Capture uses all-field query pages at one committed revision, including empty
sparse pages. It returns complete owned project/scene values and the revision,
preserving global-cell metadata and opaque extras. Entity extras follow their
loaded handle incarnation; deleting and recreating a durable ID cannot restore
deleted metadata. New entities append in stable runtime slot order with empty
extras. Current Name/Transform and hierarchy always come from the active Scene.

Activation requires an expected instance/revision and protects unsaved edits by
default. Explicit discard does not bypass pending work, active visits, closed
state, wrong-thread access or invalid input. The complete candidate and metadata
are prepared before the old Scene closes; publication then uses a nonthrowing
ownership swap. Failed activation preserves the active revision, entity state,
metadata, queue and dirty baseline. Successful identical-source reload still
creates a fresh instance and invalidates old handles/references/cursors.

Dirty tracking is relative to the last in-memory activation. Capture does not
acknowledge a save. Borrowed Scene references end at successful replacement;
owned captures and checked values have the documented separate lifetimes.
Only authored scope is constructed. World metadata never grants live server
authority or performs render-origin rebasing.

New production files are `engine_tools/include/shark/tools/authored_scene.hpp`
and `engine_tools/src/authored_scene.cpp`; EngineTools CMake adds the implementation.
`tests/authored_scene_tests.cpp` and its separate `AuthoredSceneTests` target add
the public behavior fixtures. The source-dependency fixture now checks the exact
three-file EngineTools compilation set. The capture/activation guide, persistence
plan, architecture/build/CI guidance, coverage, repository skills and `.gitignore`
are maintained. Generated evidence remains under ignored `out/08b-work/`.

## Qualification

All seven full CPU suites pass: **489 CTests, zero failures or skips**. The final
source freeze contains **291 source, build, test and fixture inputs**, with 14
qualification helpers. The new family has **23 cases and 26,624 assertions**,
seed 1337 and a 60-second timeout. It includes 4096-entity complete/sparse capture
and a reversed depth-256 hierarchy without relaxing source or Core limits.

| Profile or check | Result |
| --- | --- |
| Windows Debug | PASS, 79/79 CPU CTests |
| Windows Release | PASS, 79/79 CPU CTests |
| Windows ASan | PASS, 79/79 CPU CTests |
| WSL client GCC Debug | PASS, 72/72 CPU CTests |
| WSL client Clang ASan+UBSan | PASS, 72/72 CPU CTests |
| WSL Server GCC Debug | PASS, 54/54 CPU CTests |
| WSL Server Clang ASan+UBSan | PASS, 54/54 CPU CTests |
| Independent Tools-only build | PASS on Windows and WSL, 2/2 focused CTests each |
| Protected builds and isolated packages | PASS on Windows and WSL |
| Public API example and private dependency audit | PASS, all six native build profiles |
| Formatting, contract, repository and ignore checks | PASS |

Each standalone Tools configuration registers 56 CTests but deliberately runs the
two authoring families. Those four additional focused passes are separate from
the 489 full-suite total. The unchanged SDK-free WSL userspace has 92 packages;
its installed Server package has exactly 11 files. The Windows Runtime package
has 33 files, launches from a path with spaces and Unicode outside the checkout
with a system-only PATH, and rejects each of ten missing dependency notices.
Neither protected package acquires JSON, authoring code or source fixtures.

The documented complete C++ example compiles with warnings as errors and only
public engine, central GLM and system headers. The six-profile native audit checks
actual compiler inputs, private JSON/EnTT/TLS boundaries and central GLM options.
All four codec/activation implementation/test objects carry Windows ASan flags;
WSL separately verifies Clang ASan+UBSan instrumentation and private dependencies.
Four in-memory compile-command corruption controls reject as expected. All frozen
source inputs and helpers remain unchanged throughout final qualification.

Native tests use this Windows 11 x64 PC (build 26200, i9-14900HX, 24 cores/32
logical processors), MSVC 19.51 and Windows SDK 10.0.28000.0. WSL uses the
retained pinned Ubuntu userspace with GCC 13/Clang 18 and native window/graphics
disabled. No host or WSL resource settings are changed.

The exact local driver commands are:

```text
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08b-work/focused.py','focused-attempt1')
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08b-work/focused.py','focused-attempt2')
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08b-work/focused.py','focused-attempt3')
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08b-work/preflight/windows-dependency.py')
wsl -d Ubuntu -u root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/08b-work/preflight/wsl-run.sh
.tools/venv/Scripts/python.exe out/08b-work/audit-negative.py
& ./tools/windows_dev.ps1 -CommandArguments @('python','tools/check_format.py')
.tools/venv/Scripts/python.exe out/08b-work/freeze.py
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08b-work/verify-windows.py')
wsl -d Ubuntu -u root --exec bash /mnt/c/Users/mrkda/Projects/shark/out/08b-work/wsl-run.sh
.tools/venv/Scripts/python.exe out/08b-work/wsl-runtime-audit.py
.tools/venv/Scripts/python.exe out/08b-work/wsl-receipt.py
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08b-work/api-audit.py','--final')
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08b-work/verify-runtime-package.py')
& ./tools/windows_dev.ps1 -CommandArguments @('python','out/08b-work/final-checks.py')
.tools/venv/Scripts/python.exe out/08b-work/record-evidence.py
```

Ignored drivers retain expanded configure/build/test/package commands, toolchain
reports, inventories, JUnit, network regression receipts and artifact hashes.
They are local evidence helpers, not checkout prerequisites. The versioned
[building guide](../building.md) documents ordinary builds and focused tests.

Focused attempt 1 built successfully, passed the codec suite and failed exactly
two assertions in one activation case: the fixture expected `conflict` for old
handles/references, while existing Core correctly returns `invalid_id`. Those
assertions were corrected; cursor/revision conflicts remain distinct. Original
source snapshots and failing logs are retained. The capture scan budget was then
reduced from 4096 to 256 so the sparse fixture exercises empty continuation pages;
the test explicitly traverses 16 pages to the sole survivor. Focused attempt 2
passes. No rejection or acceptance limit was weakened.

An initial compile-audit negative-control helper failed its own assertion because
it matched slash-separated paths against native backslashes. Normalizing the
synthetic command before mutation corrected the helper; all four controls reject.
This was evidence preparation, not an engine failure. The original helper remains
in the ignored preliminary evidence directory.

Full attempt 1 had 78 passing Windows Debug CTests and one failing dependency
test; both WSL Server profiles passed 54/54. The dependency fixture still expected two EngineTools
translation units after this increment added a third. It now checks the exact
three-source set while retaining all private JSON and protected-target checks.
GCC 13 rejected a test's temporary `reference_wrapper` expression under
`-Wdangling-reference`; the fixture now names that wrapper before borrowing its
Scene reference. Production activation code compiled successfully. Both drivers
exited nonzero naturally; their frozen inputs, summaries, logs and exact changed
fixture snapshots remain in the ignored attempt-1 directories. No production
behavior or acceptance limit changed in response. The corrected Windows authoring
families (2/2), Windows dependency family (1/1), and WSL GCC/Clang authoring plus
dependency families (3/3 each) all pass.
A new source freeze then qualifies the complete matrix above. Only the two test
files changed between full attempts; production code remained unchanged.

## Coverage and limits

The implemented manifest maps `tools.authored-scene` to the new CTest, source and
contract. Name/Transform and hierarchy reuse Core rather than new validation.
No command/Lua bindings or FeatureWorld cases are invented; FeatureWorld/Persistence
connects in 10. General transactions/history remain 09 and terminal/editor adapters
15/16. The owner is not a cross-process project lock.

No file is opened, saved or replaced. Actual filesystem resolution, containment,
atomic writes and external-edit conflicts retain 08c; integrated persistence
qualification remains 08d. Live character/economy durability remains M03. Full
REQ-08 stays NOT_RUN. GPU checks are not repeated for this CPU authoring change;
other machines, native Linux graphics, hosted-runner gates and production MMO
capacity are outside the current qualification scope.

Next checkpoint: **08c: Safe project files and conflicts**.
