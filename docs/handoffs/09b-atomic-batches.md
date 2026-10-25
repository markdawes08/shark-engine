# 09b: Atomic transactions and preview

Date: 2026-10-25. **09b is PASS under the current-machine scope.**
Assignment: 09b, the second of the [five authoring checkpoints](../roadmap/authoring-plan.md).
Base: `c527f426a828c23e43621dfccae5e36c9d9de91c` on initially clean `main`.
Result: uncommitted working tree; user owns staging, commits and pushing.
Final [qualification receipt](../coverage/evidence/09b-atomic-batches.json): **PASS**.
Full REQ-09 remains **NOT_RUN**; history/retry/integrated gates remain 09c-09e.

## Behavior and contracts

AuthoredScene now accepts AuthoringBatch values containing a typed caller
correlation ID and at most 256 existing typed commands. Empty batches are allowed.
`preview(batch,expected)` and `execute(batch,expected)` share ordered validation
and nested owned Result semantics. A later operation can use a candidate entity
created earlier. Every intermediate hierarchy, component and capacity state must
be valid. Late failure leaves the complete live owner unchanged.

Core provides an internal candidate/publication port; it is not a supported
public mutation API. Candidates copy records, checked slot generations,
identity maps and cached transforms. They do not consume a new Scene instance.
Tools applies the existing five command implementations to one nonauthoritative
candidate. Result storage and the sorted unique refresh-ID union exist before
publication. Commit rechecks the live owner/revision/immediate phase and swaps
private state without allocation. The Scene object and surviving handles remain
valid. Delete/recreate advances generation and cannot revive source extras.
Metadata, world/cell identity and the saved baseline survive normal edits.

Changed batches advance the live revision once. Empty/all-no-op batches do not;
an edit followed by its inverse is still a changed batch. Preview discards its
candidate, preserves dirty state/cursors and reserves no revision. Commit always
rebuilds/revalidates; stale previews and worker completions cannot overwrite an
intervening edit. Batch IDs correlate diagnostics, with no retained retry or
exactly-once claim. Errors identify the batch and failing command index.

[ADR0021](../architecture/0021-atomic-authoring-batches.md) and the
[transaction contract](../authoring-transactions.md) specify ordering, limits,
identity, errors, handle lifetime, allocation boundaries and worker-value
reconciliation. Source/component versions remain1; no wire/database format or
dependency change. Single commands remain source-compatible and retain all 24
cases. Workers only return bounded owned values plus the observed revision;
actual job lifecycle retains11-14, using the existing completion primitive.

## Changed files and coverage

- `engine/core/include/shark/scene.hpp`, `engine/core/src/scene.cpp` and private
  `scene_edit_access.hpp`: checked nonauthoritative candidate and one publication.
- EngineTools command/owner headers, implementation and private invariant fixture
  access: batch/preview values, common single-command path, bounded effects and
  one-shot failure injection. EngineTools privately includes the engine-owned
  Core port; no EnTT/native type enters public headers.
- `tests/authoring_command_tests.cpp`: 17 additive batch cases, preserving the
  original24 command cases. `tests/core_scene_edit_tests.cpp`: 6 Core cases.
  Test-only allocation files sweep Linux ordinary/nothrow allocations; Windows
  uses private bad_alloc hooks at candidate/command/result/commit boundaries.
- `tests/CMakeLists.txt`: `shark.tools.authoring-batches` (60s) and
  `shark.core.scene-edit` (30s), both seed 1337 with explicit labels. Existing test
  names/properties/limits are retained. `.gitignore` keeps output under out/build.
- Requirement/component/command mappings add real batch preview/execute coverage;
  plans/current guidance/skills and the handoff describe later owners.

FeatureWorld/AuthoringCommands attaches in 10; these real CPU fixtures justify
external coverage until then. Human/agent equivalents share one owner through
C++ services. Terminal/process ownership and editor adapters remain 15/16.

## Qualification

Final qualification is **PASS**. Inputs are frozen at 351 source/build/test/
fixture/contract files and 16 helpers. Freeze SHA256:
`54f4accbd76e305ce63d9608e9b0690a6bd867c47e39ea05fcb0640afe663ab0`.
All frozen inputs and helpers remained unchanged throughout final qualification.

| Final profile | CTests | Status |
| --- | ---: | --- |
| Windows Debug | 97 | PASS |
| Windows Release | 97 | PASS |
| Windows ASan | 97 | PASS |
| WSL client GCC 13 Debug | 90 | PASS |
| WSL client Clang 18 ASan+UBSan | 90 | PASS |
| WSL Server GCC 13 Debug | 67 | PASS |
| WSL Server Clang 18 ASan+UBSan | 67 | PASS |
| Standalone Windows Tools | 7 | PASS, separate |
| Standalone WSL Tools | 7 | PASS, separate |

Total: **605 full CPU CTests plus 14 standalone Tools checks**, with zero failures
or skips. Server suites are refreshed because Core changes. All seven full
profiles preserve their historical inventories plus the declared new families.
Both standalone Tools configurations retain the exact 62-test inventory and pass
all seven selected authoring families and all seven named Catch groups.

