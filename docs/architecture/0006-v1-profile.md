# ADR 0006: Shark MMORPG release profile and intermediate client milestones

Status: accepted intended scope; none of these engine features is implemented in
assignment 00. Date: 2026-10-17.

Amended 2026-10-17 for the user's seamless open-world MMORPG direction.
[ADR 0007](0007-mmorpg-architecture.md) and the
[execution plan](../roadmap/mmorpg-plan.md) add mandatory M01-M10 gates. Original
client features below remain required; static assets, primitive collision and
the minimal HUD are intermediate milestones rather than the complete MMO profile.
The later [current-machine scope](0010-current-machine-scope.md) limits acceptance
to the Windows client/editor on this PC and its WSL headless Server. Here
"release" means a local development release candidate, not a public service or
qualification on unavailable platforms. LOCAL-MMO-1 replaces the former large
load target; correctness and feature contracts below remain required.

## Required release behavior

Shark's release contains a client runtime and editor on the separately qualified platform rows
in [0003](0003-platforms.md), static glTF scene assets, authoring and transform
gizmos, HDR forward PBR, HDRI IBL, filtered shadows, quality SSAO, rigid-body physics,
Lua behavior, prefab/entity spawning, input, a small HUD, audio, standalone export,
and agent authoring through shared local commands. It additionally requires a
headless authoritative server, authenticated multiplayer, persistent characters/
economy, seamless world streaming and authority transfer, animated characters,
navigation/AI, a small MMORPG reference module and operational recovery. The
[requirements manifest](../coverage/requirements.json) assigns the retained 00-35
and inserted M01-M10 increments to observable gates. Existing Tetris/physics samples
remain useful tooling checks, but only M09/M10 qualify the MMO experience and load.

Human editor and terminal LLM development are equally required under
[ADR 0008](0008-human-agent-authoring.md). The terminal workflow needs no editor
window, provider SDK or MCP client; the human can open and continue its projects.
Shared schemas/services, structured diagnostics, conflict-aware edits and public
tool examples are release requirements. Device-dependent capture/audio evidence
remains separately qualified, and no authoring interface exists in the bootstrap.

FeatureWorld grows from 10 onward with each component, command and Lua entry
point. Isolated error fixtures cover incompatible or destructive cases. Non-scene
features such as installation use named external fixtures. Feature/API manifests
must map implemented entries to real automated tests and exercises as they land;
planned requirements do not constitute implementation coverage.

## glTF import and material profile

The assignment-12 subset is **static glTF 2.0**, not full glTF support. M06 extends
it with the bounded character profile below before an MMORPG release. The
[Khronos specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)
is the semantic reference; the following limits are Shark product decisions.

| Area | v1 choice and error behavior |
| --- | --- |
| Containers | `.gltf` and `.glb`; local external buffers/images, embedded buffer views and bounded data URIs; no network URI resolution |
| Scene graph | Import the selected/default scene and static node transforms; accept TRS and decomposable matrix nodes, reject local shear/singular matrices with node context |
| Geometry | Indexed or nonindexed triangles; expand triangle strips/fans deterministically; reject points/lines and invalid indices/counts/offsets; support sparse accessors |
| Vertex data | Positions, normals, tangents, UV0/UV1 and COLOR_0; missing normals generate flat normals; normal-mapped meshes without tangents generate a documented tangent basis in 12; reject missing referenced UV sets and texture-coordinate sets above 1 |
| Images | PNG/JPEG for glTF, with checked dimensions/decoded byte counts; preserve sampler filtering/wrap, generate mip chains; missing/invalid referenced images fail import without replacing the last valid asset |
| Core materials | Metallic-roughness, base color, normal/scale, emissive, occlusion/strength, factors and samplers; all OPAQUE/MASK/BLEND modes, alpha cutoff and double-sided rendering preserved |
| Extensions | Supported rendering/import extension list is initially empty. Any unsupported `extensionsRequired` entry fails with its name. Optional extensions emit diagnostics and use a valid core fallback; absent fallback is an error |
| Unsupported content in 12 | Skins, animations and morph targets fail import explicitly until their declared support lands. M06 adds the skin/TRS subset below; morphs remain unsupported. No Draco, meshopt, KTX2/Basis, transmission, clearcoat, specular-glossiness or other extension functionality is implied by cgltf parsing |
| Additional nodes | Static mesh hierarchy is required. Camera/light import is not part of this profile; present camera nodes or optional light extensions produce explicit diagnostics, and users author engine Camera/Light components separately |

