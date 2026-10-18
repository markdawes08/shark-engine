# Offline frame and input consumption

04c connects the [raw input contract](input-contracts.md) and
[owned window](window-lifecycle.md) to the existing [Core timing policy](time-and-work.md).
`OfflineFrameLoop` is an EngineClient coordinator around `OfflineScheduler`, not
a second scheduler. Its 60 Hz callbacks are for a future local/offline play world.
They are not authoritative MMO simulation, client prediction or network messages.

## Frame and fixed-tick observations

The serialized client owner polls the window, captures input once, then calls
`advance(now, input, control, callback)`. Each capture is supplied exactly once.
The caller keeps that owned frame snapshot for presentation even when no fixed
tick executes; consuming tick input never changes it. There is no renderer yet.
The callback borrows its snapshot only for that synchronous call; copy to retain
it. Do not destroy the coordinator from its callback or call it concurrently.
Nested advance returns conflict without disturbing the outer pass.

When no tick executes, the coordinator retains a bounded pending snapshot. Latest
held state and cursor replace earlier observations; edge/repeat and focus flags
coalesce. A quick press/release retains both edges. Text appends in order up to
64 scalars, retaining its accepted prefix and an overflow flag. Scroll adds both
axes atomically only when both sums are finite; otherwise it keeps prior sums
and marks overflow. These bounds apply across captures as well as within one.

The first executed tick receives the pending interval. Later catch-up ticks see
held state, focus and cursor, with edge/repeat, text, scroll, focus transitions
and overflow flags cleared. A press therefore never repeats once per catch-up
tick. Cancellation, callback errors and exceptions cannot cause a replay: the
existing scheduler remains terminal after cancellation/fault, retaining tick
progress/debt for diagnosis. A failed callback never advances its tick index.

Snapshots normally come from `InputState`/`Window`. Oversized text lengths,
nonfinite scroll/valid cursor coordinates and invalid Unicode scalars reject
before changing pending input or timing. This validation does not turn raw input
into an authenticated or versioned gameplay protocol.

## Pause, focus and stepping

`OfflineFrameControl` separates manual `paused`, per-call `single_step` and
observed `iconified`. An unfocused or iconified window pauses offline ticks;
iconification also reports presentation suspended. Native event polling continues
so restore, focus and close still work. Restoring cannot clear manual pause.

Core's `OfflineControl` extends the existing scheduler. Entering pause clears
whole and fractional time debt. Paused observations update only the clock anchor;
the first resumed pass also executes zero ticks and excludes paused wall time.
Clock regressions never lower the previous high-water observation. Single-step
requires pause, executes exactly one successful callback and remains paused with
zero interpolation alpha. The client additionally requires explicit manual pause
and an active window for step; invalid requests reject before input/time mutation.

Entering or remaining paused discards pending positive edges/repeats, text and
scroll. Latest held state, release edges and diagnostics remain observable. A
step may consume positive actions from that step's current capture, together
with latest held/released state; earlier paused actions never replay. New actions
in the first resumed capture may wait for the next scheduled tick.

Focus loss cancels older pending positive actions before merging the new capture.
It releases held controls and clears the cursor. If loss and regain both occur
within one capture, newly accepted post-regain actions survive. Automatic timing
pause follows the focus/iconify state observed at each frame; a loss/regain wholly
between frames cancels old input but cannot reconstruct an unobserved duration.
The ordinary long-frame bound still applies. Overflow flags survive focus cancellation.

This coordinator never touches `ServerScheduler`. Tests progress a separate
20 Hz server schedule while local input is unfocused, minimized or manually
paused. Actual online session neutral-input messages and prediction belong to
M02/M04. Pausing a local window grants no control over the shared world.

## Long frames and window sample

Running offline timing retains the existing rational 60 Hz policy: admit at most
250 ms per pass, execute at most eight callbacks, report/discard remaining whole
ticks and retain the fractional remainder. `StepReport` exposes executed,
discarded and pending counts, clamped delta and interpolation alpha in `[0,1)`.
Discarding time does not advance indices for unexecuted callbacks. Callback work
itself must remain bounded; a callback budget cannot interrupt arbitrary code.

Runtime and Editor share the same pump and accept:

```powershell
.\build\windows-debug\bin\Editor.exe --window
.\build\windows-debug\bin\Runtime.exe --window --window-ms 1000
```

F6 toggles manual offline pause. F7 requests one step while manually paused and
active; OS key repeat does not generate extra steps. These two keys are reserved
by the sample and removed from tick input. Control changes emit a small JSON
diagnostic with pause state and next callback index. This is development sample
output, not an authoring command API. The callback currently performs no world
work, so `--window` stays empty. The 05b [`--graphics` sample](presentation-lifecycle.md)
uses the same snapshot/control helper and offline scheduler, then presents a clear
color through the owned device. F6/F7 pause or step empty offline callbacks;
they do not freeze visible presentation or a Server. Neither mode is an authoring UI.

The pump reports callback diagnostics outside native callbacks and stops before
another tick when close is observed. Runtime/Editor own RAII cleanup on failure.
The bounded sample mode measures elapsed time after native creation and requests
waits of at most 10 ms; it is not a real-time bound on OS calls. Native qualification
uses the existing external 60-second process-tree watchdog.

## Qualification

`shark.client.frame` supplies deterministic input/time fixtures; added window
fixtures exercise the shared pump, control keys, diagnostic failures and close
suppression through an injected backend. Core timing tests cover pause/step
failures, regression, cancellation and overflow. These tests require no display.

The explicit `WindowSmoke` uses real Windows focus/iconify/restore and targeted
messages to its owned HWND, plus controllable scheduling observations. It proves
the shared pump's first-tick input, native sample controls, bounded stall handling
and independence from a separate server schedule. It does not inject system-wide
input, qualify physical devices or render graphics. The
[04c handoff](handoffs/04c-frame-loop.md) records the final current-PC results.
