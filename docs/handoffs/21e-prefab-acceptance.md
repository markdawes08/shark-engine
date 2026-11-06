# 21e: connected prefab acceptance

Date: 2026-11-05. Base: `f6ec59a`, initially clean `main`. The user owns staging,
commits and pushing. Follow the [prefab plan](../roadmap/prefab-plan.md),
[acceptance contract](../prefab-acceptance.md),
[ADR 0058](../architecture/0058-runtime-prefab-mutation.md) and
[testing policy](../testing-policy.md).

Implementation and connected automated acceptance are complete under the current
machine scope. Full REQ-21 remains NOT_RUN for the native Editor interaction
deferred in [BL-21D-UI-01](../backlog.md#deferred-items). The manual UI pause remains
active; automated offscreen fixtures do not qualify actual Prefabs tab input.

## Delivered

`prepare_runtime_prefab` connects validated Tools source to an owned Core prefab
value, preserving local references and components and checking composed transforms.
It drops source-only metadata by type and performs no IO, import, asset closure,
cook, Scene mutation or authority grant. Runtime and Server gain no source parser.
The existing FeatureWorld instance case now exercises this bridge; its 21-case
catalog and saved source stay unchanged.

Four connected CPU families cover source-valid composed-transform overflow;
32 independent three-entity authored instances through edit/save/reload/reopen;
16 contact-driven pairs of runtime instances; and four source/script/owner reloads.
The contact family saturates eight VMs/four bodies, checks complete initialization,
original replay, independent velocity/movement, reverse teardown, stale references
and capacity return. CPU render snapshots deliberately retain their immutable mesh
lease until released, then the weak lease expires.

The existing SceneRenderSmoke gains an opt-in `--spawning` mode. On each GPU, two
fresh runtime/Scene incarnations each execute four spawn/destroy cycles with three
VMs, one body and four entities at peak. All 16 occupied/empty readbacks use literal
independent color/depth references. Packed requests remain usable after actor
teardown; the final pair also outlives explicit script, physics and Scene closure.
Per-frame GPU resources retire after completion, and final owned resource counters
return to zero. These counters do not measure driver allocations or production
MMORPG capacity. Existing renderer modes, workloads and tolerances are preserved.

## Verification

All 13 selected Windows Debug CTests pass on final relevant inputs. The affected
prefab inventory covers source/instances/preparation, runtime/Lua, shared protocol,
real terminal process, Editor prefab adapter, CPU extraction and FeatureWorld.
The complete scene-render Python tooling module also passes all 12 tests, including
three new occupied/empty oracle and invalid-evidence families. Reuse its successful
`python -m unittest tools.tests.test_scene_render_smoke -q` result; no duplicate
CTest tooling run was needed. This was PATH-resolved Python from the repository.

Four selected WSL Clang ASan+UBSan CTests pass with leak detection and halt-on-error:
runtime spawning, prepared content, prefab instances and connected acceptance.
Windows uses the existing `windows-debug` preset via `tools/windows_dev.ps1`;
WSL reuses `/work/build-05a-final-client/linux-asan-ubsan`. Both use two build jobs,
listed inventories, fixed seed 1337, explicit timeouts and `--no-tests=error`.
No new package/dependency boundary or optimization-sensitive production path calls
for another Release/package matrix. Applicable 21a–21d evidence remains valid for
unchanged owners; the new connected cases supply the missing lifetime coverage.

| Selection | Result | Evidence under `out/21e-work/` |
| --- | --- | --- |
| Windows prepared content and FeatureWorld/native process | 3 PASS in the first focused selection | `windows-focused.log`, `.xml` |
| Windows connected acceptance after fixture corrections | 1/1 PASS, four families | `windows-acceptance-final.log`, `.xml`; earlier corrected run retained |
| Windows remaining affected regression | 9/9 PASS | `windows-regression.log`, `.xml` |
| WSL ASan+UBSan affected portable checks | 4/4 PASS | `wsl-final/tests.log`, `tests.xml`, `inventory.log`, `build.log` |
| RTX 4070 Laptop, driver 616.56 | PASS, 16 readbacks, required validation | `spawning-rtx-final/summary.json`, raw color/depth files, token receipt |
| Intel RaptorLake-S Mobile, driver 101.6790 | PASS, 16 readbacks, required validation | `spawning-intel/summary.json`, raw color/depth files, token receipt |

Both native reports contain zero validation warnings/errors/dropped messages,
eight unique actor roots, two closed runtime lifetimes and zero final owned frame,
offscreen, queue-worker, recording, submitted, swapchain and target resources.
The runs were serial, through the same user's standard token, with no manual UI.
They used the locked scene recipe generation
`c1cf5e21d7eadadee2579940e049d961b11055aaa9062f49a81dcd03d31b8ffc`.
Actual commands, device/driver metadata, executable/shader/tool identities and
pixel comparisons are in their existing summary format.

### Failures and corrections

The initial contact fixture attempted a Lua Name write while physics owned the
Scene. That rejection is intentional in the existing ScriptRuntime/physics
contract. The fixture now preserves source Names and proves independent runtime
mutation through supported opposite velocity writes. It explicitly confirms that
ordinary Name writes remain blocked; the existing Lua owner/physics lease test
also passes. No authority bypass or Core behavior change was made. The original
failure remains in `windows-focused.log` and `.xml`.

WSL initially rejected an omitted aggregate `parent` initializer in the new
overflow fixture under warnings-as-errors. Explicitly initializing the same empty
parent fixes portability. `wsl-final/build-initial.log` retains that compile
failure; the corrected Windows acceptance and WSL checks pass.

The first GPU preparation attempt rejected older shader/validation receipts tied
to the pre-Lua dependency lock, before device access. `spawning-rtx/summary.json`
retains that failed prerequisite check. Reusing the canonical offline validation
build and rebuilding the unchanged scene recipe produced current passing receipts
in `validation-tools/` and `scene-shaders/`. The pinned tool preparation is recorded
in `validation-preparation.log`; native qualification then passed on both GPUs.
No image baseline or tolerance was changed.

Pinned changed-file formatting, whitespace, requirement/link validation and
repository/ignore checks pass. `.gitignore` already covers generated outputs and
tools; no new rule is needed. README remains unchanged. No unrelated whole-project
or manual UI checks were run.

## Next

M04, authoritative movement and gameplay, is next in manifest order under the
existing UI backlog amendment. Keep the existing server one-writer model, actual
authenticated ownership, input sequencing and durable operation owners. These
prefab actors are ephemeral and expose no durable entity/reward operation; local
grants and epoch tests do not qualify network reconnect or cross-region handoff.
REQ-21's deferred native interaction stays open without blocking later coding.
