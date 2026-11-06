# Region process execution

M05f adds trusted local process execution under
[ADR 0068](architecture/0068-process-region-execution.md). It extends
[gameplay cuts](gameplay-transfer-cuts.md) and
[durable ownership](regional-ownership.md). Verification belongs in the handoff;
this contract does not assert a test result.

## Owners and supported island

The network host keeps the actual player mTLS connections alive across child
transfers. RegionGateway borrows fresh live transports only for each call; it
retains CharacterAuthority, original durable operations and shared interest/replica
publication. CharacterAuthority keeps its sole SessionAuthority and Store worker.
One actual `Server --region-worker` child executes
the complete interacting Scene, PhysicsWorld, traversal, movement, combat and
NPC state. Two to eight logical regional rectangles remain distinct writer
domains inside that island; they are not concurrent independent simulations.
Transfer moves all player writers to the successor instance atomically, including
players whose position stays in the same logical region.

An executor keeps one explicit fixed frame, fixed hierarchy and content set.
Checked combat death/respawn can change its managed body lifetime. General
authoring, entity scripts, arbitrary spawning and live content replacement are
outside this process profile. Existing actor, FIFO, history, flight, resident
cell and prepared-asset bounds still apply.

RegionProcess owns a closed child channel. Launch supplies only the worker mode
and trusted startup arguments. The child has no database, player listener,
authoring importer or autonomous tick loop. Parent-only inherited stdin/stdout
carry uint32 big-endian lengths and at most 16 MiB per complete frame. Windows
uses private overlapped pipe ends, restricted handle inheritance, a hidden process
and a kill-on-close job; Linux uses owned pipes, a process group and parent-death
termination. RegionProcess inherits the caller's diagnostic stderr separately
from binary replies, including process-exit sanitizer diagnostics. The lower
RegionChild helper defaults to discarding stderr unless explicitly configured.

Each exchange has one outstanding request and a deadline. A failed or lost reply
poisons the channel; another command cannot reinterpret late bytes as a new reply.
`terminate` succeeds only after actual process exit is observed. Timeout and a
copied PID are not an exit receipt. EOF tears down the child's native owners.
Normal retirement checks the final cached exit status is zero after the cleanup
reply; that reply alone cannot hide a later process-exit failure. Intentional
hard termination confirms exit without treating its signal/nonzero status as a
successful normal retirement.
Normal retirement closes native owners through an acknowledged control command
and confirms exit. Poisoned streams require a hard exit fence instead. A bounded
StoreId lease prevents two gateways in this host from using the same live store.

## Portable image and native reconstruction

`RegionProcessImage` version 1 is a bounded engine-owned encoding, at most
16 MiB minus 4 KiB reserved for its control envelope. It includes the fixed frame,
regional definitions, Scene values, complete physics transfer state, movement and
combat cuts, portable rules, cell/asset bytes, NPC intents, peer observations,
writers, executor instance and route watermark. Unknown versions, malformed
values, incomplete mappings and unsupported script state reject.

Build destination Scene/native/traversal owners without simulation. Source
EntityReferences and body handles are lookup keys, never destination authority.
Checked complete maps relocate actors, incoming/outgoing flights and contact
participants onto fresh native generations. Body settings, source/previous poses,
linear/angular velocity, pending force, kinematic targets, active contact pairs
and contact sequencing are explicit owned values. Movement FIFOs and accepted/
applied watermarks, life/health/cooldown/deadlines, original flight decisions and
history continue without another bind, launch decision or input admission.

Jolt's internal solver caches are rebuilt, not serialized. Restoring explicit
state is separate from reproducing a previous native allocation or claiming
bit-identical future solver trajectories. The installed executor remains frozen
until its gateway supplies the exact committed writer context.

Portable combat rules use `create_portable` from their first evaluation. Each
evaluation loads a fresh value-only VM; its sixth `on_combat` argument carries
exactly eight int64 slots. Successful decisions publish those slots; snapshots
also retain source/configuration and evaluation count. Ordinary mutable rule VMs
cannot be converted by capture, and entity ScriptRuntime state rejects. Loading
source alone is not a substitute for preserving an arbitrary Lua VM.

## Input admission and original receipts

An active route first reserves its immutable values and asks the child to prepare
the exact command. Preparation reaches the existing lower-owner admission boundary
without applying the command. The gateway then admits through its actual mTLS
SessionAuthority once and commits the same bytes with before/after peer context.
The child's trusted host adapter consumes that receipt; session observations on
their own cannot authorize a player.

