# Graphics device and presentation

Assignment **05a** adds an owned Windows Vulkan/NVRHI device foundation in
EngineClient. The [05a handoff](handoffs/05a-graphics-device.md) records its passing
local gate. Assignment **05b** extends it with
[clear-only presentation](presentation-lifecycle.md), owned submission and
swapchain retirement; the [05b handoff](handoffs/05b-presentation-lifecycle.md)
records that checkpoint. Full `REQ-05` remains **NOT_RUN** until the
[05c failure/stress gate](roadmap/graphics-device-plan.md) is complete.
Public contracts still expose no native GPU
handles or authoring APIs. Editor/Runtime `--window` retains its empty-window
sample; explicit `--graphics` uses the same [offline pump](frame-loop.md) and
adds a clear-color window.

The accepted capability profile is unchanged from
[ADR 0003](architecture/0003-platforms.md) and the assignment-02 probe. Native
qualification uses this PC's RTX 4070 Laptop and Intel Raptor Lake-S Mobile GPUs.
Existing WSL Ubuntu environments cover CPU, headless Server and package boundaries;
they do not provide a native Linux graphics gate. Mac and additional hardware are
outside the [current-machine scope](architecture/0010-current-machine-scope.md).

## Public contracts and ownership

The public header is
[`shark/client/graphics.hpp`](../engine/client/include/shark/client/graphics.hpp).
It exposes engine-owned values and `Result<T>`/`Error`; Vulkan/NVRHI objects remain
private. `GraphicsConfig` selects an adapter UUID, validation policy and existing
`WindowConfig`. No scene, persistent format, authoring command or Lua API is added.

`GraphicsDevice::create(config)` returns one noncopyable, nonmovable owner. Startup
acquires the existing `Window`, Vulkan instance, Win32 surface, logical device and
NVRHI device/validation wrapper in that order. The surface uses the HWND owned by
the Window through
[`vkCreateWin32SurfaceKHR`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateWin32SurfaceKHR.html);
it does not initialize GLFW again. The same existing process-wide
Window lease rejects a second owner before it can alter GLFW or the shared
Vulkan-Hpp dispatcher. Core and Server do not acquire these dependencies.

Creation on the process main thread is a caller precondition, inherited from the
[window lifecycle](window-lifecycle.md). The implementation records that thread
and rejects methods called from another thread. Destroying a live owner on the
wrong thread terminates as a programmer error. Once closed, its inert storage may
be destroyed elsewhere. `adapter()` returns an owned copy of the selected metadata;
`diagnostics()` returns cumulative owned diagnostics, including after close.

Every attempted startup phase is marked before entry. Failure, exceptions or normal
shutdown release NVRHI, device, surface, instance and Window in reverse order.
`close()` is idempotent and retains diagnostics. The callback storage outlives all
native destruction callbacks. Once presentation has started, bounded drain proves
submitted use complete before retirement; timeout retains resources. The
[presentation contract](presentation-lifecycle.md) defines terminal failure and
destructor behavior. An owner that never presents needs no GPU wait.

`GraphicsDevice::inventory(config)` uses the same ownership path through surface
creation, with a hidden window, then returns owned adapter values after cleanup.
It does not create a logical device. An inventory request does not accept an
adapter selector. Inventory therefore requires native window/GPU access even
though its results are machine readable. `native_graphics_available()` only reports
whether the native backend was compiled; it does not prove that a driver, display,
validation layer or supported GPU is currently available.

## Selection and the retained capability profile

An empty `adapter_uuid` selects a supported hardware adapter, preferring discrete,
then integrated, then other hardware; UUID order breaks ties. Software adapters
are rejected. An explicit UUID is exactly 32 lowercase hexadecimal characters and
never falls back to another device. Missing or ambiguous identities, invalid
bounded UTF-8 metadata and unsupported capabilities return owned errors. Adapter
metadata includes UUID/name, vendor/device IDs, raw API/driver versions, driver
strings, kind, queue family and the capability values used for admission.

`valid_graphics_uuid` and `missing_graphics_requirements` are pure checks: neither
opens a window nor initializes Vulkan. Native enumeration caps each result at
4,096 entries and adapter inventory at 64 devices. Changed counts or Vulkan
`VK_INCOMPLETE` fail the request without an automatic retry loop.

The profile checks Vulkan 1.3, `timelineSemaphore`, `synchronization2`,
`dynamicRendering`, `VK_KHR_swapchain`, one graphics/compute/present queue, an sRGB
surface format and FIFO presentation. Device creation enables the three required
feature bits and swapchain extension, plus optional EXT swapchain-maintenance
feature/fences when available. Missing maintenance support selects bounded host
completion without changing admission requirements. Ray tracing, bindless
descriptors and vendor features are not enabled. A usable surface/format query is
admission evidence, not proof of presented images.

| Limit | Minimum |
| --- | --- |
| 2D image extent / cube face extent | 4096 / 2048 |
| Image array layers / color attachments / bound descriptor sets | 12 / 4 / 4 |
| Sampled images and samplers per shader stage | 16 each |
| Uniform-buffer / storage-buffer range | 16 KiB / 128 MiB |
| Compute invocations per workgroup | 128 |

Combined optimal-tiling feature and image-usage queries retain the exact probe
requirements: RGBA8 sRGB sampled/filtered/transfer; RGBA8 UNORM additionally color
attachment/blend; RGBA16 float additionally storage image; D32 float sampled,
depth attachment and transfer source. D32 must support a 2048 by 2048 image with
at least 12 array layers. Missing requirements are named individually by the pure
profile check and selected-device rejection. The format/feature arrays in the
public header document their fixed ordering. This increment does not replace
assignment-02 draw/compute/readback evidence or introduce a format fallback.

