# 18d: collision filters, triggers and owned events

Date: 2026-10-31. Base: `d34f0c2` (`feat(physics): add fixed-step scene
synchronization and interpolation`), initially clean `main`. **18d is complete
under its headless scope.** Full REQ-18 remains NOT_RUN. Desktop interaction
remains paused, and staging/commits/pushing remain the user's work.

## Delivered behavior and contracts

[ADR 0048](../architecture/0048-physics-contacts.md) and the
[contact contract](../physics-contacts.md) append collision_layer, collision_mask
and is_trigger to the existing RigidBody. Stable property IDs 21-23 bring the
catalog to 23 properties without adding a component, command, terminal method or
Lua entry. Native per-body rules enforce both masks; sensors detect without
collision response. Settings remain immutable for each native body and retain
the existing root/scale/authority and end/edit/rebuild boundaries.

Native Added/Persisted callbacks collect unique pairs into fixed preallocated
storage. They never allocate, throw, access Scene, lock bodies or invoke game/Lua
code. After Update, the creating-thread owner checks exact native/engine
generations, filters against the native contact cache and sorts pair transitions.
Complete owned events publish only with a successful Scene/tick boundary. Native
sleeping is disabled so stable overlap does not become a false exit. Sampling
retains native discrete/preintegration semantics; it is not CCD or a geometry
query of final integrated poses.

Independent configured limits bound observed pairs and queued events. Worst-case
event reservation rejects before movement without faulting the world. Native
collection overflow faults without partial Scene/new-event publication. Explicit
bounded take returns a FIFO prefix of owned sequence/tick/phase/cause and scoped
participants. Earlier events survive faults/removal; close accounts for discarded
queued values and detaches the listener before reverse native teardown.

`remove_body(scene, token)` uses the same leased runtime Scene owner, preserves
the visual entity, removes RigidBody in one revision, invalidates the native body
generation and queues exits after complete preparation. The detached overload
uses the same lifetime path, including after source destruction. Previously taken
values remain owned after entity generation reuse and Scene/world teardown.

Shared source, create/set, history restoration, retained input comparison and
terminal discovery/query/save/reopen preserve all eleven fields. Source/component
schema 1 accepts absent collision fields with legacy defaults; older strict
readers explicitly reject newly saved fields. Prepared version 3 stores nondefault
collision settings, while version-1/2 decoding and applicable byte encoding remain.
Core-only loading preserves settings without adding Runtime importers.

The existing eighteenth FeatureWorld case now prepares version-3 filters, adds a
runtime-only sensor and independently checks gravity/nonresponse and enter/exit
transitions. Saved fixture IDs/bytes, live authored source/revision/history and
the case inventory remain unchanged. Coverage adds `physics.contacts` and links
the real owning cases.

## Selected verification

Selection follows the [testing policy](../testing-policy.md): physics, changed
schema/source/prepared behavior, shared authoring/history/retry consumers, terminal
processes and FeatureWorld; portable Debug/sanitizer smoke and protected builds.
All tests are CPU/headless. Logs below are relative to `out/18d-work/`.

| Selection | Result and evidence |
| --- | --- |
| Windows x64 MSVC Debug affected regression | PASS, 24/24 CTests in 30.29s; `windows-tests.log/.xml`. |
| WSL Ubuntu GCC 13 Debug portable selection | PASS, 8/8 in 3.73s; `wsl/linux-debug-tests.log/.xml`. |
| WSL Clang 18 ASan+UBSan same portable selection | PASS, 8/8 in 11.18s; `wsl/linux-asan-ubsan-tests.log/.xml`. Leak detection/vptr checks remain enabled. |
| Final physics family after collector hash correction and 16-actor trigger/bulk-removal case | PASS, Windows 1/1 in 0.29s (`final-physics-tests.log/.xml`); WSL Debug 1/1 in 0.07s and ASan+UBSan 1/1 in 0.36s (`wsl-final/linux-debug-tests.log/.xml`, `linux-asan-ubsan-tests.log/.xml`). Unaffected consumer results above are reused. |
| Protected Windows Release Runtime | PASS, final build; `runtime-final-release.log`. |
| Protected WSL GCC 13 Release Server | PASS, final build and dependency boundary; `wsl-final/server-build.log`. |
| Repository, coverage/links, pinned formatting and diff | PASS; `repository.log`, `contract.log`, `format-check.log` and `diff-check.log`. |

