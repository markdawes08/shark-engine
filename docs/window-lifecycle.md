# Native client window lifecycle

05b's [graphics sample](presentation-lifecycle.md) reuses this owner inside
`GraphicsDevice`; it does not create another GLFW lifecycle. `--window` remains
the standalone empty-window diagnostic described below.

Native Windows client executables declare `PerMonitorV2` in their embedded
manifest before DLL or UI initialization, alongside the graphics loader's UTF-8
process code page. This makes the existing GLFW DPI policy explicit at process
startup. The native smoke verifies window/thread awareness and the actual
content/framebuffer/DPI relationship. Microsoft recommends
[manifest DPI configuration](https://learn.microsoft.com/en-us/windows/win32/hidpi/setting-the-default-dpi-awareness-for-a-process)
over setting the process default after startup.

04b connects EngineClient's [input contract](input-contracts.md) to the pinned
GLFW backend on Windows. Editor and Runtime use the same owned `Window` and
explicit launch mode. This opens an empty native window without a graphics
context. Scene authoring, rendering and gameplay remain later increments.
[04c's frame loop](frame-loop.md) now connects input to existing Core timing;
final local qualification is PASS in the [handoff](handoffs/04c-frame-loop.md).

## Launch and build capability

From the repository root after building `windows-debug`:

```powershell
.\build\windows-debug\bin\Editor.exe --window
.\build\windows-debug\bin\Runtime.exe --window --window-ms 1000
```

Close the window normally to exit. `--window-ms` accepts an integer from 1 through
60000 and bounds the event-pump interval after successful creation. Platform calls
can stall; qualification uses an external watchdog to bound the whole process.
Invalid arguments fail before
display initialization. No arguments, `--help` and `--version` retain informational
behavior without opening a window. F6 toggles manual offline pause and F7 steps
once while manually paused and active. They are reserved sample keys and are
removed from the tick snapshot. Pause/step changes emit diagnostic JSON lines
containing the manual pause state and next tick index. Focus/minimize can pause
offline ticks without clearing the manual pause choice. This is a contributor
smoke interface, not a project-authoring CLI or usable editor UI.

`SHARK_NATIVE_WINDOW` defaults ON for Windows client builds and OFF elsewhere.
05a also enables the [graphics device backend](graphics-device.md) by default
for native Windows clients. Set both `SHARK_NATIVE_WINDOW` and
`SHARK_NATIVE_GRAPHICS` OFF for a device-free client configuration.
This keeps injectable client tests and returns `capability_unavailable`
for native creation. Enabling it without `SHARK_BUILD_CLIENT` or on a non-Windows
target rejects before dependency population. Server-only builds never fetch or
link GLFW. Existing WSL client tests remain display-free and C++-only. No Mac or
native Linux window implementation is introduced.

`SharkWindowDependencies` supplies the exact static GLFW archive already in
`dependencies.lock.json`; the graphics probe reuses it. Ordinary native client
builds enable the C compiler for GLFW but fetch no NVRHI, Vulkan headers or Vulkan
loader. The client dependency boundary remains separate from Core/Server and
authoring. The opt-in graphics probe still has its own explicit dependencies.

## Owner, lifetime and errors

Create `Window` on the process main thread. That initial caller precondition is
required by GLFW; recording a creating thread cannot prove it is the process main
thread. Every subsequent method checks the controlling thread before touching the
backend. Wrong-thread calls return `conflict`. Destroying a live owner from another
thread terminates as a programmer error; move the work, not the owner, to its
creating thread.

`Window::create` returns `Result<std::unique_ptr<Window>>`. The owner is noncopyable
and nonmovable. The native backend claims one process GLFW lease; a second claim
fails without initializing, terminating or changing the first owner's callbacks.
All GLFW use in that process must go through this owner. The independent graphics
probe is a separate executable, not an embeddable competing GLFW owner.

Startup validates a nonempty, well-formed UTF-8 title of at most 256 bytes without
NUL and requested content dimensions of 1..16384. It then claims GLFW, initializes,
creates the no-API window, installs callbacks and queries initial state. Every
attempted resource stage has reverse cleanup even after partial failure. Callback
detachment precedes destruction; termination precedes releasing the lease and
restoring the previous GLFW error callback. `close()` is idempotent and the
destructor supplies nonthrowing cleanup. Cleanup follows GLFW's void destruction
contract and cannot promise recovery from an arbitrary OS failure.

Operational failures use existing owned `Result`/`Error`, with stable operation
context and retained numeric GLFW error code when present. The native error bridge
uses a lock-free atomic code; it does not retain GLFW's borrowed description.
Owned messages are built after native calls, outside callbacks. The private
`window_backend.hpp` seam injects failures and observations in CPU tests; it is
not a public plugin or backend compatibility API.

## Events, state and coordinates

`poll_events()` dispatches native events and refreshes an owned `WindowState`.
`state()` reads the latest cached observation. Resize, iconify, restore, show,
focus and request-close commands report errors and refresh observations; requested
OS focus is not a guarantee that the desktop grants it. Close requests remain
latched until teardown. A close callback requests shutdown without destroying
the owner from inside the callback.

`capture_input()` returns the existing owned interval snapshot. Native key codes
map explicitly to all 120 engine key locations; eight mouse buttons and keyboard
repeat map separately. Character callbacks supply Unicode scalars independently
of physical keys. Callback work is bounded and nonthrowing, with no allocation,
logging or scheduler changes. Invalid/overflow events increment saturating counts;
`take_diagnostics()` constructs owned errors and consumes those counts outside
callbacks. Text capacity remains 64 scalars, and rejection cannot suppress release.

Native focus loss invokes existing input cancellation. Iconification also
neutralizes input even if an OS observation still reports focus. Regain/restore
does not resurrect held controls. GLFW may synthesize release events after focus
loss; duplicate releases remain harmless under the input contract.
[GLFW window reference](https://www.glfw.org/docs/latest/group__window.html)

Keep these observations separate:

| Value | Meaning |
| --- | --- |
| `WindowState::content` | Window content dimensions in native content coordinates |
| `WindowState::framebuffer` | Actual drawable pixel dimensions, including valid zero observations |
| `WindowState::scale` | Positive finite monitor content-scale hint for sizing UI |
| `InputSnapshot::cursor` | Content-area coordinates, top-left origin, +x right, +y down |

`cursor_to_framebuffer` multiplies by the actual framebuffer/content ratio per
axis. It does not multiply by monitor scale or clamp outside coordinates. Invalid
cursors, zero dimensions and nonfinite/overflow results reject explicitly. A
minimized/zero-size observation cannot manufacture a valid render extent.
The native backend requests `GLFW_SCALE_TO_MONITOR`; the current monitor's scale
and framebuffer/content dimensions are independently recorded in smoke evidence.
GLFW distinguishes window size, framebuffer size and content scale, so callers
must not assume all three have the same ratio.
[GLFW window guide](https://www.glfw.org/docs/latest/window)

## Verification and remaining integration

`shark.client.window` runs display-free backend and mapping fixtures with seed
1337 and a 20-second timeout. It covers malformed configuration, every startup
failure, reverse cleanup, ownership/thread checks, native-code error retention,
event bursts, focus/minimize cancellation, close latching, zero sizes and
fractional/extreme coordinate conversion. Runtime/Editor window CLI tests reject
malformed options before touching a display and test explicit unavailability in
CPU-only builds.

The native test target is deliberately separate from ordinary CTest:

```powershell
.tools/venv/Scripts/python.exe tools/window_smoke.py --executable build/windows-debug/bin/WindowSmoke.exe --output out/window-smoke-new
```

The output directory must be fresh. This opens owned windows briefly and has a
60-second external process watchdog. Cases exercise actual startup/title,
resize/DPI, Win32-to-GLFW input callbacks, focus transfer, minimize/restore,
overflow diagnostics, close and 16 further RAII lifetimes. Synthetic messages
target only the test's HWND; no system-wide keyboard/mouse injection or display
setting change occurs. It verifies native callbacks and observed current-monitor
scale, not physical input hardware or multiple monitors. 04c extends the native
fixture through the same launch helper for first-tick input, F6/F7 pause/step,
focus/minimize/resume and close-before-tick behavior. Reports retain JSON
records, logs, binary hash and failure status. The
[04b handoff](handoffs/04b-native-window.md) records its original local results;
the [04c handoff](handoffs/04c-frame-loop.md) tracks the extended gate.

The launch sample uses the existing monotonic clock for short event-pump waits
and passes snapshots to `OfflineFrameLoop`. Input survives zero-tick frames and
one-shot actions reach only the first catch-up tick; manual/focus/iconify pause
admits no elapsed time. The first resumed frame executes no ticks. A close request
is handled before scheduling another tick. The sample's callback is intentionally
empty because no local scene exists yet; diagnostic tick indices are not MMO
simulation progress. Window focus/minimize never controls authoritative server
time. The full 04 gate is complete. Production surface/device/presentation
ownership starts in the next assignment, 05.