Import limits apply before allocation, including encoded/decoded lengths, accessor
arithmetic, nesting and asset count. Assignment 12 fixes concrete limits and uses
licensed offline valid/malformed fixtures and an independent validator. Negative
and nonuniform scale follow [0005](0005-simulation-and-rendering.md). Import
success only claims data support; 13's temporary unlit renderer cannot establish
PBR correctness, which belongs to 25–29.

M06's initial character profile supports skins with at most 128 joints per skin,
four influences per vertex (`JOINTS_0`/`WEIGHTS_0`), validated inverse-bind matrices,
finite normalized nonnegative weights and bounded TRS animation clips on nodes/
joints. Support STEP and LINEAR interpolation (quaternion shortest-arc rotation);
reject CUBICSPLINE, additional influence sets, morph channels and unsupported
required extensions explicitly. Pin tested import/cook limits before claiming
support. Provide clip looping, transitions/blending, attachments and animation LOD.
Server-authoritative movement drives actor displacement; visual root motion and
animation events do not grant movement, damage or rewards. Retargeting and morphs
remain outside the first release profile. Any expansion requires fixtures and a
versioned compatibility decision, not silent parser-driven support.

## Lighting and image quality

Use metallic-roughness GGX microfacet PBR with a documented energy-conserving
diffuse/specular model, directional/point/spot lights, environment diffuse
irradiance, prefiltered environment specular and a BRDF lookup. Initial authoring
limits are 4 directional, 256 point and 32 spot lights; reject excess active lights
with a diagnostic rather than silently omitting them. Assignment 26 measures
these limits and proposes any needed contract amendment before changing them.

Opaque/masked draws write depth; masked shadow depth uses the same cutoff.
Blended draws sort back-to-front by world-space bounds center in camera depth,
with persistent ID breaking ties, use straight-alpha material values and
premultiplied shader output/composition, and do not write depth. Intersecting or
self-overlapping translucent geometry is a documented sorting limitation. Blended
objects receive lighting/shadows but cast no v1 shadow and do not contribute to
the opaque SSAO depth surface. Double-sided materials adjust back-face shading.

One selected directional light casts four cascaded shadow maps; up to eight spots
cast shadow maps. Store the four cascades and eight spot maps in one D32 texture
array with up to 12 layers and separate per-layer render passes. Layers use
512/1024/2048-pixel quality settings, default 1024; the maximum fits the platform
profile. One sampled-array binding keeps material, IBL, AO and shadow inputs
within the 16-image stage budget, without geometry shaders or descriptor indexing.
02/06 must probe rendering and sampling all array layers. Point lights and other
directional lights are unshadowed.
PCF filtering with stable cascades, bias and transitions is required; PCSS and
contact-hardening penumbrae are outside v1. Quality/resolution controls serialize
and are shared by editor and agent commands. Exceeding the shadow budget rejects
the setting rather than silently dropping requested shadow casters.

SSAO uses a GTAO-style horizon evaluation with depth/normal reconstruction,
edge-aware spatial denoising, radius/quality controls and a clean disabled path.
No temporal accumulation is selected, so no incomplete motion/history mechanism
is implied. Reference cases must establish contact quality, halos, thin geometry,
camera-motion stability and GPU cost. HDR lighting outputs SDR through the
defined exposure/tonemapper; HDR display output is not required.

Environment import supports Radiance `.hdr` equirectangular images; EXR is outside
the initial profile. Cook/cache sky/irradiance/specular/BRDF data and ship a pinned,
licensed Poly Haven reference with origin metadata; startup needs no network.

## Gameplay and authoring limits

