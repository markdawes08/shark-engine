# Architecture contract

Decision date: 2026-10-17. Scope: roadmap assignment 00, Shark v1.
These are accepted implementation decisions, not claims of implemented features.
Platform feasibility remains an explicit assignment 02 gate.

MMORPG amendment, 2026-10-17: the user selected a seamless open world. ADR 0007
and the [versioned execution plan](../roadmap/mmorpg-plan.md) supersede the original
offline-only v1 scope. Earlier records are amended in place where their contracts
conflicted; historical handoffs keep their original evidence. All MMO behavior
remains planned. Original 00-35 IDs are retained with M01-M10 inserted by dependency.

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
closes scheduling, bounded work and the full Core gate. **04a input contracts are
complete; next is 04b native GLFW window integration.** The
[input guide](../input-contracts.md), [04a handoff](../handoffs/04a-input-contracts.md)
and [checkpoint plan](../roadmap/window-input-plan.md) record the contract and scope.
Full `REQ-04` remains NOT_RUN pending 04b/04c.

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

The [machine-readable requirements](../coverage/requirements.json) assign
implementation and verification ownership. A later assignment may refine an
implementation detail without weakening its acceptance gate. A change to a public
contract requires an ADR amendment, a compatibility/migration decision, and a
manifest/handoff update. A scope reduction, graphics-stack replacement, or platform
removal requires a concrete decision request to the user with evidence.

Documented release candidates must never be promoted to supported platforms from
CPU compilation alone. Only observed results in the prescribed hardware and
packaging gates can establish that claim.
