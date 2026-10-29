# 18a: private CPU physics foundation

Date: 2026-10-29. Base: `6fe897e` (`test(editor): connect viewport transforms
and terminal handoffs`), initially clean `main`. User-owned staging, commits and
pushing remain untouched. **18a is complete under the selected headless scope.**
Desktop interaction remains paused. Full REQ-18 stays
NOT_RUN; this increment qualifies the empty native foundation, not bodies or
connected gameplay simulation.

## Delivered behavior and contracts

[ADR 0045](../architecture/0045-physics-foundation.md) and the
[physics-world contract](../physics-world.md) introduce a uniquely owned real
empty Jolt PhysicsSystem in EngineCore. Public engine-owned values cover bounded
configuration, fixed-duration updates, tick/body statistics, owner-thread guards
and explicit idempotent close. Private native jobs are single-threaded; no new
scheduler, mutable Scene, gameplay protocol or body/component stub is introduced.
Native update failures fault the owner rather than retrying uncertain progress.

A private process lease registers types/factory, rejects foreign ownership and
restores saved allocator/diagnostic callbacks after the final native owner releases
resources. A public-header-only example performs three updates and closes the
world. Six focused cases exercise validation, native lifecycle, shared leases,
callback restoration, foreign factory rejection and owner-thread behavior.

