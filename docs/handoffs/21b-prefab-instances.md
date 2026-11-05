# 21b: authored prefab instances

Date: 2026-11-05. Base: `237e31b`, initially clean `main`. The user owns staging,
commits and pushing. Follow the [prefab plan](../roadmap/prefab-plan.md),
[instance contract](../prefab-instances.md), [ADR 0057](../architecture/0057-prefab-source-assets.md)
and [testing policy](../testing-policy.md). Full REQ-21 remains NOT_RUN.

CPU preparation produces one InstantiatePrefabCommand with an explicit fresh-ID
map and optional Name/local Transform overrides. Existing execute, preview,
batches and retained mutations revalidate and instantiate the complete hierarchy
through the existing private Scene candidate. Source extras and ordering are
staged with complete history and replies before publication. A changed command
publishes one revision and history group. Undo/redo preserves authored values
while recreated entities receive fresh handle generations. Retained retries
compare every new input and return the original outcome without creating again.

Ordinary instantiation supports the source entity bound independently of the
256-command batch limit. Mixed batches admit at most 4,096 new entities across
ordinary and prefab creation. Existing 512 KiB retained-request admission remains
separate; large ordinary commands can be explicitly not_admitted by that API.
Project/mapping/collision, hierarchy/body and original owner/lease fences remain
binding. Preparation uses OS entropy in production and explicit injected entropy
in deterministic fixtures. No source, prepared, wire or database version changes.

Ten focused cases cover independent instances, overrides, source metadata and reopen,
atomic preview/history, invalid input rollback, retained replay and owner fences.
The nineteenth FeatureWorld case connects instance isolation, exact original
replay, fresh redo handles and metadata round trips on an owner made from the
saved baseline. Saved fixture bytes and the runner's original owner stay unchanged.

## Selected verification

| Selection | Result |
| --- | --- |
| Windows Debug affected authoring and consumers | PASS 14/14 in 29.65 s: prefab source/instances, authored scenes, commands/batches/history/retries/integration, rigid bodies, FeatureWorld/native process, terminal editing process and CPU Editor physics/GUI adapters. `out/21b-work/windows-tests.log` and `windows-tests.xml`. |
| WSL Clang ASan+UBSan affected authoring and connected cases | PASS 11/11 in 33.51 s: prefab source/instances, authored scenes, commands/batches/history/retries/integration, rigid bodies and FeatureWorld/native process. `out/21b-work/wsl-final/tests.log` and `tests.xml`. |
| WSL final prefab case additions | PASS 1/1 in 0.31 s, all ten prefab test cases. The original WSL build compiled eight cases before the last two checks were added; only PrefabInstanceTests was rebuilt and rerun. Other regression inputs remain unchanged. `out/21b-work/wsl-prefab-final/tests.xml`. |
| Repository checks | PASS changed-file pinned formatting, requirement/link validation, ignore/skill boundaries and diff whitespace. |

Commands/logs use `out/21b-work/` and canonical builds; inventories were listed
before selection/execution. Exact secondary commands are in
`out/21b-work/wsl-final-commands.sh` and `wsl-prefab-final-commands.sh`. The Windows build uses
`tools/windows_dev.ps1`; both builds include the changed public header through
the behavior tests and FeatureWorld. No manual UI, GPU/package,
Release or unrelated full-project matrix is selected for this CPU authoring
change. The existing manual UI pause and backlog remain applicable. `.gitignore`
already covers generated output and retains the new source files.

Next: **21c**, runtime/Lua spawn/destroy with explicit deferred scheduling,
initialization/teardown and authority/ownership fencing. 21d retains actual
Editor/terminal prefab file and instantiation adapters. Authored instances create
no native physics bodies or script VMs and grant no live or durable authority.
