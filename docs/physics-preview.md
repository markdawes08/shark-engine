# Shared physics preview

[ADR 0049](architecture/0049-editor-physics-preview.md) connects the native
Editor and public terminal to one retained CPU preview inside the existing
project owner. `scene.run` remains static integrity checking. Preview implements
fixed-step rigid-body simulation, not Lua or gameplay execution.

## Source and runtime ownership

Start requires the project's declared startup Scene, an exact source revision,
an explicit Camera and a client target. It reuses current export preparation: all visible or hidden mesh references require
built-ins or fresh client-permitted imported assets and explicit catalog inventory.
Preparation never imports implicitly. Complete prepared bytes and immutable mesh
assets load into a fresh Core Scene; bodies are created before one simulation
lease is acquired. Failed admission destroys the candidate without changing source.

Only one preview may be active. Source mutation, pure authoring preview and
reload reject until Stop. Source reads and explicit save remain available; runtime
steps never change authored transforms, revision, dirty state, history or files.
Runtime body removal detaches RigidBody but preserves the visual entity. Stop
discards runtime state; starting again reconstructs the authored bodies and poses.
There is no merge or apply-play-changes operation. Closing the project destroys
the preview before assets, authored state and the exclusive project writer.

The fixed offline profile is 60 Hz, one collision substep, 128 bodies, 512 native
body pairs, 256 contact constraints, 64 observed contact pairs and 128 queued
events. Existing conservative arena admission still applies; these individual
limits do not promise every worst-case combination is admitted. Operations step
1..8 ticks and retain at most 1024 owned contact observations. Each successful
tick drains events on the owner thread. A fault is not native rollback; the
receipt preserves completed ticks, earlier events and the original error.

## Terminal operations

Discovery advertises `physics_preview: true` and one additive method,
`scene.physics`. Its `params` object requires `op` and rejects unknown fields.
64-bit counters use canonical unsigned decimal strings; UUIDs are engine-owned
stable identity values. No Jolt type or native body handle is exposed.

| `op` | Other parameters | Result / fence |
| --- | --- | --- |
| `status` | None | Owner scope, preview generation, active session/source revision/tick, next sequence, fault/body/entity counts, limits and retained last start/operation |
| `start` | `owner_scope`, `expected_generation`, `expected_revision`, `camera`, `target: {id, side:"client"}` | Current observed owner/generation and source revision required; returns fresh `session_id`, generation, source revision, tick `"0"` and next sequence `"1"` |
| `step` | `session_id`, `sequence`, `expected_tick`, integer `ticks` 1..8 | Exact active scope and next operation; returns original success/error, completed tick, next sequence and owned events |
| `remove` | `session_id`, `sequence`, `expected_tick`, `entity_id` | Same operation fence; removes only that runtime body and returns owned removal exits without advancing tick |
| `query` | `session_id`, `expected_tick`, optional integer `offset`/`limit` | Exact tick; bounded current entity values including body settings, with `next_offset` and `complete`; limit 1..128, default 64 |
| `stop` | `session_id` | Exact active scope; destroys runtime, returns original final tick and inactive receipt |

Start retains its original exact input and receipt. Repeating that start returns
the original receipt even after Stop; it cannot revive a stopped runtime. A new
start needs the latest observed generation and receives a fresh session UUID.
An old authored owner scope rejects across project close/reopen.

Step/removal share a separate consecutive sequence because removal does not
advance time. One last immutable operation is retained. An exact repeat returns
its original receipt/events without repeating native work; changed input under
the same sequence, expired sequences, gaps and stale expected ticks reject.
Observe `status.last_operation` after a lost reply and reconcile the original
identity. A serialized `success:false` is a known retained operation outcome,
including any partial tick progress. Do not replace it with a fresh operation
under an assumed rollback. A faulted preview permits status/query/stop, not more
steps. Stop replay is safe while inactive; an old stop cannot stop a newer session.

Events have sequence, tick, enter/exit phase, simulation/body_removed cause and
two owned `{entity_id,is_trigger}` participants scoped by the enclosing preview
session. They are observations at the existing native contact stage, before final
integration, not gameplay callbacks or geometric queries of final poses.

## Human controls and rendering

The inspector exposes all eleven existing RigidBody fields through typed drafts
and the retained shared batch owner. Body removal precedes transform changes
when needed so intermediate constraints cannot prevent a valid final edit; the
final body is validated in that same atomic history group. Invalid edits retain
the draft and leave source/history unchanged.

Play/Pause/Step/Stop and runtime body removal use the same `scene.physics`
operations. The portable controller owns only observations, exact unresolved
input and the existing OfflineScheduler. Blocked focus/modal/minimize intervals
admit no time; resumption resets the offline anchor. Single step while paused
executes one fixed tick and shows its current pose. Ambiguous completion pauses
play until original-input reconciliation or scoped Stop.

Typed viewport requests can select `physics_session` and `physics_alpha`.
Extraction uses its frozen assets and interpolated runtime poses before subtree
bounds/culling. The source revision/camera/target must match the preview's capture.
Snapshots carry a fresh runtime incarnation and physics tick marker; picking and
authoring gizmos cannot use them as source authority. Returned immutable render
assets remain owned through GPU completion even if the preview stops first.

CPU shared-owner/adapter and public-process fixtures establish protocol,
isolation, retries and lifetime behavior. Actual motion/input/focus/rendering and
bidirectional human/terminal saved-project checks follow
[physics acceptance](physics-acceptance.md) on both current-PC GPUs.
