# 21a: prefab source assets

Date: 2026-11-05. Base: `9bf652a`, initially clean `main`. The user owns staging,
commits and pushing. This is the first checkpoint of the [prefab plan](../roadmap/prefab-plan.md),
following the [testing policy](../testing-policy.md). Full REQ-21 remains NOT_RUN.

EngineTools now provides `PrefabDocument` and pure decode/validate/encode Results
for `.sharkprefab` schema 1. The asset contains one nonempty declared root and
stable prefab-local entity/parent IDs. It shares the existing bounded JSON,
component, extras, hierarchy and canonical encoding helpers with scene source.
There is no synthetic Scene identity or second component policy. Legacy/future
prefab versions, nesting, unsupported fields, unresolved/cyclic links and invalid
components reject. Root-only rigid bodies retain the existing physics contract.
The [source contract](../prefab-assets.md) and [ADR 0057](../architecture/0057-prefab-source-assets.md)
define the public format and its limits.

The independent beacon fixture places its child before the root and asserts
existing render, physics and script source values without loading those resources.
Seven focused cases cover identity/reference preservation, strict envelopes,
contextual graph/body failures, hierarchy/entity boundaries, component constraints,
canonical round trips and shared lexical/extras budgets. Coverage maps the actual
codec family; FeatureWorld spawning remains with the later checkpoints.

## Selected verification

| Selection | Result |
| --- | --- |
| Windows Debug | PASS 4/4 in 14.54 s: prefab documents, existing source documents, authored scenes and rigid bodies. `out/21a-work/windows-tests.log` and `windows-tests.xml`. |
| WSL Clang ASan+UBSan | PASS 4/4 in 20.13 s, the same affected selection. `out/21a-work/wsl-final/tests.log` and `tests.xml`. |
| Repository checks | Pinned changed-file formatting, requirement/link validation, ignore/skill boundaries and whitespace checks pass. |

Commands use the canonical Windows build through `tools/windows_dev.ps1`, and the
retained WSL sanitizer build through the existing isolated userspace helper.
Both inventories are listed before test execution; exact WSL commands are retained
in `out/21a-work/wsl-final-commands.sh`. No new dependency or protected-target edge
is introduced. Native graphics, packages and manual UI checks are unaffected by
this CPU source codec. Existing interactive deferrals remain in the backlog.
README is unchanged by user direction; `.gitignore` already covers generated work
and leaves the new source fixtures eligible for version control.

Next: **21b**, checked instance creation, fresh entity IDs, complete internal
reference remapping, simple overrides and shared undo/redo. 21a adds no source-file
publication, importer, prepared prefab runtime payload, Lua spawn operation or
Editor/terminal method. Preserve exact sidecar identity checks when file/catalog
integration lands. No live entity/reward or replay-deduplication claim is made.
