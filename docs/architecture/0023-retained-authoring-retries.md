# ADR 0023: Scoped retained authoring mutations

Status: accepted for 09d.

Human and terminal callers can lose a reply after an edit or history movement.
Request correlation alone cannot establish whether repeating it is safe. Extend
the existing AuthoredScene owner, command services and shared history with a
bounded in-memory outcome ledger; do not add a second mutable scene or a player
protocol. See [ADR 0008](0008-human-agent-authoring.md),
[ADR 0022](0022-shared-authoring-history.md) and the
[retry contract](../authoring-retries.md).

The owner generates a fresh UUIDv4 scope using Core's existing entropy service.
Callers supply the next consecutive nonzero uint64 sequence. An admitted decision
advances a nonrewinding high-water mark, including terminal validation failures.
Retained identical inputs return the same immutable outcome; mismatched payloads
reject. Expected revision is part of the input. Batch correlation is excluded.
This covers typed edits, atomic batches, undo, redo and explicit history clearing.
Preview is pure and needs no mutation identity. Existing nonretained APIs remain.

Reserve the owned request, outcome and ledger slot before dispatch. Successful
Scene/history publication is followed only by nonthrowing ownership moves and
bounded whole-entry eviction. Count/byte limits evict oldest entries, never the
high-water mark. A request that cannot fit alone rejects without consuming an ID
or evicting prior evidence. The contract publishes conservative retained charges,
not process memory or a claim about external references.

Save, activation and history clearing preserve this owner ledger. Destruction
expires it. A new owner, including one loading identical files, gets a new scope.
Evicted/foreign identities return unknown outcome and require inspection and an
explicit new intent. Neither refreshed revisions nor empty history authorize
reexecution. No timer, durable retry journal, restart replay, network adapter or
new source/protocol format is introduced. File publication and M03 economic
transactions retain their separate owners and guarantees.
