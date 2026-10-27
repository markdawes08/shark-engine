# 11: Asset database, import jobs and cache checkpoints

Date: 2026-10-25. Base: `7b68135` on initially clean `main`. Owner: REQ-11.
The user approved the following four-checkpoint split. Full REQ-10 is complete.
11a implements identity/metadata/catalog only; full REQ-11 remains NOT_RUN until
the later import/cache/job gates pass. **11a is PASS** in its
[handoff](../handoffs/11a-asset-identity.md) and
[receipt](../coverage/evidence/11a-asset-identity.json). **11b is PASS** in its
[handoff](../handoffs/11b-asset-import-cache.md) and
[receipt](../coverage/evidence/11b-asset-import-cache.json). **11c is PASS** in its
[handoff](../handoffs/11c-asset-import-jobs.md) and
[receipt](../coverage/evidence/11c-asset-import-jobs.json). Next: **11d**. Follow [ADR 0025](../architecture/0025-asset-identity.md),
the [asset identity contract](../asset-identity.md), [MMORPG plan](mmorpg-plan.md)
and [human/agent plan](human-agent-plan.md).

## 11a - Persistent identity and metadata

Implement UUIDv4 asset identity, versioned adjacent source sidecars, provenance/
license, declared importer/settings, dependency IDs and client/server/shared
audience. Extend ProjectFiles' existing bounded native reads, revision checks and
atomic publication. Derive a bounded revisioned catalog from an explicit complete
sidecar inventory; preserve IDs/references when source and sidecar move together.
Define duplicate, missing-source, unknown-ID, stale-query and owner behavior.

Gate: canonical codec and exact version rejection, malformed/oversized metadata,
rename/move/reopen, duplicate IDs/paths, case collisions, native alias/escape
rejection, conflicting writes, injected publication failures, bounded inventory/
pages and all-or-nothing refresh. Extend actual FeatureWorld cases, independent
coverage inventory and mappings together. Preserve the earlier workloads and add
`shark.tools.asset-catalog` (60 seconds, Catch seed 1337). CPU Tools stays independent
of presentation, SQLite and source decoders. No directory watcher or multi-file
rename transaction is claimed; coordinated moves are observed on refresh.

## 11b - Dependencies and reproducible development cache

Add the focused importer interface and a complete synchronous, deterministic
import/cache path. Define source/settings/tool/dependency/target keys, dependency
closure/invalidation, supported test importer discovery and validated atomic
cache publication. Preserve the last valid output with explicit stale/missing
status. Reuse ProjectFiles/path and bounded value-completion foundations rather
than adding a broad plugin or generic scheduler framework. Cache data is
disposable; define reuse/cleanup and disk limits before populating it. Preserve
metadata needed for separate client/server cook roots and later cell streaming.

Gate: source/settings/tool/dependency/profile changes invalidate exactly the
required output, missing/cyclic dependencies diagnose, matching inputs reproduce
bytes and failed/interrupted publication preserves valid entries. glTF/image
decoders remain 12; the final cooked format remains 31.

**Status: PASS under current-machine scope.** The [cache contract](../asset-import-cache.md)
and [ADR 0026](../architecture/0026-development-asset-cache.md) define the actual
synchronous byte importer, revision-fenced preparation/publication, target/privacy
keys, bounded last-valid cache and explicit maintenance. Its handoff/receipt above
record 724 selected CPU/Tools outcomes, 36 public-example compiles, 24 actual
sanitizer objects and protected packages. Background work/cancellation remain 11c;
full REQ-11 remains NOT_RUN until 11d.

## 11c - Bounded background jobs

Move the established import path onto bounded private workers. Add stable job
identity, queue/concurrency/resource bounds, progress/diagnostics, matching-work
deduplication, cancellation and terminal outcomes. Recheck source/settings and
owner revisions before publication; late/cancelled completions cannot replace
newer output. Importers never run on live simulation/render threads.

Gate: bounded admission, deduplication, cancellation before/after publication,
worker errors, stale completions and explicit retry/recovery. Shared CPU services
prepare both audiences' later adapters; the full terminal/editor interfaces keep
their owners in 15/16.

**Status: PASS under current-machine scope.** The [job contract](../asset-import-jobs.md)
and [ADR 0027](../architecture/0027-bounded-asset-jobs.md) define four retained
slots, one private worker, bounded immutable captures, pending-work deduplication,
progress, cancellation, explicit take/retry and original-revision publication.
The handoff/receipt above record 595 passing profile results, 39 public-example
compiles, 26 actual sanitizer objects and protected packages. All earlier
workloads/limits remain. Full REQ-11 remains NOT_RUN until 11d.

## 11d - Integrated qualification

Connect actual identity/dependency/cache/job behavior through FeatureWorld and
isolated interruption/fault fixtures. Refresh relevant Windows/WSL CPU/sanitizer
regressions, public examples and protected dependency/package gates. Preserve
fixed seeds/limits and earlier cases. Full REQ-11 passes only after all four gates.
Reuse canonical build directories/archive caches; clean successful temporary
deployments and retain compact receipts and useful failure evidence.
