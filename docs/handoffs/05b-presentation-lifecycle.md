# 05b - Swapchain, submission and presentation lifecycle

Date: 2026-10-18. Base: `d71f88d16bd93d85a2bd9757e05a0e93ce0c1e32` on `main`, initially clean.
Result: uncommitted worktree; the user owns staging, commits and pushes.

**05b is complete and PASS under the current-machine scope.** Fresh final
Windows and WSL qualification passed after the command-recording ownership audit.
Full `REQ-05` remains NOT_RUN until **05c, failure and stress qualification**.
The [checkpoint plan](../roadmap/graphics-device-plan.md) and
[presentation contract](../presentation-lifecycle.md) define scope and limitations.

## Delivered behavior

The existing `GraphicsDevice` now owns lazy swapchain creation, NVRHI command slots,
clear submission and presentation. Public methods return owned window/input values,
bounded presentation outcomes, counters and diagnostics. No native handles escape.
Resize drains the old generation before replacement; zero extent/minimize suspends
new GPU work while event pumping continues. Acquisition waits, out-of-date and
suboptimal outcomes yield to a later call. Pending fallback presentation retains
the original submitted image instead of resubmitting it.

NVIDIA optionally uses EXT maintenance present fences, per-image semaphores and
at most two frames in flight. Intel retains the unchanged minimum capability
profile with one frame in flight, host-completed rendering and no presentation
wait semaphore. Its retirement path uses one owned queue-idle worker and Core's
existing bounded completion queue. Caller timeout retains the worker and GPU
owners; later queue access waits for completion and join. Queue-idle is never
started on an ordinary frame. The documented unextended Vulkan presentation
completion limitation remains explicit; local validation is not a scanout or
other-driver portability proof.

Validation errors stop later frame stages. Surface/device loss and unexpected
failures remain visible, with no automatic GPU switch or validation downgrade.
Explicit close drains before reverse teardown and retains statistics/diagnostics.
If the destructor's final bounded drain cannot release resources safely, it fails
fast instead of freeing still-used resources. Native driver calls are additionally
contained by the fixture's external process-tree watchdog. Actual hardware loss
is not induced in this checkpoint; 05c owns wider failure qualification.

Editor and Runtime expose explicit `--graphics`, optional bounded duration,
adapter UUID and validation mode. They share the existing offline frame/input
pump and F6/F7 controls with `--window`. The graphical sample emits a final JSON
record with its selected GPU and completed counters; qualification is NOT_RUN.
It defaults to Vulkan validation off for interactive use, with NVRHI validation
retained. Required validation is explicit in every qualifying native fixture.
Help, version, invalid arguments and CPU fixtures remain display-free. There is
still no scene renderer, shader pipeline, authoring editor or game world.

## Verification

The [final durable receipt](../coverage/evidence/05b-local.json)
includes exact commands, current source/binary/tool
identities, hardware/driver data, native traces and retained failures.

| Configuration/check | Result |
| --- | --- |
| Windows Debug / Release / AddressSanitizer | 38/38 CTests each PASS |
| WSL CPU client GCC Debug / Clang ASan+UBSan | 36/36 CTests each PASS |
| WSL headless GCC Debug / Clang ASan+UBSan | 25/25 CTests each PASS |
| WSL Release Server install/closure and SDK-free runtime | PASS |

The seven principal configurations total **236 passing CTests**, without CTest
skips. The presentation family has **36 cases and 1,548 assertions**, fixed seed
1337: coordinator failure/lifetime tests, strict launch parsing, shared offline controls, completion
output and five owned-waiter cases covering timeout retention, duplicate work,
exceptions, completion/join and reuse. The evidence parser has 18 tests; native
description/help and invalid argument tests do not initialize graphics.

The declared native workload is four device lifetimes, each with 96 presentations,
four measured resizes and two minimize/restore cycles, per GPU/build. All six runs
passed: **24 lifetimes, 2,304 presentations, 96 resizes and 48 minimize/restore
pairs**, with zero warnings, errors or dropped diagnostics through close.
Required validation means Vulkan core/synchronization plus NVRHI. NVIDIA uses
present fences with a two-frame limit; Intel uses host completion and the owned
retirement worker with a one-frame limit. A separate validation-off diagnostic
is retained as NOT_RUN and cannot qualify the checkpoint.

Regression checks also passed: 96 required-validation device-foundation
lifetimes, three native window suites (11 cases and 17 window lifetimes each),
six bounded Runtime/Editor window samples, and 14 graphical samples across
Runtime, Editor and the Runtime-only build. Every graphical sample reported the
requested GPU, positive presentation counters and completed retirement. Runtime-only
and Server-only dependency boundaries passed. Formatting for all 81 C++ files,
contract validation and its negative cases, repository checks and whitespace
checks passed. Captured build, test and tool inputs remained unchanged throughout
the final Windows/WSL runs.

Two preflight failures remain retained. The first NVIDIA process completed cleanly
but the parser rejected its DPI-scaled 960x600 startup extent against the declared
640x400 workload. The fixture now explicitly normalizes initial size before
creating a swapchain and accepts zero minimized content only while iconified.
The second failure was a real Intel retirement bug: rendering completion alone
did not retire the presentation queue, and validation reported
`VUID-vkDestroySwapchainKHR-swapchain-01282`. The owned retirement waiter fixes
that path. Neither failed receipt was relabeled PASS; final qualification uses
fresh output directories against unchanged corrected sources.

The final source audit also found that pinned NVRHI retains a command-list
self-reference between `open()` and successful execution, with no discard API.
An error or exception during recording cannot claim ordinary safe cleanup.
The native owner now retains that state and fails fast with a bounded diagnostic
at destruction, including when device loss is also reported. Normal release
requires a nonzero execution serial. This finding came from source inspection,
not induced native failure; 05c retains that fault-injection work.

## Boundaries and next work

All dependency pins remain unchanged. Core/Server and device-free WSL clients
keep native window/graphics options OFF, with no GLFW/Vulkan/NVRHI fetch/link/package
requirement. This is local Windows and existing WSL userspace evidence on one PC;
no Mac, native Linux GPU, hosted runner or independent-machine qualification is
claimed. Host settings, drivers and remote branch protection were not changed.
`.gitignore` continues to cover the local roadmap and generated output, with an
explicit presentation-evidence note. Sources, tests and documentation remain
eligible for version control.

Next is **05c**: sustained presentation and resize/minimize loops, measured resource
behavior, further recover-or-exit failure cases and final protected package checks.
Do not mark full `REQ-05` PASS or start 06 shader/resource work before that gate.
MMORPG networking, persistence, seamless streaming and authoritative simulation
remain with their planned owners; presentation does not control Server time.

Suggested commit: `feat(graphics): add bounded presentation lifecycle`
