# 15c: terminal imports and jobs

Date: 2026-10-28. Base: `3971222` (`feat(authoring): add terminal editing and persistence`),
initially clean `main`. **15c is PASS** under current-machine scope. Full REQ-15
remains NOT_RUN until 15e connected acceptance. Staging, commits and pushing remain
the user's work.

## Delivered behavior

`SharkAuthor` adds 21 implemented asset methods, for 40 terminal methods total.
Discovery reports their schemas/examples, import/job capabilities and limits.
The [protocol contract](../authoring-protocol.md) and dated
[ADR 0039 extension](../architecture/0039-terminal-authoring-protocol.md) define
the wire surface; protocol version 1 and native asset/cache formats are unchanged.

- Typed metadata reads/writes use the retained ProjectFiles owner and 32 exact
  file-revision tokens. Writes reject external edits, foreign projects/tokens and
  source/sidecar overlap with any owned project or declared scene path. Tokens
  have explicit release and no silent eviction. Replacement at capacity works.
- The native catalog accepts an explicit complete sidecar inventory and exposes
  bounded revisioned query/resolve operations. Cache inspection preserves native
  freshness, last-valid keys, dependency attribution and validated descriptors;
  binary payloads stay outside the bounded protocol response.
- The same byte, glTF-resource and glTF importers use the existing four-slot,
  single-worker AssetImports service. Submit/list/cancel/drain/take/close expose
  complete state, progress, diagnostics and publication/durability outcomes.
  Only explicit drain publishes ready work. Terminal outcomes retain their slots
  until take; taking prepares its bounded reply before releasing the outcome.
- Import work preserves dirty scene state, common history and mutation retries.
  Project-file and native source/metadata/catalog/cache fences reject stale
  publication without replacing last-valid output. Cleanup methods remain usable
  after an external project edit blocks publication. Successful cancellation
  cannot roll back an already published import.
- Cache opening is lazy, so corrupt disposable cache bytes do not block metadata,
  catalog or stage recovery access. Job close permanently closes admission even
  before the cache opens. Project close/EOF joins the worker before borrowed
  owners disappear and never publishes pending work or saves unsaved scene edits.

The new service and wire helpers are private EngineTools sources. No second
project writer, cache owner, scheduler, player protocol, source decoder or public
C++ API is introduced. Native callbacks must still cooperate/return for shutdown;
arbitrary callback preemption is not promised. A lost response requires explicit
file/cache/job reconciliation, not inference of rollback from RPC correlation.

## Selected verification

Evidence is under `out/15c-work/`. Inventories were listed before selection. The
affected regression comprises protocol unit/process, editing process, new import
process, native asset import/jobs/catalog and glTF import: eight CTest families.
The six new real-process cases exercise all 21 methods, independent expected byte
payload digests, licensed glTF dependencies, server-content rejection, dirty
history, stale candidates, retained capacity/cancellation, corrupt-cache repair
access, metadata token limits, EOF and actual process death/lock handoff.

| Selection | Result / evidence |
| --- | --- |
| Windows Debug six affected targets | Build PASS; `windows-debug-build.log` |
| Windows Debug affected regression | 8/8 PASS, 52.27 s; `windows-debug-tests.log`, `windows-debug.xml` |
| Final process case plus coverage audit | 2/2 PASS, 3.80 s; `windows-final-process.log/xml` |
| WSL Debug same affected targets/regression | Build and 8/8 PASS, 11.86 s; `wsl/linux-debug-build.log`, `wsl/linux-debug-tests.log/xml` |
| WSL final process case | PASS, 0.84 s; `wsl/linux-final-process.log/xml` |
| Windows ASan six affected targets/regression | Build and 8/8 PASS, 100.99 s, no reported findings; `windows-asan-build.log`, `windows-asan-tests.log`, `windows-asan.xml` |
| New private source instrumentation | Both objects reference ASan load checks; concise `asan-new-objects.log` |
| Published temporary-byte import example | PASS; `documented-example.log` |

The final capacity/repair case was added after the first Debug selections;
only its owning process family was rerun on Windows/WSL. The ASan selection
already includes all six cases. No failed checks or relaxed bounds were needed.

Primary commands:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset windows-debug --target SharkAuthor AuthoringProtocolTests AssetImportJobsTests AssetImportTests AssetCatalogTests GltfImportTests --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug --show-only
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark\.tools\.(authoring-(protocol(-process)?|editing-process|import-process)|asset-import(-jobs)?|asset-catalog|gltf-import)$" --output-on-failure --no-tests=error
```

Windows ASan uses the matching preset. WSL reuses the retained Debug build,
read-only source mount and exact offline inputs. The testing policy selects one
relevant sanitizer; no additional WSL sanitizer run is claimed. No host settings,
compiler flags, drivers or dependency inputs changed. Unchanged native parser,
texture/material, Server, GPU and package matrices were not repeated. The 15a
CPU/protected dependency closure remains applicable: added files stay in existing
EngineTools, with no production target edge or protected input changes.

Pinned formatting, repository/link/manifest checks and diff review pass. The
coverage audit maps the 21 new methods to actual subprocess tests; seventeen
FeatureWorld cases and 56 in-process CPU surfaces remain unchanged. External CPU
commands now comprise forty terminal methods and the exporter; the two GPU
commands remain separate. No Lua surface is advertised. `.gitignore` already
covers build/output, native project/cache/stage files, local environments and the
optional roadmap. Temporary successful projects are cleaned; compact evidence
uses the existing ignored output directory.

## Next work

15d connects deterministic runs, export and optional capture to the retained
project owner. Consume the active scene and preserve unsaved edits/history;
do not launch an independent source-reloading writer behind the session. Reuse
the existing run/export owners and candidate validation/publication. Keep CPU
authoring independent of Client, with device capture in a separate capability-
gated adapter and actual available-GPU evidence. Implemented discovery must stay
truthful. 15e then qualifies the connected public workflow and closes REQ-15.

Suggested commit: `feat(authoring): add terminal asset imports and job control`.