During a cut, at most eight original movement/combat intents may be buffered.
`buffered` means no player command sequence has been spent. Explicit drain
revalidates the actual current session and processes the immutable originals in
FIFO order. Full capacity rejects before admission. Session commands, route
ordinals, input/action IDs and authoritative ticks remain separate counters.

After actual admission, an uncertain child completion retains its original
input, route ordinal and before/after session observations. No newer route or
tick proceeds through that unresolved gap. Recovery replays only original
admitted receipts above the durable image's route watermark, without another
SessionAuthority admission. Already covered receipts are not executed again.
An admitted movement input can still be pending in its captured FIFO; admission
does not claim that its movement has already been applied.

## Journal and atomic writer commit

RegionJournal holds one exclusive creating-thread directory lock. It owns exactly
two published slots, `checkpoint.sharkregion` and `staged.sharkregion`, with two
fixed private unpublished stages. Each record binds StoreId, WorldId, incarnation,
tick, route watermark, complete owner list, optional complete cut batch and image
payload. Payloads are at most 16 MiB; the private portable-image/frame limits can
be smaller. Readers validate the complete envelope and actual payload digests.
The host supplies an existing managed journal directory. Store/world metadata is
captured during gateway startup; live checkpoints use that immutable identity
without invoking blocking CharacterAuthority maintenance while jobs are pending.

Publishing is synchronous trusted coordinator work outside native ticks. Null
expected digest is create-only; replacement requires the exact observed envelope
digest. File data is flushed before native rename. The returned FilePublication
is the original publication/durability observation. Inspecting matching bytes
after a lost reply does not recover that receipt. Unpublished stages are inert
and never promoted; a later publication can discard only its own verified fixed
stage. A durability limitation does not mean that publication rolled back.

The gateway journals the final portable bytes before submitting SQL. Every cut
request binds SHA256 of those actual image bytes, its original source writer,
destination instance, final tick/input watermarks and shared session watermark.
The journal separately records exact request context. Its existence alone grants
no execution rights.

`commit_regions` accepts one through 32 writers with a common image digest,
final tick and destination instance. It rejects duplicate characters/transfer IDs,
mixed new/replayed cuts, incomplete latest-batch retries and fresh attempts to
split a retained source group. All persisted writers are checked inside one SQL
transaction before any writer updates. All successors and exact last-cut records
commit together. One retained worker control slot covers the batch; overlapping
account controls conflict while original gameplay jobs may remain retained.
Schema 4 and economic OperationId semantics remain unchanged.

Every accepted route image and complete tick is checkpointed before its new
publication state, correction or outcomes become visible. A failure retains the
gateway's last known complete image; a published-but-uncertain file requires
inspection before selecting recovery state. Original durable
jobs stay on their existing worker; a child restart never recreates their tickets
or rewrites their save/economic requests.

## Handoff, loss and restart

A normal handoff freezes the source, records a complete cut and constructs a
non-simulating destination. Only the exact all-writer SQL successor receipts
permit activation. The gateway checkpoints the installed native state and
retires the original source. The source's successful freeze acknowledgement is
required; a partition or elapsed deadline cannot be treated as that acknowledgement.
Gateway stats report the actual steady-clock quiescence interval and whether it
exceeds M05e's 500 ms provisional budget. Exceeding it never permits timeout abort
or an unsafe second writer; functional correctness alone makes no latency claim.

Recovery first confirms actual exit of both owned children. Any original SQL
ticket must settle; uncertain storage requires the existing explicit durable-owner
reconciliation. The gateway selects complete journaled state and actual current
writers, stages a fresh RegionInstanceId and commits a new complete writer batch
before execution. It never retargets an old immutable request or revives an old
writer because a control exchange timed out.

This is recovery from source/destination child loss while the actual gateway
owners remain alive. It does not implement gateway-session restoration, automatic
SQL-fault recovery, activation of stale backups or a distributed lease. The
existing durable recovery API can invalidate sessions/runtime tickets and therefore
requires explicit reconciliation outside this gateway's continuity claim.

The manual UI pause is unchanged. Full REQ-M05 remains NOT_RUN until M05g's
complete connected authored-world gate; process implementation and CPU checks
do not replace its remaining acceptance.
