# Shark MMORPG execution plan and foundation assessment

Date: 2026-10-17. Baseline reviewed: `3fecfc3` on `main`, initially clean.
User direction: tune the engine for an MMORPG, with a **seamless open world**.
This is a planning amendment, not an implementation of multiplayer features.

This versioned plan and [ADR 0007](../architecture/0007-mmorpg-architecture.md)
supersede the original roadmap's exclusion of networking, skeletal animation and
world streaming. The ignored `AI_Game_Engine_Agent_Roadmap.txt` is updated locally
to point to the same amendment; a clean checkout uses this plan and the
[requirements manifest](../coverage/requirements.json). Original assignments
00-35 keep their identities and retained requirements. M01-M10 are mandatory
insertions, not optional work after the old release candidate.

The later [human/terminal-agent amendment](human-agent-plan.md) and
[ADR 0008](../architecture/0008-human-agent-authoring.md) strengthen shared
authoring requirements across these owners. Both human editor use and independent
terminal development are mandatory; assignment order and MMO authority stay intact.
The pre-M01 assessment below is historical; the later plan assesses `d9f53ef`.
The [current-machine decision](../architecture/0010-current-machine-scope.md)
supersedes the wider Windows/Linux qualification targets in the earlier
[platform decision](../architecture/0009-windows-linux-platform-scope.md).
Required work uses the available Windows PC: Windows clients/editor, the RTX
4070 Laptop and Intel UHD/Raptor Lake-S Mobile GPUs, and isolated Ubuntu server/CPU
tests inside existing WSL. Native Linux graphics, additional machines, cloud
services, paid accounts and new hardware are not prerequisites. Mac and checkpoint
02c remain outside scope.

The engine retains seamless travel, authoritative servers, persistent state and
equal human/terminal authoring. A local development release is the current
deliverable. Same-machine multiprocess tests exercise real boundaries; they do
not establish production scale, real WAN behavior or independent failure recovery.

## Historical assessment of the pre-M01 foundation

| Existing work | Assessment and action |
| --- | --- |
| Chunk 00 architecture | Amend now: offline-only release scope, one-scene identity, process-main-thread simulation, fixed client/render scheduling and minimal acceptance games cannot define an MMORPG release. ADRs 0001-0007 now distinguish the planned authority, world, persistence and timing contracts. |
| Engine bootstrap, Editor/Runtime entry points | Keep. Seven owned C++ source/header files contain version/help/argument behavior, not game simulation. No network, ECS, save format, physics or renderer exists to rewrite. |
| CMake target separation | Keep working foundation; extend in M01 before graphics integration. Current Engine is only a tiny common library, and `runtime-only` is a client bootstrap, not a headless server. Add Core/Client/Server boundaries rather than growing GPU dependencies inside a shared server core. |
| Existing boundary checker | Keep its authoring exclusion. M01 adds independent Server/Core roots and rejects client/presentation dependencies even if they were previously classified as runtime-safe. Test transitive, generated and configure-time dependencies; checking for EngineTools alone is insufficient. |
| Dependency pins, formatting, CTest, sanitizers and CI | Keep. Add server-only Linux CI and later fault/network/database/load tiers with real fixtures. No current dependency needs replacing merely because the product is an MMO. No database, transport or hosting vendor is selected yet. |
| Current tests and coverage | Keep the real bootstrap/tooling coverage. It establishes no player capacity, headless server behavior, persistence, simulation or seamlessness. Planned MMO requirements stay NOT_RUN and never enter implemented-surface entries until real code/tests exist. |
| Local agent guidance | Amend now so future work preserves authority, client/server splits, continuous-world coordinates and separate source/database/wire formats. Existing skill names and commands remain valid. |
| `.gitignore` | Existing rules already cover local roadmap, builds, tools, logs and evidence. Keep versioned plans, schemas, fixtures and skills visible. Add future generated database/content staging paths when their actual locations are introduced; do not blanket-ignore assets, SQL or fixture files. |

No C++/CMake/dependency changes are justified in this planning-only update. The
concrete structural changes above are assigned to M01; adding a fake network or
Server target now would not satisfy them. The contract validator is updated to
track the expanded ordered assignment set and reject false future passes.

