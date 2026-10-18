# ADR 0008: Human and terminal-agent authoring

Status: accepted product and architecture contract; authoring implementation is
planned. Date: 2026-10-17. Baseline reviewed: `d9f53ef`.

## Decision and scope

Shark must support both a human using the editor and an LLM agent working from
an ordinary terminal. Both are first-class ways to develop the same seamless
MMORPG project. Neither requires the other to perform routine authoring work.
The public terminal interface must work with ordinary subprocesses and files,
without an LLM provider SDK, model subscription, editor plugin or MCP client.
An optional thin MCP adapter can wrap the same interface in 15 if needed.
The engine does not embed an LLM or grant an agent special engine authority.

07c implements the first [schema and semantic-inspection foundation](../scene-inspection.md)
in EngineCore: a fixed Name/Transform catalog with explicit IDs/version 1,
typed defaults/ranges/units/mutability, pure shared validation and bounded owned
query pages tied to a scene revision. These CPU C++ APIs run without an editor,
display or registry access. They provide inputs for the later shared services;
they are not terminal commands, JSON framing, source files, transactions or
gameplay messages. Their passing current-machine qualification is recorded in the
[07c handoff](../handoffs/07c-schema-inspection.md) and
[receipt](../coverage/evidence/07c-schema-inspection.json); the broader authoring gates
below remain assigned to their original owners.

This strengthens the original shared-command and agent-interface requirements;
it does not replace the editor, [MMORPG contract](0007-mmorpg-architecture.md),
graphics gates or assignment order. The [execution and assessment plan](../roadmap/human-agent-plan.md)
assigns each addition to an existing owner. None of these authoring capabilities
is provided by the current Editor/Runtime/Server bootstrap flags.
The later [current-machine scope](0010-current-machine-scope.md) qualifies these
workflows on the available Windows PC, with CPU/server fixtures in its existing
WSL environment. It requires no new machine, external model service, hosted agent
runner or provider account. Human and terminal parity remains mandatory.

## One behavior, two interfaces

EngineCore owns validation and safe mutation; EngineTools owns project services,
authoring transactions, history and jobs. Human widgets, terminal commands,
local protocol handlers and tests use those services and schemas. Do not create
a second mutable scene, validation path or renderer inside an agent adapter.
The editor may offer spatial selection, navigation and gizmos; the terminal must
offer equivalent semantic queries and resulting edits, without pixel clicking.
The human must be able to open, inspect and continue an agent-authored project,
and the agent must be able to query and continue a human-authored project.

Every supported semantic authoring operation must have a documented terminal
path when its feature lands after 15. Earlier services and schemas gain that
path in 15. Editor controls arrive in 16/17 and subsequent feature owners.
UI presentation preferences need not become game-authoring API methods; any
exception to semantic parity must be explicit in coverage and cannot silently
exclude a release feature. Lua APIs remain a deliberate gameplay surface, not
an automatic mirror of every authoring command.

## Terminal and protocol contract

Assignment 14 introduces the initial export executable; 15 completes the CLI
and local JSON-RPC stdio host. Exact flags, methods, schemas, framing and limits
are published with executable tests in their owning assignment, not guessed now.

- One-shot CLI operations accept explicit inputs and finish with documented exit
  codes. They support a versioned JSON result mode as well as readable help and
  human diagnostics. No required TTY, prompt, file picker or visible editor.
- Machine input/output uses UTF-8 with bounded stdin/file input and explicit
  framing. In JSON mode, stdout contains only the documented machine result. In stdio
  protocol mode, stdout carries only framed protocol traffic; ordinary logs go
  to stderr. Progress is a defined event or job query, never stray console text.
- Discovery reports protocol/schema versions, available methods and capabilities,
  component properties, types/defaults/ranges, enums, units, mutability, examples
  and required execution context. Reject incompatible versions with supported
  version details. Advertise only implemented capabilities.
- Errors have a stable code, readable message, operation/request correlation and
  relevant field/entity/asset/path context. State, validation, revision conflict,
  missing capability, cancellation and execution failure are distinguishable.
  A human message is not the machine discriminator; do not scrape log text.
- Queries return stable IDs, revisions and bounded structured data. Use filters,
  field selection and pagination with an explicit snapshot/revision consistency
  policy. Truncation is visible; it must not look like a complete scene result.
  Provide semantic diagnostics and state summaries so agents do not need to dump
  whole scenes or infer success from screenshots.
- Long operations expose bounded job status, progress, diagnostics, cancellation
  and artifact references. Define timeout, stdin EOF, disconnect and process-exit
  behavior. A cancelled/failed job cannot publish a half-import or half-export.
  If commit already happened, report that outcome rather than claiming rollback.
- Versioned results identify produced artifacts, their format and relevant
  project/scene revision and execution profile. Capture seeds, clocks, tool and
  content versions needed to reproduce tests. Test scripts fail nonzero on real
  failures and never label absent tests or unavailable devices as PASS.

## Editor and agent sharing a project

