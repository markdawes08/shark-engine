# Bounded background asset imports

11c extends `shark/tools/asset_import.hpp` under
[ADR 0027](architecture/0027-bounded-asset-jobs.md). It reuses the
[development cache](asset-import-cache.md) and the same ProjectFiles/catalog
owner. This CPU service needs no editor, display or GPU. Public terminal/editor
adapters remain 15/16; source decoders remain 12. Full REQ-11 closes in 11d.

## Ownership and execution

AssetImports and every public operation belong to their creating offline
authoring thread. ProjectFiles, catalog and importer instances must outlive it.
Each importer instance is exclusively borrowed by one live AssetImports service;
do not concurrently use it elsewhere. Trusted callbacks cannot retain views,
mutate project/scene state, reenter the service, or change their descriptor.

`submit` checks current cache/catalog revisions and captures exact metadata and
immutable source FileRevisions on the owner. Owned revisions retain the bytes
behind worker views. Capture invokes no importer callbacks. One lazy private
worker normalizes settings, builds keys, validates reusable payloads and runs
the established import path. It never calls ProjectFiles, catalog or Scene.
The same code serves synchronous offline `prepare`.

All importer callbacks, including discovery, query, explicit reads and publication
validation, are serialized by a per-service mutex. Callback-bearing owner calls
return conflict if the worker currently holds it. Status, cancellation and
admission remain available. Cache maintenance can advance the cache revision
while a job runs; the original job then conflicts on publication.

The worker transfers trivial slot/sequence tokens through the existing bounded
Core CompletionQueue. It retains nontrivial data in owned private slots and catches
exceptions at thread entry. No generic pool, second scheduler, directory watcher
or live-thread importer is introduced.

## Identity, progress and retention

`AssetImportJobId` contains the service's fresh UUID scope and a monotonically
increasing nonzero sequence. Sequence values never rewind after take/cancellation.
Foreign scopes conflict; unknown/taken IDs reject. Reopening creates a new scope;
query the cache/reconcile inputs rather than repeating an old identity blindly.

Matching pending requests deduplicate by a domain-separated SHA-256 fingerprint
of root, target, original catalog/cache scopes and revisions, sorted dependency
IDs, source/sidecar paths and exact source/metadata content digests. A ready job
still counts as pending. This deliberately conservative identity includes path
and metadata formatting; content artifact keys keep their existing normalized,
path-independent semantics. Different captures may ultimately reuse the same
artifact. Cancellation-requested and terminal work do not deduplicate.

`jobs` returns at most four owned statuses ordered by sequence. States are
`queued`, `running`, `ready`, `succeeded`, `failed` and `cancelled`. Progress counts
completed topological nodes against the captured total, including validated reuse;
it is monotonic and never exceeds 128. Ready means computation has ended, with
publication still pending. Success includes the original input summary, root
artifact key and actual commit/revision/durability result. Failure has a structured
Error; cancellation is its own terminal state. Returned status copies are snapshots.

`drain_jobs` consumes at most one ready completion, performs publication through
the existing owner and dispatches the oldest queued job. It returns zero if none
is ready, without waiting. It is the only job publication path. Calling `jobs`
alone never publishes. A failed job is a terminal status, not a failed outer
drain Result. Check the outer Result and each terminal status/commit.

`take_job` requires a terminal job and returns its owned outcome, then frees its
slot. Taking is acknowledgement, not rollback. Terminal outcomes are never
automatically evicted. Capacity exhaustion returns conflict until explicit drain,
cancellation and take free the relevant resources. No durable job journal exists.

## Publication, cancellation and recovery

Every job retains its admission cache/catalog revisions. Before replacement,
the owner rechecks those scopes/revisions, exact source and sidecar bytes,
descriptor/settings and complete cache validity. Changes from a human edit,
another job, reload, prune or clear can make a completion stale. It fails with
conflict and preserves last-valid output. The owner never rebases a queued job
onto a newer revision automatically; reconcile, take and submit again explicitly.