## Execution order

One assignment at a time; retain working increments and evidence. Original
numbers remain stable for links and historical handoffs. Order is explicit in
manifest schema 3 because lexical/numeric order no longer expresses dependencies.

```text
00 -> 01 -> M01 -> 02 -> 03 -> 04 -> 05 -> 06 -> 07 -> M02
   -> 08 -> M03 -> 09 -> 10 -> 11 -> 12 -> 13 -> 14 -> 15 -> 16 -> 17
   -> 18 -> 19 -> 20 -> 21 -> M04 -> M05 -> M06 -> 22 -> 23 -> 24 -> M07
   -> 25 -> 26 -> 27 -> 28 -> 29 -> 30 -> 31 -> M08 -> 32 -> 33 -> M09
   -> 34 -> M10 -> 35
```

Assignment 01's hosted CPU acceptance and deliberate-failure/red-to-green gate
is complete; the [CI verification handoff](../handoffs/01-ci-verification.md) records
the tested revision and run evidence. **M01's gate is complete.** The
[M01 handoff](../handoffs/M01-headless-server.md) records
implementation/local qualification; the
[hosted follow-up](../handoffs/M01-ci-verification.md) closes its native CPU,
headless Docker and deliberate-failure/restored-success gate. **Assignment 02 is
closed under the explicit current-machine scope amendment; assignment 03 is complete.**
The [Core checkpoint plan](core-plan.md) separates 03a contracts, 03b lifecycle
and 03c scheduling/bounded work. All three pass; the
[03c handoff](../handoffs/03c-time-and-work.md) records the full local Core gate
without adding game services. **04c and full assignment 04 are complete under
the current-machine scope.** The [window/input plan](window-input-plan.md)
records all three completed checkpoints.
The [input guide](../input-contracts.md), [window lifecycle guide](../window-lifecycle.md),
[frame-loop guide](../frame-loop.md) and [04c handoff](../handoffs/04c-frame-loop.md)
describe local input, windows and offline frame scheduling. None grants gameplay
authority. The frame loop reuses Core's existing 60 Hz offline scheduler with
bounded catch-up, first-tick input and explicit pause/step; Server timing retains
its separate policy. Editor/Runtime expose `--window` with F6/F7 sample controls
without a production renderer, world simulation or game-authoring UI. Full
`REQ-04` is PASS after final Windows/WSL, native-window and standalone/package
qualification. **05a device ownership is now complete; next is 05b, swapchain/submission/presentation.**
The [05 checkpoints](graphics-device-plan.md), [device contract](../graphics-device.md)
and [05a handoff](../handoffs/05a-graphics-device.md) record native validation on both
local GPUs and WSL CPU/Server isolation. Full `REQ-05` remains NOT_RUN until 05c.
Presentation stays in Client and cannot acquire authority over server time or world state.
The [final local handoff](../handoffs/02d-final-verification.md)
preserves the Windows lifecycle and isolated Ubuntu CPU/package evidence. The
[graphics checkpoint plan](graphics-plan.md) applies the new local gate to those
observed results. Former baseline GPU rows and independent clean-machine work
are outside active scope, not retrospectively passed. Existing hosted CI remains
useful supplemental evidence; local progress does not require another hosted run.
Earlier handoffs and the assessment above retain their historical repository/gate
status. The probe qualifies its exercised local graphics cases, not a production
renderer, editor or any multiplayer feature. Future gates use the same available
machine and still require actual behavior evidence.

## Inserted assignments and gates

### M01 - Headless server and client/core build separation

After 01, before 02. Introduce EngineCore, EngineClient and a real minimal Server
process as specified by [ADR 0001](../architecture/0001-targets-and-boundaries.md).
Keep existing bootstrap behavior; provide only actual lifecycle/configuration
behavior available at this stage. No networking or simulated MMO success claim.
Record the provisional world/load profile from ADR 0007 without inventing capacity.