Jolt 5.6.0 is pinned to commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`
and exact archive SHA-256
`1f32328fb763135de10a244568d6ccb2ed9b1e6593fafe6dc6db5b2719d330bd`.
The [official release](https://github.com/jrouwe/JoltPhysics/releases/tag/v5.6.0)
and immutable upstream CMake/source were reviewed. Bounded population validates
the complete archive/inventory and reused source bytes. The static single-precision
SSE2 profile disables compute backends, optional applications, object streams,
profiling/debug renderer, PCH/unity/IPO and uses matching CRT, RTTI and sanitizers. SDK types
and compile requirements stay private; notices enter Server/Runtime packages.

Existing offline/configuration helpers, strict Server closure, Runtime notice
assembly and headless CI now consume the same fourth Core dependency. No parallel
dependency resolver or competing native ownership path is introduced. The
[five checkpoints](../roadmap/physics-plan.md) define root/positive-uniform-scale,
mode-specific pose authority, distinct ticks/substeps and global/local-coordinate
rules before 18b adds bodies, schemas, persistence and authoring.

## Selected verification

Selection follows the [testing policy](../testing-policy.md): physics/public
example and affected lifecycle/timing/transform consumers; changed dependency,
configuration, closure and notice tooling; retained WSL Debug/ASan+UBSan; protected
Runtime/Server builds. A later Release compiler finding adds only the existing
scene-render family. No whole-project or full Server behavior matrix, native
window, GPU or desktop interaction is selected. Logs below are relative to
`out/18a-work/`.

| Selected check | Result and evidence |
| --- | --- |
| Windows x64 MSVC 19.51 Debug physics/public example, lifecycle, timing and transform | PASS, 5/5 CTests, 0.15s; `windows-core-final.log/.xml`. |
| WSL Ubuntu GCC 13 Debug same selection | PASS, 5/5, 0.04s; `wsl-final/linux-debug-tests.log/.xml`. |
| WSL Clang 18 ASan+UBSan same selection | PASS, 5/5, 0.18s; `wsl-final/linux-asan-ubsan-tests.log/.xml`. Leak detection and vptr checks remain enabled. |
| Windows dependency/closure/package tooling | PASS, seven owning families across `windows-tooling-final-interrupted-last-test.log`, `windows-tooling-optimized.log/.xml`, and `windows-source-document-managed.log/.xml`. The last six-case family passes in 53.27s; final two-metadata Server closure controls pass 49 cases in 17.49s under the unchanged 30-second limit (`windows-server-closure-metadata-set.*`). |
| Final Core full-verifier TLS drift rejection | PASS, existing one-method fixture, 71.401s; `windows-core-tls-drift-final.log` and its recorded command. |
| Scene-render regression after Release warning correction | PASS, Windows Debug 0.29s, WSL Debug 0.22s and ASan+UBSan 0.58s; `windows-scene-render-final.log/.xml` and `wsl-final/recovery/` named scene-render logs. |
| Protected Windows Release Runtime | PASS, final build/boundary; `runtime-scene-render-final-build.log`. |
| Actual Windows package assembly/CPU inspection | PASS, 35 files including exact Jolt MIT and DLL/CRT closure; `runtime-package-final.log`, `runtime-package-final-inspection.log`, retained manifest and closure. Deployment qualification remains NOT_RUN; nothing was launched. |
| Protected WSL GCC 13 Release Server | Build/install/actual closure PASS; `wsl-final/recovery/server-build.log`, `server-install.log` and `server-closure-final-2.json/.log`. Actual inventory: 19 targets, 282 compiled sources, 13 package files. |
| Repository, links/coverage, pinned formatting and diff | PASS; `repository.log`, `contract.log`, `format-check.log` and `diff-check.log`. |

```text
tools/windows_dev.ps1 cmake --build --preset windows-debug --target CorePhysicsTests PhysicsPublicExample Tests Server --parallel 2
tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark\.core\.(physics|physics-public-example|timing|lifecycle|transform)$" --output-on-failure --no-tests=error
tools/windows_dev.ps1 cmake --build --preset runtime-only --target Runtime --parallel 2
```

Tooling inventory was listed before selection. Its seven owners are
`physics-dependencies`, `server-closure`, `runtime-package`, `scene-dependencies`,
`storage-dependencies`, `graphics-dependencies` and `source-document-dependencies`
under `shark.tooling.`. Failed/interrupted runs are not called complete passes;
unchanged passing families are retained while only corrected behavior reruns.
The final Core verifier is exercised by the corrected source-document family,
TLS drift fixture and actual Server audit. WSL uses canonical retained builds,
two jobs, native window/graphics OFF and no duplicate offline configure matrix.
`wsl-final-commands.sh` and `wsl-final/recovery/` retain exact commands; no host,
WSL resource or kernel settings changed.

The first Runtime configure exposed an imported Threads target promoted from a
child directory; Core now promotes it only when it is not already global. The
first Runtime build exposed Jolt base-header ordering after include sorting;
separate include blocks preserve its required base-first order. Original failures
remain in `out/18a-work/runtime-configure.log` and `runtime-build.log`. Neither
fix weakens compiler warnings or changes upstream source bytes.

The first WSL ASan+UBSan link exposed missing native typeinfo with vendor RTTI
disabled. The lock and reviewed helper now enable RTTI to match Core and preserve
UBSan vptr checks; no sanitizer suppression was added. Original link diagnostics
remain in `out/18a-work/wsl/linux-asan-ubsan-build.log`. The new wrapper and native
PhysicsSystem objects were inspected for final ASan/UBSan/vptr references in
`wsl-final/asan-wrapper-symbols.log` and `wsl-final/asan-jolt-symbols.log`; this was a focused
new-dependency check, not an audit of unchanged objects.

Two Windows storage-configuration attempts reached the unchanged 240-second
family timeout during fresh Core setup; removing concurrent
SDK builds did not resolve it. Original detail remains in
`windows-tooling-interrupted-last-test.log` and
`windows-tooling-final-interrupted-last-test.log` under `out/18a-work/`.
TLS/Jolt population now creates each candidate directory once and reuses its
validated archive map for the same invocation's complete disk verification.
Every file write, byte/inventory comparison, atomic publication and drift
rejection remains; standalone verification still validates the archive afresh.
The unchanged storage family passes in 226.18 seconds after that correction.

The source-document configuration fixture also exposed stale pre-13 source
expectations: the actual 24-source EngineTools library and its two explicitly
JSON-enabled CLI consumers now have separate include/exclusion checks. Its six
cases and existing drift rejection remain. Corrected assertions passed, but the
family then timed out during its third fresh configuration. Controlled profiles
measured that same Editor configuration at 77.78 seconds in Windows Temp versus
13.16 seconds under project scratch; cleanup was only 1.07 seconds. No host or
security setting changed. CTest now supplies an explicit writable build scratch
parent, preserving independent fresh trees, complete file checks, automatic
cleanup and the original 120-second configure / 240-second family limits.
Original failures are in `windows-tooling-optimized.log` and
`windows-source-document-final.log`; the scoped profiles remain under
`tls-population-profile/`, `editor-config-profile/` and `editor-fixture-profile/`.

The protected GCC 13 Release rebuild exposed `-Wstringop-overflow` in the
existing preview traversal's vector range insertion. An equivalent bounded
`push_back` loop preserves its guard, traversal order and preview behavior,
without suppressing warnings. The original failure remains in
`wsl-final/server-build.log`; only the owning scene-render family reran across
the three selected profiles. Physics inputs were unchanged, so their passing
results remain applicable. One native PowerShell build wrapper also stopped on
an upstream CMake deprecation warning; `windows-scene-render-build.log` preserves
it, and the existing forwarding wrapper confirms the final target is current.

The actual Server audit exposed two non-header metadata paths in Jolt's full
317-entry noncompiled inventory: `Jolt/Jolt.cmake` and
`Jolt/Physics/Collision/Shape/TaperedCapsuleShape.gliffy`. The checker admits only
these exact archive-verified paths on the Jolt target without a compile group;
unknown, foreign or compiled metadata still rejects. Neither metadata file is
packaged. No generic CMake/diagram/source exception is added. Initial audit
failures remain in `wsl-final/recovery/server-closure.json` and
`server-closure-final.json`; `jolt-noncompiled-inventory.json` records the full set.
Final actual closure passes in `server-closure-final-2.json/.log` without another
configure/build or physics test run. The first new synthetic compiler-input
negative control forgot to publish its mutated fixture command file; that test
setup was corrected, its initial failure preserved in
`windows-server-closure-metadata.*`, and only the same owning family reran.

## Coverage, limits and next work

Coverage maps the native foundation and new public example to real CPU fixtures,
with external fixture justification for native process registrations, dependency
integrity and protected deployment. No scene component, terminal method, Lua entry
or FeatureWorld body case is advertised before its implementation. Existing
render-only transform/hierarchy behavior remains intact.

Empty updates do not qualify collision, gravity on bodies, body capacity, event
queues, interpolation or native OOM recovery. The fixed temporary arena has no
unbounded fallback; 18b must qualify work/capacity before bodies can use it.
No current-machine MMO capacity or cross-platform bitwise determinism is claimed.
Historical rendering/authoring results remain tied to their original inputs;
actual 16/17 UI acceptance is still deferred under the user's amendment.

Next is **18b: bodies and colliders**, including real lifetime behavior,
root/scale/authority validation, component/schema/source persistence, shared
commands/history/retry matching, terminal authoring and actual FeatureWorld
mappings. Preserve the native lease/teardown and existing Scene candidate owners.

`.gitignore` was reviewed: existing build/out/cache/local-roadmap patterns cover
all outputs and the canonical archive. New source, fixtures, tools and docs remain
eligible for version control. Reuse canonical builds and clean successful temporary
deployments; retain unique failure evidence. About 788.57 MiB of duplicate
interrupted/profile SDK inputs were removed after preserving small CMake
diagnostics and original logs. Both inspected temporary Runtime payloads were
also removed, along with the audited temporary Server package; their manifests
and closure reports remain. An initial cleanup
command was rejected by automatic review; exact verified literal-path cleanup
subsequently succeeded. No staging, commits or pushes occurred.

Suggested commit: `feat(physics): add private Jolt world foundation`.
