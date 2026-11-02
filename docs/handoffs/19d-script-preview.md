# 19d: connected Lua preview and authoring

Date: 2026-11-01. Base: `31135ca`, initially clean `main`. The user owns
staging, commits and pushing. [ADR 0052](../architecture/0052-editor-script-preview.md),
the [preview contract](../physics-preview.md), [Lua API contract](../script-runtime.md)
and [checkpoints](../roadmap/lua-plan.md) describe the behavior.

The shared `scene.physics` owner now captures enabled Lua files through scoped
ProjectFiles reads, excludes typed project/scene paths and rechecks exact source
revisions before publishing a frozen client-presentation preview. Text writes
reject during play without consuming their revision tokens. The existing 60 Hz
preview invokes update then fixed-update before each physics tick, and destroys
scripts before physics/Scene teardown. Logs, contextual errors, completed tick
and fault state stay in bounded original operation receipts; exact retries do not
repeat callbacks. Failed initialization releases the candidate. Source edits,
authored history and saved Scene remain separate. `scene.run` remains static.

The native Editor adds a Scripts tab using the same text read/write operations,
create-only or exact revision publication and draft-close protection. Script
output and errors use the same preview receipts; runtime errors preserve path,
entity and traceback. The terminal advertises `script_preview` in discovery.
Existing export and Runtime guards continue rejecting Script-bearing scenes;
this development preview does not supply cooked source, packaged execution,
server gameplay integration or a hostile-code sandbox.

REQ-19 passes under current-machine scope for the stated Lua lifecycle, checked
API and authoring criteria. The [19c handoff](19c-script-runtime.md) supplies
unchanged Core Lua, stale-handle, FeatureWorld and protected boundary coverage;
its Windows and WSL final-input results remain applicable. This is not a claim
that later packaged gameplay execution or physics Lua bindings are implemented.

## Selected verification

| Check | Result |
| --- | --- |
| Windows Debug | Eight of nine affected Core, Editor and public-process selections initially passed; `windows.xml`. The discovery fixture expected the old exact capability set and failed. After adding `script_preview`, its focused retry passed (`windows-protocol-retry.xml`). |
| Windows Debug after UI diagnostic fix | Editor physics and GUI adapter passed 2/2; `windows-editor-final.xml`. The new test verifies that later frames retain the original runaway diagnostic. |
| WSL Ubuntu Clang ASan+UBSan | Public authoring editing process passed, including script source revisions, replay, syntax/runtime/runaway recovery, failed initialization and aggregate log overflow; `wsl/tests.log` and `tests.xml`. The affected Editor physics/source adapter passed after the diagnostic fix; `wsl-editor/tests.log` and `tests.xml`. |
| Native Editor on RTX 4070 Laptop | A fresh public-terminal-authored project opened. The runaway callback stopped at the instruction limit; after the fix its traceback, source path and entity remained visible across frames. Stop remained usable. The native Scripts tab opened, edited and explicitly wrote a corrected script, then Play displayed its initialization log; source editing and recovery were observed before the final diagnostic-only fix. Standard-user Editor exited cleanly. |
| Repository checks | Contract and link validation, ignore/skill boundaries, pinned C++ formatting and diff whitespace passed. `.gitignore` already covers generated outputs and the local roadmap. |

Windows used `tools/windows_dev.ps1 cmake --build --preset windows-debug` for
`SharkAuthor Editor EditorPhysicsTests EditorGuiTests EditorInspectorTests
EditorProjectSessionTests SceneRunner` and the selected inventory in `windows.xml`.
The WSL build/selection commands are in `wsl-commands.sh` and
`wsl-editor-commands.sh`; both reuse the retained ASan+UBSan build. The desktop
run used the existing GUI/scene shader artifacts and explicit RTX device with
validation **off**. The earlier validation-on launch was NOT_RUN: its retained
validation-tools receipt predates the Lua dependency-lock change and was rejected
as stale. This CPU/UI acceptance does not claim new GPU validation or repeat
unaffected graphics acceptance. The initial native run exposed a generic message
overwriting the original script failure; the final Editor run verifies the fix.

Next: follow the manifest's explicit assignment order and its next owner. Preserve
the preview authority and isolation boundaries; later physics Lua APIs and cooked
script closure need their own integration and acceptance before export/Runtime
rejection can be lifted.
