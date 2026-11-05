# 21c: runtime and Lua prefab mutation

Date: 2026-11-05. Base: `e630178`, initially clean `main`. The user owns staging,
commits and pushing. Follow the [prefab plan](../roadmap/prefab-plan.md),
[runtime spawning contract](../runtime-spawning.md),
[ADR 0058](../architecture/0058-runtime-prefab-mutation.md) and
[testing policy](../testing-policy.md). Full REQ-21 remains NOT_RUN.

Core RuntimePrefab values retain bounded local content references without source
JSON or Tools dependencies. An explicit revocable trusted-host grant binds the
exact live Region/Scene/epoch. The host must establish actual authority before
granting it; copied Region metadata remains insufficient. New Lua spawn and
checked self-destroy requests share the deferred contact queue and apply after
the complete callback batch through private dual-lease Scene access.

Spawn reserves fresh live identities once, remaps parent references, creates
complete transforms, attaches bodies and initializes captured scripts. Teardown
runs eligible destroy callbacks before body removal, VM release and checked
hierarchy deletion. Receipts record original sequence identity and completed owned
operations, including partial failure and uncertain progress from faulted native
owners. Identical retained requests do not apply or
deliver completion again. Old generations, revoked grants and replacement Scene
incarnations cannot retarget mutation.

Thirteen new CPU test families and the twentieth FeatureWorld case connect script/native ordering,
independent instances, source/catalog capture, callback deferral, original replay,
checked teardown and partial effects. Existing saved FeatureWorld sources and
authoring state remain unchanged. These are ephemeral actors; no durable record,
reward, player authentication or live cross-region capacity is claimed.

## Selected verification

| Selection | Result |
| --- | --- |
| Windows Debug affected Core and consumers | All 20 selected families PASS on final relevant inputs. The primary selection passed 19/20 in 28.88 s; only the new raw-VM fixture's incorrect load context failed. After its correction, the Lua family passed 1/1 in 0.04 s. `out/21c-work/windows-final.xml` and `windows-lua-final.xml`. |
| WSL Clang ASan+UBSan affected portable behavior | All 18 selected families PASS on final relevant inputs. The selection passed 17/18 in 23.49 s using the Lua fixture compiled before that correction; the rebuilt Lua family passed 1/1 in 0.04 s. `out/21c-work/wsl-final/tests.xml` and `wsl-lua-final/tests.xml`. |
| Repository checks | PASS pinned changed-file formatting, requirement/link validation, skill/ignore boundaries and whitespace. |

The selection covers Core Lua, ScriptRuntime, spawning, physics/public example,
Scene/edit/query/hierarchy/transforms, CPU Editor physics and GUI adapters,
authored/prefab/body consumers, FeatureWorld/native process, terminal editing and
headless Server unit/lifecycle. WSL omits the unchanged GUI adapter and terminal
editing process, while covering the portable Core, physics adapter, authoring,
FeatureWorld and headless Server consumers. New spawning passes all 13 cases on
both platforms, including nonzero callback ticks, capture independence, replay
and receipt expiry, contact deferral/stale exits, overflow, init-failure partial
effects and repeated spawn/destroy plus revocation-safe stop cleanup.

Canonical builds and `out/21c-work/` retain inventories and logs. Exact WSL
commands are in `wsl-final-commands.sh` and `wsl-lua-final-commands.sh`; Windows
uses `tools/windows_dev.ps1`. The initial focused run is preserved in
`windows-focused.xml`/`.log`: new fixtures incorrectly used static bodies with
the dynamic-only velocity API and expected bindings in a raw VM. Correcting
those fixtures preserved the production velocity, profile and load-phase rules.
The remaining load-context fixture error was corrected with separate contextual
load and callback invocation. Only the affected Lua family was repeated after
that edit; the other final-input results were reused. No tolerance, existing
workload or timeout was weakened.

New tests use the existing Catch2/CTest seed 1337, controlled updates and explicit
60-second timeout. Dependency edges and source parser exclusions are unchanged;
the selected headless Server compiles and runs with the new Core sources. No manual
desktop, GPU, package or unrelated whole-project matrix is selected for this CPU
change. Manual UI remains paused under the existing backlog. `.gitignore`
already covers generated output and leaves new sources eligible for versioning.

Next: **21d**, shared Editor/terminal prefab source publication and instantiation
workflows. 21e retains connected lifetime/assignment acceptance. Runtime/export
still reject Script-bearing packages until real cooked source closure and
integrated execution land. Existing M03 durable transactions and M04/M05 network
and handoff gates remain necessary for their separate live gameplay outcomes.
