# 11d: Integrated asset qualification

Date: 2026-10-27. **11d and full REQ-11 are PASS under the current-machine scope.**
Base: `6ece37e` on initially clean `main`. Changes remain unstaged/uncommitted;
the user owns staging, commits and pushing. **Next: assignment 12, glTF import.**

Follow the [four completed asset checkpoints](../roadmap/asset-plan.md),
[qualification contract](../asset-qualification.md) and
[receipt](../coverage/evidence/11d-asset-qualification.json), retaining
[identity](../asset-identity.md), [cache](../asset-import-cache.md) and
[job](../asset-import-jobs.md) contracts under ADRs
[0025](../architecture/0025-asset-identity.md),
[0026](../architecture/0026-development-asset-cache.md) and
[0027](../architecture/0027-bounded-asset-jobs.md).

## Connected behavior

The ninth native case, `featureworld.asset-pipeline`, follows ONE persisted scratch
project/scene and its actual saved root AssetId through a two-asset pipeline.
Actual async imports publish a complete root/dependency closure. Coordinated root
source/sidecar movement preserves the ID, saved reference, content key and cache
reuse. A condition-variable gate proves the worker is running while the owner
changes dependency bytes and settings. That captured completion fails its original
fence, preserving exact cache bytes and the last-valid artifact. Explicit take,
catalog reconciliation and a new job publish current content.

Independent expected literals check root and transformed dependency payloads,
dependency IDs/keys, changed root/leaf keys and retained old artifacts. A genuinely
changed ready candidate is cancelled without any cache-byte/revision change;
cancellation after commit reports the committed result. Missing/private dependency
controls preserve last-valid output. Reopen changes owner scopes while retaining
persistent IDs and the saved scene reference. Prune, reload, recover and clear
use the existing owner. The original AuthoredScene and all four saved entity IDs
remain unchanged. This simulates another author's file edits through existing
services; it does not claim an editor UI or terminal protocol implementation.

The new `shark.tools.asset-jobs-process` family has **113 checks, 39 actual child
processes and seven abrupt-exit phases**. It runs real submit/private-worker/owner-
drain work on a client root with a shared dependency. Controlled exits occur in a
running importer, ready-before-drain, and native staged/written/flushed/
before-publish/published phases. An explicit owner signal orders the bounded job
marker before a worker-side exit. `_Exit(77)` skips cleanup; no returned mock
failure substitutes for process interruption.

Fresh processes inspect exact old/new cache bytes, root/dependency payloads and
keys, source/sidecar preservation, stage counts and ordinary-source-stage evidence.
Only four native prepublication phases leave a cache stage; it blocks another async
publication until explicit recovery. Fresh owners reject the original job scope
on cancel/take, reconcile the actual cache, submit a new job and prove idempotent
recovery/reuse/no-op publication. The marker is fixture evidence, not a durable
job journal. Unicode/space-containing roots use actual native UTF-8 conversion.

The existing synchronous modes and 59 interruption checks remain unchanged.
The new family is CPU-only with a 150-second limit, 20-second child deadlines and
a 15-second running gate. No public asset API, source/cache/key/payload version,
runtime dependency or source decoder changes. Core/Client/Server and dependency
locks are unchanged. Existing eight cases and all prior workloads/limits remain.

## Qualification

| Profile | Passing selected CTest outcomes |
| --- | ---: |
| Windows MSVC Debug | 114 |
| Windows MSVC Release | 114 |
| Windows MSVC ASan | 114 |
| WSL Ubuntu GCC Debug | 107 |
| WSL Ubuntu Clang ASan/UBSan | 107 |
| Standalone Windows Tools | 23 |
| Standalone WSL Tools | 23 |

All **602** outcomes pass: 556 covering complete full-profile inventories and 46
additional standalone Tools selections. Full inventories use disjoint functional
and I/O-heavy CTest executions at unchanged limits; a shared local qualification
gate serializes contract validation and the four dependency-fixture families.
Their union is exact, with original per-test JUnit/time/hash provenance. These are
partitioned executions, not a claim of one uninterrupted full CTest invocation.
No failed result is relabeled, test omitted or limit increased.

The receipt verifies the same 490 frozen inputs, implemented manifest surface and
qualification configuration throughout. Earlier family names, labels, timeouts
and WILL_FAIL properties match the completed 11c inventory. Catch retains seed
1337: catalog 18 cases/at least 6190 assertions, imports 22/at least 4361, jobs
25/at least 2957, base runner 13 and FeatureWorld five Catch cases over nine native
cases. FeatureWorld has 27 process checks; all 40 CPU surfaces and the one external
GPU command remain. Nineteen actually exercised asset commands connect to the new
case; fifteen connect to the async process fixture. Earlier mappings remain.

Across all seven selections, **35 synchronous and 49 asynchronous final snapshots**
reproduce exact container bytes and root keys within each fixture; async snapshots also reproduce
dependency keys. All thirteen public examples compile under MSVC/GCC/Clang (39
compiles) without private parser/storage/presentation headers. Actual instrumentation
is verified in 26 ASan/UBSan objects, including the changed FeatureWorld and
process-fixture sources.

Protected Runtime/Server builds and packages pass. The 35-file native Runtime
package launches after Unicode relocation as a standard user with system-only
PATH. The 12-file Server package passes source/link/ELF closure and SDK-free,
network-isolated WSL launches. Managed native deployment copies are removed;
the Server deployment is archived with verified hashes/original paths and its
exact expanded files removed. Canonical profile directories, archive inputs and
retained WSL userspaces are reused. No host, resource or kernel settings change.

Full Server suite evidence remains the completed 11b evidence because all protected
Core/Server source inputs are unchanged; current protected packages are refreshed.
No new GPU qualification is claimed for these CPU-only changes. This completes
local backend asset identity/import/cache/jobs, not glTF decoding, final cooked
containers, adapter parity, world streaming or production MMO capacity.

## Rerun and next assignment

The recorded local qualification used the frozen helpers below. Their exact
commands, input hashes, per-profile selections and original logs are pinned by
the receipt; these generated helpers remain local evidence under `out/`.

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python out/11d-work/native.py
wsl.exe -d Ubuntu -u root --cd / -- bash /mnt/c/Users/mrkda/Projects/shark/out/11d-work/wsl.sh
.tools/venv/Scripts/python.exe out/11d-work/verify_profiles.py
```

For the versioned CPU Tools checks, use:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 cmake --build --preset tools-debug --parallel 2
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug --show-only
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 ctest --preset tools-debug -R ^shark.tools. --output-on-failure
```

Assignment 12 implements the documented bounded glTF mesh/material/texture profile
through this importer/job/cache path and licensed offline fixtures. Plan its
checkpoints before implementation. Human/terminal adapters retain 15/16; world
streaming and final cooked output retain M05/31.

Suggested commit: `test(tools): qualify integrated asset import pipeline`.