- Jolt rigid bodies in 18/20: box/sphere/capsule colliders; static/dynamic/kinematic modes,
  gravity, layers, triggers, raycasts/overlaps and safe queued contacts. No mesh
  colliders, joints, vehicles, cloth or soft bodies in that initial milestone.
  M05 must add validated static terrain/world collision and tiled navigation with
  bounded asynchronous NPC pathfinding before seamless-world acceptance. Rigid
  bodies use bounded local origins; global float world space is not the contract.
- Lua scripts: checked entity/component access, logging, math/time, lifecycle,
  physics, input, spawning, audio and documented material controls. Enable base,
  table, string, math, utf8 and coroutine facilities under engine lifecycle limits;
  omit `io`, `os`, `debug`, native module loading and unrestricted filesystem APIs.
  Server gameplay and client presentation use distinct allowed capabilities and
  cooked module roots; remote clients cannot invoke arbitrary scripts or grant
  authoritative outcomes. A project-scoped module loader is the only `require` route. Memory/instruction
  budgets and errors stop the affected behavior with diagnostics. This is not a
  security sandbox for untrusted native or script code.
- Prefabs: one level, stable local references, per-instance component-field
  overrides and ID remapping. Nested prefab references fail with a diagnostic;
  no generalized inheritance or implicit apply-back from play to edit state.
- Input: keyboard/mouse actions, rebind-ready maps, deterministic tick injection,
  focus routing. HUD: licensed font, simple text/score/pause/restart/game-over,
  resize-aware layout in Runtime; no ImGui runtime dependency. M07 adds character
  selection, health/target/action UI, inventory/equipment, party, bounded chat and
  connection/recovery state using the public engine/game-module interfaces.
- Audio: WAV and Ogg Vorbis source import, cooked PCM payloads, play/pause/stop,
  looping/volume and distance attenuation; one active listener and a bounded
  voice pool. Assignment 24 chooses and tests its concrete voice limit and
  device-change policy. No streaming music or advanced DSP requirement.
- Authoring: shared transactions, undo/redo, inspector/hierarchy/asset browser/
  console, local/world gizmos and snapping, play/pause/one-tick-step/stop,
  reload/recovery, local CLI/JSON-RPC discovery and project-scoped text editing.
  Add a thin MCP adapter in 15 if required by the chosen agent environment.
- Export: local Windows client/editor and WSL Server packages, reproducible final
  cooked content, dynamic asset roots, licenses and relocated dependency-isolation
  checks on the available machine, following ADR 0010.
  M08 separates client and headless server packages/content; server rules and
  credentials cannot enter client exports. Cross-host toolchain/export support
  is not implied.

Audio decoding dependencies must be selected consistently with
[0002](0002-dependencies.md); Vorbis decoding is offline in EngineTools, and
Runtime consumes engine-owned PCM. Source imports stay out of distributed games.

## Explicit future scope and release evidence

Networking, skeletal characters and streamed terrain/world data are now mandatory
MMORPG scope in M01-M10, replacing the original exclusions. Morph animation,
advanced retargeting, ray tracing, vehicles, soft bodies and a marketplace remain
future projects. Mac compatibility and a native Metal backend are outside the
current scope under [ADR 0009](0009-windows-linux-platform-scope.md). No exclusion
removes the retained client features or the new authoritative server, persistence
and seamless-world gates.

Assignment 33 must have a fresh agent create, save, reopen, test, capture, export
and launch a playable Tetris-style game through documented public tools only,
with movement/rotation, line clearing, scoring, pause/restart, game over and sound.
A separate 3D physics sandbox proves collision/spawning/audio behavior. Assignment
34 sets quantitative frame-time/memory budgets from named hardware measurements
and enforces them; no performance figure is claimed in 00. Assignment 35 requires
relocated local installation/launch, isolated WSL Server closure, notices, samples,
references and archived build inputs/symbols on the current baseline. Public
signing certificates and independent physical machines are outside current scope;
content-trust behavior uses explicit local development keys and tamper rejection.
It also depends on M08-M10's server packages, multi-client world acceptance,
capacity/soak, recovery and operational evidence. The provisional targets in
ADR 0007 are not observed performance or a claim of production readiness.