## Validation and failures

`GraphicsValidation::required` is the default. It requires the Khronos validation
layer, debug-utils extension and synchronization-validation support; missing
prerequisites reject startup. Core and synchronization validation are enabled
explicitly through
[`VkValidationFeaturesEXT`](https://docs.vulkan.org/refpages/latest/refpages/source/VkValidationFeaturesEXT.html),
with no disabled checks. The NVRHI validation wrapper is always
enabled, including explicit Vulkan-validation-off diagnostic runs. Validation-off
execution cannot qualify the checkpoint.

Vulkan and NVRHI warning/error callbacks copy at most 511 bytes into each of 64
fixed diagnostic slots. They perform no allocation, I/O or user callback. Counts
remain cumulative; overflow and truncation increment `dropped`. Snapshot creation
escapes control/non-ASCII native bytes outside the callback so returned messages
remain valid UTF-8. Startup and explicit close reject recorded validation errors;
the qualification fixture additionally requires zero warnings and dropped messages.

Expected native failures return stable error codes, operation names and owned
messages. Vulkan failures include their numeric `VkResult`. Missing profile or
validation prerequisites use `capability_unavailable`; conflicting ownership uses
`conflict`. Unsupported devices are rejected before logical-device creation.
Unexpected startup exceptions become `execution_failed` and unwind attempted
phases. Explicit callers can report the error and terminate with a nonzero status.

The device never silently replaces a GPU, disables validation, reduces the profile
or retries a failed enumeration/create operation. Presentation defers out-of-date
recreation to a later call and treats device/surface loss as fatal; it has no
device-recovery loop. 05c qualifies further stress and failure behavior. Native driver
calls are not preemptible by a C++ destructor; qualification uses an external
process-tree deadline to bound a stalled process. A timeout is retained as a
failure, not retried into a pass.

## Build and terminal verification

Windows client configurations default `SHARK_NATIVE_GRAPHICS=ON` when native
windows are enabled. They reuse the existing exact Vulkan-Headers, Vulkan-Loader,
GLFW and NVRHI pins through `SharkGraphicsDependencies`. The loader is built beside
the client executables; the UTF-8 process manifest covers native client consumers.
There is no runtime source shader compiler. The independent locked validation
tools remain development inputs, outside the engine dependency graph.
Sanitizer configurations apply the same instrumentation to the NVRHI common and
Vulkan libraries, whose public C++ boundary shares standard-library objects with
engine code. This keeps MSVC AddressSanitizer container annotations consistent;
engine warning policies remain scoped to engine targets.

Set both `SHARK_NATIVE_GRAPHICS=OFF` and `SHARK_NATIVE_WINDOW=OFF` for a device-free
client build. Native graphics ON requires Windows, Client and native Window;
invalid combinations fail before dependency population. WSL CPU and all Server-only
configurations leave both native capabilities OFF, do not enable C for presentation
dependencies and do not fetch/link GLFW, Vulkan or NVRHI. EngineTools retains its
Core-only dependency; the future CPU authoring path does not require this device.

Use the [build guide](building.md) and native compiler wrapper, for example:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/windows_dev.ps1 python tools/ci.py --preset windows-debug --jobs 2
```

`shark.client.graphics` is a CPU fixture family with fixed seed 1337 and an explicit
timeout. It uses injected backends, so it can verify admission, ownership, reverse
cleanup, threads and callback bounds without creating a window/GPU. The native
test executable `GraphicsDeviceSmoke` is built only for Windows native-graphics
test configurations. Its `--describe`/`--help` and invalid-argument CTest checks
do not initialize graphics. `--inventory` and `--smoke` are explicit hardware work,
not ordinary CTest execution.

After preparing the locked validation tools as described in the
[probe guide](graphics-probe.md), run from the configured development environment
with a fresh output path:

```text
python tools/graphics_device_smoke.py --executable build/windows-debug/bin/GraphicsDeviceSmoke.exe --build-dir build/windows-debug --mode inventory --validation-manifest out/graphics-validation/05a-local/summary.json --output out/graphics-device/manual-inventory
python tools/graphics_device_smoke.py --executable build/windows-debug/bin/GraphicsDeviceSmoke.exe --build-dir build/windows-debug --mode smoke --device UUID_FROM_INVENTORY --validation-manifest out/graphics-validation/05a-local/summary.json --output out/graphics-device/manual-smoke
```

The receipt path above is an example: use the actual current-lock receipt from
tool preparation and substitute the inventory's UUID. The wrapper validates source,
lock, executable and validation-tool identities, controls layer configuration,
records JSONL/stdout/stderr plus hardware/toolchain metadata and enforces a
60-second process-tree deadline. Elevated Windows shells must use the existing
`windows_unelevated.py` helper documented in the probe guide so required layer
discovery runs under the standard desktop identity. Do not change system loader
settings or install validation layers globally.

Each native smoke performs 16 create/close lifetimes for the selected device,
rejects a second owner, checks explicit selection/profile results and verifies
idempotent close plus clean cumulative diagnostics after destruction. Both local
GPU UUIDs require their own run. Auto-selection is separately testable with
`--device auto`. These are terminal-accessible diagnostics for human and agent
contributors, not game-authoring commands or player protocols. The separate
[05b fixture](presentation-lifecycle.md) qualifies presentation; 05c closes full `REQ-05` only after its stress
and failure gates pass.
