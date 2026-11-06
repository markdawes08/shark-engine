# Asynchronous static cell residency

M05b extends [static cell content](world-cells.md) under
[ADR 0064](architecture/0064-world-cell-residency.md). Follow the
[M05 checkpoints](roadmap/seamless-world-plan.md) for native readiness, navigation,
gameplay and authority transfer. This service is shared Core runtime content
infrastructure; it exposes no player, terminal authoring or editor operation.

## Request, observation and publication

`WorldCellResidency::create(reader, limits)` owns one private worker and four job
slots. All public methods and final destruction belong to its creating thread.
The trusted WorldCellReader runs serially on the worker with an exact cell key,
positive content revision, maximum byte count and cooperative stop token. It
returns owned bytes; it cannot borrow/mutate a Scene or reenter the residency
owner. Core validates the complete cell, exact requested key/revision and every
mesh reference, including hidden meshes, against the captured immutable assets.

`submit` reserves pending data before dispatch and captures the current resident
generation (or absence). Only one queued/running/ready request per key is allowed;
conflicting requests reject. No deduplication silently substitutes a source or
asset snapshot. Content revisions need not increase: a trusted host may select an
older revision explicitly, but a returned different revision always rejects.

`jobs` observes at most four statuses ordered by job sequence. States are queued,
running, ready, published, failed and cancelled. Ready work has finished reading,
with owner publication still pending. `drain` handles at most one oldest ready
completion without waiting for running IO. It checks the original target/cancel
fences, constructs a fresh LoadedWorldCell on the owner thread and replaces the
resident only after complete validation and budget admission. A terminal failure
is recorded in job status; an outer successful drain means the completion was
handled, not that activation succeeded. Failed candidates retain the exact old
resident incarnation, generation and checked references.

`residents` returns a bounded owned inventory. `resident(key)` returns a borrowed
const cell pointer or an invalid_id error for absence. That pointer does not extend
residency and expires on replacement, eviction or destruction. Each successful
publication receives a fresh nonzero per-owner generation. Job success keeps the
original generation as a historical receipt, not permission to mutate a newer
resident. `evict(key, expected_generation)` requires the current exact generation
and no pending work for that key. There is no automatic eviction or hidden retry.

## Cancellation, acknowledgement and shutdown

`cancel(id)` terminalizes queued/ready work without publication. Running work
records cancellation and receives stop; it remains retained until the reader
returns and the owner drains, or close joins it. A reader that ignores stop still
cannot publish after cancellation. Cancellation after any terminal result returns
the original result, including a published generation or failure diagnostic.

`take(id)` acknowledges a terminal outcome and releases its slot. Status queries
and unrelated publication do not discard outcomes. Four retained terminal jobs
therefore apply backpressure until explicit take. An acknowledged/foreign job ID
cannot be reused; scope/sequence values are process-local and grant no world,
session, storage or region authority.

`close` permanently stops admission, requests cancellation, joins the worker and
terminalizes unfinished jobs. It is idempotent and preserves original completed
receipts and residents. Query, take and explicit eviction remain usable; drain
after close has no pending work. Destruction closes before releasing callbacks,
assets or Scene owners. Stop callbacks can execute synchronously on the requester;
stop is requested outside internal locks and service reentry rejects. Trusted
readers must bound work and return/cooperate; native IO cannot be preempted by a
stop token and no hard shutdown latency is promised.

## Resource and IO budgets

| Resource | Default / hard bound |
| --- | --- |
| Jobs / worker reads in flight | Four retained jobs / one serial callback |
| Resident cell count | Default eight; configurable positive up to eight |
| Pending reservation | Default and hard maximum 160 MiB |
| Resident reservation | Default 128 MiB; configurable positive up to 256 MiB |
| Returned payload | At most the existing 4 MiB + 128-byte cell envelope cap; excessive vector capacity also rejects |
| Scene structure | Existing per-cell 4,096-entity and 256-depth profile |

Each pending request reserves the complete payload cap plus its captured
PreparedAssets::retained_bytes(), even when snapshots are shared or the file is
small. Each resident is charged its encoded payload size plus a 16 MiB Scene
reservation plus its assets' retained byte charge. Shared asset snapshots are
charged independently per retained owner, conservatively. Replacement must fit
old plus complete candidate charges before Scene creation; it cannot temporarily
hide the old owner by subtracting its charge first. Count allows only the one
temporary replacement candidate beyond the resident count. Rejections preserve
old content; a host explicitly evicts another permitted cell or retries later.

These are logical reservations and structural bounds, not an exact allocation,
peak RSS, GPU memory or native-physics budget. Worker decoding and one owner
activation can temporarily retain multiple bounded value candidates under the
existing codec limits; caller-held asset snapshots and trusted callback transient
work are outside service retention accounting. M05c must account native resource
ownership before allowing collision/nav consumers to survive eviction. M05g/M10
retain connected workload/resident-memory measurements.

`usage` reports current/peak pending and resident reservations, resident/job
counts, closed state and reader-returned byte counts. bytes_read counts returned
callback payload lengths, including rejected results; it is not physical disk IO
and excludes bytes read before a failed callback returned an error. Metrics never
authorize activation or override an error. Callback errors retain bounded owned
diagnostics; excess string sizes/capacities are discarded before retention.

`read_world_cell_file(path, maximum_bytes, stop)` is an optional trusted-host
adapter. It validates a bounded Unicode path and regular file, checks size before
allocating and reads at most that size in 64 KiB chunks with stop checks. An EOF
probe may inspect one additional byte to detect growth. Truncation/read failure,
growth, oversize and cancellation reject. Filesystem calls themselves can block;
concurrent in-place changes are not a transactional snapshot. A complete cell
digest, key/revision and asset closure still require Core validation. Paths never
become public player capabilities, and the helper imports no source content.
