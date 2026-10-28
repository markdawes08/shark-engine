# Whole-project testing bloat review

Date: 2026-10-27. Base: `190cd4d`, initially clean `main`. The user requested
assessment and cleanup of the entire project's testing bloat. This maintenance
work does not advance the roadmap: assignment 13 remains next. Staging, commits
and pushing remain with the user.

## Assessment

The clear excess was repeated execution, retained generated report copies and
duplicated instructions. The number of behavioral tests alone was not evidence
that they should be deleted. The review inventoried tracked source/docs/fixtures,
CTest registrations, runners, generated Windows artifacts, retained WSL build
outputs and attributable temporary directories. Timing analysis used the existing
12e results, without running another full matrix.

| Area | Finding and disposition |
| --- | --- |
| Behavioral tests and fixtures | Native/Python test sources and fixtures total about 2.75 MiB. No byte-identical test files or substantial identical Python functions were found. Unit, process-failure and FeatureWorld paths exercise different boundaries; retain their cases, independent expectations and limits. |
| Execution selection | Four dependency/configuration families took 587.43 of 975.95 seconds in 12e's Windows run (60%). The configured preset includes unrelated owners. Clarified that local assignment closure means the affected subsystem's inventory, with other checks triggered by actual changes. Hosted CI remains unchanged. |
| Validator controls | Six storage supervisors retained a complete mutated report per successful rejection control: 812 redundant files across Windows/WSL. Keep primary reports and named control outcomes; use managed scratch and preserve only unexpected failures. |
| Build artifacts | Native build output was about 14.87 GB, dominated by binaries, symbols and linker state. Removed regenerable incremental-link state. MSVC test executables now use `/INCREMENTAL:NO`, keeping PDB debug symbols; application/vendor settings are unchanged. |
| Agent and coverage guidance | Repeated checkpoint histories obscured active rules. Reduced AGENTS, coverage guidance and five skills from 2,201 to 796 lines, preserving actionable constraints and authoritative links. Also removed copied histories/stale status from the main build/CI/architecture guides. |
| Historical evidence | About 34.7 MB of tracked receipts and roughly 1.65 GB of existing evidence archives remain. They carry historical integrity/reference value; reformatting/compressing the receipts would change pins and consumers for comparatively little gain. |
| Temporary deployments/configures | Removed two abandoned dependency-configure trees, the superseded partial native archive, and both 12e package copies. The complete native archive matches its original recorded digest. |

## Changes and space removed

The tiny shared `tools/tests/control_report.py` replaces permanent successful
negative-report copies in all six storage supervisors. Their original validators,
116 rejection controls, primary report fields and failure assertions remain.
An unexpected control failure retains its actual report at the original diagnostic
path. The scene-execution wrapper now checks ordered case IDs against discovery,
replacing a stale count; the independent qualification inventory remains intact.

Deleted file lengths:

- Windows: **3,214,771,044 bytes (2.99 GiB)**: 348 successful control copies,
  89 incremental-link files and five explicit temporary/archive targets.
- WSL: **591,837,076 bytes (0.55 GiB)**: 464 successful control copies. This is
  guest filesystem cleanup, not a claim of immediate virtual-disk shrinkage.

All 42 corresponding primary reports remain, as do licensed fixtures, source,
historical receipts/archives, current executables/PDBs, object files, dependency
caches and canonical build configurations. No host/WSL resource settings or
virtual-disk compaction changed. Existing ignore rules already cover generated
cleanup output. Exact inventories and removal results are in `out/testing-cleanup/`.

Test-only nonincremental linking prevents its scratch from returning at the next
selected rebuild. Application and third-party linker scratch may regenerate;
their iteration settings were retained. Full test executable relinks can cost
more than incremental links, while retaining normal debugger symbols.

## Verification

- Replayed all **116 rejection controls** through the existing supervisor entry
  points and validators using previously passing scenario data; five complete
  saved scenario collections were revalidated. No database/native child matrix
  was needed for this report-lifecycle change.
- Three scratch success/failure controls and a WSL cleanup/failure-preservation
  smoke pass. The initial replay comparison included randomized temporary paths;
  its comparison was corrected without changing tracked validator assertions.
- The existing scene-execution process check passes with its ordered discovery
  assertion. The separate independent coverage inventory was not weakened.
- A representative MSVC test executable was relinked, retained its PDB, produced
  no `.ilk`, and passed its nineteen glTF owner cases. Generated link settings
  apply the option to exactly 46 owned test executables, not apps or vendor
  targets. Runtime and Server/Core dependency boundary targets pass.
- Skill structure, repository/contract links and diff checks pass. No full engine,
  GPU, dedicated Server, sanitizer or package matrix was rerun for cleanup.

Results are in `out/testing-cleanup/summary.json`, `control-replay-summary.json`,
the focused JUnit/log files and `link-settings.json`. Earlier qualification records
remain historical; this review does not rewrite their results or count them fresh.

Suggested commit: `chore(testing): remove redundant artifacts and streamline verification`.
