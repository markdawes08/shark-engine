# Offscreen scene viewport

13d adds [scene_viewport.hpp](../engine/client/include/shark/client/scene_viewport.hpp)
and `GraphicsDevice::render_scene_offscreen` in EngineClient. It consumes the
existing [CPU extraction](scene-rendering.md) without borrowing a mutable Scene.
[ADR 0035](architecture/0035-offscreen-scene-viewport.md) records the ownership
decision. Runtime file launch/connected scene acceptance remains 13e.

## Preparation and shader profile

`make_scene_viewport_request(snapshot, vertex_shader, pixel_shader)` is CPU-only.
It preserves snapshot incarnation/revision and the complete origin value, packs
each distinct immutable primitive once, and emits ordered draws sharing index
ranges. Positions/color become a 28-byte vertex; each draw carries a column-major
float MVP, base-color factor and combined model-XOR-camera reflection. Double-sided
material values disable backface culling. No vertex transform or global-cell
calculation occurs on a worker or outside the existing snapshot boundary.

MVP composition uses Core doubles before checked float conversion. Nonfinite,
overflowing and nonzero subnormal/underflowing matrix coefficients reject.
The float projection must retain distinct finite near/far planes and a positive
perspective near plane. Reconstructed float clip distances must stay within
`1e-6 + 1e-3 * abs(double_distance)` of the double projection. Shader clip
products/sums must also remain finite, using uploaded coordinate maxima and an
eight-float-epsilon arithmetic margin. Remaining rounding is ordinary float precision, not a
promise to reproduce double clip boundaries exactly. Matrix conventions remain
column vectors with `projection * view * world`, Vulkan depth [0,1] and one
projection Y flip. The native viewport compensates NVRHI's backend inversion to
produce positive Vulkan viewport height; the scene shader does not flip Y again.

Build [scene_recipe.json](../shaders/scene_recipe.json) through the existing
[offline pipeline](shader-pipeline.md). `scene_unlit.hlsl` uses `vsScene`/`psScene`,
position float3 and color float4 inputs, one color interpolant, and constant
buffer slot zero: matrix offset 0/stride 16 plus float4 factor offset 64. The
pixel shader has no resource binding. Use `ShaderInterface::scene_unlit` explicitly
when loading/decoding those artifacts. The native bounded interface reader
validates the emitted DXC layout. Default shader APIs still select the original
textured-cube profile; the two layouts reject interchange. Runtime never invokes
a compiler; artifact identity and independent spirv-val checks remain offline.

Public requests own mutable values, so the device revalidates shader profile,
geometry, factors, indices, draw ranges, metadata and budgets before native calls.
Empty draw/vertex/index arrays are valid and clear/read back the target. Source
assets and extracted snapshots can be released after packing; submitted GPU owners
do not borrow their arrays. This is a local rendering request, not a gameplay or
authoring protocol, and metadata grants no authority over a Scene.

| Budget | Limit per request |
| --- | --- |
| Target dimensions | 1..2048 each; minimized/zero views must skip rendering |
| Draws | 4096 complete indexed-triangle ranges |
| Unique packed vertices | 262144 |
| Packed indices | 786432 |
| Submitted indices across all draws, including repeated instances | 3,145,728 |
| Shader artifact | Existing 4 MiB per stage / bounded SPIR-V reader |

Geometry budgets are charged before output allocation; repeated instances share
geometry but still consume draws. Positions retain the extraction range of
+/-65536 meters. Colors/factors must be finite in [0,1]. Every index addresses an
uploaded vertex; each draw range starts and ends on a triangle boundary.
These are admission bounds, not frame-time targets or process-wide memory caps.

## Device, viewport and completion

The existing main-thread GraphicsDevice owns one scene viewport. The first frame
creates five persistent handles: RGBA8 UNORM color, D32 depth, two staging
textures and a framebuffer. Same-size rendering keeps their generation. Resize
constructs all replacement target/frame resources before changing the current
target. Creation failure leaves old extent/generation usable. Valid resize retires
the completed old target once; `close` retires the final target before NVRHI.

Each call creates bounded transient shared vertex/index buffers, per-draw 80-byte
constants/bindings, up to three raster pipelines and one command list. The command
list clears color to black/alpha one and depth to one, draws every primitive into
the same depth surface, then copies color/depth to staging. Depth uses LESS_OR_EQUAL
with writes; opaque draw order cannot overwrite nearer geometry with farther
geometry. The unlit profile writes `vertex_color * base_color`, including alpha,
without blending, alpha testing, textures, lighting or PBR.

The existing one-second timeline wait proves completion before CPU mapping.
Successful readback owns tightly packed top-left RGBA8 linear bytes and D32 floats,
the queried subpixel precision, submission serial, draw count, scene/origin
metadata and viewport statistics. Readback creation/retirement counts describe
transient frame handles only; `live_target_resources` separately reports five
persistent handles. Generation/retired-generation and successful readback-frame
counters remain inspectable after close. These counts do not measure driver
allocations. Geometry uploads and pipelines are currently rebuilt per frame;
this synchronous diagnostic path makes no interactive performance claim.

Timeout or failed readback retains frame/target owners. Pending scene work blocks
new scene, diagnostic offscreen and presentation submissions; callers explicitly
`drain` before retrying. Drain does not replay the old request or publish a lost
readback. Ambiguous recording/submission retains resources and uses the existing
fatal shutdown policy if bounded close cannot prove safety. No queue-idle spin,
second completion service or host settings change is introduced.

Readbacks identify the snapshot actually requested. Callers deciding which frame
is current must compare scene incarnation/revision and origin generation; this
synchronous API neither chooses newer scenes nor silently updates old snapshots.
CPU Tools and Server retain their dependency independence; GPU execution is an
explicit capability through Client, not a prerequisite for scene authoring.

```cpp
#include <shark/client/graphics.hpp>

shark::Result<shark::client::SceneViewportReadback>
capture_scene(shark::client::GraphicsDevice& device, const shark::RenderScene& scene,
              const shark::client::ShaderArtifact& vertex,
              const shark::client::ShaderArtifact& pixel) {
    auto request = shark::client::make_scene_viewport_request(scene, vertex, pixel);
    if (!request) return request.error();
    return device.render_scene_offscreen(request.value());
}
```

The explicit `SceneRenderSmoke` fixture and `tools/scene_render_smoke.py` wrapper
qualify geometric readbacks, target reuse/resize and resource-failure behavior on
both available GPUs. CPU CTest selections cover preparation, shader interfaces,
front-end ownership, device-free CLI and independent oracle/rejection controls;
they never establish native rendering success.