`cancel_job` requests stop without waiting for an importer. Queued work that has
not been dispatched becomes terminal immediately. Dispatched/running/ready work
remains retained until drain or closure. The worker passes `AssetImportInput.stop`
and checks cancellation before each import/reuse node and after `import()`. Even an importer
that ignores stop cannot publish after cancellation. Cancellation after success
returns the succeeded status and its original commit; it cannot undo publication.
Do not interpret timeout or a delayed ready result as rollback.

`close_jobs` permanently closes admission, requests all uncommitted jobs to stop,
joins the worker and marks them cancelled. It is idempotent, preserves succeeded/
failed outcomes for take, and leaves synchronous offline cache operations usable.
Destruction uses the same join-before-release order. Trusted callbacks must bound
their work and return/cooperate; shutdown cannot preempt arbitrary C++. Registered
stop callbacks execute on the thread requesting stop, cannot access owner state
from another thread and cannot reenter job control (conflict). No real-time
shutdown latency is promised. Disconnect/EOF adapter policy remains 15.

Callback exceptions, invalid payloads and publication faults produce terminal
failures without changing last-valid content. Diagnostic fields longer than
1024 bytes, or with retained string capacity over 2048 bytes, are discarded with
their allocated storage (string allocation may round upward). The error code and
fixed worker operation remain. Native cache stage recovery stays explicit through
`AssetImports::recover`; it does not revive a job. After a failed native publication,
recover/reload/reconcile as appropriate, take the old outcome and submit a new job.
A published directory-flush warning remains a successful commit.

## Resource bounds

There are four retained slots, one in-flight job and one request/completion token
per private ring. Each closure keeps the previous 128 nodes, 1 MiB/source,
2 MiB/payload and independent 16 MiB source/result limits. Across retained active
jobs, exact source bytes total at most 32 MiB and copied reusable cache payloads
at most 32 MiB. Each of the four jobs reserves the established 16 MiB result
budget, hence at most 64 MiB retained computed payloads before drain. Capturing a
candidate for admission can temporarily hold one additional bounded source closure;
dedup/rejected admission releases it. Cache payload snapshots are copied only
after aggregate admission succeeds.

Metadata is separately bounded by four times 128 entries: sidecar bytes are at
most 64 KiB each; decoded fields/dependencies follow that schema's fixed limits;
normalized settings are at most 4096 bytes/node. Source and sidecar revisions own
immutable shared bytes; worker views never outlive them. Cache snapshots retain at
most 128 artifacts/bindings and the existing index/payload bounds. Terminalization
releases captured inputs, reusable payloads and computed plans, keeping only a
small bounded status. These are logical data/structural bounds, not measured total
process RSS or a sandbox for trusted native callback transient allocations.

The current cache, one owner publication candidate/encoded snapshot and one
importer's at-most-2-MiB returned payload use the existing independent limits.
Disk remains one committed cache plus one unpublished stage (at most 48 MiB).
Jobs create no additional disk spool or log. Runtime and Server acquire no Tools,
JSON, source import or worker dependencies.

An owner can submit and later drain/query without blocking on import computation:

```cpp
#include <shark/tools/asset_import.hpp>

shark::Result<shark::tools::AssetImportJobAdmission>
start_import(shark::tools::AssetImports& imports, shark::tools::AssetId root,
             const shark::tools::AssetTargetProfile& target,
             shark::tools::AssetCatalogRevision catalog) {
    auto cache = imports.revision();
    if (!cache) return cache.error();
    return imports.submit(root, target, catalog, cache.value());
}

shark::Result<std::vector<shark::tools::AssetImportJobStatus>>
poll_imports(shark::tools::AssetImports& imports) {
    auto drained = imports.drain_jobs();
    if (!drained) return drained.error();
    return imports.jobs(); // Inspect each terminal state, commit and durability.
}
```
