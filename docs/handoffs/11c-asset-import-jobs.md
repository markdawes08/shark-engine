# 11c: Bounded background asset import jobs

Date: 2026-10-27. **11c is PASS under the current-machine scope.** Base:
`ae023a0` on initially clean `main`. Changes remain unstaged/uncommitted; the
user owns staging, commits and pushing. **Next: 11d, integrated qualification.**
Full REQ-11 remains NOT_RUN until that checkpoint closes.

Follow the [asset checkpoints](../roadmap/asset-plan.md),
[ADR 0027](../architecture/0027-bounded-asset-jobs.md),
[job contract](../asset-import-jobs.md), existing
[cache contract](../asset-import-cache.md) and
[qualification receipt](../coverage/evidence/11c-asset-import-jobs.json).

## Delivered behavior

AssetImports extends the established capture/prepare/publish path with one lazy
private worker. The owner captures exact immutable source/metadata revisions,
catalog/cache revisions and a bounded reusable cache snapshot. Worker views keep
their FileRevision owners alive; the worker never calls ProjectFiles, catalog or
Scene. The same preparation logic serves the existing synchronous offline API.
All importer callbacks are serialized; callback-bearing owner operations return
conflict while busy. Importer instances are exclusively borrowed and cannot
reenter owner services. Runtime/Server acquire no authoring dependencies.

The six typed operations submit, query jobs, cancel, drain, take and close share
one service for later human/terminal adapters. IDs contain the owner's fresh UUID
scope and nonrewinding sequence. Matching captured pending/ready work deduplicates;
terminal outcomes remain until explicit take. Taking acknowledges an outcome;
reopening/unknown IDs require reconciliation rather than blind repetition.

There are four retained slots, one in-flight job and two single-token Core
CompletionQueue rings. Independent aggregate source/cache-payload budgets are
32 MiB each; every job keeps the established 16 MiB output budget. Metadata,
128-node closures, per-source/per-payload limits and managed cache disk limits
remain bounded separately. Jobs add no disk spool, scheduler or generic executor.
Progress is a monotonic completed/total-node count plus explicit phase.

Only owner drain publishes. It consumes at most one ready completion before
dispatching the next queued job. Original cache/catalog/source/metadata/descriptor
fences are rechecked through the existing atomic writer; late work cannot replace
newer output. A queued job captured before another commit may conflict and needs
explicit reconciliation/retry. Last-valid content remains intact on failure.

Cancellation requests cooperative stop and fences publication even when a trusted
callback ignores stop. Cancellation after publication retains the actual commit
and durability outcome. Closure stops admission, requests stop, joins the worker
and cancels uncommitted work; callbacks must return/cooperate because C++ cannot
be forcibly interrupted. Registered stop callbacks cannot reenter job control.
Errors retain at most 1024 value bytes/field; allocations with capacity over
2048 bytes are released, allowing normal standard-library rounding at the exact
value boundary. Callback exceptions become owned failures. Explicit cache-stage
recovery remains separate from job retry.

Source, sidecar, cache/key and payload versions are unchanged. No source decoder,
import CLI, editor interface, streaming system or new gameplay authority is added.

## Coverage and qualification

The new `shark.tools.asset-import-jobs` family has 25 deterministic cases,
90-second timeout and Catch seed 1337. Gates/condition variables exercise queued,
running, ready and committed cancellation; stop-callback reentry; retained capacity
and deduplication; stale source/settings/catalog/cache; worker/native publication
failures; actual commit durability; explicit recovery/retry; owner/foreign/expired
IDs; shutdown; positive 128-node closure; and independent source/cache budgets.
Diagnostic tests preserve exactly 1024 bytes, reject 1025 bytes and release large
allocations holding short strings. Earlier cases/limits are unchanged.

The eighth native case, `featureworld.asset-jobs`, uses the saved asset UUID and
real dependency through submit/dedup/progress/drain/publication, pre/post-commit
cancellation, retained take, cached no-op retry and closure. Authored metadata and
the existing AuthoredScene remain unchanged. Six new semantic mappings extend the
independent inventory to 40 CPU surfaces plus one external GPU command; eight
native cases and 26 process checks preserve all older saved identities/workloads.

| Current profile | Qualified CTest outcomes |
| --- | ---: |
| Windows MSVC Debug | 113 |
| Windows MSVC Release | 113 |
| Windows MSVC ASan | 113 |
| WSL Ubuntu GCC Debug | 106 (105 + isolated contract retry) |
| WSL Ubuntu Clang ASan/UBSan | 106 |
| Standalone Windows Tools | 22 |
| Standalone WSL Tools | 22 |

The receipt selects 595 passing outcomes: 551 from full-profile inventories
and 44 additional standalone Tools checks. All thirteen public examples compile
with MSVC/GCC/Clang (39 compiles), without private dependency headers.
Actual ASan/UBSan symbols are audited in 26 object files, including the new
job family. Protected Runtime/Server builds and packages pass. The 35-file native
Runtime package launches after Unicode relocation as a standard user with a
system-only PATH. The 12-file WSL Server package passes source/link/ELF closure
and SDK-free network-isolated launches. Successful deployment copies are removed;
the Server copy is archived with verified original paths/hashes.

The receipt resolves exact inventories, JUnit results, process checks, examples,
object audits and package evidence. Linux Debug's full run has one 30.03-second
contract timeout and 105 passes. Its exact isolated contract retry passes in
28.9 seconds at the unchanged 30-second limit. The receipt selects those actual
passes with per-test provenance; the original failed full run remains failed.
Overlapping I/O-heavy qualification is a possible cause, not a proven root cause.
Other final profile runs pass without failures/skips. No limits, host settings or
implementation inputs change for the retry.

Final qualification uses the same 489 frozen inputs and implemented manifest
surface. Earlier session interruptions and the diagnostic-boundary draft failure
remain preserved separately; no partial run is counted. Exact boundary tests
exposed standard-string capacity rounding and reused operation storage; both are
corrected in the final passing freeze.

Core/Client/Server implementation and dependency locks are unchanged. Full Server
suite evidence remains the completed 11b evidence; protected builds/packages are
refreshed here. No new GPU runs are claimed because graphics behavior is unchanged.
This qualifies current-PC offline authoring services, not production MMO capacity,
editor/terminal adapter parity or final REQ-11 acceptance.

## Rerun and next checkpoint

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset tools-debug --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug --show-only
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug -R ^shark.tools. --output-on-failure
```

Reuse canonical build directories, dependency caches and the retained isolated WSL
userspace. Preserve all existing timeouts/seeds, the 18-case catalog/6190 assertions,
22-case import/4361 assertions, 59 interruption checks, 13 base runner cases/29
process checks and all earlier fixtures. Follow storage retention before cleanup.

11d connects identity/dependency/cache/jobs through integrated qualification and
isolated interruption/fault fixtures, then closes full REQ-11. Source decoders
remain 12, terminal/editor adapters 15/16 and final cooked output 31.

Suggested commit: `feat(tools): add bounded background asset import jobs`.