Gate: clean Ubuntu 24.04 x86-64 build/start/test with no window, Vulkan, audio,
editor/import or offline shader dependency installed, fetched or linked. Negative
fixtures reject presentation dependencies through transitive links. Validate CLI,
invalid config and bounded shutdown. Add documented server-only preset, CPU CI
job, sanitizer evidence and source/binary/package closure checks. Existing client
presets continue to pass. Do not publish command names before they actually exist.

### M02 - Sessions, transport and bounded replication spine

After 03/07 and M01. Select and pin maintained transport/authentication components
using a recorded threat/requirements assessment. Add independent gameplay-wire
versioning, authenticated session ownership, bounded typed intents, reconnect,
baseline/delta state, spatial interest and a deterministic network-fault harness.
Use simple engine-owned entity fixtures before full physics/gameplay integration.

Gate: two real client processes see authoritative entity changes through the
server on this PC using loopback or Windows-to-WSL transport. The selected identity
and transport implementation must run locally without an external account or
service. Incompatible versions, invalid/expired/revoked credentials, forged ownership,
malformed/oversized packets, replay and slow readers reject safely. Loss, jitter,
reorder, duplication, sequence wrap and reconnect preserve bounded memory and
defined state convergence. Private/uninterested fields do not leak. Record actual
bandwidth and replication work; no public editor RPC or shipping test-auth bypass.
Seeded local loss/jitter/partition fixtures are required; real WAN links are not.

### M03 - Durable characters and transactional state

After 08 and M02. Select/pin the transactional persistence integration, distinguish
authored content from live state, define account/character ownership, database
schema migration, operation identity, revision checks and crash recovery. Preserve
durable world/entity IDs across server lifetimes. A module boundary is sufficient;
do not require a service fleet for the first vertical slice.
The store must run in a local process or embedded integration without cloud
credentials. Pin its real dependency when selected; no in-memory substitute for
durable acceptance.

Gate: create/select/save/reconnect a character across actual process restart.
Concurrent stale writes and duplicate requests cannot duplicate inventory or
currency in the transaction fixture. Inject crash/timeout before and after durable
commit; retries resolve one outcome and acknowledged durable effects survive.
Database outage fails critical operations safely. Run real-store migration and
backup/restore fixtures, not only an in-memory mock; record recoverable checkpoint
loss windows and migration rollback constraints. Restore into an isolated local
database/directory; this verifies software recovery, not survival of host or disk
loss.

### M04 - Authoritative character movement and gameplay

After 18-21 and M02/M03. Add server-owned character movement/collision, input
sequencing, prediction/reconciliation, remote interpolation, bounded history and
authoritative combat/interaction validation. Separate server Lua rules from client
presentation Lua; fixed authoritative updates own outcomes. The final combat
model and any lag compensation require explicit tests and a profile decision.

Gate: movement, collision, cooldowns, range, damage, death/respawn and spawn/despawn
agree across two clients under network adversity. Forged speed, impossible move,
stale target, replayed attack and client item grants reject. Reconciliation does
not repeat durable effects or presentation events. Minimize/pause/disconnect one
client while the world continues. Server runs these cases without graphics/audio.

### M05 - Seamless world streaming, navigation and authority transfer

After 11-13, 18/20 and M04. Implement in reviewable internal steps: (1) global
cell/local coordinates and origin-safe content/collision/navigation streaming,
(2) adjacent regions in one process with overlapping interest and continuous
travel, (3) the same transfer across separate local processes with fencing and
crash recovery.
These are parts of one gate; local streaming alone does not complete M05. Use
validated engine-owned development payloads prepared by EngineTools at this stage;
31/M08 replace and requalify them as final cooked cell/dependency manifests.

Gate: an authored continuous terrain/static-world fixture traverses adjacent cells
and at least two authoritative regions without a loading screen, reconnect, scene
reset or character-ID change. Collision/nav and nearby interactions remain valid
at edges. AI pursuit, targeting, combat/projectiles, interest visibility and a
durable-operation fixture cross the boundary. Actual party/trade continuity is
integrated in M07/M09 after those features exist. Inject destination/source
process loss and shaped local network partition before/after ownership commit;
one owner and one durable outcome remain.
Stale owners cannot write. A consistent tick/input-sequence cut captures final
source state and pending operations; post-cut inputs/completions are routed and
reconciled without losing acknowledged outcomes. Inject mutations and late durable
success between prepare, cut and ownership commit. Asset prefetch/cancel/retry, IO pressure, far-from-origin
precision and bounded resident memory have tests. No generic seamless claim from
two disconnected scenes or rendering-only streaming.

