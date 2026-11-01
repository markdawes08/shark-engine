# 19c: checked Lua lifecycle and API

Date: 2026-11-01. Base: `f6b96d2` on initially clean `main`. The user owns
staging, commits and pushing. [ADR 0051](../architecture/0051-script-runtime.md),
the [runtime contract](../script-runtime.md) and the
[Lua checkpoints](../roadmap/lua-plan.md) define this increment. Full REQ-19
remains NOT_RUN until connected product acceptance.

Core has a creating-thread ScriptRuntime consuming trusted bounded source
snapshots. One weak Scene lease fences duplicate owners without retaining a Scene
borrow; a separate physics lease still governs writes. Enabled Script references
are resolved in stable identity order into isolated VMs. Explicit load, init,
frame, fixed and reverse destroy calls use checked entity generations and existing
scheduler ticks. Pending Scene mutations prevent callbacks, including stop; stale
or changed entities are discarded at teardown with owned errors. Faults preserve
prior effects and require explicit reconciliation; close can discard callbacks.
Limits cover source, VM memory, instructions and owned logs. Native library work
is not a wall-clock sandbox.

The v1 API supplies self Name, local position and Camera near clip, time, bounded
logging, finite clamp and version discovery. Client presentation is read-only;
server gameplay only permits checked self Name writes during fixed updates, and
Scene/physics write leases can still reject them. Region metadata is not proof of
authority. The Lua manifest now records these entries, and FeatureWorld executes
real Lua with independently checked values, server scheduler tick zero, phase
ordering and final state. The process fixture checks the public manifest; the
existing case IDs and saved FeatureWorld remain stable.

19c provides a Core host API; it does not attach scripts to the Editor, terminal
runner, Client or Server loops. Callers must use trusted ProjectFiles containment,
typed-path exclusion and freshness checks when obtaining source. Runtime and
export still reject Script-bearing scenes until actual source/cooked closure and
product integration exist. `scene.run` remains a static integrity case.

## Selected verification

Verification follows the [testing policy](../testing-policy.md). Results and
commands are retained in `out/19c-work/`; only affected behavior and protected
dependency boundaries are selected. `.gitignore` already covers generated output,
local environments and the optional local roadmap; it needs no change.

| Check | Result |
| --- | --- |
| Windows Debug selected Lua, runtime, physics, Scene/edit/query and FeatureWorld tests | PASS 8/8, including the real FeatureWorld process; `windows-final.xml`. |
| WSL Ubuntu Clang ASan+UBSan selected Lua, runtime, physics, Scene/edit/query and FeatureWorld tests | PASS 7/7; `wsl-qualified/tests.log` and `tests.xml`. |
| Windows Release Runtime | Build and protected dependency boundary PASS; this does not qualify script execution in Runtime. |
| WSL Release Server | Build and protected dependency boundary PASS; headless idle startup/shutdown PASS, `wsl-qualified/server-build.log` and `server-smoke.log`. |
| Repository checks | Contract/coverage links, skill and ignore boundaries, pinned C++ format and diff whitespace PASS. |

Windows used `tools/windows_dev.ps1 cmake --build --preset windows-debug` with
`ScriptRuntimeTests CoreLuaTests FeatureWorldTests SceneRunner Tests CorePhysicsTests`,
then listed and ran `shark.core.{lua,script-runtime,physics,scene,scene-edit,scene-query}`
and `shark.tools.{featureworld,featureworld-process}` through the matching CTest
preset. The WSL commands and exact selected inventory are recorded in
`wsl-qualified-commands.sh` and `wsl-qualified/inventory.log`. The earlier WSL
results precede the final lease/physics coexistence tests and are not used for
this final claim. No desktop or GPU run is needed for these CPU changes.

Next: 19d's trusted source acquisition and connected human/terminal workflows,
with diagnostics and runaway recovery. Preserve all existing source, Scene,
physics, capability and Runtime/export boundaries. Do not claim connected or
standalone execution until its actual owner and content closure are qualified.
