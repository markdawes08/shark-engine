# 15a: terminal protocol foundation

Date: 2026-10-28. Base: `def9755`, initially clean `main`.
**15a is PASS under current-machine scope.** Full REQ-15 remains NOT_RUN;
15b's editing and persistence is next in the
[terminal checkpoints](../roadmap/terminal-authoring-plan.md).
No staging, commits or pushes were performed.

## Delivered behavior and public contracts

`SharkAuthor` is a CPU executable over the new public
`AuthoringProtocol` owner in EngineTools. It supports one-shot `--request` and
persistent `--stdio`, optional fixed `--root`, and optional startup `--project`.
Input needs no terminal, editor, display, model SDK, MCP client or network listener.
The public C++ interface owns strings/Results; nlohmann JSON and wire conversion
remain private. Existing ProjectFiles, AuthoredScene, schema and query services
remain authoritative, with no second scene model or mutation path.

[ADR 0039](../architecture/0039-terminal-authoring-protocol.md) and the
[protocol contract](../authoring-protocol.md) define version 1, framing and
compatibility: 64 KiB requests, 1 MiB responses, bounded parser preflight,
JSON-RPC envelopes with independent `protocol_version`, structured errors,
implemented method/schema discovery and explicit execution prerequisites.
Notifications execute without responses. RPC batch arrays are unsupported and
dispatch nothing. Unknown versions/fields and malformed inputs reject. Windows
uses native Unicode arguments and binary protocol streams; machine stdout never
contains help/logs during a protocol session. The documented exit/channel rules
distinguish operation failures from startup/framing/I/O failures.

Exactly eight methods are implemented: `protocol.discover`, `schema.components`,
`project.open`, `project.status`, `project.close`, `scene.query`, `scene.inspect`
and `scene.continue`. Opening retains one native project lock and one loaded
authored scene; complete candidates and response bytes are prepared before
publication. Second writers reject. Explicit close rejects dirty/pending state;
EOF/destruction releases the owner without saving. No editing method exists yet.

Queries serialize durable authored IDs and scope, selected values, explicit
column-major world matrices and xyzw local rotations. Native handles stay private.
Incarnation/revision counters are decimal strings. A selected absent optional
component is null; an unselected field is omitted. Core row/scan limits and
revision checks remain intact. At most 32 outstanding cursors are retained;
successful continuation consumes its token after response admission and publishes
a fresh token if needed. Failed admission preserves the old token. Lost output
requires restarting the query; request IDs are not mutation retry identities.

Discovery reports the four actual Core component schemas and all twelve properties,
plus only these implemented methods. Mutations and device capture are explicitly
unavailable. Later checkpoints add editing/history/retries, jobs, run/export and
a separate capture adapter through the same retained owner.

## Selected verification

Logs/JUnit are under `out/15a-work/`. Inventories were listed before CTest selection.
The new native family compiles the public header without private includes and
checks creating-thread rejection followed by successful owner-thread access.
The process family has eight focused cases using actual Windows/POSIX pipes:
Unicode/CRLF/fragmentation, no TTY, discovery, independent hierarchy/projections,
revision and consumed-cursor conflicts, cursor-capacity recovery, owner exclusion,
EOF/process-death/broken-output release, malformed streams and exact size limits.
Source documents are checked for preservation.

