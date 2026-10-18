# ADR 0005: Space, time, color and mutation

Status: accepted contract; behavior tests land with the respective systems.
Date: 2026-10-17.

MMORPG amendment: seamless open world authority, clocks and partition behavior
below are planned, not implemented. [ADR 0007](0007-mmorpg-architecture.md) and
the [MMORPG plan](../roadmap/mmorpg-plan.md) own the added M01-M10 stages.
Existing local rendering and authoring conventions remain applicable.

## Spatial conventions

- Meters, seconds, kilograms and radians internally; editor angles may display
  degrees with explicit conversion. Gravity defaults to `(0, -9.81, 0)` m/s².
- Right-handed world: +X right, +Y up, +Z backward; cameras and engine forward
  vectors face -Z. Preserve imported node transforms rather than rotating assets
  to guess a semantic model-facing direction.
- Column vectors and column-major matrix storage. `world = parent_world * local`,
  `local = T * R * S`, `clip = projection * view * world * position`. Shader
  interfaces use explicit column-major declarations and matrix-times-vector
  multiplication. Never depend on a compiler's default packing.
- Unit quaternions serialize as `[x,y,z,w]`, identity `[0,0,0,1]`; multiplication
  composes `parent * local`. Convert explicitly to APIs with constructor order
  `(w,x,y,z)`. Reject zero-length/nonfinite quaternions; normalize valid inputs.
  Interpolation uses shortest-arc slerp, accounting for equivalent `q` and `-q`.
- Local transforms are TRS, without shear. Static hierarchy matrices may produce
  world shear from rotated nonuniformly scaled ancestors; render with the full
  world matrix and inverse-transpose normal matrix. A world-space gizmo or
  keep-world reparent that cannot decompose into local TRS fails atomically with
  a diagnostic. A keep-local reparent preserves local TRS. Delete defaults to the
  full subtree through one atomic operation; removing only a parent requires an
  explicit reparent operation first. Authoring transactions/history arrive in 09.
- Nonzero negative/nonuniform scales are allowed for static renderable entities;
  mirrored determinants reverse culling and tangent handedness. Reject singular
  or near-singular scale (absolute local component below `1e-6`). At physics
  integration, initial rigid bodies are scene roots with positive uniform scale; collider dimensions bake
  that scale. Parenting, shear and negative/nonuniform rigid-body scale reject
  explicitly. Those physics restrictions are implemented with the owning physics,
  editor, command and Lua increments; 07b adds no rigid bodies.

07b implements the [Name/Transform and hierarchy contract](../transforms.md) in
EngineCore. Centrally configured GLM supplies double local translation, quaternion
rotation and scale. Inputs are finite, quaternions are normalized and canonically
signed, and every affected cached world matrix is validated before mutation.
World shear is retained; keep-world reparenting requires representable local TRS
and checks descendant matrix preservation at the fixed documented tolerance.
These matrices are in a common scene-local frame, not a global MMORPG location.
07c implements [component schemas and bounded revisioned semantic inspection](../scene-inspection.md).
Transform metadata exposes double local translation in meters, dimensionless
scale with an absolute component floor of `1e-6`, and dimensionless quaternion
values/defaults in explicit `[x,y,z,w]` order. Finite nonzero quaternion input is
normalized and canonically signed by the existing validator. Pure schema
validation checks local values; Scene separately validates world composition and
the affected hierarchy before committing. Parent links remain separate Scene
operations, and schema mutability does not grant gameplay authority.

## Seamless world positions and internal partitions

The world is logically continuous. Streaming cells and authority regions are
internal partitions, not player-visible zone boundaries or mandatory loading
screens. This does not promise an unbounded world or that every player shares
one process. M05 defines finite supported world extents and partition budgets.

Global world location remains distinct from a local Transform: world ID,
integer cell coordinates and a normalized
double-precision offset within that cell. The cell size, integer bounds, offset
range and canonical boundary rounding are versioned schema decisions with
round-trip tests. Global position calculations use this cell-plus-double-offset
representation; never convert the entire world's coordinates to a single float
vector. Spatial cells remain stable when region authority is reassigned.

07a fixes the in-memory coordinate contract at version 1: 1024-meter cells,
int64 storage bounded to [-2^31, 2^31-1], finite canonical double offsets in
[0, 1024), positive zero and checked round-to-nearest arithmetic. See the
[scene contract](../scene-contracts.md) for tested boundary rounding and failure
rules. This representation limit is not M05's supported content extent. 07b's
local TRS and hierarchy do not replace this representation or add origin mapping;
07c's local component schemas do not replace global-cell coordinates, and
serialization remains assignment 08.

