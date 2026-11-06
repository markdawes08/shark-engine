# M05a: world-cell coordinates and static content

Date: 2026-11-06. Base: `e00d619`, initially clean `main`. The user owns staging,
commits and pushing. Follow the [cell contract](../world-cells.md),
[ADR 0063](../architecture/0063-world-cell-content.md),
[M05 checkpoints](../roadmap/seamless-world-plan.md) and
[testing policy](../testing-policy.md).
M05a PASS; full REQ-M05 remains NOT_RUN. M05b is next.

## Delivered

Core now converts canonical WorldPositions through explicit bounded local origins
without constructing absolute global floats. Float packing retains the caller's
nonzero origin generation; stale unpacking rejects. Quantized reconstruction is
checked before publication, including a float carry past the maximum world cell.
Nearby detail survives distant integer cells; existing source/wire versions stay
unchanged.

`SHKCELL1` carries a world/cell key, positive content revision and the existing
complete prepared Scene envelope. Static-only profile validation preserves
hierarchy/component rules and rejects implicit anchor relocation, cameras,
scripts, moving bodies and triggers. EngineTools prepares through exact existing
AuthoredScene capture, preserving dirty state, source metadata and history.

LoadedWorldCell returns a complete immutable content owner with a fresh checked
Scene incarnation and shared PreparedAssets. It resolves hidden mesh references,
retains owned values after input release and leaves existing owners intact on
failure. Construction/query/destruction remain on the creating thread. This is
synchronous per-cell content activation; no native simulation/readiness, async
residency, region transfer, editor panel or terminal method is claimed.

The connected CPU fixture prepares and loads two adjacent authored cells near a
distant origin. Literal seam coordinates agree in one packed frame; a generation
change rejects old values while fresh conversion preserves global positions.
Authored IDs remain stable and private handles cannot cross cell/authoring owners.
Independent cell-envelope bytes and malformed digest-valid controls cover field
order and semantic admission, alongside complete mesh closure/lifetime checks.

## Verification

Selected Windows Debug and WSL Clang ASan+UBSan checks cover world coordinates,
new Core/Tools cells and their existing prepared Scene/assets/Tools consumers.
All six selected CTests PASS on both profiles. Protected Server/Core dependency
checks PASS in both affected builds. No behavior or compiler check failed.

| Profile | Result | Evidence under `out/M05a-work/` |
| --- | --- | --- |
| Windows Debug | 6/6 PASS, 0.77 s | `windows-final-build.log`, `windows-inventory.log`, `windows-tests.log`, `windows-tests.xml` |
| Retained WSL Clang ASan+UBSan Debug | 6/6 PASS, 1.17 s | `wsl/build.log`, `wsl/inventory.log`, `wsl/tests.log`, `wsl/tests.xml`, `wsl/driver.log` |

Both build the affected WorldCellTests, WorldCellToolsTests, Tests,
PreparedSceneTests, PreparedAssetsTests, PreparedContentTests and Server targets,
with two jobs. Windows uses `tools/windows_dev.ps1` and the matching
`windows-debug` build/test presets. WSL reuses
`/work/build-05a-final-client/linux-asan-ubsan` through the retained isolated
userspace; ASAN_OPTIONS enables leak detection and halt-on-error, UBSAN_OPTIONS
enables halt-on-error and stack traces. No host/WSL resource or kernel settings
changed. WSL's initial in-progress build was canceled while finalizing the
review-discovered packing edge, before any behavior execution; its partial log
remains `wsl-initial-build.log`. Final inputs were then built before running tests.

Inventory precedes selection/execution with `--output-on-failure --no-tests=error`
and JUnit. The exact selection for both profiles is:

```text
^shark\.(core\.(world-position|world-cell|prepared-scene|prepared-assets)|tools\.(world-cell|prepared-content))$
```

The fixture sources are `core_world_position_tests.cpp`, `world_cell_tests.cpp`
and `world_cell_tools_tests.cpp` under tests/. New CTests use seed 1337 and
60-second timeouts; coordinate checks retain the existing 20-second owner.
Formatting, whitespace, contract/manifest links and repository/ignore checks
PASS. The additive APIs map to these external CPU fixtures with explicit
FeatureWorld justification; no serialized component, Lua API, command, dependency
or public example changes.

No whole-project, GPU, manual UI, Release or package matrix was selected for this
additive CPU content change. Existing normalization/translation, prepared-scene
and asset regressions are included; no native rendering/physics, seamless travel,
aggregate residency or capacity result is inferred.

## Continuation

M05b is next: asynchronous byte preparation, prefetch/cancel/retry, complete
last-valid owner publication, eviction and explicit aggregate resident/IO bounds.
Workers return owned values/bytes; owner-thread Scene construction and teardown
remain mandatory. Content revision labels need explicit expected-key/revision
selection, not an inferred authority or automatic newest-content policy.

M05c retains world collision, navigation and readiness; M05d-f retain actual
gameplay continuity and fenced process transfer/recovery. Full REQ-M05 remains
NOT_RUN. Independent manual UI/backlog statuses are unchanged. `.gitignore`
already covers build/output/cache and local roadmap artifacts; no new generated
path exclusion is needed. README is unchanged.
