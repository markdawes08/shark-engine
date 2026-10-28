# ADR 0035: Offscreen scene viewport through the existing device

Status: accepted. Date: 2026-10-28. Owner: 13d, partial REQ-13.

Core's immutable render snapshots now feed an EngineClient CPU packing boundary
and the existing GraphicsDevice. One device owns one persistent offscreen viewport:
linear RGBA8 color, D32 depth, staging readbacks and framebuffer. Same-size frames
reuse those five target handles. Resize creates a complete candidate target and
frame before replacing the idle old target. The existing Vulkan/NVRHI device,
queue, timeline waiter, diagnostics and bounded destruction rules remain owners;
there is no second renderer backend, queue or scheduler.

Pack each distinct prepared primitive once per frame, then reference its index
range from bounded ordered draws. Use an explicit precompiled scene shader
profile: position/color vertices and an 80-byte column-major MVP/base-color
constant block. Preserve the existing textured-cube profile and bundle unchanged.
The already-flipped projection receives no second shader Y inversion. Draws
share one clear and depth attachment, use conventional LESS_OR_EQUAL depth, and
select ordinary/reflected/double-sided raster state from extracted parity and
material values. The [viewport contract](../scene-viewport.md) fixes bounds.

This initial path uploads geometry/constants and builds pipelines per call; only
targets persist. It returns synchronous owned readback after a one-second bounded
completion observation. It is useful for deterministic development and inspection,
not a frame-throughput or MMO-capacity claim. Persistent mesh upload/pipeline
caches and direct interactive presentation are not implied by this checkpoint.

Install frame ownership before recording/submission. A timeout retains frame and
target; no new offscreen/presentation work is admitted until explicit drain.
Unresolved recording/submission follows the existing fatal-retention policy.
Candidate creation failure leaves the previous target usable. Close retires idle
targets before NVRHI. Resource counts describe engine handles, not driver memory.

Material behavior is temporary opaque unlit vertex color times base-color factor.
Alpha is written as a channel without blending or alpha testing. No textures,
PBR, lighting, normal-map shading or final transparency quality is claimed.
Normal/tangent data remains in prepared assets for later owners. CPU Tools stays
independent of Client; an explicit external GPU fixture qualifies this API.
SceneRenderer initialization is never inferred from CPU tests. Runtime's authored
scene file launch and connected acceptance remain 13e, export/cooking 14/31.
