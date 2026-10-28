# Assignment 15: terminal authoring checkpoints

Date: 2026-10-28. Base: `def9755`, initially clean `main`. Owner: REQ-15.
The user approved five subchunks after completed assignment 14. This refines the
three broad phases in the [human/agent plan](human-agent-plan.md), following
[ADR 0008](../architecture/0008-human-agent-authoring.md), the
[MMORPG plan](mmorpg-plan.md) and the [testing policy](../testing-policy.md).

| Checkpoint | Deliverable and acceptance |
| --- | --- |
| 15a: Protocol foundation | CPU one-shot/stdio host, bounded versioned UTF-8 messages, structured errors, implemented capability/schema discovery, one retained project owner and read-only semantic queries. Real Windows/WSL pipes cover framing, Unicode, pagination, conflict/closure and EOF/process-death release. |
| 15b: Editing and persistence | Project creation, shared edits/batches/preview, save/reopen, undo/redo, revision conflicts and retained mutation retries. Scoped text access must preserve typed source-document ownership and detect external edits. |
| 15c: Imports and jobs | Adapt existing asset/import/job services with bounded status/progress, cancellation, diagnostics and truthful publication outcomes. Do not introduce a second scheduler or cache owner. |
| 15d: Run, export and capture | Connect deterministic runs and export to the active project owner; optional GPU capture uses a separate client adapter and explicit capabilities. Calling the standalone exporter cannot acquire a second writer or silently close/discard the active session. |
| 15e: Connected acceptance | A public subprocess workflow creates/edits/imports/saves/reopens/runs/captures/exports, with failure/retry/cancellation and ownership handoff evidence. Publish usable examples and complete coverage; reuse valid focused results from earlier checkpoints. |

**15a is PASS**, recorded in the [protocol handoff](../handoffs/15a-terminal-protocol.md).
15b is next. Full REQ-15 remains NOT_RUN until 15e. Advertise
only implemented methods at each checkpoint; later work extends the same protocol
and services. No network listener, LLM SDK, MCP client or editor is required.
Ordinary terminal subprocesses are the initial adapter; an MCP bridge is optional
only if a real client requires it. Assignment 16 owns the human editor UI.

Use one creating-thread project owner over ProjectFiles and AuthoredScene. Retain
unsaved state/history while open, reject a second writer, and explicitly save or
resolve dirty state before close/reopen. Query cursors and native revisions are
session-local; authored IDs are durable. Request IDs correlate responses and do
not promise mutation retries or exactly-once behavior.

Each increment uses affected Debug checks, an applicable sanitizer selection,
portable WSL pipe coverage and relevant boundary checks. New device adapters need
actual available-GPU evidence. Do not repeat unrelated matrices or create new
per-checkpoint testing frameworks. Later acceptance must use the public interface,
not private caches or a second mutable scene.
