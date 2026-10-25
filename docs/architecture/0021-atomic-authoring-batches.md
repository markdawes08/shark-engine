# ADR 0021: Atomic authoring batches and pure preview

Date: 2026-10-25. Status: accepted implementation decision for 09b.

## Decision

Extend AuthoredScene with bounded ordered AuthoringBatch inputs and pure preview.
Reuse the five existing typed commands and their Core validation/mutation path.
A batch carries a typed correlation UUID and a mandatory live SceneRevision.
The [transaction contract](../authoring-transactions.md) defines the exact limits,
result/error semantics and compatibility. No source, component, wire or database
format changes; this is an additive C++ API. Single-command behavior is preserved.

Core supplies an internal candidate/publication port, hidden from supported
public callers. Candidate copying retains Scene instance, slot generations,
identity maps, cached hierarchy/transforms and deferred ticket sequencing.
Core/Tools share only engine-owned internal declarations; EnTT remains private
inside scene.cpp. EngineTools alone owns public batch orchestration and results.
The candidate is temporary nonauthoritative state, never a second editing owner.
No registry, candidate handle or staged cursor escapes through authoring APIs.

Each command validates in order against that candidate. The complete refresh-ID
union and reply payload are allocated before commit. Commit rechecks the original
owner, live revision and immediate mutation phase, then swaps private state in
the existing Scene object and advances its revision once. No allocation or user
callback follows publication. Any earlier rejection/allocation failure destroys
the candidate; complete live state and dirty/save baselines remain unchanged.

No-op means every operation was a Core no-op, including an empty batch. Editing
then restoring a value still counts as one changed transaction. Invalid temporary
states reject rather than relying on later commands to fix them. This choice
keeps existing validators and deterministic ordered semantics. Source extras
remain attached to exact retained handles, so delete/recreate cannot resurrect
metadata. Activation is still a separate replacement/clean-baseline operation.

Preview uses the same candidate path and discards it. It cannot consume a Scene
instance, reserve a revision or serve as a commit token. Commit always rebuilds
and revalidates the original revision. A worker returns bounded immutable command
values plus its observed revision; only the owner thread can commit. Real async
import/export lifecycle belongs to 11-14 and the existing completion primitive.

## Tradeoffs and boundaries

Copying the bounded authored scene costs O(scene storage) per preview/commit,
plus ordered command validation. This is an explicit local authoring tradeoff;
there is no claim about MMO simulation throughput. Gameplay retains existing
Core mutation without authoring history. The queue is not reused for atomicity,
and activation would incorrectly reset handles/baseline.

Batch identity is correlation only in 09b. Exactly-once retry behavior, retained
payload/outcome evidence and expiry belong to 09d. Shared bounded history is 09c;
CLI/process ownership, editor integration and FeatureWorld retain 15,16 and10.
Full REQ-09 remains NOT_RUN pending 09c-09e. Qualification uses this PC and WSL,
including refreshed Server suites because Core's implementation changes.
