# 10d: Integrated FeatureWorld qualification

Date: 2026-10-25. **10d and full REQ-10 are PASS under the current-machine scope.**
Base: `2532512` on initially clean `main`. Result: uncommitted changes; staging,
commits and pushing belong to the user. Next: **11, asset database, import jobs
and cache**. No assignment-11 implementation is included.

Follow the [qualification contract](../featureworld-qualification.md),
[checkpoint plan](../roadmap/featureworld-plan.md) and
[complete receipt](../coverage/evidence/10d-integrated-qualification.json).

## Delivered behavior

The new `shark.tools.scene-qualification` CPU integration family checks actual
native CPU/GPU catalogs, generated CTest registrations and an independently
reviewed public exercise inventory. All nineteen scene surfaces and the one
external GPU command are accounted for. All current component/command test IDs
resolve to actual passing executions in the final profile union. Lua remains
empty. Optional assignment-02 probe CLI/report declarations are checked against
their registration source and retain separate historical qualification; they
are not counted as new executions.

The same fixture runs saved FeatureWorld through the existing supervisor, then
an intentional scene assertion. It checks stdout, complete published supervisor
and native results, and every artifact's exact bytes/hash. The expected failure
stays exit 1 with `intentional-failure`, tick zero, tick phase and entity
`10000000-0000-4000-8000-000000000004`. Both paths remain CPU-only. Embedded
snapshots survive managed temporary cleanup. Fifteen rejection controls detect
missing/stale mappings, catalog drift, missing profile tests, substituted status
and corrupt artifacts. This is nineteen connected checks in one 90-second family.

Execution uses an owned writable copy of the saved source. Discovery reads the
real generated registration tree through a private metadata directory, avoiding
changes to its enclosing CTest logs. Original source bytes stay unchanged.

Changed implementation files are `tools/tests/test_scene_qualification_process.py`
and `tests/CMakeLists.txt`; coverage mappings add the new family and source.
The qualification guide, skills, roadmap/status documents and handoffs describe
the resulting gate. No engine C++ implementation, library/dependency, public API
or format changed. Existing thirteen runner cases, five saved cases, 29/23/21
process checks, fourteen synthetic evidence tests, source IDs and all earlier
seeds/workloads/limits/timeouts remain intact.

## Verification

| Profile | Passing CTests |
| --- | ---: |
| Windows Debug | 108 |
| Windows Release | 108 |
| Windows ASan | 108 |
| WSL GCC Debug | 101 |
| WSL Clang ASan/UBSan | 101 |
| WSL Server Debug | 67 |
| WSL Server ASan/UBSan | 67 |
| Standalone Windows Tools | 18 |
| Standalone WSL Tools | 18 |

All **660 full-suite results plus 36 standalone Tools checks** pass without
failures or skips. Complete JUnit is checked against actual inventories, with
rejection controls for omitted/duplicate/failed/skipped results. The new family
passes in all seven Tools-enabled configurations. Existing network/storage
process workloads and both full Server suites are refreshed.

Both current GPUs, RTX 4070 Laptop and Intel Raptor Lake-S Mobile, pass the two
explicit native cases in Windows Debug/Release/ASan: **twelve runs and 48
readback comparisons**, with required clean Vulkan core/synchronization/NVRHI
validation and completed resource retirement. Fixed tolerances/oracles/masks are
unchanged. Both actual Debug window captures were visually inspected and show
the expected multicolor cube. These are fixed graphics fixtures; authored scene
rendering remains assignment 13.

All ten public C++ examples compile on MSVC/GCC/Clang, **30 compiles**, with
compiler-reported private-include audits. Four actual sanitizer objects have
the required compile options and sanitizer symbols. Fatal sanitizer options
remain enabled. Protected native/WSL Runtime and Server builds/packages pass;
CPU Tools has no presentation dependency. The current native Runtime package
passes license/payload/import inspection and standard-user Unicode relocation
with a system-only PATH. SDK-free, network-isolated WSL Server/CPU Runtime startup
passes in the retained userspace. Formatting, contract self-tests (45 negative
controls), repository/ignore checks and diff checks pass.

