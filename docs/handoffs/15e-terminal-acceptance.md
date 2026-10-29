# 15e: connected terminal acceptance

Date: 2026-10-29. Base: `7f9cc57` (`feat(authoring): add retained scene runs exports and capture`),
initially clean `main`. **15e and full REQ-15 are PASS** under current-machine
scope. The user retains staging, commits and pushing. Assignment 16, the human
editor UI using these shared services, is next.

## Delivered behavior

[examples/terminal_authoring.py](../../examples/terminal_authoring.py) is a usable
Python standard-library client of the public redirected protocol. Its
[documented invocation](../authoring-protocol.md#complete-terminal-example)
requires a built host and fresh output directory. CPU mode works on Windows/WSL;
configured Windows export and Client capture are explicit optional flags.

The same script is the connected acceptance client. It:

1. Creates a project/startup scene through the retained owner, copies the licensed
   offline glTF triangle, external buffer and complete MIT notice, then writes
   typed metadata and imports the two-node closure.
2. Modifies the copied glTF generator and scene-selection settings, refreshes and
   reimports. The root source/settings/key change while the resource is reused.
   Direct file writes ingest/edit these external inputs; typed project, scene and
   metadata writes use public services. Original fixtures remain unchanged.
3. Previews and commits a Camera/MeshRenderer batch referencing the imported
   asset, saves, closes and reopens in a new process. It refreshes the explicit
   catalog and retains the cache rather than substituting built-in geometry.
4. Makes an unsaved Unicode name edit, runs the static integrity case with seed
   1337 for three ticks, and optionally captures/exports those exact values.
   Authoring status, history, retry retention and saved source bytes stay unchanged
   throughout these operations.
5. Explicitly saves the final edit and verifies a third process can reopen the
   clean project. The CPU acceptance fixture independently acquires a further
   owner and checks the resulting semantic fields.

The example bounds pipe messages/waits, import completion and cleanup. It checks
all nested mutation results and publication receipts, returns structured JSON and
nonzero failure, and preserves failed output for inspection. Fixed UUIDs are
sample identities, not guidance to reuse IDs in new production content. Static
integrity is not gameplay/Lua execution. Captures remain unlit offscreen color/
depth artifacts; this adds no human UI, presented screenshot or PBR claim.

There are **no production C++, public protocol/schema, dependency or target-boundary
changes**. The existing artifact fixture gains a connected mode and reuses the
existing imported-triangle oracle. One new CPU workflow family independently
checks saved source/components, modified inputs/import descriptors and handoff;
it is also used by the actual artifact workflow. No new fixture framework or
duplicate negative-control matrix was added.

## Selected verification

Evidence is in `out/15e-work/`. Inventories were listed before selection. The
complete terminal subsystem selection comprises protocol unit/process, editing,
imports, execution, export failure controls and the new workflow family. Existing
native backend behavior was unchanged and its earlier evidence remains valid.

| Selection | Result / evidence |
| --- | --- |
| Windows Debug affected hosts/protocol test/SceneRunner | Build PASS; `windows-build-final.log` |
| Complete terminal Debug inventory | 7/7 families PASS, 4.82 s; `windows-selected-inventory.log`, `windows-tests.log`, `windows-debug.xml` |
| Windows ASan run/workflow process selection | Build and 2/2 PASS, 1.40 s, no findings; `windows-asan-build.log`, `windows-asan-tests.log`, `windows-asan.xml` |
| Retained WSL Debug run/workflow selection | Build and 2/2 PASS, 1.24 s; `wsl/linux-build.log`, `wsl/linux-tests.log/xml` |
| CPU-only host and actual export workflow | Build/export PASS; `tools-build-final.log`, `tools-dependents.log`, `cpu-export/summary.json`, `cpu-export-token.json` |
| Connected capture on RTX 4070 Laptop and Intel, plus Client-host export | PASS, 10.717 s; `artifacts/summary.json`, per-workflow summaries and `artifact-token.json` |
| CPU/Client package payload comparison and isolated deployment | PASS; `deployment.json`, `isolated/summary.json`, `isolated-token.json` |
| Connected coverage audit | PASS; `coverage-final.log/xml` |
| Repository/link/manifest and diff checks | PASS; `contract-final.log`, `repository-final.log`; `git diff --check` |

Primary Debug command after the affected build and inventory:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark\.tools\.authoring-(protocol(-process)?|editing-process|import-process|execution-process|export-controls|workflow-process)$" --output-on-failure --no-tests=error
```

ASan/WSL select `^shark\.tools\.authoring-(execution|workflow)-process$`; the
second check connects new behavior while the first includes 15d's final schema
assertions. CPU-only export runs the public example using `build/tools-debug/bin/SharkAuthor.exe`
with the explicit export flags from the protocol guide. That cache has Client,
Editor, native window and native graphics OFF; its host DLL inventory has no
graphics dependency. Export itself initializes no renderer.

Actual GPU acceptance invokes `tools/tests/test_authoring_artifact_process.py
--connected` through the existing standard-user supervisor. It uses the retained
`out/14b-work/scene-shaders` and validation inputs, exact current GPU UUIDs, fixed
160x120 dimensions and the existing `runtime_scene_smoke.reference("imported")`.
Both captures check 18,640 pixels with zero color/depth error and clean required
validation/resource close. Root/resource attributions, the modified root key,
prepared scene digest and one packaged imported mesh bind the artifacts to the
new authored project. Full MIT text and the existing Runtime dependency/CRT
checks remain required.

The CPU-only package was relocated outside the checkout through a Unicode path
and launched on both GPUs from initially empty external working directories with
sanitized environment, observed modules and validation off. This is same-machine
deployment evidence, separate from required-validation captures. Its manifest is
`752331614bc113c1ba59d771d8f3a068978ca5937f6f5b620fbdd5e81dd50512`.
The successful temporary deployment and launcher working directories were cleaned;
the authored examples, original packages and compact results remain under `out/`.

### Reused evidence and resolved check issues

The original [15a](15a-terminal-protocol.md), [15b](15b-terminal-editing.md),
[15c](15c-terminal-imports.md) and [15d](15d-terminal-execution.md) handoffs retain
their exact platform/profile results. The current Debug terminal selection also
exercises their malformed/version/size/no-TTY streams, scoped-file conflicts,
retained lost-reply replay/mismatch/expiry, cancellation/publication, second-writer
and EOF/process-death controls. Native owner/import/history/export code and
dependency edges are unchanged, so earlier native sanitizer, fault and protected
Runtime closure evidence is reused. CPU checks do not qualify GPUs.

15d's ASan Runtime consumer limitation remains a verification limit; this turn
claims ASan CPU authoring only. The unchanged Runtime's default-startup/F5 and
native lifecycle gates retain [14c](14c-isolated-export-acceptance.md) and earlier
Runtime evidence. No whole-project CTest, additional sanitizer matrix, native
Linux graphics, remote CI, external machine or production-capacity claim is made.

Two verification issues were resolved without changing engine behavior:

- Initial redirected Windows builds triggered PowerShell's native-stderr error
  handling on a vendor CMake deprecation warning. The final invocations captured
  both native streams while preserving the command exit code; builds pass and
  warning/initial logs remain. No vendor sources or warning policy changed.
- An initial local helper incorrectly required identical export manifest hashes.
  Only `timestamp_utc` differed between the independently created CPU/Client
  packages. The corrected comparison requires equality of every other manifest
  field, including exact payload identities; each isolated package still verifies
  its own complete manifest digest. `cpu-export-check.json` retains the original
  failure. The passing CPU workflow was reused, not rerun, before isolated checks.

## Coverage, retention and next work

REQ-15 is PASS and `current_assignment` is 15. Four existing terminal feature
mappings and the 22 CPU methods exercised by the example link the actual public
script and connected fixture; capture/export retain their separate native artifact
mapping. The independent coverage audit enforces those links. Counts remain
seventeen FeatureWorld cases, 56 in-process CPU surfaces, 43 external CPU commands
and three GPU surfaces. No unimplemented Lua/editor operation is advertised.

`.gitignore` was reviewed: existing `out/`, build/cache, native stage and Python
bytecode rules cover this increment. The new public example and fixture remain
eligible for version control. No new ignore rule, storage tree, dependency cache
or host/WSL setting is needed.

Next, plan assignment 16's human editor checkpoints around the existing shared
owner, schemas, commands/history/retries, persistence and import services. Preserve
terminal availability and explicit project handoff; do not introduce a second
mutation path. Editor/terminal semantic parity and human continuation remain
actual assignment 16/17 deliverables, not inferred from this terminal acceptance.

Suggested commit: `feat(authoring): qualify connected terminal workflow`.