### M06 - Animated characters and dense-world presentation

After M05 and 12/13. Extend the static glTF milestone to the explicitly bounded
skeletal profile in [ADR 0006](../architecture/0006-v1-profile.md). Add skinning,
clip sampling/blending, semantic animation state, actor/mesh/animation LOD,
visibility and crowd measurements. Keep authoritative motion independent of
visual root motion and animation events.

Gate: licensed fixtures cover joint limits/weights, bind pose, interpolation,
attachments, loop/transitions and invalid data. Import/cook/reload/renderer behavior
agrees with the profile; LOCAL-MMO-1 uses 32 visible animated actors as the
provisional measurement fixture, not a proven capacity. Record RTX 4070 Laptop
frame/memory/streaming budgets and the Intel correctness/reduced-quality profile
and LOD quality/error thresholds before accepting. Validate origin shifts and
network corrections without broken attachments, unbounded uploads or actor popping
that hides gameplay-relevant entities. Original static assets remain supported.

### M07 - Small MMORPG reference gameplay and UI

After 23/24 and M03-M06. Build a small game module/data set: character selection,
equipment/inventory, a combat/loot/quest loop, death/respawn, atomic two-player
trade, party and bounded authenticated chat. Add HUD/input focus for health,
targeting, actions, confirmed inventory and connection/recovery status. EngineCore
provides contracts; game-specific formulas/content belong to the reference module.

Gate: two distinct authenticated players in real local client processes complete
the loop across an internal region boundary, including
trade in flight and party visibility during handoff, then log out
and reconnect with correct state. Concurrent trade, full inventory, disconnect
during reward/trade, invalid ownership and chat flood fixtures preserve integrity
and enforce limits. Public authoring tools can create the relevant reference
content. A working Tetris demo alone cannot satisfy this assignment.

### M08 - Separate packages, rollout and operational recovery

After 30/31 and M07. Produce distinct client and headless server packages plus
versioned compatible content. Add readiness/health, admission stop/drain, metrics,
redacted logs, administration authorization/audit, database migration, backup/restore,
content authenticity and rollback. Select deployment infrastructure only from
the measured requirements; no hosted deployment is authorized by writing this plan.

Gate: relocate the Windows client package outside the checkout with development
paths removed, and run the Linux server package in an isolated SDK-free Ubuntu
userspace on existing WSL. Both run the reference world on this PC. Client package
contains no server-only rules, credentials or private data; Server excludes all
presentation/authoring dependencies. Failures during drain, content rollout,
migration, restart and restore have observable recovery. Incompatible versions
reject or follow an explicitly tested compatibility window; cached hashes alone
do not establish update authenticity. Exercise process termination and a restored
backup, not just configuration/documentation checks. Local test keys qualify
signature/update-trust behavior without an external signing provider. No additional
machine, public deployment, managed service or off-site backup is required; report
the retained shared-host limitations explicitly.

### M09 - Multi-client seamless MMORPG acceptance

After 32/33 and M08. Retain the original fresh-agent Tetris and physics tests as
tooling regressions. Add a fresh-agent-authored small MMORPG world using documented
public tools and the reference game contracts, with automated real-client/load
drivers and separate multi-process server execution on the same PC.

The available terminal agent uses a fresh context with public discovery/docs/results,
without GUI automation or a required MCP/provider integration. A human then opens,
inspects and continues the same authored world in the editor; both directions of
handoff preserve revisions and content. This complements real-client MMO proof.

Gate: create/save/cook/export/start, authenticate two graphical clients, move/fight/
loot/quest/trade/chat/party, cross regions, reconnect and restore durable state.
Replay with seeded local network adversity and server process failure. Exercise
ADR 0007's provisional LOCAL-MMO-1 profile: 16 active synthetic players and 16
NPCs, all 16 players in a hot spot, at least two separate region processes and
100 crossings. Run the two graphical clients' functional case separately from
the headless load case. Measure rather than claim production capacity. Map each
supported API to tests and a FeatureWorld/MMOWorld or justified external fixture.

