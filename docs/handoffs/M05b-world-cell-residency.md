# M05b: asynchronous cell content and residency

Date: 2026-11-06. Base: `9c5d420`, initially clean `main`. The user owns staging,
commits and pushing. Follow the [residency contract](../world-cell-residency.md),
[ADR 0064](../architecture/0064-world-cell-residency.md),
[M05 checkpoints](../roadmap/seamless-world-plan.md) and
[testing policy](../testing-policy.md).
M05b PASS; full REQ-M05 remains NOT_RUN. M05c is next.

## Delivered

WorldCellResidency keeps one trusted reader worker, four retained job slots and
one creating-thread content owner. Prefetch returns a scoped process-local job;
worker validation never constructs a Scene. Observation stays separate from
explicit drain, which publishes at most one complete LoadedWorldCell candidate.
Exact key/revision, captured assets and resident-generation fences apply. One
pending intent per key prevents stale replacement; eviction rejects pending work.

Queued/ready cancellation cannot publish. Running cancellation requests stop and
keeps resources until the callback returns and the owner drains, or close joins.
Original terminal outcomes remain until take; cancellation after success returns
the original publication generation. Callback failures, malformed content,
cancelled/stale work and resource pressure preserve last-valid residency.
Shutdown joins the reader before releasing captured data or Scene owners.

Limits cover resident count, pending payload/asset reservations, resident logical
reservations and simultaneous old/candidate replacement peaks. Status reports
current/high-water charges and returned-reader byte counts. These are explicit
accounting/structural bounds, not exact allocator or RSS measurements. Trusted
callback transient work and native IO latency require cooperation.

The Core local file adapter reads bounded regular files in 64 KiB chunks
with cancellation checks and complete-read/EOF controls. It imports no source and
promises no transactionally consistent in-place file update. The service validates
the complete cell digest/key/revision/hidden mesh closure before owner activation.

The connected real-file fixture loads far-origin adjacent content, preserves a
live cell across a missing replacement, retries with a new job, prefetches two
cells before explicit one-at-a-time publication, rejects resident pressure and
uses exact generations for eviction. Independent worker tests exercise active
and queued cancellation, synchronous stop-callback reentry, retained slot pressure,
byte/peak limits, malformed results, original receipts and joined shutdown.

## Verification

Select the new residency/file family plus Core/Tools M05a cells and their existing
prepared Scene/assets consumers in Windows Debug and retained WSL Clang
ASan+UBSan. All six unique selected CTests PASS on both profiles. Server/Core
dependency checks PASS. No behavior check failed.

| Profile | Result | Evidence under `out/M05b-work/` |
| --- | --- | --- |
| Windows Debug, final inputs | 6/6 PASS, 0.70 s | `windows-final-build.log`, `windows-inventory.log`, `windows-tests.log`, `windows-tests.xml` |
| WSL Clang ASan+UBSan Debug | 6/6 PASS, 0.99 s | `wsl/build.log`, `wsl/inventory.log`, `wsl/tests.log`, `wsl/tests.xml` |
| WSL final changed fixture | Residency 1/1 PASS, 0.07 s; not an additional unique test | `wsl-final/build.log`, `wsl-final/inventory.log`, `wsl-final/tests.log`, `wsl-final/tests.xml` |

Both build WorldCellResidencyTests, WorldCellTests, WorldCellToolsTests,
PreparedSceneTests, PreparedAssetsTests, PreparedContentTests and Server with two
jobs. Windows uses `tools/windows_dev.ps1` with matching `windows-debug` build/test
presets. WSL reuses `/work/build-05a-final-client/linux-asan-ubsan` in the retained
isolated userspace; leak detection and ASan/UBSan halt-on-error remain enabled.
No host/WSL resource or kernel settings changed. Inventory precedes selection,
with `--output-on-failure --no-tests=error` and JUnit. Exact six-test selection:

```text
^shark\.(core\.(world-cell-residency|world-cell|prepared-scene|prepared-assets)|tools\.(world-cell|prepared-content))$
```

The new family contains nine controlled worker/owner cases, three real-file input
cases and one connected real-file streaming case, with seed 1337 and a 60-second
CTest timeout. Gate/observation watchdogs are five seconds; there are no arbitrary
sleeps. Successful managed file scratch is removed. The connected case enforces
three residents, exact returned-file byte totals, zero released pending/job charge,
current/peak budget compliance and generation-fenced eviction/retry. Unit cases
independently check exact reservation and simultaneous old/new peak charges;
these are logical accounting observations, not RSS or native-memory qualification.

Initial compile failures are preserved. Both compilers found internal helper
`take` hidden by the public member (`windows-build.log`, `wsl-initial-build.log`);
it is now `require_value`. MSVC then rejected an unreachable statement after
Catch's fatal assertion in the fixture timeout path (`windows-build-corrected.log`).
That timeout now throws one diagnostic exception; final Windows compilation and
all selected behavior pass. WSL rebuilt/reran only the changed residency family;
the other five WSL results remain valid on unchanged relevant inputs. The same
fixture timeout and successful behavior are preserved; no tolerance/workload
changed to obtain a pass. Review also fixed queued cancellation before execution:
it withdraws dispatch under the mutex before requesting stop outside that mutex.

Formatting, whitespace, manifest/links and repository/ignore checks PASS. No
component, Lua API, terminal command, dependency or public example changed.
M05a's unchanged coordinate-family result is reused from `9c5d420`; this increment
changes neither its conversion helpers nor the synchronous cell format/loader.
No manual UI, native GPU, Release/package or whole-project matrix was selected
for this CPU content increment.

## Continuation

M05c is next: static-world collision and tiled navigation, explicit readiness and
bounded NPC pathfinding across cell edges. Native consumers must pin or lease
resident content before retaining it across publication/eviction; borrowed const
cell pointers do not extend ownership. Scene construction/destruction stays on
the creating thread. M05b publication alone does not make cells traversable or
grant region/player/native authority.

Later M05 checkpoints retain seamless gameplay, process transfer, fencing and
recovery. Full REQ-M05 remains NOT_RUN. Independent manual UI/backlog statuses are
unchanged. Existing `.gitignore` rules cover all build/output/cache and local
roadmap artifacts; real-file test scratch is unique and scoped to the CTest build
directory. No new ignore rule is needed. README is unchanged.
