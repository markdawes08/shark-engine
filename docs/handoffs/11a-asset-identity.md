# 11a: Persistent asset identity, metadata and catalog

Date: 2026-10-25. **11a is PASS under the current-machine scope.** Base:
`7b68135` on initially clean `main`. Changes remain unstaged/uncommitted; the
user owns staging, commits and pushing. **Next: 11b, dependencies and reproducible
development cache.** Full REQ-11 remains NOT_RUN pending 11b/11c/11d.

Follow the [four checkpoints](../roadmap/asset-plan.md),
[ADR 0025](../architecture/0025-asset-identity.md),
[identity contract](../asset-identity.md) and
[qualification receipt](../coverage/evidence/11a-asset-identity.json).

## Delivered behavior

EngineTools adds typed UUIDv4 AssetId and exact schema-1 `.sharkasset` sidecars.
They retain project/asset identity, kind, origin/license, declared importer,
settings, dependency IDs, audience and extras. The codec reuses the existing
bounded JSON validator and deterministic encoder. It rejects unsupported versions,
duplicate/unknown fields, invalid UTF-8/numbers/IDs and oversized data. Declared
importer identifiers are metadata; importer execution remains 11b.

ProjectFiles supplies typed reads/writes, exact content revisions and regular
source-presence checks through its existing owner/native path/publication service.
Native sidecar reads and replacement comparisons enforce 64 KiB before allocation;
project/scene limits remain 4 MiB. Failed prepublication writes preserve valid
bytes; committed durability warnings retain a usable new revision.

AssetCatalog validates an explicit complete bounded sidecar inventory, then
publishes one owned snapshot. IDs survive coordinated source/sidecar renames,
directory moves and reopen. Duplicate IDs/paths, foreign projects, unsafe native
aliases and stale/foreign revisions reject. Unknown IDs and missing sources are
distinct; a missing source retains its metadata/ID. Queries return owned sorted
pages tied to a fresh owner UUID and revision. No automatic directory watcher,
two-file move transaction, importer, cache, jobs or cooked runtime reader is claimed.

The saved scene gains asset reference
`20000000-0000-4000-8000-00000000000a` in bounded extras. The new native
`featureworld.asset-identity` case exercises metadata publication, queries,
rename/move, missing/unknown distinction and reopen while preserving its scene
owner. It creates its own authored identity fixture in managed scratch. This is
not an imported/renderable asset or a new component/schema. Earlier four entity
IDs, five native cases and all previous workloads remain.

The independent inventory now covers **24 scene surfaces and one external GPU
command**. Five real metadata/catalog C++ service operations map to the native
case and behavior tests. Human/terminal adapters retain 15/16; shared services
remain CPU-only and require no editor/display/provider SDK.

## Qualification and preserved attempts

| Profile | Qualified CTest outcomes |
| --- | ---: |
| Windows Debug | 109 |
| Windows Release | 109 |
| Windows ASan | 109 |
| WSL GCC Debug | 102 |
| WSL Clang ASan/UBSan | 102 |
| Standalone Windows Tools | 19 |
| Standalone WSL Tools | 19 |

**531 full-profile outcomes plus 38 standalone Tools checks** qualify without
remaining failures/skips. These are selected passing profile/test outcomes from
retained complete runs and final Tools reruns, not one uninterrupted final run.
Every Tools family reran against the final frozen implementation; unchanged
Core/Client/Server regressions retain their actual earlier results. Receipts
identify the exact JUnit source/hash for each selected outcome. Unaffected
Core/Client archive hashes remain identical through the final rebuilds; WSL also
records Storage archive equality. Native Storage sources/graph are unchanged.

The new `shark.tools.asset-catalog` family has **18 cases/6,190 assertions** in
each Tools-enabled profile, Catch seed 1337 and its original 60-second timeout.
Limits remain 4096 entries, 16 MiB combined input, 64 KiB metadata, 256 dependency
IDs and 128 records/page. Preserve the new 4096-path/1500-directory-level workload.
The earlier thirteen runner cases/29 process checks, five FeatureWorld cases/23
process checks and 21 supervisor checks remain; the added case makes six native
FeatureWorld cases and 24 FeatureWorld process checks. The connected qualification
family retains its 19 checks/15 rejection controls and 90-second timeout.

Initial attempts are preserved under `out/11a-work`:

- A new fault assertion expected normal Windows durability while explicitly
  injecting a flush failure. The fixture now expects the existing committed
  warning contract on both OSes. Production publication behavior was correct.
- The dependency-boundary fixture needed the newly implemented `asset_catalog.cpp`
  in its exact source inventory. The list was extended; private JSON and protected
  target checks remain intact. Its initial WSL failures are retained.
- WSL Debug's contract check timed out during concurrent full profiles. With those
  competing full runs complete, its original 30-second check passes unchanged
  (including a 9.11-second recorded retry). No host/resource settings changed.
- Review found that retaining every path prefix could amplify memory for deep
  inventories. The catalog now compares sorted full paths. The new deep case
  initially exceeded 60 seconds in Windows Debug; charging the complete byte
  budget before parsing fixes the work amplification. The exact workload and
  timeout are unchanged; final Debug qualification passes.
- Some earlier orchestration summaries correctly remain FAIL after their frozen
  inputs changed or a helper looked for JUnit relative to the wrong directory.
  Completed raw results are preserved; final receipts explicitly reconcile actual
  passing results. No failed summary was relabeled as a complete passing run.

The final implementation freezes **609 inputs** in
`out/11a-work/source-freeze-budget.json`. All eleven public examples compile with
MSVC/GCC/Clang (**33 compiles**) using public headers and compiler include audits.
Twelve actual objects verify ASan/UBSan options and symbols; fatal options remain
enabled. Protected native/WSL Runtime/Server builds pass with Tools/JSON excluded.
The 35-file native Runtime package passes exact payload/license/import inspection
and standard-user Unicode relocation with a system-only PATH. Retained SDK-free,
network-isolated WSL Server/CPU Runtime launches and Server closure pass.
Graphics implementation/dependencies are unchanged; historical 10d hardware
evidence is retained rather than counted as new GPU runs.

## Rerun and storage

Use the existing developer environment/presets:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset tools-debug --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug --show-only
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug -R ^shark.tools. --output-on-failure
```

Use the corresponding existing Windows Debug/Release/ASan and WSL GCC/Clang
builds for regression/sanitizer checks. Qualification-specific ignored helpers
`requalify_budget.py`, `wsl-budget.sh`, `receipt.py` and their retained raw inputs
record this run; they are create-only audit commands, not checkout prerequisites.
The local roadmap remains ignored and is not required by builds/CI.

Canonical build trees, dependency archives and WSL userspaces were reused. Both
successful native package copies were cleaned. The twelve-file WSL Server
deployment was verified against its closure, archived with original paths/hashes,
read back and removed file by file. `out/11a-work/server-package-retention.json`
indexes `server-package.zip` for restoration. Keep raw failure receipts and source
snapshots. Do not delete retained environments or `out/` wholesale. `.gitignore`
keeps generated audits/stages ignored and source/metadata/docs/skills eligible.

After the gate, navigation/status and two stale explanatory comments were updated
without changing C++ behavior, schemas, tests or limits. The separate closure
metadata receipt records exact before/after hashes and comment-only comparisons.

Suggested commit: `feat(tools): add persistent asset metadata and catalog`.
