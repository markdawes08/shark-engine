# 21d: shared prefab authoring

Date: 2026-11-05. Base: `6fa72f8`, initially clean `main`. The user owns staging,
commits and pushing. Follow the [prefab plan](../roadmap/prefab-plan.md),
[authoring contract](../prefab-authoring.md), [ADR 0057](../architecture/0057-prefab-source-assets.md)
and [testing policy](../testing-policy.md).

Typed ProjectFiles prefab IO requires matching adjacent asset metadata. Seven
terminal methods connect exact source observations, subtree capture, separate
source publication and immutable prepared instance commands to the retained
project owner. Instantiation uses its existing mutation outcomes/history; a
retry preserves the original ID map and receipt even after source changes.
Metadata and source have independent file receipts. No cache importer or cooked
prefab format is advertised.

The Editor Prefabs tab adapts these same services. Source drafts, explicit Write,
preparation and instantiation remain separate. File uncertainty and Scene mutation
uncertainty retain their own reconciliation. Play and close/discard include prefab
drafts; Scene Save and undo do not publish source. The twenty-first FeatureWorld
case exercises capture/publication, prepared replay, one history group and saved
Scene/source independence on an isolated copy.

## Verification

**PASS:** 14 unique selected Windows Debug CTests and four selected WSL
ASan+UBSan CTests. The native Editor builds successfully. These are affected
selections, not whole-project runs. Windows checks cover prefab codecs/instances,
project files, shared protocol and actual terminal processes, FeatureWorld,
portable Editor prefab/physics/session adapters and CPU ImGui composition.
WSL covers prefab protocol, project files, the portable Editor adapter and the
actual terminal prefab process with leak detection and halt-on-error enabled.

Builds used matching existing presets through `tools/windows_dev.ps1` and the
retained WSL `/work/build-05a-final-client/linux-asan-ubsan` directory, with two
parallel jobs per directory. Inventories were listed before selected CTest runs;
all runs used `--output-on-failure --no-tests=error` and absolute JUnit paths.
No Core implementation, dependency, package or protected target boundary changed.

| Selection | Result | Logs under `out/21d-work/` |
| --- | --- | --- |
| Windows prefab protocol and native FeatureWorld | 2 PASS; process harness corrected below | `windows-focused.log`, `windows-focused.xml`, `windows-build.log` |
| Windows corrected FeatureWorld process | 1/1 PASS | `windows-featureworld-corrected.log`, `.xml` |
| Windows source/file/instance and protocol/process regression | 7/7 PASS | `windows-tools.log`, `.xml`, `windows-regression-build.log` |
| Windows Editor prefab, physics, project-session and CPU GUI adapters | 4/4 PASS | `windows-editor.log`, `.xml`, `windows-editor-build.log` |
| WSL ASan+UBSan affected portable checks | 4/4 PASS | `wsl-final/build.log`, `inventory.log`, `tests.log`, `tests.xml` |

Review corrected canonical source reconciliation and expired-token cleanup in
the new portable panel. A missing-sidecar IO failure followed by independent
canonical publication tests reconciliation of a differently formatted draft;
it does not claim to have dropped a successful publication reply. Original
durability remains unknown after that observation. Final formatting-only changes
reuse behavioral results. Pinned clang-format, `git diff --check`,
`tools/validate_contract.py` and `tools/check_repository.py` pass.

The first focused Windows selection passed prefab protocol and native FeatureWorld
checks. Its process harness failed because the independent expected revision list
still contained 20 cases. Appending the twenty-first case's unchanged revision
corrected the fixture; the process check then passed. Original output remains in
`out/21d-work/windows-focused.log` and `windows-focused.xml`; corrected output is
`windows-featureworld-corrected.log` and `.xml` in that directory.

Manual native interaction is NOT_RUN under the user pause and is recorded as
[BL-21D-UI-01](../backlog.md#deferred-items). No GPU, package or live-server
qualification is claimed. `.gitignore` covers generated outputs and local tools;
no new ignore rule is needed. README remains unchanged.

## Next

21e connects repeated instance/runtime lifetime and reload acceptance. Preserve
the existing Scene/physics/script owners, original partial-effect receipts and
bounded prepared inputs. Full REQ-21 remains NOT_RUN pending its applicable
acceptance, including the separately deferred native interaction.