### M10 - Local capacity, endurance and development release qualification

After 34 and M09, before 35. Turn the provisional budgets in ADR 0007 into recorded
results on this PC's named CPU/GPU/memory/network/storage, with exact workload,
client counts, NPC density, content and process topology. Compare client and server versions and
separate wall-clock tick lag from tick computation time. Set aggregate memory,
process count, disk/artifact and bounded overload-ramp limits before execution;
do not change workstation/WSL configuration implicitly to achieve them.

Gate: two-hour LOCAL-MMO-1 active workload soak, bounded population/hot-spot ramps,
bandwidth/queue/memory bounds, p95/p99 tick/interaction/correction metrics,
overload/admission behavior,
streaming stalls and recovery/backup drills. No unresolved duplication, acknowledged
durable-state loss, split authority, critical exploit or sustained resource growth.
Publish achieved local workload and CPU/memory/disk/bandwidth costs, remaining limits
and untested conditions. No production concurrency, hosting price, WAN, multi-host
or high-availability claim follows from a same-machine run. If provisional budgets
fail, fix or explicitly amend them with evidence; never lower load, widen tolerances
or relabel idle connections to obtain a PASS.

## Amendments to retained assignments

| Original owner | Added planning constraint |
| --- | --- |
| 02, 05, 06 | Graphics work is client-only behind M01's split; use the two available Windows GPUs under ADR 0010. No native Linux graphics or external hardware requirement. |
| 03, 04 | Separate server monotonic ticks from rendering/input. Client focus/minimize/pause cannot stop a shared world. Define cancellation/backpressure and explicit runtime role. |
| 07, 08 | Region/world identity, authority metadata, checked replica lifetime and global-cell coordinates precede schema commitment. Source scenes are authored content, not durable player records. |
| 09, 15-17 | Local authoring services remain distinct from public gameplay and operational admin APIs. Editor previews may visualize server/region data through authorized development adapters. |
| 10 | FeatureWorld remains; later add MMOWorld and isolated multi-process/network/database fixtures. A scene runner alone cannot test distributed ownership. |
| 11-14 | Establish dependency graphs and separate client/server content roots; static meshes are an intermediate profile. Plan async cell streaming without blocking render/simulation or importing sources at runtime. |
| 18-21 | Server-owned movement/physics/spawning and side-qualified Lua; bounded queues, identity and replays. Extend primitive-only collision for world geometry in M05 and animated characters in M06. |
| 22-24 | Pause/step applies only to offline/editor sessions. Live clients display world/session state; server never opens an audio device. Add RPG UI and chat in M07. |
| 25-29 | Preserve visual features; measure dense actors, large-world depth/precision, culling/LOD and streaming cost. Revisit depth/lighting limits only with evidence, not a speculative renderer rewrite. |
| 30, 31 | Coordinate content versions, separate server rules from client payloads, never live-reload critical server state arbitrarily. Requalify M05 streaming against final cooked cell manifests. M08 owns controlled rollout/migration/recovery. |
| 32-35 | Include MMO coverage, local M09 acceptance and M10 workload/recovery gates. Assignment 34 measures the RTX performance profile and Intel correctness/reduced-quality profile. Assignment 35 qualifies a Windows/WSL local development release through relocated packages and an isolated SDK-free server userspace; independent clean machines, remote hosting and production certification are outside scope. Original offline demos alone remain insufficient MMO evidence. |

## Still open, without blocking this planning update

Combat model, world dimensions/travel speed, content/NPC workload and precise
frame/memory/IO budgets need refinement before their owning implementation gates.
LOCAL-MMO-1 fixes the current small test population; it does not promise final
production concurrency. External hosting, additional platforms and larger-scale
capacity are separate future scope decisions rather than blockers in this plan.
Transport/auth/database/terrain/navigation dependencies require measurements and
pinning at first integration. Neither the current bootstrap nor the new documents
prove any MMO feature, scale or release support.