09 defines revisions/transactions; 15 implements ownership across processes;
16 qualifies the editor using it. One authoring service owns mutable project
state, including unsaved edits and history. An independent CLI process must
never silently open a second writable copy while the editor has unsaved state.
The first implementation may reject a second writer with a structured busy
error and support explicit save/close/reopen handoff. Concurrent editor/agent
editing is optional: if supported, both connect to that same owner through a
bounded local adapter, and tests prove history and unsaved-state consistency.
Do not imply cross-process attachment merely because stdio exists.

The active owner retains its unsaved state and history. Handoff must save or
explicitly resolve dirty state and preserve saved content/revisions. Document
whether closing/reopening resets history; durable undo across process restarts
is not required unless deliberately implemented and tested.

Mutations carry an expected revision; stale writes fail with the current revision
and conflict context. Batch edits validate a complete candidate before atomic
commit and return changed IDs and the new revision. Pure validation/preview is
available for edits; it has no side effects and does not reserve the revision.
Execution rechecks the revision. Undo/redo acts on the common ordered history and
rejects stale assumptions instead of undoing an unrelated person's new work.

Separate request correlation from mutation retry identity. 09/15 define deduplication
scope, retention, payload matching and recovery after a lost reply. Within that
scope an identical retried mutation returns its recorded outcome, and a reused
identity with different input rejects. Across restart or expiry, report an unknown
outcome or reconcile through durable IDs/revisions; never blindly repeat an edit
or promise exactly-once behavior without evidence. Async job identity and final
publication follow the same defined rules.

Project-scoped text reads/writes (including Lua after 19) use expected content
revision/digest and atomic replacement. External file edits and stale writes are
detected before reload/save; never silently overwrite another writer. Source is
readable and versioned under [ADR 0004](0004-formats-and-identity.md). Terminal
authors may edit documented source formats; the acceptance workflows use public
tools and may not patch private caches or engine internals to bypass validation.

## CPU authoring and device-dependent work

EngineTools' base services and CPU authoring host depend on Core, not the editor,
GLFW, GPU runtime or audio device. Keep preview/capture in a separately gated
client adapter/target; an optional dependency must not make device libraries
mandatory for a CPU-only configure/build or startup. CPU importers and offline
shader compilers are allowed authoring dependencies with their own locks; this
is distinct from the stricter no-import/no-shader Server boundary in M01.

CPU project creation, schema/query/edit, validation, save/load, import, simulation
tests and supported cook/export operations run without a display or editor.
Target SDK/cross-export limits remain explicit. GPU rendering, frame capture and
hardware audio checks require the appropriate qualified device/profile. They
return a clear unavailable-capability result otherwise; no fake screenshot,
silent software-renderer substitution or CPU-only graphics PASS. In 15's terminal
GPU test a separate render process may use a qualified graphics/display backend;
no manual GUI interaction is required. Nonvisual assertions and structured
readbacks complement images; visual acceptance still requires actual GPU evidence.
Use the two recorded local Windows GPU profiles for these device cases. WSL CPU
authoring/server execution does not establish native Linux graphics support.

## Trust and MMORPG separation

Local authoring is a trusted development interface scoped to a canonical project
root, with bounded requests, queues, files and responses and tested path-escape
rejection. It does not expose a public network listener by default. Any later
remote adapter needs its own authorization and transport contract. Project text,
logs and asset metadata are data; they do not authorize shell execution. An
authoring endpoint must not become an unrestricted host-shell command service.
Authored Lua runs only in its explicitly defined execution mode and trust profile;
project path validation alone is not a sandbox for arbitrary code.

An agent developing a game has the same local authoring capabilities as a human;
neither can use them as a player cheat or live production admin bypass. Gameplay
intents (M02), durable operations (M03) and authorized operational administration
(M08) remain separate. Agent-driven MMO tests launch local fixtures under bounded
process supervision; this does not authorize production deployment, migration or
live-world edits. Logs and reports redact credentials, and private server rules
and secrets never enter client exports.

## Acceptance

15 must prove a separate terminal process can discover, create, inspect, edit,
validate, save/reopen, import, run assertions and export a project without GUI
interaction. A CPU-only run qualifies CPU behavior; a separate GPU case captures
a real frame. Both use closed/no-TTY input and parse public results.

Negative fixtures cover malformed/oversized input, mixed stdout, unsupported
versions/capabilities, stale revisions, second writers, lost replies/retries,
cancellation before/after commit, EOF, worker failure and path escapes. 16/17
compare human/editor and terminal semantic outcomes and exercise both directions
of project handoff. 32 audits parity; 33 uses the available terminal agent in a
fresh context with only public docs/tools and a separate human continuation check;
M09 repeats this for
the local seamless MMORPG loop. Both interfaces and their packaged getting-started
paths run on this PC. No particular model/provider integration is part of the
engine contract; no additional subscription or hosted session is a prerequisite.
Missing required local UI, GPU or agent-session evidence stays NOT_RUN.
