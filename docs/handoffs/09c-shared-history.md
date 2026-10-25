# 09c: Shared undo and redo

Date: 2026-10-25. **09c is PASS under the current-machine scope.**
Assignment: the third of the [five authoring checkpoints](../roadmap/authoring-plan.md).
Base: `21fd4b325a69663f4746819ae83756cc09ec0429` on initially clean `main`.
Result: uncommitted working tree; staging, commits and pushing belong to the user.
Final [qualification receipt](../coverage/evidence/09c-shared-history.json): **PASS**.
Full REQ-09 remains **NOT_RUN**; 09d and 09e remain. **Next: 09d, Safe retries and lost replies.**

## Behavior and contracts

[ADR 0022](../architecture/0022-shared-authoring-history.md) and the
[history contract](../authoring-history.md) extend the existing AuthoredScene
owner. Each changed single command or atomic batch forms one history group.
Empty batches, noops, preview and rejected requests preserve both directions,
charged bytes and the next ticket. A changed batch whose edits cancel to equal
source values still forms a group. Grouping across separate calls is unsupported.

`history(expected_revision)` reports bounded counts, charged bytes, configured
limits, synchronization and optional next undo/redo groups. Each group has an
owner-local monotonic uint64 ticket and command count. `undo` and `redo` require
both the next ticket and current SceneRevision. Check both Result levels; the
reply includes the requested ticket and observed live revision on admitted errors.
Tickets are never reused by an owner, including after clear or activation.

Defaults are 32 entries and a 32 MiB conservative retained snapshot charge, with
hard maxima of 32 entries and 64 MiB. Both directions count toward these limits.
Only successful changed publication truncates redo and evicts oldest entries.
A group too large to fit alone rejects before mutation. Charge includes both
source document/vector/string capacities and replacement IDs; shared snapshots
are charged again per entry. This is neither allocator overhead nor process RSS.

Entries retain shared immutable before/after SceneDocuments and IDs replaced
within their group. Undo/redo build a private Core candidate, detach deepest
first, reconstruct through Core validation and restore source order, hierarchy,
component values and exact extras. Surviving incarnations keep handles; removed
or replaced incarnations receive fresh generations. Stale handles never revive.
The sorted refresh-ID union conservatively includes all current/target source
IDs, bounded to 8192. Internal child iteration order is not a source guarantee.

Candidate, metadata and result allocations precede publication. Core's private
force-revision option publishes each history movement once, including an unchanged
source result. The Scene object/instance and saved baseline remain. History moves
invalidate query cursors and conservatively mark dirty even when values equal a
prior save. Saving retains history; rejected saves/reloads retain it. Successful
activation/reload replaces the instance and clears both directions.

Out-of-band Core changes with retained history mark it unsynchronized. Commands,
preview and history movement then reject. `clear_history(current_revision)` is
explicit reconciliation: it clears both directions without changing live values,
handles, revision, dirty baseline or cursors. Owner/visit/pending/closed guards
remain. Gameplay continues to use Core safe mutation independently of Tools.

The revision-bound snapshot cache is an immutable derived value. Single subtree
deletion prepares a removal projection and history storage before reusing the
original atomic Core deletion. Other edits/batches use the existing private
candidate. There is one authoritative mutable Scene, and no public mutable port.

Internal capture reuses canonical extras already validated at activation, fixed
empty extras for new entities and Core-validated components/hierarchy. It rechecks
the aggregate extras budget. Public capture and caller-owned source validation
retain their full codec path. Extend capture, restoration and Core candidate
copying together when persisted components or metadata mutation land.

The optional fourth `create` argument selects history limits; existing calls
retain source compatibility. Rebuild C++ consumers. No source, component, wire,
database or authoring-protocol version changes; no new dependency or adapter.

## Changes and coverage

- `authoring_history.hpp`, AuthoredScene headers/implementation and private fixture
  access implement discovery, movement, bounds, reconciliation and phase faults.
- Core's private edit port adds immediate guard reuse and forced publication;
  public Core mutation and default batch publication retain their behavior.
- `tests/authoring_command_tests.cpp` adds 18 history cases. Original 24 command
  and 17 batch cases remain. `core_scene_edit_tests.cpp` adds forced publication,
  bringing that family to seven cases.
- `shark.tools.authoring-history` uses 60 seconds and seed 1337. Original batch/
  command 60-second and Core scene-edit 30-second limits remain unchanged.
- CPU cases cover alternating callers, grouped replay, source ordering/extras,
  hierarchy/world values, stale generations, net-neutral groups, redo invalidation,
  revisions/tickets, explicit reconciliation, save/reload, entry/byte bounds,
  lifecycle/thread/visit/pending guards, cursors, allocation rollback, maximum
  source size and aggregate extras overflow.
