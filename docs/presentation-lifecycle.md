# Presentation lifecycle

Assignment 05b extends the [owned graphics device](graphics-device.md) with a
clear-only presentation path. Full `REQ-05` remains NOT_RUN until the
[05c failure/stress gate](roadmap/graphics-device-plan.md) passes. Shader pipelines,
scene rendering, uploads and readback belong to 06. No frame-rate or visual
correctness claim follows from successful presentation calls.

## Ownership and synchronization

`GraphicsDevice::present(ClearColor)` lazily creates presentation resources and
returns owned counters plus `presented`, `waiting`, `recreate_pending`, `suspended`
or `closed` (window close requested). A closed device rejects further presentation.
The four linear color components must be finite and in [0,1]. They clear an sRGB
color attachment using dynamic rendering inside an NVRHI command list. This uses
the existing COLOR_ATTACHMENT surface profile without adding TRANSFER_DST support
as a requirement. Vulkan handles and NVRHI objects remain private.

The existing device owns its Window, surface, queue, NVRHI device, two fixed command
slots and at most one swapchain generation. The native swapchain is bounded to
eight images. Every slot waits for its prior NVRHI queue timeline value before reuse.
Acquisition fences also cover failures between successful acquisition and submission.
Resize drains the old generation before destroying its image views/wrappers and
creating a replacement; no growing queue of retired swapchains is retained.

The unchanged minimum capability profile admits both current-PC GPUs:

| Available synchronization | Behavior |
| --- | --- |
| `VK_EXT_swapchain_maintenance1` feature | Enable it optionally; at most two submitted frames in flight, per-image present semaphores and present-completion fences. Current NVIDIA path. |
| No maintenance feature | At most one submitted frame; wait for rendering's timeline value before presenting with no wait semaphore. At retirement, an owned queue-idle worker drains queued presentation operations. Current Intel compatibility path. |

