# ADR 0027: Bounded private asset import jobs

Date: 2026-10-26. Status: accepted for 11c. Extends
[ADR 0026](0026-development-asset-cache.md) through the existing AssetImports
owner and [asset checkpoints](../roadmap/asset-plan.md).

One lazy private worker executes the established normalization, key construction,
reuse, import and payload validation path. The owner captures exact immutable
source/metadata values, catalog/cache revisions and a bounded cache snapshot;
the worker never calls ProjectFiles, the catalog or a Scene. It receives no
writer or live simulation/render state. The synchronous offline API uses the
same preparation logic. A per-service mutex serializes every importer callback;
callback-bearing owner calls try-lock and return conflict while busy.
Importer instances are exclusively borrowed by one live service and must outlive
it. No thread-safety of arbitrary native importer implementations is assumed.

Four retained slots include queued, running, ready and terminal jobs. At most one
job is in flight. Existing Core CompletionQueue rings carry trivial slot/sequence
tokens; owned bounded inputs/results remain in private slots. This is a focused
asset worker, not another scheduler or a generic executor. The owner drains a
completion before dispatching the next queued job, keeping publication and all
callback validation serialized. A drain never waits for import computation.

Job identities use the service UUID scope and a nonrewinding sequence. Exact
captured pending work shares a job, including ready work awaiting drain. Terminal
outcomes remain until explicit take; no automatic eviction loses an unseen result.
Retry/recovery is explicit and gets a new identity. There is no durable job log
or promise to recover an in-memory identity across service/process restart.

Progress is a bounded node count and phase, not arbitrary callback text. Source,
cached payload, result and metadata limits are independent. Cancellation requests
cooperative stop and always fences owner publication. An ignored stop cannot
publish; a completed commit remains a commit, including durability limitations.
Shutdown closes admission, requests stop, joins, then invalidates uncommitted
results. C++ callbacks cannot be forcibly interrupted; trusted bounded callbacks
must return or cooperate. Stop callbacks cannot reenter job control.

Publication reuses all existing source/metadata/catalog/cache/descriptor fences
and atomic snapshot replacement. A queued job captured before a different commit
may fail its original revision; it is never silently rebased. Last-valid content
survives cancellation, callback failures and stale completions. Native unpublished
stages still require explicit recovery. Source/cache formats and key/payload
versions do not change because execution context is absent from content identity.

The [job contract](../asset-import-jobs.md) specifies limits, terminal outcomes
and the CPU API for both later adapters. Source decoders remain 12; terminal and
editor adapters remain 15/16; full REQ-11 closes in 11d.