- Coverage maps the history feature and inspect/undo/redo/clear C++ services to
  real fixtures. FeatureWorld/AuthoringCommands attaches in 10. Terminal/editor
  adapters remain 15/16 and Lua retains its owner. No bootstrap editor UI is added.

## Final qualification

The final receipt validates **354 unchanged source/contract inputs and 16 frozen
helpers**. Freeze SHA-256: `9b7f2431cd9e7381b965f7ab4ea44b11aa1c7172a595cf3f1c976a49a2338881`.
All final full suites have zero failures/skips. Earlier attempts do not contribute
to the following totals.

| Profile | Full CPU CTests |
| --- | ---: |
| Windows Debug | 98 |
| Windows Release | 98 |
| Windows ASan | 98 |
| WSL Client Debug | 91 |
| WSL Client ASan/UBSan | 91 |
| WSL Server Debug | 67 |
| WSL Server ASan/UBSan | 67 |
| **Total** | **610** |

Standalone CPU Tools registers 63 tests; all eight authoring families pass on
both OSes: **16 additional checks**. All six public examples compile on
MSVC/GCC/Clang: **18 compiles** with public headers and configured GLM only.
Native sanitizer auditing checks 12 actual objects. WSL retains/audits 13 client
and two Server objects for ASan/UBSan symbols and exact compile/object hashes.

Protected native/WSL Runtime and Server builds, dependency closures and packages
pass. Native Runtime relocates outside the checkout through a Unicode/space path
with system-only PATH; all 33 files and ten missing-license rejections pass.
Server packages contain the exact 12 files. The installed WSL Server runs in the
retained SDK-free 92-package Ubuntu userspace with isolated network and no compiler
or development tools; all 12 checksums match the installed package. These are
current-PC/WSL CPU/package results, not production MMO capacity or other hardware
qualification. No new GPU gate is introduced.

Formatting, contract/repository self-tests, diff checks and an unchanged empty
index pass. Generated evidence/helpers remain ignored under `out/09c-work`;
normal builds and CI do not depend on these local helpers or the ignored roadmap.
The receipt retains commands, toolchains, inventories, JUnit/Catch reports,
process receipts, packages, failed preparatory attempts and artifact hashes.

## Investigations retained

An initial full-clone single-delete history path exceeded the existing 60-second
AuthoredScene gate. The original maximum-size fixture still creates 4096 entities
and performs 4095 individual deletions. The prepared removal projection restored
that workload without changing its timeout; a corrected Debug run took 9.17 s.

WSL preflight r1 exposed `std::bad_alloc` termination in pinned JSON DOM destruction
while history reparsed immutable extras. The retained backtrace resolves through
`json_value::destroy`, ExtrasBudget parsing, source validation and before_snapshot.
History now reuses already validated metadata plus Core validation and the total
extras bound. The vendor library, public codec and original 512-allocation sweep
limit are unchanged. Corrected Windows phase-fault and Linux ordinary/nothrow
allocation sweeps pass. Vendor/runtime memory exhaustion outside these boundaries
is not a recoverability guarantee.

The first frozen native run passed 95/98 tests and timed out in graph boundary,
SQLite dependency and source-document dependency checks. Reduced agent build
concurrency alone did not eliminate the two dependency timeouts. The user then
reported a resource-heavy game was running and closed it. Three speculative
setup optimizations were preserved and fully reverted before final qualification.
No such changes remain in production or tests. All 354 source/doc inputs then
matched the first freeze. Original game-off preflight passed those checks in
1.72/183.41/191.45 seconds under unchanged 10/240/240-second limits. Final full
suites pass as recorded above. Diagnostic timing/location comparisons during
changing load are confounded and are not controlled benchmarks. No host/WSL
resource settings or drivers were changed.

The first freeze/helpers and failed runs remain under `preflight/`. A bare
exploratory CTest selected a Linux runner against Windows inventory and could
not launch tests; recorded native qualification uses the pinned Windows wrapper.
An early receipt invocation rejected an unfinished preclose check; the validated
receipt was generated only after every required check completed.

## Resume with 09d

Read AGENTS.md, this handoff, ADR 0022, the history/command/transaction contracts
and the authoring checkpoint plan. Add bounded retained mutation outcomes and
payload matching through this same owner. Mutation identity is separate from
batch correlation IDs and history tickets. Cover lost replies, identical retries,
changed payloads, retention pressure, expiry/restart and reconciliation for edits,
batches and history changes. Do not blindly repeat an unknown outcome or reuse
activation as a command/history shortcut. Preserve all existing fixtures, workload
bounds and both Result levels. Keep actual terminal/editor integration in 15/16.

Suggested commit: `feat(authoring): add shared undo and redo`.
