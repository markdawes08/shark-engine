# ADR 0005: Space, time, color and mutation

Status: accepted contract; behavior tests land with the respective systems.
Date: 2026-10-17.

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
  full subtree through one transaction; removing only a parent requires an
  explicit reparent operation first.
- Nonzero negative/nonuniform scales are allowed for static renderable entities;
  mirrored determinants reverse culling and tangent handedness. Reject singular
  or near-singular scale (absolute local component below `1e-6`). Initial rigid
  bodies are scene roots with positive uniform scale; collider dimensions bake
  that scale. Parenting, shear and negative/nonuniform rigid-body scale reject
  explicitly. This restriction is checked in editor, commands and Lua.

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

## Simulation and rendering clocks

Fixed simulation timestep is exactly `1/60` second, represented by an integer
tick count and double-precision accumulator. Use a monotonic wall clock. Clamp
each incoming wall delta to `[0, 0.25]` seconds, perform at most 8 ticks per
render iteration, discard remaining whole overdue ticks, retain the sub-tick
remainder, and report dropped time. Never enlarge the physics timestep to catch
up. Deterministic tests advance explicit ticks and bypass wall-time dropping.

After ticks, render interpolation alpha is `remainder / fixed_dt` in `[0,1)`.
Interpolate previous/current physics translation and rotation for rendering only;
never write interpolated poses back to simulation. Static and kinematic poses
come from validated scene/target state; dynamic body poses come from physics.
Teleports reset both interpolation endpoints. Frame rendering has no authority
to advance physics. Cross-platform floating-point bit identity is not a promise.

Pause freezes ticks, game time and callbacks; UI/event processing and rendering
continue. A step command runs exactly one fixed tick and renders its resulting
state. Minimize suspends swapchain work and freezes local gameplay (no networking
in v1); restore resets wall-time sampling. Focus loss clears held inputs. Focus
loss alone does not advance or synthesize input; project pause-on-focus-loss
defaults on. Stop discards the play scene and clears accumulated time.

## Main-thread mutation phases

Each tick: commit queued validated changes; initialize newly created entities in
stable persistent-ID order (resolve hierarchy/components, create physics bodies,
then initialize scripts); latch input for the tick; invoke script `fixed_update`;
apply validated body controls; step/join physics; synchronize authoritative dynamic
poses; dispatch buffered contact/trigger events in documented stable pair/event
order; then commit deferred structural changes. Script callback removal requests
mark handles pending destruction immediately, so subsequent access fails, but
storage removal and destruction callbacks occur at the boundary. A script runs
at most once per applicable callback phase; new spawns do not recursively run the
phase that spawned them. Destruction invalidates lookup before releasing bodies,
VM state and deferred render/audio references.

Once per active rendered frame after fixed ticks, call script `update` with the
bounded frame delta for visual/gameplay presentation work, commit queued edits,
then extract an immutable render snapshot. Physics-affecting controls from this
callback apply at the next fixed tick. Reproducible game logic and injected input
use fixed ticks; frame `update` is not promised render-rate determinism. Scene
edits while paused commit at an application boundary without advancing time.
Lua never executes on physics workers. Scripts cannot keep raw component pointers.

Authoring transactions commit on that same controlling thread, before simulation
or extraction, and validate scene revision atomically. Render/audio/import workers
receive immutable data or handles and return bounded messages tagged with scene
generation and request revision; stale results are discarded with job diagnostics.
GPU submission and retirement initially remain on the controlling thread. Worker
counts and queue bounds are introduced only with the subsystem that needs them.

Owners: 03/04 implement clocks, 07/09 mutation and hierarchy, 18–22 physics and
script lifecycles, 24 audio threading, and 25–29 renderer conventions. Their
tests must cover boundaries, callback ordering and invalid inputs, not just the
nominal update path.
