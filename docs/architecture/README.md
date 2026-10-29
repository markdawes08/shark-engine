# Architecture contract

The [testing policy](../testing-policy.md) governs verification selection and
evidence work, superseding older matrix and source-freeze schedules in these
records. Architectural boundaries and behavioral acceptance criteria remain
binding; select their tests when affected.

The original decisions date to 2026-10-17, assignment 00. They describe accepted
contracts, not automatic claims of implemented features. The
[machine-readable requirements](../coverage/requirements.json) and
[coverage index](../coverage/README.md) retain implementation status, original
handoffs and qualification receipts.

The seamless-world MMORPG amendment in [ADR 0007](0007-mmorpg-architecture.md)
and the [execution plan](../roadmap/mmorpg-plan.md) supersedes the original
offline-only scope. Original assignment IDs 00-35 remain; M01-M10 are inserted
by dependency. [ADR 0008](0008-human-agent-authoring.md) and the
[human/agent plan](../roadmap/human-agent-plan.md) make shared human-editor and
terminal-LLM authoring equal requirements without implying finished adapters.

[ADR 0009](0009-windows-linux-platform-scope.md) removes Mac compatibility.
[ADR 0010](0010-current-machine-scope.md) limits required acceptance to this
Windows PC, its two GPUs and existing WSL2 Ubuntu. Former hardware rows remain
historical evidence, not active requirements. The user's
[backlog decision](../backlog.md) authorizes continued development under each
increment's own acceptance criteria. Historical evidence and behavioral limits
remain intact.

Implemented foundations include Core services, offline input/frame scheduling,
Windows device/presentation/resource ownership and the precompiled cube sample;
CPU scenes, source persistence, shared commands/batches/history/retries;
authenticated transport/replication and server-only durable state; FeatureWorld,
asset identity/cache/jobs and bounded glTF conversion through the existing asset
owner. Their domain contracts and completed-owner handoffs are linked from
[AGENTS.md](../../AGENTS.md) and the [coverage index](../coverage/README.md).

Full REQ-12 is complete under current-machine scope; the
[12e handoff](../handoffs/12e-gltf-qualification.md) records connected owner/process
acceptance, independent fixture validation and applicable CPU/package checks.
Assignment 13 follows [five checkpoints](../roadmap/scene-rendering-plan.md),
with shared [Camera/MeshRenderer components](../render-components.md) and
[prepared runtime content](../prepared-content.md), and CPU
[scene extraction/camera math](../scene-rendering.md). EngineClient now consumes
those snapshots through a bounded [offscreen scene viewport](../scene-viewport.md);
application scene launch and connected Runtime acceptance completed in 13e.
14b adds [standalone scene export](../standalone-export.md) through CPU Tools
and a Core startup descriptor; its verification status is recorded in the
[export checkpoints](../roadmap/export-plan.md).
Editor/Runtime currently provide `--cube`, `--graphics` and `--window` diagnostics
with offline F6/F7 pause/step controls. Authored-scene rendering remains 13,
terminal/editor adapters remain 15/16, and MMOWorld remains M09. Schema IDs do
not establish file or gameplay-wire formats.