Rendering and Jolt operate on bounded single-precision coordinates relative to
explicit origins. The conversion records world/cell and origin generation;
stale-origin results are rejected or explicitly transformed. Client render-origin
rebasing does not teleport authoritative entities. Server region origins and
client render origins are independent. Rebase only at an ownership-safe boundary,
updating physics bodies, queries, interpolation history and dependent references
consistently. Keep the existing Jolt single-precision selection; do not claim that
double-precision world storage alone qualifies large-world physics.

M05 must gate boundary round trips, negative coordinates, large supported cell
indices, neighboring-cell rendering and repeated rebases against declared
precision tolerances. Streaming preloads neighboring cells and overlaps interest
before handoff; publication follows validated content and authority readiness.
Movement, collision, combat queries, AI/navigation and projectiles crossing a
partition retain defined continuous behavior. Ghost/replica state has no second
mutation authority. A missing neighbor, unavailable authority or failed preload
has an explicit bounded recovery policy; silently teleporting or showing a normal
zone-loading transition does not satisfy the seamless-world gate.

## Raster and color conventions

Use finite perspective and orthographic projections with conventional Vulkan
depth: NDC z in `[0,1]`, near 0, far 1, depth clear 1 and LESS_OR_EQUAL testing.
Perspective near must be positive; all cameras require far greater than near.
No reversed depth or infinite far projection in v1. This range follows the
[Vulkan depth convention](https://docs.vulkan.org/guide/latest/depth.html).

Authored triangle winding is counterclockwise viewed from the front. Use a
positive-height Vulkan viewport and negate projection Y exactly once. The normal
nonmirrored draw then uses COUNTER_CLOCKWISE framebuffer front-face state; mirrored
transforms reverse that state. Probe visible winding, depth and UV orientation in
02/06 before adopting an NVRHI backend configuration. NVRHI must not apply a
second flip. This matches the
[Khronos projection/front-face example](https://docs.vulkan.org/tutorial/latest/05_Uniform_buffers/01_Descriptor_pool_and_sets.html).
Texture/image origin is top-left; glTF UVs preserve their defined
orientation, with any decoder normalization done once at import.

Linear scene lighting uses linear-sRGB/Rec.709 primaries with D65 white.
Base-color and emissive texture RGB decode sRGB; alpha, normals, roughness,
metalness, occlusion, depth and HDR environment values remain linear. Intermediate
scene color is RGBA16F, subject to the format gate in 02. Exposure scales by
`2^EV`. Choose luminance-preserving Reinhard for v1: with nonnegative exposed RGB,
`L = dot(rgb, [0.2126,0.7152,0.0722])`, output `rgb / (1 + L)`, then clamp to the
SDR output gamut. This is a deliberate bounded first tonemapper; its limitations
and bright saturated reference cases are visible in 25. Guard nonfinite values.
Apply exact sRGB output transfer once, through an sRGB attachment or explicit
encoding as established in 02/25. Never both. HDR monitor output is future scope.

The ordered pass list is shadow depth, camera depth/normal, SSAO and edge-aware
filter, opaque/masked forward HDR shading with direct light plus IBL, sky, sorted
transparent HDR composition, exposure/tonemap, linear display-referred HUD/editor
composition, and final sRGB presentation. AO modulates indirect diffuse/occlusion
terms, never emissive or the entire final image. Overlays are not exposed or
tonemapped again. No temporal AO/history in v1. Details and visual tolerances land
in 25–29; 00 establishes no visual baseline.

## Simulation, network and rendering clocks

03c implements the reusable scheduler and bounded completion primitives below;
see the [time/work contract](../time-and-work.md) and
[handoff](../handoffs/03c-time-and-work.md). These have real CPU callback/thread
fixtures; world simulation, replication and persistence remain planned. The
Server bootstrap uses only the injectable idle clock until those services land.

The planned authoritative server uses a configured fixed timestep, provisionally
20 Hz (`1/20` second), with integer tick indices and a monotonic scheduling clock.
This is an initial tuning target, not a chosen combat model, capacity claim or
final performance promise. Change the rate only through a validated world/session
configuration that clients can discover; never enlarge individual timesteps to
catch up. Physics substeps, if needed, are an explicit configuration measured in
M04. Render rate, input capture and network snapshot frequency are independent
from server tick frequency. Client timestamps are not authoritative time.

Server scheduling is independent of a window or render iteration. It never
silently discards authoritative ticks, advances tick indices over unexecuted
simulation, or freezes because a client pauses, minimizes or loses focus. Each
scheduling pass has a bounded catch-up budget. Remaining debt is measured and
retained while within the configured bound; excess debt triggers an explicit
overload policy with admission limits, reduced optional replication work or
controlled unhealthy-region shutdown/recovery. It cannot grow without bound or
be hidden by dropping time. Assignment 03 establishes controllable-clock tests, queue/debt
limits and overload diagnostics; M10 qualifies the selected budgets under load.

Simulation-duration effects use authoritative ticks. Durable real-time deadlines
that must survive downtime use a separately specified server-time policy with
clock-skew and restart tests in M03/M07. Wall-clock adjustments never directly
change tick ordering or let clients extend a cooldown. Deterministic tests advance
explicit ticks; cross-platform floating-point bit identity is not promised.

The original 60 Hz accumulator remains an explicit local/offline profile only:
clamp incoming wall delta to `[0, 0.25]` seconds, run at most 8 ticks per render
iteration, report/discard overdue whole ticks and retain the sub-tick remainder.
An offline world using the MMORPG simulation profile instead uses its configured
tick rate; it must not silently inherit two different gameplay timesteps. The
offline 60 Hz profile is not the server scheduler or a multiplayer prediction
clock. Local interpolation alpha is `remainder / fixed_dt` in `[0,1)`.
Interpolate previous/current translation and rotation for presentation only;
never write interpolated poses back to simulation. Static/kinematic poses come
from validated scene/target state and dynamic body poses from physics.

Clients render buffered authoritative snapshots, with interpolation timing
independent of server execution. M04 defines bounded local movement prediction,
input sequence acknowledgements and correction/replay of still-valid inputs.
Prediction cannot grant authoritative position, collision, damage, inventory or
rewards. Reconciliation updates client presentation/simulation state against the
server result; render interpolation is never written back into server state.
Teleports and authority/origin changes reset or transform interpolation history
explicitly. Snapshot gaps and delayed/out-of-order updates have bounded buffering
and resynchronization behavior, tested under packet loss and jitter.

Pause, one-tick step and discard-on-stop apply only to a local/offline editor
play world. Minimize may suspend presentation; in an online session it never
pauses the shared world. Focus loss clears held inputs and sends the defined
neutral/released input state; local pause-on-focus-loss defaults on only offline.
Reconnect does not rewind or restore the server from client state. Stopping a
client closes that session; it does not discard the authoritative world.

04a implements local held-control release and cancellation of unconsumed input on
focus loss in the [client input contract](../input-contracts.md). It never changes
a scheduler or sends a network message. 04b connects native focus/iconify callbacks
through the [owned window](../window-lifecycle.md). 04c adds
`OfflineControl` to the existing Core scheduler and an EngineClient
[`OfflineFrameLoop`](../frame-loop.md). Pending one-shot input survives frames
with zero ticks and is consumed once by the first catch-up tick; later ticks get
held/cursor state. Presentation retains an independent current-frame snapshot.
Focus cancellation removes older pending positive actions before accepting any
fresh post-regain input. Text and scroll remain bounded across captures.

Manual, unfocused or iconified offline pause admits no elapsed time and clears
whole/fractional time debt. The first resumed pass executes zero ticks and anchors
fresh elapsed time. Explicit single-step requires manual pause and an active
window, executes one successful tick with zero interpolation alpha, and remains
paused. Paused interval positives do not replay; a step consumes only current
positive actions with latest held state and pending releases. Existing callback
fault, cancellation and tick-index rules remain in force. These are compatible
extensions to the offline C++ scheduler API; the original unpaused poll overload
retains its behavior. There is no persistent-format or wire-protocol change.

The native sample reserves F6/F7 for pause/step and reports diagnostic JSON lines.
These controls are not authoring commands, action bindings or server gameplay
messages. No world simulation is implemented. 05b's
[clear presentation lifecycle](../presentation-lifecycle.md) uses this same
offline pump with independent GPU submission; it adds no Server authority. Final
04c local qualification is PASS in the [handoff](../handoffs/04c-frame-loop.md);
neutral network intents remain M02/M04.

## Simulation ownership and mutation phases

Every authoritative region has one designated simulation owner/writer. An initial
headless world can run on the process main thread; later co-located regions may
have separate owner threads. This is not a global requirement that every region
share the process main thread. GLFW initialization/window events stay on the
process main thread. Local editor/play mutation remains on its controlling main
thread. A per-region Lua VM and physics state belong to that region's owner;
multiple region owners never concurrently call the same VM or mutate shared ECS
storage. Cross-region effects use bounded messages and explicit ownership fences.

Each tick: validate and commit eligible queued intents/completions for the current
authority; initialize new entities in stable persistent-ID order (resolve
hierarchy/components, create physics bodies, then initialize scripts); latch
validated input for this tick; invoke server script `fixed_update`; apply body
controls; step/join physics; synchronize authoritative dynamic poses; dispatch
buffered contact/trigger events in documented stable pair/event order; then commit
deferred structural changes. Input sequencing, cutoff and deterministic queue
ordering are specified in M02/M04, not inferred from packet arrival races.
Publish immutable replication/render observations after the commit boundary.

The implemented 07b scene owner is a CPU mutation foundation for these later
simulation phases. Its bounded FIFO owns deferred Name/Transform/reparent inputs
and applies each operation at the explicit boundary, with its own outcome and one
revision increment for an actual change. Visits permit inspection and enqueueing
only; failed visitors cancel their own newly queued requests. This is not 09's
atomic multi-operation authoring transaction or a server gameplay scheduler.
07c's bounded queries read committed state on that same owner thread, including
during visits or while deferred work is pending. Owned immutable pages survive
later mutation, but their references may become stale. Actual committed changes
invalidate continuation revisions; enqueueing or a no-op does not advance them.
07b destruction takes effect when its operation commits; the script-specific
pending-destruction behavior below belongs to the later script lifecycle.

Script removal requests mark handles pending destruction immediately; subsequent
access fails while storage removal and destruction callbacks wait for the safe
boundary. New spawns do not recursively run the phase that spawned them. A script
runs at most once per applicable callback phase. Destruction invalidates lookup
before releasing physics, VM and deferred presentation references. Physics
workers never invoke Lua or commit authoritative mutations.

Frame `update` is presentation-only in the MMORPG client. It cannot mutate
authoritative gameplay or run server combat/economy logic. Input capture may queue
an intent for a future server tick. The local/offline profile may retain its
documented next-tick body controls, but those calls are not an online authority
grant. Render/audio consumers operate on immutable snapshots or checked handles;
scripts cannot retain raw component pointers. GPU submission/retirement initially
remain on the client controlling thread, independently of server ownership.

Authoring transactions validate scene revisions and commit only in the trusted
local authoring world; they are not gameplay messages. Paused editor edits can
commit without advancing simulation. Network, storage and worker results carry
world/region incarnation, authority epoch and request/operation context as
applicable. Owners reject stale results after unload, transfer or restart. Region
handoff uses the identity/fencing protocol in [ADR 0004](0004-formats-and-identity.md),
including prepare, commit and abort/recovery; it cannot leave two committing
owners. M05 proves co-located handoff first, including overlapping interest,
preloaded neighbors and cross-boundary combat/AI/projectile continuity.

Server drain stops admission, resolves or rejects bounded pending work, persists
required committed state and invalidates the exiting authority before teardown.
Disconnect, region unload and process shutdown are distinct lifecycle events;
none implies deleting durable character state. M01/M03/M05 define deadlines,
failure results and recovery behavior. No automatic restart is a substitute for
proving that old callbacks, packets or persistence completions cannot reappear as
valid work in a new incarnation.

Owners: 03/04 retain local clocks/window work; 07/09 retain scene and authoring
mutation; 18–22 retain local physics/script/play lifecycles; 24 and 25–29 retain
audio/render conventions. M01 owns the headless build/lifecycle boundary; 03 owns
the reusable headless scheduler and ownership primitives. M02 owns session and
message boundaries, M03 durable lifecycle, M04 authority/prediction, M05 seamless
partition/streaming/navigation behavior, M06 animated actors, M07 the RPG slice,
M08 live operations, M09 MMO acceptance and M10 measured load qualification.
The 03c clock/scheduler/queue primitives have their own executable gates. Remaining
server gameplay, network, persistence and partition behavior stays planned until
its owning gates pass.
