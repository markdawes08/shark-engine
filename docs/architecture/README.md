# Architecture contract

Decision date: 2026-10-17. Scope: roadmap assignment 00, Shark v1.
These are accepted implementation decisions, not claims of implemented features.
Platform feasibility remains an explicit assignment 02 gate.

MMORPG amendment, 2026-10-17: the user selected a seamless open world. ADR 0007
and the [versioned execution plan](../roadmap/mmorpg-plan.md) supersede the original
offline-only v1 scope. Earlier records are amended in place where their contracts
conflicted; historical handoffs keep their original evidence. MMO behavior was
planned at that amendment; the current implementation status appears below.
Original 00-35 IDs are retained with M01-M10 inserted by dependency.

Human/agent amendment, 2026-10-17: [ADR 0008](0008-human-agent-authoring.md) and
the [terminal-development plan](../roadmap/human-agent-plan.md) make human editor
and terminal LLM workflows equal product requirements. They strengthen existing
authoring assignments without changing order or implying a working CLI/editor.

Platform amendment, 2026-10-17: [ADR 0009](0009-windows-linux-platform-scope.md)
records the user's removal of Mac compatibility and qualification work. The later
[ADR 0010](0010-current-machine-scope.md) limits required acceptance to the current
Windows PC and its WSL2 environment. The two actual local GPUs and local package
gates pass assignment 02; the former six rows remain historically untested but
are no longer requirements. Assignment 03 is split into the
[Core checkpoints](../roadmap/core-plan.md); the
[03a handoff](../handoffs/03a-core-contracts.md) and
[03b handoff](../handoffs/03b-application-lifecycle.md) record completed contracts
and lifecycle verification. The [03c handoff](../handoffs/03c-time-and-work.md)
closes scheduling, bounded work and the full Core gate. **05b, swapchain,
submission and presentation lifecycle, is complete under the current-machine
scope.**
The [device guide](../graphics-device.md),
[presentation contract](../presentation-lifecycle.md),
[05 checkpoint plan](../roadmap/graphics-device-plan.md) and
[05b handoff](../handoffs/05b-presentation-lifecycle.md) record the lifecycle and
passing local gate. The [05a handoff](../handoffs/05a-graphics-device.md) retains the
completed device-foundation evidence. The user's 2026-10-18
[backlog decision](../backlog.md) defers remaining qualification work and authorizes
06b and later development, superseding the earlier CPU-only exception. The
[06 checkpoint plan](../roadmap/shader-resource-plan.md) and
[06a handoff](../handoffs/06a-offline-shaders.md) record the completed offline checkpoint and its verification limits.
The [input guide](../input-contracts.md),
[window lifecycle guide](../window-lifecycle.md),
[frame-loop guide](../frame-loop.md), [04c handoff](../handoffs/04c-frame-loop.md) and
[checkpoint plan](../roadmap/window-input-plan.md) record the contracts and scope.
Editor/Runtime provide explicit `--graphics` clear presentation and `--window`
diagnostics through the same offline frame/input pump, with local F6/F7 sample
pause/step controls. Scene rendering and authoring UI remain planned. Full `REQ-04` is PASS
after final Windows/WSL, native-window and standalone/package qualification.

Assignment 07 implements the shared CPU scene foundation: checked identities,
mandatory Name/Transform, validated hierarchy and fixed version-1 schemas with
bounded revisioned semantic inspection. The [inspection contract](../scene-inspection.md)
defines the implemented discovery and query APIs; the
[07 checkpoint plan](../roadmap/scene-plan.md) and
[07c handoff](../handoffs/07c-schema-inspection.md) track qualification.
07c and full REQ-07 are PASS under the current-machine scope, recorded in the
[07c receipt](../coverage/evidence/07c-schema-inspection.json). M02 is complete; the
[M02a handoff](../handoffs/M02a-transport-wire.md) closes the transport foundation,
and [M02b authentication and owned sessions](../handoffs/M02b-owned-sessions.md)
is **PASS** under the current-machine scope. [ADR 0012](0012-owned-sessions.md) and the
[session contract](../network-sessions.md) define explicit certificate enrollment,
transport-bound ownership, revocation and reconnect fencing. M02c replication is
PASS under current-machine scope, following [ADR 0013](0013-authoritative-replication.md)
and its [handoff](../handoffs/M02c-authoritative-replication.md). M02d interest/budgets
is complete under [ADR 0014](0014-spatial-interest.md). The
[M02e handoff](../handoffs/M02e-integrated-qualification.md) and
[receipt](../coverage/evidence/M02e-integrated-qualification.json) close integrated
qualification and full REQ-M02 under the current-machine scope. Assignment 08,
Project and scene persistence, is next. Authoring transactions, terminal
adapters and editor UI retain their later owners;
schema IDs do not establish file or gameplay-wire formats. The current-machine
scope remains unchanged.

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

The [machine-readable requirements](../coverage/requirements.json) assign
implementation and verification ownership. A later assignment may refine an
implementation detail without weakening its acceptance gate. A change to a public
contract requires an ADR amendment, a compatibility/migration decision, and a
manifest/handoff update. A scope reduction, graphics-stack replacement, or platform
removal requires a concrete decision request to the user with evidence.

Documented release candidates must never be promoted to supported platforms from
CPU compilation alone. Only observed results in the prescribed hardware and
packaging gates can establish that claim.