## Preserved attempts and provenance

10c was closed first after the user's explicit Ubuntu restart approval. Its
[handoff](10c-gpu-execution.md) and [receipt](../coverage/evidence/10c-gpu-execution.json)
retain the original kernel stall and unchanged 40-check recovery. Only Ubuntu
was terminated/restarted; no resource, kernel or driver settings changed.

The first 10d WSL run passed 100 existing checks and failed the new fixture at
the read-only project's ownership lock. The correction uses a writable copy
and isolates CTest metadata discovery. The failed run, source snapshot and
original fixture source remain under `out/10d-work`. No production behavior,
required workload or limit was weakened. The final **376 input files** remain
unchanged throughout final qualification.

The final WSL Debug suite passes all 101 checks. A public-example helper then
referenced the superseded source-freeze receipt; it was routed to the correct
final freeze, preserving completed Debug results. The continuation records the
remaining profiles and examples without repeating those passing tests.

Windows Debug and its four GPU runs completed before its original coordinator
was stopped. Remaining Windows CPU profiles use separate canonical build
directories concurrently; no directory has concurrent builds. GPUs remain
serial. Raw Debug/worker/GPU outputs are combined by explicit references, not
relabeled incomplete summaries. The original stop had child-exit race errors;
subsequent process inspection confirmed no active owned build before dispatch.

The first native relocation attempt used Python 3.13's restricted temporary
directory permissions, which denied the limited desktop token. The existing
inherited-permission staging helper resolved the launch on both ASCII/Unicode
paths, with both ordinary and isolated environments. No existing ACL or host
setting changed. Completed public examples/protected builds were reused; the
package relocation and remaining audits continued separately. Original failed
summary/diagnostics remain preserved.

Ignored helper commands below record this qualification; they require their
retained inputs and fresh output paths. They are not checkout prerequisites.
For ordinary reruns use the documented presets and qualification guide.

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/10d-work/qualify-final.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/10d-work/native-profile.py windows-release
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/10d-work/native-profile.py windows-asan
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/10d-work/native-profile.py tools-debug
wsl.exe -d Ubuntu -u root --cd / -- /bin/bash /mnt/c/Users/mrkda/Projects/shark/out/10d-work/wsl-final.sh
wsl.exe -d Ubuntu -u root --cd / -- /bin/bash /mnt/c/Users/mrkda/Projects/shark/out/10d-work/continue-wsl-final.sh
wsl.exe -d Ubuntu -u root --cd / -- /bin/bash /mnt/c/Users/mrkda/Projects/shark/out/10d-work/final-wsl-protected.sh
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/10d-work/final-audit-windows-api.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/10d-work/continue-audit-windows.py
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/10d-work/native-gpu.py
python out/10d-work/receipt-ready.py
```

Receipts under `windows-final`, `windows-release-final`, `windows-asan-final`,
`tools-debug-final`, `windows-gpu-final`, `wsl-final`, `wsl-continued-final`,
`windows-audits`, `windows-audits-continued` and `wsl-protected` preserve exact commands and inputs. Final
status/navigation changes follow the gate and are recorded separately in
`closure-metadata.json`; implementation and coverage mappings do not change.

## Storage and scope

Current build directories, canonical archives and WSL userspaces are reused.
Both managed native package copies were removed after verification. The expanded
WSL Server deployment was archived, verified against its closure hashes and
removed file by file. `out/10d-work/server-package-retention.json` records its
original paths, hashes and verified archive for restoration. Keep receipts and
the final integrity index; do not remove retained environments or old evidence
wholesale. `.gitignore` retains generated audits under build/out and keeps the
local roadmap ignored while source, tests, skills and documentation stay visible.

No broader hardware, native Linux graphics, public hosting or MMO capacity is
qualified. No editor authoring UI, general terminal adapter, imported asset
pipeline or authored scene renderer is introduced by this checkpoint.

Suggested commit: `test(tools): complete FeatureWorld coverage qualification`.
