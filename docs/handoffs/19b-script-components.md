# 19b: shared Script settings

Date: 2026-11-01. Base: `5d4a5be`, initially clean `main`. The user owns
staging, commits and pushing. The [Script contract](../script-components.md),
[ADR 0050](../architecture/0050-script-components.md) and
[Lua plan](../roadmap/lua-plan.md) define this checkpoint. Full REQ-19 remains
NOT_RUN: the stored Script is inert and no lifecycle, gameplay bindings, cooked
script closure or user-facing execution is claimed.

Core now validates a bounded `scripts/**/*.lua` source reference and enabled flag,
with checked Scene setters/deferred edits, revisioned owned queries and schema
discovery. Shared commands, atomic history, capture/reconstruction, exact retained
retries and dynamic string charges carry the component. Source schema 1 adds
optional `shark.script`; prepared scene v4 preserves Script as inert data while
v1-v3 remain readable and retain their existing no-Script encoding. Editor
inspector drafts and terminal discovery/query/mutation use the shared owner.
The existing revision-bound text operations remain separate from component edits.

The editor viewport and physics preview can reconstruct Script metadata without
opening the referenced file. Standalone export rejects any Script before staging;
Runtime activation rejects it before device creation, including disabled settings
and candidates beyond the first query page. CPU package inspection remains
available. F5 retains its existing complete-candidate replacement semantics.
Script metadata by itself grants no server authority and is not a new replicated
field. 19c must establish server/client capability separation and resolve script
source using existing ProjectFiles containment and typed-path conflict checks.

FeatureWorld's existing component case exercises an independent value, invalid
path, source and prepared round trips and undo on a separate owner. The source
fixture and case IDs remain unchanged. The coverage manifest adds Script and its
shared command; the Lua API list stays empty because no binding is implemented.

## Selected verification

Selection follows the [testing policy](../testing-policy.md): affected Core,
source/history/retry, terminal/editor, prepared/Runtime and FeatureWorld behavior,
one portable sanitizer profile and protected target boundaries. No whole-project
matrix or connected Lua acceptance is claimed. Results are in `out/19b-work/`.

| Check | Result |
| --- | --- |
| Windows Debug Core, Tools, Client, terminal processes and Editor adapter | PASS 17/17 selected CTests in 19.01 s; `windows-debug.xml`. |
| Windows Debug viewport and query continuation | PASS 2/2 in 0.66 s; `windows-viewport.xml`. The viewport case retains a usable snapshot with a missing referenced script and no source/history changes. |
| Windows Debug Script inspector widget | PASS 1/1 in 1.86 s after adding a scripted draft to the headless GUI fixture; `windows-gui.xml`. The native Editor executable was rebuilt; no new manual desktop session is claimed. |
| WSL Ubuntu Clang ASan+UBSan | PASS 10/10 affected CTests in 14.82 s and the additional query-continuation test in 0.15 s; `wsl-final/tests.log/.xml` and `wsl-server/query.log/.xml`. |
| Windows Release protected Runtime | Build and dependency boundary PASS; this does not qualify standalone script execution. |
| WSL Release protected Server | Build and dependency boundary PASS; headless idle startup/shutdown PASS, `wsl-server/server-build.log` and `server-smoke.log`. |
| Repository checks | Requirement/link/coverage validation, skill/ignore checks, pinned C++ formatting and diff whitespace PASS. `.gitignore` already covers generated outputs and the local roadmap. |

Windows builds used `tools/windows_dev.ps1 cmake --build --preset windows-debug`
for the affected Core, Tools, Client, Editor and terminal targets, followed by
the selected CTest names in `windows-debug.xml` and `windows-viewport.xml`.
The WSL build and selections are recorded in `wsl-final-commands.sh` and
`wsl-server-commands.sh`; they use the retained builds without host/WSL setting
changes. The Release Runtime uses the `runtime-only` preset.

The first WSL Clang build rejected an existing exhaustive test switch after the
new mutation kind was introduced. The switch now covers `set_script` and the
query-continuation case verifies immediate and deferred invalidation. The first
build failure remains in `wsl/build.log`; final selected results are recorded
separately.

Next: 19c's ordered lifecycle and checked API, with explicit server gameplay and
client presentation capabilities. Preserve the distinction between a project
source path and a cooked script artifact; do not lift Runtime/export rejection
until script loading, capability enforcement and dependency closure are real.