All 15 public-example compiles pass across MSVC/GCC/Clang. Actual instrumentation
checks pass for 12 native ASan objects, 13 WSL client ASan+UBSan objects and two
WSL Server ASan+UBSan objects. WSL object copies are retained with their symbol
logs and compile-command hashes. Six native compile databases preserve the
protected Core/Tools/private-JSON boundary.

Protected Runtime/Server builds and package gates pass on both systems. The
native 33-payload-file Runtime package launches from a Unicode/space directory
outside the checkout with a system-only PATH; all ten missing-notice controls
reject. Both 12-file Server installs pass; the WSL package also starts in the
retained SDK-free 92-package runtime environment with isolated networking.
Format (196 C++ files), contract (37 negative controls), repository/skills/ignore,
whitespace and empty-index checks pass. No GPU/scene renderer/editor/CLI gate is
added by this increment.

Completed preparation: native 14 focused CTests; WSL 15 focused tests in each CPU
profile and 7 standalone Tools tests; exact native/WSL inventories; Windows
Release/ASan/Tools new-family checks; all five public examples on MSVC/GCC/Clang.
The new batch family has 17 cases (5,975 Windows or 8,510 WSL assertions); Core
staging has 6 cases and 111 assertions. All original persistence
case names remain present. Evidence parsers reject 27 invalid controls; the final
phase consumer rejects 7 controls. Preparatory counts never enter final totals.

Retained preparatory failures: native-r1 failed a new Catch assertion using an
unparenthesized logical-and; the test assertion was corrected. Native-r2 timed
out after 60 seconds during the exploratory Windows global allocation sweep at
its second injected allocation. Windows now injects one bad_alloc at each
engine-owned preparation/publication boundary without replacing library
allocators. WSL-r1 passed GCC but ASan caught a mismatch in the test allocator's
missing nothrow overloads. Completing ordinary/array nothrow allocation and
matching deletion fixed it; corrected WSL-r2 passes with sanitizers enabled.
These original attempts stay failed/preparatory evidence. Initial direct test
compilation also corrected the RegionSceneScope fixture and an ambiguous private
namespace; no production warning/timeout or acceptance limit was suppressed.

The WSL execution driver and isolated package smoke passed. A later read-only
consumer precheck found that `clean-runtime-sha256.txt` used host paths under
`/var/tmp/.../runtime-root/package-09b`, while the frozen receipt reader expects
runtime paths under `/package-09b`. The original checksum list is preserved in
`wsl-final/checksum-followup/clean-runtime-sha256-host-paths.txt`. A separate
read-only checksum command inside the same SDK-free runtime produced the expected
path namespace. All 12 digests matched both the original list and installed
package bytes. The original WSL summary, launch/log records, all source/helper
hashes and package files remained unchanged. Follow-up before/summary/log/script
artifacts record the command and hashes; no suite or package smoke was rerun and
no acceptance condition changed.

Exact final workflow commands, all from the repository root:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09b-work/verify-windows.py final
.tools/venv/Scripts/python.exe out/09b-work/launch-wsl.py final
.tools/venv/Scripts/python.exe out/09b-work/wsl-final/checksum-followup/reconcile-checksums.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09b-work/public-api.py --compiler cl --build build/windows-debug --output out/09b-work/public-api-msvc --final
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09b-work/native-audit.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09b-work/verify-runtime-package.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09b-work/repository-checks.py --output out/09b-work/preclose-checks
.tools/venv/Scripts/python.exe out/09b-work/final-receipt.py --checks out/09b-work/preclose-checks/summary.json
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/09b-work/repository-checks.py
.tools/venv/Scripts/python.exe out/09b-work/final-receipt.py
```

Drivers record exact configure/build/CTest commands, archives, build parallelism 2,
seed 1337, exit/timeout/cleanup results, logs and case/process receipts. WSL uses
read-only source and writable evidence mounts in the retained pinned Ubuntu 24.04
userspaces. Each profile has fresh evidence; caches are reused. The installed
Server smoke uses the separate SDK-free 92-package runtime userspace and an
isolated network. This is same-PC deployment evidence.

## Hardware, limits and next checkpoint

Hardware: Lenovo 83DG, Intel Core i9-14900HX (24 cores/32 logical), 34,053,414,912 bytes
physical RAM; Windows 11 Home 10.0.26200 build 26200. Linux qualification uses the
existing WSL2 Ubuntu environment on this PC. MSVC 19.51, GCC 13, Clang 18 and the
pinned tool wrappers are recorded per profile. No host/WSL resources changed.

The bounded scene-copy approach is a local-authoring tradeoff, not an MMO
throughput claim. Vendor/runtime OOM termination recovery is not promised;
qualified failure boundaries are described above. History/undo/redo, retained
mutation identity/lost replies and integrated authoring qualification are still
09c/09d/09e. There is no new public scheduler/job framework, simulation service,
player/admin protocol, persistence format, terminal adapter or authoring UI.

No blocker remains for the next checkpoint.
**Exact next checkpoint: 09c - Shared undo and redo.**
Suggested commit: `feat(authoring): add atomic batches and pure preview`.