| Selection | Result / evidence |
| --- | --- |
| Windows Debug `SharkAuthor`, `AuthoringProtocolTests` | Build PASS; `windows-build-metadata.log` |
| Windows Debug new native/process families | 2/2 PASS, 0.70 s; `windows-tests-final.log`, `windows-debug-final.xml` |
| Existing boundary rejection fixture | PASS, 2.55 s; initial `windows-tests.log` records this unchanged passing selection |
| Independent CPU `tools-debug` host | Build PASS; process family PASS, 0.67 s; `tools-build-final.log`, `tools-tests.log`, `tools-debug.xml` |
| WSL Debug | Final build PASS; 2/2 families PASS, 0.57 s; `wsl-final/linux-debug-*` |
| WSL ASan+UBSan | Final build PASS; 2/2 families PASS, 0.91 s, no findings; `wsl-final/linux-asan-ubsan-*` |
| New sanitizer target/source coverage | Both ASan/UBSan symbol families present in the three newly added service/wire/main objects; `wsl-final/new-protocol-instrumentation.json` |
| CPU host and protected Runtime dependency closure | PASS; `tools-boundary-final.log`, `runtime-boundary.log` |
| Published Python terminal example against saved FeatureWorld | PASS, all four durable IDs/names queried and normal close; `documented-example-final.log` |
| Existing scene/FeatureWorld coverage audit | PASS, 2.23 s; `coverage-audit.log`, `coverage.xml` |

Primary commands:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset windows-debug --target SharkAuthor AuthoringProtocolTests --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug --show-only
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset windows-debug -R "^shark\.tools\.authoring-protocol(-process)?$" --output-on-failure --no-tests=error
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset tools-debug --target SharkAuthor --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset tools-debug --target shark_check_server_boundary
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset runtime-only --target shark_check_runtime_boundary
```

The WSL drivers reuse the canonical retained Debug/ASan builds, read-only source
mount and offline dependency caches. No host/WSL resource, kernel, compiler or
driver settings changed. These results cover the new CPU adapter; no GPU run,
package redeployment, full Server suite or unrelated engine matrix was selected.
Core component/query/source services and Runtime payload inputs are unchanged.
The boundary checker now explicitly audits `SharkAuthor` as Core/authoring-only;
protected Runtime still excludes Tools/JSON/source parsers.

## Corrections and retained failures

The first process run had two incorrect test expectations: Core roots have depth
one, so the fixture's grandchild has depth three, and `BooleanConstraints` is
intentionally empty. The tests were corrected from the independent Core contracts
and source hierarchy; both focused reruns and the final full process family pass.
Original output remains in `windows-tests.log`, with `process-fix-tests.log`.

Pinned clang-format crashed once on nested launcher error construction. The
duplicated nesting was refactored into context/data/error/response values, then
formatting passed; `format-first-failure.txt` records the observation without
claiming an underlying tool fix. WSL CMake regeneration also exited 132 once.
A direct generation with a targeted trace passed, followed by both final builds
and test selections; `wsl-launch.log` and `wsl-followup/` preserve that investigation.
Neither tool-process failure is an engine sanitizer finding.

An incorrectly forwarded direct CMake script command hit the repository's
in-source-build guard. Its small generated files were moved into
`boundary-command-failure/`; the named boundary targets then passed. The first
Python documentation example hit the parent console's CP1252 encoding for a
Unicode name. It now explicitly sets UTF-8 stdout; the same example passes.

## Coverage, retention and next work

The manifest maps the new feature and all eight methods to their native/process
fixtures. FeatureWorld retains 17 cases and 56 in-process CPU surfaces; nine
external CPU commands now comprise the exporter plus eight terminal methods.
The external-process mapping is deliberate: an in-process callback cannot prove
framing or cross-process ownership. The existing coverage audit is extended for
these actual methods; no new evidence framework is introduced.

`.gitignore` already covers canonical builds, local environments, generated outputs,
project locks and the optional roadmap. No new ignore rule is needed. Passing
test copies are managed scratch, and unexpected failures/logs remain available.
Final formatting, repository/link/manifest checks, coverage audit and diff review
pass. Source control remains with the user.

15b must adapt project creation, shared commands/batches/preview, save/reopen,
undo/redo, retained retries and scoped text access. Retain this owner through
all operations, prevent text writes from bypassing typed document/dirty-state
rules, and distinguish request IDs from the existing mutation retry ledger.
Update discovery only as each method is implemented. Export in 15d must use the
active owner rather than silently reopening saved files in another process.

Suggested commit: `feat(authoring): add terminal protocol discovery and read-only queries`.
