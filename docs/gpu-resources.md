# GPU resources and offscreen rendering

06b extends the existing `EngineClient` graphics device with bounded precompiled
shader loading, typed uploads, a fixed textured pipeline and owned readback.
The public [resource types](../engine/client/include/shark/client/gpu_resources.hpp)
contain engine-owned values. NVRHI/Vulkan remain private; Core and Server do not
load shaders or acquire graphics dependencies. This diagnostic draw is not a scene,
material, streaming or public authoring API. Window integration belongs to 06c.

## Artifact and upload contract

The [offline pipeline](shader-pipeline.md) independently validates SPIR-V and
publishes a hash-verified artifact generation. The native loader accepts explicit
artifact paths without discovering or executing a compiler. Its bounded interface
reader is not a full SPIR-V validator, signature verifier or sandbox for hostile
GPU programs. The fixture verifies the complete offline manifest before and after
execution; 06c owns packaged artifact discovery and distribution closure.

Each shader is at most 4 MiB, little-endian SPIR-V 1.6, with a bounded ID space.
The supported interface is `vsMain` (vertex) and `psMain` (pixel), float3 position,
float2 UV, float2 interpolated UV and float4 color. Set 0 contains the sampled
2D float texture at binding 0, sampler at 128 and 64-byte matrix constant buffer
at 256. Descriptor type, stage, member offset/stride and vertex/interstage shapes
are checked before native creation. Public mutable artifact values are revalidated
at every draw. Unsupported stage/layout input returns an owned error.

Host/HLSL matrices are column-major, multiplying column vectors. Pinned DXC lowers
this particular HLSL matrix to a transposed SPIR-V representation decorated
`RowMajor`, offset 0, stride 16; the reader checks that actual emitted interface.
It does not change the host storage convention. The shader applies the single Y
inversion and the NVRHI viewport compensates for its internal Vulkan inversion,
leaving a positive native viewport height.

Both texture and target extents are 1..1024 per dimension. Vertices are bounded
to 3..65,536, indices to 3..196,608 and complete triangles; every index must be
in range. Positions, UVs and matrix values must be finite. Texture storage is
exactly width × height × 4 bytes. Uploads use explicit vertex/index/constant
buffers and a nearest/clamp RGBA8 UNORM texture. RGB is linear, origin top-left,
front faces counterclockwise and back faces culled. The RGBA8 target clears to
black with alpha 1; D32 clears to 1 and uses conventional 0..1 `LessEqual` depth.

## Ownership and bounded completion

`GraphicsDevice::render_offscreen` uses the creating thread, existing device,
queue and timeline waiter. Its synchronous diagnostic wait has a one-second
budget. It copies color and depth staging rows into tightly packed owned vectors
only after observing submission completion. Each successful draw releases its 17
top-level NVRHI handles after completion and reports matching creation/retirement
counts. These counts describe engine handle ownership, not driver allocations.

Failures before recording unwind local resources. Submitted work whose completion
is not observed retains its owners; callers explicitly `drain()` before further
rendering or presentation. Close/destruction covers these owners even when no
swapchain has ever been created. A recording/submission ambiguity retains owners
and follows the existing fatal destruction policy rather than freeing resources
still potentially referenced by a queue. No new scheduler or unbounded main-thread
queue-idle wait is introduced. Native fault fixtures exercise partial creation,
retained submission and recording failure in bounded child processes.

## Repeatable fixture

`OffscreenSmoke --describe` is device-free. Explicit GPU qualification uses
`tools/offscreen_smoke.py`, one UUID, the locked validation-tools receipt and a
verified shader generation. Output goes to a fresh ignored directory under `out/`.
The wrapper checks executable/source/lock identity, requires core and synchronization
validation, preserves process logs and raw readbacks, and independently rasterizes
the known inputs. Validation-off execution cannot qualify a GPU.

The cases render a rotated textured cube at 96×80 and 128×128. A smaller cube lies
behind it with a different UV pattern; its later draw must remain occluded. The
second case reverses triangle order and changes texture content. A fixed affine
matrix avoids platform trigonometry differences. The independent reference uses
pixel centers, barycentric depth and nearest sampling; color tolerance is one
8-bit channel value and depth tolerance is 0.00001. Triangle-edge and texture
discontinuity exclusions are fixed before hardware comparison, with minimum
coverage and occlusion assertions. Generated PNGs permit visual inspection; they
are not automatically accepted baselines.

The current-machine reference rounds framebuffer vertex X/Y to the nearest
1/256-pixel grid before interpolation. Each native readback reports the actual
queried `subPixelPrecisionBits`; this fixture requires 8 and cannot qualify an
unknown precision model. The engine itself imposes no such device restriction.
Vulkan exposes that framebuffer precision in
[VkPhysicalDeviceLimits](https://docs.vulkan.org/spec/latest/chapters/limits.html#limits-subPixelPrecisionBits).
This reference models the two current devices, not all possible Vulkan
interpolation implementations. The first hardware comparisons exposed the
missing rounding step in the original reference; that evidence is retained,
and neither acceptance tolerance nor fixture geometry was changed.

CPU coverage is `shark.client.gpu-resources`, `shark.client.offscreen`,
`shark.tooling.offscreen-smoke`, `shark.tooling.offscreen-faults` and native-build
`shark.client.offscreen-cli` / `shark.client.offscreen-faults-cli`.
The explicit fixtures live in [offscreen_smoke.cpp](../tests/offscreen_smoke.cpp)
and [offscreen_faults.cpp](../tests/offscreen_faults.cpp). FeatureWorld connects
these isolated cases when scene ownership lands in assignment 10.

From the repository root, after the normal Windows Debug build:

```powershell
python tools/shader_pipeline.py build --recipe shaders/recipe.json --output out/06b-shaders --validation-manifest out/05a-work/validation-tools/summary.json
python tools/windows_unelevated.py --timeout 70 -- python tools/offscreen_smoke.py --executable build/windows-debug/bin/OffscreenSmoke.exe --mode render --device 863d6f59a51894ab8b4829aa9460c78d --shaders out/06b-shaders --validation required --validation-manifest out/05a-work/validation-tools/summary.json --build-dir build/windows-debug --output out/06b-nvidia
```

Use `868088a7040000000002000000000000` and a different fresh output directory for
Intel. `windows_unelevated.py` launches with the same user's standard desktop
token when needed because the Vulkan loader ignores development-layer search
paths at high integrity. The validation receipt comes from the existing locked
[tool preparation](graphics-probe.md); an absent receipt is an explicit missing
prerequisite. Inspect `summary.json` and the generated cube PNGs in the output.
