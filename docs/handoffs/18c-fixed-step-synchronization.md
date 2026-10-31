# 18c: fixed-step simulation and Scene synchronization

Date: 2026-10-31. Base: `038c717` (`feat(physics): add authored rigid bodies
and collider lifetimes`), initially clean `main`. **18c is complete under its
headless scope.** Full REQ-18 remains NOT_RUN. Staging, commits and pushing remain
the user's work; desktop interaction remains paused.

## Delivered behavior

[ADR 0047](../architecture/0047-fixed-step-physics.md) and the
[simulation contract](../physics-simulation.md) extend the existing PhysicsWorld
with explicit begin/end and populated fixed-duration updates. Admission validates
complete component/native inventory and conservative arena capacity before
acquiring one Scene writer. Scene holds a weak lease; native owners keep no Scene
pointer. Ordinary immediate/deferred/candidate writes reject while leased. End
and native teardown restore ordinary access, including after Scene destruction.

Static reposition, dynamic teleport and bounded kinematic targets establish
mode-specific transform authority. Gravity/native contacts update dynamic poses;
kinematic targets are consumed once, and a rejected newest target cannot execute
an older candidate. Teleport clears velocity and interpolation history. A private
Core port validates all canonical root/subtree poses before one Scene revision
publication. Failures after native work fault the world and retain the last full
Scene; they do not claim native rollback or permit blind retry.

Thin adapters use existing OfflineScheduler/ServerScheduler and reject mismatched
rate/index before polling. No scheduler, accumulator, focus policy or gameplay
protocol is added. Existing pause/single step and authoritative retained debt
remain in their original owners. Server tick frequency and collision substeps
remain independent. The public Core-only example now executes this path.

Native work uses discrete primitives, zero damping, bounded linear/angular speed,
single-threaded jobs, explicit in-flight pair capacity and disabled large-island
splitting. The conservative temporary requirement includes the pinned native
step/contact/island buffers; the fixed allocator meters actual high water without
a fallback. Exact-budget and undersized admission controls cover 64 active bodies
and 16 substeps. These qualify the declared bounded CPU profile, not MMO capacity
or arbitrary native heap exhaustion recovery.

Owned presentation snapshots interpolate translation and shortest-arc quaternion
rotation without Scene writes. CPU extraction validates revision/scoped identity,
recomposes full subtrees before camera/culling and retains physics-tick provenance.
It rejects mixed authoring/physics previews; viewport picking rejects interpolated
snapshots, including tick zero. Native rendering was not launched.

The existing eighteenth FeatureWorld case now loads a prepared Core runtime Scene,
executes gravity through OfflineScheduler, independently checks displacement and
proves authored source/revision/history remain unchanged. Source/prepared schemas,
component/property IDs, semantic commands and terminal method inventory remain
unchanged. The static terminal integrity runner gains no physics/gameplay claim.
Coverage adds `physics.fixed-step` with real owning fixtures and the same case.

## Selected verification

Selection follows the [testing policy](../testing-policy.md): changed physics,
Scene scheduling/ownership, CPU extraction/picking, body persistence and authoring
consumers, FeatureWorld and its process; portable Debug/sanitizer smoke and
protected Release builds. Logs are relative to `out/18c-work/`.

| Check | Result and evidence |
| --- | --- |
| Windows x64 MSVC Debug selected regression | PASS, 16/16 CTests in 15.57s; `windows-tests.log/.xml`. |
| Final Windows cases after arena/quaternion controls and query-fixture correction | PASS, 5/5 in 10.29s; `final-cases-tests.log/.xml`. Reuses unaffected passing families. |
| WSL Ubuntu GCC 13 Debug portable selection | PASS, 8/8 in 0.96s; `wsl-fixed/linux-debug-tests.log/.xml`. |
| WSL Clang 18 ASan+UBSan same portable selection | PASS, 8/8 in 2.70s; `wsl-fixed/linux-asan-ubsan-tests.log/.xml`. Leak detection and vptr checks remain enabled. |
| Protected Windows Release Runtime | PASS, final build; `runtime-release.log`. |
| Protected WSL GCC 13 Release Server | PASS, final build and actual dependency boundary; `wsl-fixed/server-build.log`. |
| Repository, coverage/links, pinned formatting and diff | PASS; `repository.log`, `contract.log`, `format-check.log` and `diff-check.log`. |

The Windows selection is physics/public example, timing, Scene/edit/query,
hierarchy, scene-render, rigid-body, authoring commands/batches/history/retries,
FeatureWorld/process and editor viewport-navigation. The WSL subset is physics/
public example, scene-render, rigid-body, FeatureWorld, timing, scene-edit and
hierarchy. All are CPU/headless; the editor navigation fixture uses portable
geometry, not desktop interaction. Inventories were listed before selections.

```text
tools/windows_dev.ps1 cmake --build --preset windows-debug --target CorePhysicsTests PhysicsPublicExample Tests SceneRenderTests EditorViewportNavigationTests RigidBodyTests AuthoringCommandTests FeatureWorldTests SceneRunner --parallel 2
tools/windows_dev.ps1 ctest --preset windows-debug --show-only -R "^shark.core.physics(-public-example)?$"
tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark.core.physics(-public-example)?$" --output-on-failure --no-tests=error
tools/windows_dev.ps1 cmake --build --preset runtime-only --target Runtime --parallel 2
```

The exact broader selections are retained in the inventory/JUnit files.
`wsl-commands.sh` retains canonical WSL build/test commands with two jobs and
native window/graphics OFF. No host/WSL resource settings changed. The x64 MSVC
wrapper selected the locked Windows tools. Existing dependency archives/builds
were reused; no fresh dependency matrix, whole-project CTest or package deployment
was selected because install/linkage/license/format inputs did not change.

Initial WSL compilation exposed a pre-existing 18b omission in the query fixture's
exhaustive SceneMutationKind switch. Its immediate/deferred cursor-staleness
inventory now includes set_rigid_body; warnings remain errors and its Windows
family passes. Original diagnostic: `wsl/linux-debug-build.log`. An initial
regression build named nonexistent per-history/batch targets; the existing shared
AuthoringCommandTests target was selected instead (`build-regression.log`).
No tolerance, timeout, workload or historical evidence was weakened.

## Limits and exact next work

Proceed with **18d**, layers/triggers and bounded owner-delivered events in the
[physics plan](../roadmap/physics-plan.md). Preserve the same native lease and
Scene writer, fixed scheduler policies, checked references and last-complete
Scene on failure. Review arena admission before enabling any new native temporary
allocation path. Keep safe removal/teardown while events are queued; physics jobs
never invoke Lua. Queries/Lua retain 20; streaming/rebasing retains M05.

18e retains actual editor play/body controls and connected human/terminal saved
project acceptance. Native UI/GPU play, rendered interpolation and actual focus/
input behavior remain NOT_RUN in the [backlog](../backlog.md#desktop-ui-deferral).
Full REQ-16, REQ-17 and REQ-18 are not closed. No actual Server region loop,
terminal physics runner, CCD, production capacity, new package deployment or
hosted CI qualification is claimed by these CPU checks.

`.gitignore` was reviewed: build/out/cache/local-roadmap rules already cover all
generated outputs, and new source/docs/tests remain eligible for version control.
No staging, commits or pushes occurred.

Suggested commit: `feat(physics): add fixed-step scene synchronization and interpolation`.
