# ADR 0064: asynchronous cell content and bounded residency

Date: 2026-11-06. Status: accepted for M05b. Extends
[ADR 0063](0063-world-cell-content.md) and follows the
[M05 checkpoints](../roadmap/seamless-world-plan.md) and
[residency contract](../world-cell-residency.md).

## Decision

Keep one creating-thread WorldCellResidency owner, one private reader worker and
four retained job slots. The trusted host supplies a bounded byte-reader callback
and immutable PreparedAssets snapshot per request. Workers may validate owned
development bytes and asset references; they never construct or mutate a Scene.
Explicit owner drain builds a complete LoadedWorldCell candidate and publishes
at most one completion. Status reads cannot publish content.

Allow one pending intent per cell key. Capture the existing resident generation
at admission and check it again before publication. Reject eviction while work
for that key remains pending. These fences prevent obsolete completions from
overwriting or resurrecting content without an unbounded key/tombstone map.
Canceling or failing a replacement preserves the last valid owner. New requests
after a terminal result are explicit retries with new process-local job IDs.

Retain original terminal receipts until explicit take. Cancellation after a
terminal result returns that original result and cannot undo publication. Close
permanently stops admission, requests cooperative stop and joins the reader before
releasing pending data. Residents remain available for explicit eviction/teardown.
Stop requests occur outside the worker mutex; reject API reentry while callbacks
run synchronously during cancellation/close.

Bound pending bytes/assets at admission, resident count and logical reservations
before activation, and simultaneous old/candidate replacement reservations.
The 16 MiB Scene reservation is an explicit accounting policy, not a measurement
of allocator storage or process RSS. Entity/format bounds and a single active
reader separately bound structural work. Trusted callback transient allocations
and native filesystem latency need cooperation; no arbitrary C++ sandbox or
hard real-time shutdown promise is introduced.

## Compatibility and continuation

This is an additive Core API and local file reader. Cell/source/prepared/wire
formats and M05a loaders remain unchanged; no importer, authoring dependency or
new third-party library enters Core/Server. A callback selects exact expected
key/revision content; revision labels do not authenticate files or establish a
newest-content policy. The optional local file helper returns bounded owned bytes
without promising transactional snapshots of concurrent in-place writes.

Borrowed resident pointers remain valid only until replacement, eviction or
owner destruction. Native collision/navigation integration in M05c must add
appropriate pins/leases before those consumers can outlive a content observation.
Content publication alone is not traversal readiness or region authority.