Submission completion alone does not prove a presentation engine has finished
using a present semaphore. The optional fence path provides that proof; the
fallback avoids handing an application semaphore to presentation. See the
[Khronos semaphore reuse guidance](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html).
On fallback destruction, application image use and queued operations must complete;
Vulkan retains image backing until presentation finishes under the
[`vkDestroySwapchainKHR` contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroySwapchainKHR.html).
The Intel path uses the legacy unextended queue-idle compatibility route. It
does not provide a maintenance present fence or a formal presentation-engine
completion signal; the historical specification gap is recorded in
[Khronos issue 1678](https://github.com/KhronosGroup/Vulkan-Docs/issues/1678) and the
[swapchain recreation sample](https://docs.vulkan.org/samples/latest/samples/api/swapchain_recreation/README.html).
No application semaphore is handed to Intel presentation. Local validation
qualifies the exercised driver behavior, not physical scanout or a portability
proof for other drivers. Neither path uses per-frame queue/device idle waits or
requires a shader compiler. A submit marker alone cannot prove presentation done.

## Bounded outcomes and failures

Each call makes one attempt. Slot wait, fallback completion and recreation drain
budgets are 5 ms; acquire is 1 ms. Reuse of a previously presented image may wait
up to 1 second for its present fence. Explicit drain/close uses a single aggregate
1-second deadline. On Intel, the one owned worker may remain inside queue-idle
after the caller's deadline; timeout retains it and every GPU owner. Further queue
access must wait for it to finish and join. Cleanup never detaches a live waiter.
These bound caller waits, not arbitrary driver-call duration;
native qualification additionally runs under a process-tree watchdog.

On Windows, the worker deadline includes thread-local destructors and DLL thread
detach callbacks. Publishing its integer result alone does not prove thread exit.
The private waiter polls or waits on the owned OS thread handle, then joins only
after termination is observed. Nanoseconds are rounded down to whole milliseconds
and capped at the existing one-second maximum; a zero or sub-millisecond budget
remains a poll. Timeout or a failed native wait retains the worker and completion.
The non-Windows, CPU-only fixture path still bounds the action wait; standard C++
does not provide a timed thread join. This does not add native Linux GPU support.
Microsoft documents the thread object's termination signal and native wait
semantics in [ExitThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitthread)
and [WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject).

Zero framebuffer size or iconification returns `suspended` without creating,
acquiring, submitting or presenting GPU work. Window events continue to be pumped.
An acquire timeout returns `waiting`. OUT_OF_DATE defers recreation until a later
call; SUBOPTIMAL finishes the current image and marks the generation for recreation.
A fallback present timeout retains the exact submitted image for a later attempt;
it never acquires another image in its place. No internal retry loop spins on a
surface outcome. Callers use the existing monotonic clock to yield between passes.

Surface loss and other terminal native failures return an owned error and latch a
fault; later frames cannot submit more work. There is no automatic adapter switch,
validation downgrade or device recovery. Validation errors are checked between
native stages, including before presenting a submission. The original fault remains
visible after cleanup. Device loss permits terminal destruction once recording
and worker ownership are resolved, but is never PASS.

`drain()` waits for application GPU completion and the selected strategy's
retirement conditions without destroying the current generation. A timeout
retains every live resource and returns failure; an explicit retry may complete.
`close()` drains, then destroys swapchain resources, command slots, NVRHI, device,
surface, instance and Window. Diagnostics and statistics remain readable. If the
destructor's final bounded drain cannot establish safe destruction, it terminates
the process rather than freeing resources still in use. Unknown submission state
after a native exception is treated conservatively. The pinned NVRHI command
list retains a self-reference during recording and exposes no discard API; see
the pinned [command-list implementation](https://github.com/NVIDIA-RTX/NVRHI/blob/6b96fb03e07539f08327aea76c56d55f1de9d906/src/vulkan/vulkan-commandlist.cpp).
Once recording starts, only a successful nonzero execution serial establishes
normal retirement ownership. A recording error, rejected execution or exception
before that point retains the device even if device loss is later reported;
its destructor emits a bounded diagnostic and fails fast. It never submits
invalid work merely to release an internal reference. This is a recover-or-exit
policy, not a device-recovery implementation. 05c's private native fixture now
injects this path immediately after actual NVRHI recording opens, preserving
the owner and requiring the expected fatal child exit. See the
[failure/stress contract](presentation-qualification.md); its full qualification
gate remains open independently of these failure-policy tests.

## Native 05b workload declared before execution

Run `PresentationSmoke` separately on both explicit current-PC GPU UUIDs in
Windows Debug, Release and ASan, with required core, synchronization and NVRHI
validation. Each run has four complete device lifetimes. Each lifetime presents
96 clear frames: 16 at 640x400, then 16 after each of four resizes (700x420,
530x350, 800x450, 640x400); after resize 2 and 4, iconify, verify suspended attempts
do not increase submission counters, restore and present eight additional frames.
An explicit setup resize normalizes the initial DPI-scaled window before creating
the first swapchain; it is not a measured generation transition. Minimized content
may be zero; restored visible content must match the declared size.
That is 384 presentations, 16 resizes and eight minimize/restore cycles per run.
Waits and event observations have finite retry/deadline bounds; the wrapper has a
120-second process-tree watchdog. Preserve failed and partial runs.

Require increasing swapchain generations, retirement after completion, at most
the reported one/two frames in flight, second-owner rejection, idempotent close,
and zero warnings/errors/dropped messages after teardown. Do not require two
frames to be simultaneously outstanding on a fast GPU. The fixture records every
frame attempt as JSONL, including suspended/waiting/recreation outcomes, and
records source/lock/toolchain/device identity. Validation-off is diagnostic NOT_RUN.
No sustained soak, resource-growth measurement, scanout or readback qualification
is claimed by this short gate. 05c owns further stress and failure qualification.

The CPU-injected lifecycle cases independently cover partial construction,
malformed outcomes, bounded waits, pending-image retry, resize ordering, validation
faults, device loss and timeout retention. Ordinary CTest remains device-free.
Core/Server acquire no presentation dependencies; existing server timing tests
prove the distinct policy. Client minimize never controls authoritative Server time.

## Human and terminal use

After building a native Windows preset, open the same sample from either entry
point. For example, from the repository root in PowerShell:

```powershell
.\build\windows-debug\bin\Editor.exe --graphics
.\build\windows-debug\bin\Runtime.exe --graphics --graphics-ms 1000 --device 868088a7040000000002000000000000
```

`--graphics-ms` accepts 1..60000 milliseconds; omission runs until window close.
`--device auto` prefers supported discrete hardware; an explicit lowercase UUID
never falls back. Each option may appear once. The interactive sample defaults to
Vulkan validation off, so independent development-layer installation is unnecessary;
`--validation required` explicitly requires those tools. NVRHI validation stays on.
The completion JSON reports the chosen device and counters with qualification
NOT_RUN. Use the native fixture below for qualifying evidence. `--help`, `--version`
and invalid options never open a GPU/window. A WSL CPU build returns
`capability_unavailable` for a valid graphical launch.

Both samples use the existing offline frame/input loop and reserved F6 pause/F7
step controls. Pause affects only empty offline tick callbacks; clear presentation
continues while visible. There is still no authoring UI, project world or server
gameplay. `--window` remains available for the existing empty-window diagnostic.

Use the locked validation receipt from the [probe guide](graphics-probe.md) and a
fresh output directory for each GPU/build. An elevated development shell can use
the existing same-user desktop helper (PowerShell array preserves `--`):

```powershell
& ./tools/windows_dev.ps1 -CommandArguments @('python','tools/windows_unelevated.py','--timeout','150','--evidence','out/presentation/manual-token.json','--','python','tools/presentation_smoke.py','--executable','build/windows-debug/bin/PresentationSmoke.exe','--build-dir','build/windows-debug','--mode','smoke','--device','863d6f59a51894ab8b4829aa9460c78d','--validation-manifest','out/graphics-validation/05b-local/summary.json','--output','out/presentation/manual')
```

Replace the receipt path with the actual prepared current-lock receipt. Repeat
with the Intel UUID `868088a7040000000002000000000000` and a new output path.
The wrapper checks source/binary/lock identity, layer tools and exact workload
records. `--mode describe` is device-free; `--validation off` cannot qualify.
