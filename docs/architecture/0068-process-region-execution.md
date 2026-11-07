# ADR 0068: portable region execution in owned Server processes

Date: 2026-11-06. Status: accepted for M05f implementation; qualification is
recorded separately. Extends [ADR 0067](0067-fenced-region-handoffs.md).
Follow the [process contract](../region-process-execution.md),
[durable ownership](../regional-ownership.md) and
[testing policy](../testing-policy.md).

[ADR 0069](0069-connected-world-streaming.md) extends the original fixed-content
profile with a fixed catalog, live residency and lossless frame changes between
executor incarnations. The complete-island ownership boundary below is retained.

## Decision

Move the complete interacting native/gameplay island between actual owned
`Server --region-worker` children. Its logical regions share one fixed frame,
content set and execution order. Every player writer changes to the fresh
executor instance in one SQL transaction, including stationary players. This
profile does not run independently ticking neighboring islands concurrently.

Keep actual player mTLS, the sole SessionAuthority, CharacterAuthority, persistence
worker and interest/replica publication at the gateway. The children have no
player listener or Store. A closed parent-owned binary stdio channel carries
bounded host commands; copied session observations are context, not credentials.
Children execute only requested operations and never advance autonomous ticks.

Add an explicit portable image and fresh native reconstruction. Source handles
are relocation keys; complete checked mappings establish new Scene/body handles.
Carry body settings, poses/previous poses, velocities, queued force, kinematic
targets, active contact pairs and sequencing, alongside original movement,
combat/life/flight state and NPC intents. Rebuild Jolt's internal solver caches;
do not claim serialization of native ABI objects or identical future solver
trajectories. Retain the existing static cell and prepared asset validators.

Transferable combat rules opt into a fresh VM for every evaluation from their
first evaluation. Exactly eight int64 values and the evaluation count persist.
Ordinary mutable rule VMs and entity ScriptRuntime owners are unsupported and
reject, rather than being silently recreated from source.

## Publication and ownership

Use one trusted server-only journal with two published slots: the current
checkpoint and staged transfer. Each has a bounded complete envelope, payload
digest and exact replacement fence. Publish the actual portable image before
ownership commit; each request's `cut_digest` equals SHA256 of those image bytes.
The journal stores all source writers and exact requests. Schema 4 remains
unchanged.

`Store::commit_regions` validates the complete batch before one transaction,
rechecks persisted writers inside it and publishes cached successors only after
COMMIT. Reject partial retained retries, mixed new/replayed cuts, duplicate
identities and later attempts to split a retained source island. The existing
worker uses one bounded control slot and retains the original batch through
completion. Exact latest batch retries remain idempotent.

Checkpoint each acknowledged input admission or complete tick before exposing
its new state/outcomes. A bounded eight-entry post-cut buffer contains original
unadmitted inputs. Normal routing prepares the child first, admits through the
actual gateway session once, then commits the exact input internally. Retain an
uncertain admitted route until a durable watermark covers it or its original
receipt is replayed after recovery; never spend its session sequence twice.

## Recovery and compatibility

A normal transfer requires a successful source freeze, a non-simulating staged
destination and the exact all-writer SQL result before activation. A control
timeout alone grants no authority. Process recovery confirms both owned children
have exited before constructing another incarnation and performing a fresh
ownership CAS. Previously submitted SQL must settle first; storage uncertainty
continues to require the existing explicit durable-owner reconciliation.

Retain gateway sessions, original durable jobs, request identities and recipient
streams across child loss. No automatic SQL replay, account/session restoration,
gateway cold restart or distributed lease service is added. Source/prepared
content, player wire formats and Store schema remain compatible; the portable
image and private child protocol have their own versions and bounded rejection.

M05g retains the complete authored-world connected gate. This ADR introduces no
GPU or manual UI qualification; the existing manual UI pause remains unchanged.
