# 13a: Camera and MeshRenderer components

Date: 2026-10-27. Base: `c1a3e64`, initially clean `main`.
**13a is PASS under current-machine scope.** Full REQ-13 remains NOT_RUN;
next is **13b, runtime asset ownership and built-in geometry** in the
[five-checkpoint plan](../roadmap/scene-rendering-plan.md).
The user owns staging, commits and pushing.

## Delivered

Optional Camera and MeshRenderer values extend Core's existing Scene owner,
immediate/deferred mutations, candidate copying, schema discovery and owned queries.
Camera validates perspective/orthographic parameters and finite clipping; aspect
and camera selection remain viewport/extraction inputs. MeshRenderer holds an
explicit stable AssetId, mesh index and visibility. Neither loads an asset or
initializes a renderer. The existing Tools asset identity is now a Core type with
source-compatible Tools aliases; rebuild C++ consumers.

Shared create/set/remove commands preserve optional values through atomic preview,
undo/redo, capture/activation and native source save/reload. Retained comparison
covers presence, projection, every Camera floating-point bit and every renderer
field. Existing sizeof-based history/request charges include the new fixed values.
No authoring bypass or second owner was added. Query flags for the two optional
components default false, preserving existing three-flag identity-only calls.

[ADR 0032](../architecture/0032-render-components.md) and the
[component contract](../render-components.md) document source compatibility:
envelope 1 adds optional known component entries at component version 1; existing
sources retain their meaning. Old readers reject unknown components explicitly,
future component versions reject, and legacy-0 shape remains unchanged.
Source codecs and importers remain private to Tools.

FeatureWorld retains its four entity IDs and global cell anchor. Its saved root
has a Camera and the fourth entity a deliberately unresolved mesh reference. The
fourteenth native case independently checks component values, preview/late-invalid
rollback, history, retained removal/replay/mismatch, deleted-entity recreation and
actual native save/reopen/reload. Coverage now maps 51 CPU surfaces and one external
GPU command; the process supervisor performs 32 checks. No resource resolution or
rendering is inferred from this CPU reference. Earlier cases/limits are retained.

## Selected verification

Followed the [testing policy](../testing-policy.md), with ordinary logs/JUnit under
`out/13a-work/` and no new evidence framework.

| Profile / check | Result |
| --- | --- |
| Windows MSVC Debug | 23/23 selected CTests, 58.26 seconds: render components, scene/edit/query/hierarchy/schema, source/authoring/history/retries/persistence, FeatureWorld/process/coverage, three affected replication/interest consumers and Server unit |
| Final query-test amendment | 1/1 `shark.core.scene-query`, 0.06 seconds, after extending its mutation-kind switch to both new immediate/deferred edits |
| WSL GCC Debug | 4/4, 4.47 seconds: render components, source documents, authoring commands and FeatureWorld |
| WSL Clang ASan/UBSan | 9/9, 33.79 seconds: render components, scene edit, source documents, authored scene, four authoring families and FeatureWorld; leak detection and halt-on-error enabled |
| Consumers / API | Runtime and Server rebuilt with existing transitive boundary checks; new public-header example compiles with MSVC warnings-as-errors |
| Repository | Contract/link validation, repository checks, changed C++ formatting and diff checks pass; staging remains empty |

There are **36 distinct passing profile/test selections and 37 CTest executions**;
the final native query run is a repeat after a test-only amendment. Six new Core,
six new authoring and four new source cases extend existing suites, plus one new
connected FeatureWorld case. `shark.core.render-components` has a 30-second timeout
and seed 1337; all older workloads, timeouts and seeds remain unchanged.

Native commands use `tools/windows_dev.ps1`, `windows-debug` and explicit test-name
selections. `windows-inventory.log`, `windows-debug.xml`, `query-final.xml`, build/
test logs, `consumer-build.log` and `public-example.log` record results. WSL reuses
the retained `/work/build-05a-final-client/linux-debug` and `linux-asan-ubsan` builds.
`wsl-commands.sh` records selections; `wsl/debug.xml` and `wsl/sanitizer.xml` retain
results. No host/WSL settings were changed.

Two initial GCC warnings stopped WSL compilation: a reference-lifetime diagnostic
at parsed projection text and an existing exhaustive test switch missing the new
mutation kinds. Owning the bounded string and extending the switch fixed both
without relaxing warnings. Original failure logs remain under `out/13a-work/wsl/`.
No runtime test failed. Only the affected native query test needed repeating.

No GPU tests, package redeployment, full network/storage matrix or additional
Release/sanitizer profiles ran: this checkpoint changes CPU components and their
authoring/persistence consumers, with no graphics, lock, install or packaging
changes. Actual Runtime/Server consumer builds check the new Core source boundary;
historical package results are not counted fresh. `.gitignore` was reviewed;
existing build/out/cache/local-roadmap rules cover all generated artifacts.

13b must provide Runtime-safe prepared asset/scene loading and built-in cube/plane
assets. Keep Tools source readers/importers out of Runtime; do not turn 13b into
assignment 14's export workflow or assignment 31's final cooking. Projection,
extraction, bounds/culling and actual drawing remain 13c/13d, with connected GPU
acceptance in 13e. Editor and terminal adapters remain 16/15.

Suggested commit: `feat(scene): add authored camera and mesh renderer components`.
