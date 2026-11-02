# 20c: Lua physics and owner-thread contacts

Date: 2026-11-01. Base: `14c2ffd`, initially clean `main`. The user owns staging, commits and pushing. [ADR 0055](../architecture/0055-lua-physics-contacts.md) and the [Lua physics contract](../physics-lua.md) define this increment. Full REQ-20 remains NOT_RUN until connected assignment acceptance.

ScriptRuntime can temporarily borrow the existing PhysicsWorld during protected lifecycle calls and exposes checked self velocity, velocity assignment, force, impulse, teleport, kinematic target, and bounded ray/sphere/box observations to Lua. A trusted server gameplay host can mutate during fixed or contact phases; client presentation can read but cannot gain server gameplay authority. Queries return at most 32 complete owned results with no actionable body handles. Host authentication and cross-region authority remain separate.

The creating-thread owner explicitly drains physics contacts after native commit and invokes optional enter/exit Lua callbacks. Native callbacks never call Lua. Contact writes are queued in callback order, bounded to 64 and rechecked before application after the full batch. Removal/recreation uses captured settings and fresh checked generations. Stale observations remain in the owned dispatch report, and removal-generated exits remain queued for a subsequent drain before stepping. Callback and application errors preserve partial effects and script context without rollback or blind replay. Live body creation enforces the same populated-world temporary-arena admission as simulation start.

Editor and terminal physics preview use the same script/physics owner and include contact logs and consumed events in the original retained operation receipt. Exact retries do not execute callbacks again. Source, history and saved files remain isolated. Existing hosts without a supplied physics world retain their lifecycle but physics calls fail explicitly. No source, prepared, gameplay wire or database format changed; preview log discovery adds the `contact` phase.

The existing FeatureWorld rigid-body case now runs real trusted-region Lua controls, filtered queries, contact delivery and deferred body recreation against independent geometry/timing expectations. The coverage manifest maps all eleven new Lua functions to the real case and focused ScriptRuntime tests. The terminal process test also observes a real client-preview contact callback and exact replay. Core tests exercise invalid inputs, authority, bounded overflow, stale generations, partial failures, runtime body admission and contact ordering.

## Selected verification

| Check | Result |
| --- | --- |
| Windows Debug | PASS 7/7 selected Core Lua/physics/ScriptRuntime, Editor physics, FeatureWorld, FeatureWorld process and authoring-editing process CTests in 12.92 s; `out/20c-work/windows-final.xml`. |
| WSL Ubuntu Clang ASan+UBSan | PASS 5/5 affected Core Lua/physics/ScriptRuntime, FeatureWorld and authoring-editing process CTests in 4.98 s; `out/20c-work/wsl-final/tests.log` and `tests.xml`. |
| Repository checks | Requirement/link and skill/ignore validation, pinned formatting and diff whitespace PASS. `.gitignore` already covers generated output and the optional local roadmap. |

Windows used the x64 `tools/windows_dev.ps1` wrapper, the canonical `windows-debug` build and selected CTest inventory. WSL commands and inventory are in `out/20c-work/wsl-final-commands.sh` and `wsl-final/inventory.log`. Initial terminal verification exposed the fixture's unsupported `pcall`; the fixture was corrected, then all final-input checks above passed. Final verification includes the contextual-error and undersized-arena fixes.

No GPU evidence, production capacity, deployed server gameplay loop or full REQ-20 acceptance is claimed for this CPU checkpoint. Existing results remain unchanged. The next checkpoint is 20d: debug visualization and shared human/terminal physics inspection. Preserve the shared owners and 20c's deferred/fault semantics; 20e remains connected assignment acceptance.
