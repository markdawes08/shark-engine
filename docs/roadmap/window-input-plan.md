# Assignment 04 checkpoints

Assignment 03 is complete under the [current-machine scope](../architecture/0010-current-machine-scope.md).
Assignment 04 retains its single overall requirement, `REQ-04`, and is divided
into three reviewable checkpoints. All three and full `REQ-04` are PASS under
the current-machine scope. Reuse the [03c clocks and scheduling policies](../time-and-work.md).

1. **04a - Input contracts and tests:** implement device-free, injectable raw
   keyboard/mouse state in EngineClient with held/pressed/released/repeated
   semantics, bounded text/scroll capture and focus-loss cancellation. Test
   malformed events, rapid transitions, snapshots and repeated lifetimes without
   real keyboard/display input. Keep Core/Server free of client code. Native
   Windows Debug/Release/ASan, CPU GCC/Clang fixtures in existing WSL and protected
   standalone builds qualify this checkpoint; no GPU/window behavior is claimed.
2. **04b - GLFW window integration:** add owned window creation/teardown and
   translate native events into the shared input contract. Cover event callbacks,
   focus, close, resize, minimize, framebuffer/content sizes and high-DPI behavior.
   Keep GLFW callbacks bounded and nonthrowing, with diagnostics reported outside
   callbacks. Exercise startup failure and native Windows lifecycle smoke checks.
   This checkpoint provides a runnable native window; graphics-device/presentation
   ownership remains assignment 05.
3. **04c - Frame-loop integration and qualification:** integrate window/input with
   existing Core timing, specify input consumption across zero/multiple fixed
   ticks, offline pause/single-step and long-frame behavior. Focus/minimize may
   affect local presentation/offline play, never shared authoritative server time.
   Complete deterministic scheduling/input tests, current-PC Windows smoke runs,
   WSL CPU/headless boundary regressions and the full `REQ-04` gate.

The [MMORPG plan](mmorpg-plan.md) and [human/agent plan](human-agent-plan.md) still
apply. Raw local input is not an authenticated gameplay intent or an authoring
command. Action mappings, UI capture routing, networking and gameplay authority
belong to their existing integration owners. A captured input value does not
authorize mutation of a server world. No separate scheduler or generic event bus
is required. Source remains buildable without the ignored local roadmap.

**04c and full assignment 04 are complete under the current-machine scope.**
Next is **05: production graphics device and presentation lifecycle**.
The [input guide](../input-contracts.md)
and [04a handoff](../handoffs/04a-input-contracts.md) specify the raw input contract.
The [window lifecycle guide](../window-lifecycle.md) and
[04b handoff](../handoffs/04b-native-window.md) record ownership, callback translation,
failure rollback and native Windows verification. The
[frame-loop guide](../frame-loop.md) defines the new `OfflineFrameLoop` and Core
`OfflineControl`: bounded pending input survives zero-tick frames, one-shot actions
reach only the first catch-up tick, and presentation retains its independent frame
snapshot. Manual/focus/iconify pause discards stale positive actions and elapsed
time; explicit active-window stepping runs one tick without leaving manual pause.
The first resumed frame establishes a fresh time anchor and executes no ticks.
Server scheduling remains independent.

Editor/Runtime `--window` now pumps this shared helper with F6/F7 reserved sample
pause/step controls and bounded terminal smoke mode. These are developer controls,
not action bindings or authoring commands. Rendering and game-authoring UI remain
with their later owners. Full `REQ-04` is PASS, with final current-machine
qualification recorded in the [handoff](../handoffs/04c-frame-loop.md).
Assignment 05 is ready to begin; its rendering implementation is not part of 04.