| Record | Decision |
| --- | --- |
| [0001](0001-targets-and-boundaries.md) | C++20 targets, dependency direction, application services, errors and ownership |
| [0002](0002-dependencies.md) | Allowed libraries, binding strategy, pinning and integration ownership |
| [0003](0003-platforms.md) | Separate OS/CPU/GPU release candidates and graphics feasibility evidence |
| [0004](0004-formats-and-identity.md) | Source, cache, cooked and protocol versions; IDs and migrations |
| [0005](0005-simulation-and-rendering.md) | Units, transforms, depth, color, ticks and scene mutation |
| [0006](0006-v1-profile.md) | Exact v1 feature and glTF profile, explicit exclusions |
| [0007](0007-mmorpg-architecture.md) | Seamless world, authority, server boundary, replication, persistence and provisional capacity gates |
| [0008](0008-human-agent-authoring.md) | Shared human/terminal authoring, discovery, noninteractive results, project ownership and parity gates |
| [0009](0009-windows-linux-platform-scope.md) | User-directed Windows/Linux platform scope, retired Mac obligations and preserved evidence |
| [0010](0010-current-machine-scope.md) | Current PC/WSL targets, local MMO workload and packaging gates; revised 02 completion |
| [0011](0011-network-foundation.md) | Private Mbed TLS, bounded local gameplay-wire foundation, trust boundaries and transport tradeoffs |
| [0012](0012-owned-sessions.md) | Mutual TLS, explicit account enrollment, bounded owned sessions and reconnect fencing |
| [0013](0013-authoritative-replication.md) | Bounded pose replication, session/stream fences, application acknowledgements and resynchronization |
| [0014](0014-spatial-interest.md) | Trusted observer selection, recipient policy, bounded admission and measured reliable-stream delay |
| [0015](0015-durable-store.md) | Server-only SQLite store, explicit create/open, durable identity and local durability boundary |
| [0016](0016-owned-durable-characters.md) | Durable character ownership, persisted enrollment/revocation, authenticated server owner and schema-2 compatibility |
| [0017](0017-atomic-durable-transactions.md) | Atomic economic transfers, durable operation identity, bounded retained outcomes and schema-3 compatibility |
| [0018](0018-runtime-durable-recovery.md) | Bounded storage worker, session/entity completion fences, timeout and explicit durable reconciliation |
| [0019](0019-durable-maintenance.md) | Explicit sequential database upgrades, required pre-upgrade backup and isolated restoration |
| [0020](0020-shared-authoring-commands.md) | One authored-scene command owner, revisioned single edits, bounded outcomes and read-only inspection |
| [0021](0021-atomic-authoring-batches.md) | Bounded atomic batches, pure preview and internal handle-preserving publication |
| [0022](0022-shared-authoring-history.md) | Bounded shared undo/redo, revision/ticket conflicts and incarnation-safe restoration |
| [0023](0023-retained-authoring-retries.md) | Owner-scoped sequenced mutations, bounded immutable outcomes and explicit expiry/restart reconciliation |
| [0024](0024-headless-scene-runner.md) | Deterministic CPU cases through existing owners/scheduling, bounded assertions and versioned results |
| [0025](0025-asset-identity.md) | Persistent adjacent asset sidecars, shared file publication and bounded revisioned catalog snapshots |
| [0026](0026-development-asset-cache.md) | Dependency-keyed synchronous imports and atomic bounded development cache snapshots |
| [0027](0027-bounded-asset-jobs.md) | Bounded private import worker, retained jobs, cancellation and owner publication |
| [0028](0028-bounded-gltf-sources.md) | Bounded pure glTF source snapshots, references, parser ownership and subsequent conversion contracts |
| [0029](0029-bounded-gltf-geometry.md) | Bounded CPU static geometry, shared mesh hierarchy, transform validation and generated attributes |
| [0030](0030-bounded-gltf-textures.md) | Bounded PNG/JPEG decoding, usage-specific texture mips and sampler preservation |
| [0031](0031-gltf-material-assets.md) | Core materials, bounded engine payloads and typed resource integration through the asset owner |
| [0032](0032-render-components.md) | Optional Camera/MeshRenderer values, shared authoring/persistence and runtime-safe asset identity |
| [0033](0033-prepared-runtime-content.md) | Bounded prepared mesh/scene formats, immutable assets and built-in geometry without Runtime importers |
| [0034](0034-scene-extraction.md) | Revisioned CPU extraction, full affine cameras, bounded local render coordinates and conservative frustum culling |
| [0035](0035-offscreen-scene-viewport.md) | Persistent offscreen scene targets, bounded unlit draws and safe resize through the existing graphics owner |
| [0036](0036-runtime-scene-launch.md) | Explicit prepared Runtime loading, dynamic scene presentation and complete-candidate reload |
| [0037](0037-client-export-content.md) | Bounded CPU export candidates from the startup scene and fresh client import closure |
| [0038](0038-standalone-scene-export.md) | Live-owner export publication and bounded package-relative Runtime startup |
| [0039](0039-terminal-authoring-protocol.md) | Bounded terminal protocol, retained project ownership, discovery and revisioned queries |
| [0040](0040-human-editor-shell.md) | Native human editor shell over shared project ownership, bounded GUI presentation and explicit dirty-state decisions |
| [0041](0041-editor-scene-workspace.md) | Retained scene/asset workspace, revision-scoped drafts, shared history, explicit import publication, diagnostic console and GUI scene composition |
| [0042](0042-viewport-navigation-and-picking.md) | Display-only camera navigation, validated view overrides before culling and revision-bound snapshot triangle picking |
| [0043](0043-transform-manipulation.md) | Shared local/world transform math, cumulative snapping and pure drag previews with one retained history commit |
| [0044](0044-viewport-gizmos.md) | Native axis/ring gizmos and immutable subtree previews through existing authoring and rendering owners |

The [machine-readable requirements](../coverage/requirements.json) assign
implementation and verification ownership. A later assignment may refine an
implementation detail without weakening its acceptance gate. A change to a public
contract requires an ADR amendment, a compatibility/migration decision, and a
manifest/handoff update. A scope reduction, graphics-stack replacement, or platform
removal requires a concrete decision request to the user with evidence.

Documented release candidates must never be promoted to supported platforms from
CPU compilation alone. Only observed results in the prescribed hardware and
packaging gates can establish that claim.