Windows selected physics/public example, schema, Scene/edit/query/hierarchy,
render-components, prepared-scene, scene-render, rigid-body, source-documents,
authoring commands/batches/history/retries/protocol and discovery/editing processes,
prepared-content, client-export, FeatureWorld/process and portable editor
viewport-navigation. WSL selected physics/public example, prepared-scene,
scene-query, rigid-body, source-documents, FeatureWorld and the editing process.
Inventories were listed before selections. No windowed tests or GPU runs occurred.

```text
tools/windows_dev.ps1 cmake --build --preset windows-debug --target CorePhysicsTests RigidBodyTests FeatureWorldTests SceneRunner SharkAuthor --parallel 2
tools/windows_dev.ps1 ctest --preset windows-debug --show-only -R "^shark.core.physics$"
tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark.core.physics$" --output-on-failure --no-tests=error
tools/windows_dev.ps1 cmake --build --preset runtime-only --target Runtime --parallel 2
```

The inventories/JUnit retain exact broader selections; `wsl-commands.sh` retains
canonical WSL commands with native window/graphics OFF and two jobs. Existing
offline dependencies/builds were reused; no dependency, install, license, compiler
flag or host/WSL resource setting changed. CPU prepared encode/load, export
consumers and protected Runtime cover the new format; unchanged package assembly
and deployment were not rerun. No whole-project CTest or hosted CI was selected.

The first targeted run (`initial-tests.log/.xml`) found two premature exit
expectations and an old discovery assertion treating the renderer's visible flag
as the only Boolean property. Pinned Jolt source establishes collision evaluation
before final integration: tests now explicitly check no premature exit, then the
next fixed tick's exit, rather than retrying or changing tolerances. Stable overlap
also runs 120 held ticks. Discovery independently checks the new false Boolean
default/empty constraint object while retaining the existing visible assertion.
All corrected behavior passes in the primary regression and both WSL profiles.

Final review also corrected the collector hash so both body slots affect bucket
selection. The earlier shift placed the first slot outside the bucket bits,
creating a common-bucket cluster for many actors touching one high-slot sensor.
The final 16-actor case checks ordered unique enters, stable persistence and bulk
removal exits without late native removals duplicating participants. Only physics
reran after this internal correction; final protected binaries were rebuilt.

## Limits and next work

Proceed with **18e**, shared native body/filter controls and connected editor/
terminal project and simulation lifetimes in the [physics plan](../roadmap/physics-plan.md).
Keep authored source/history separate from runtime Core state, reuse the retained
services, expose only implemented capabilities and preserve explicit writer/
save/close/reopen handoff. Native UI/play/rendering still needs explicit desktop
resumption; the [backlog](../backlog.md#desktop-ui-deferral) records the remaining
settings/play/removal observations as NOT_RUN. This is not full REQ-18 acceptance.

Current events sample committed native contact-cache observations, not every
transient substep contact or final-position query. No stay/CCD/query/Lua callback,
actual Server region loop, production MMO capacity or new package deployment is
claimed. Queries/Lua retain 20 and streaming/origin transfer retains M05. Native
concurrent jobs must revisit the single-thread collector before adoption.

`.gitignore` was reviewed; existing build/out/cache/local-roadmap rules cover all
outputs while new source/tests/docs remain eligible for version control. No
staging, commits or pushes occurred.

Suggested commit: `feat(physics): add collision filters triggers and owned contact events`.
