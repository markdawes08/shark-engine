# Time and bounded work

Assignment 03c adds device-free scheduling primitives and a bounded completion
channel to EngineCore. They extend the [Core contracts](core-contracts.md) and
[application lifecycle](application-lifecycle.md). The Server bootstrap uses the
injectable idle clock; it does not execute a pretend world or count idle polls
as authoritative ticks. Real simulation attaches to the scheduler in its owning
assignment. There is no worker pool, task executor or service registry.

## Clock and application ownership

`MonotonicClock` supplies nanosecond observations and an expected-result wait.
Time values are nonnegative and unrelated to civil time or durable deadlines.
`SteadyMonotonicClock` measures from its instance's steady-clock origin and accepts
wait requests from zero to one second; tests explicitly advance a fake clock.
The application borrows its clock for `run_idle`, retains signal
ownership during that call and rejects nested run/stop transitions.

The idle Server checks signals/cancellation between waits of at most 20 ms,
shortening the final wait to the configured deadline. OS scheduling may delay
wakeup; this is a requested wait bound, not a real-time latency guarantee. A
regressing/stalled clock, overflowing deadline or failed/throwing wait returns
an owned diagnostic. The caller then explicitly stops the application, preserving
both the timing error and any cleanup failures. No window/focus state enters this
path. CLI flags and successful readiness/completion text are unchanged.

## Separate scheduling profiles

`ServerScheduler` and `OfflineScheduler` are noncopyable/nonmovable, uniquely
owned instances created from validated configuration and a clock origin. Polls
and callbacks belong to one serialized owner. A callback receives the next
integer tick index and returns `Result<void>`; only successful completion debits
debt and advances that index. Construct callbacks outside the poll. Reentrant
polls fail without changing the outer pass; never destroy a scheduler inside its
callback. An error or exception faults the scheduler, retaining completed progress
and unexecuted debt instead of retrying possibly mutated state.

| Policy | Authoritative Server | Local/offline |
| --- | --- | --- |
| Rate | Configured integral 1..1000 Hz, provisional default 20 Hz | Fixed 60 Hz |
| Catch-up | Configured 1..1024 callbacks per pass, default 8 | At most 8 callbacks per pass |
| Admitted time | All monotonic elapsed time | Clamp incoming delta to 0..250 ms |
| Remaining whole ticks | Retain debt; configured limit 1..1,000,000, default 200 | Report and discard overdue whole ticks |
| Excess debt | Sticky overload diagnostic; no further execution | Explicit discarded count in the report |
| Clock regression | Sticky diagnostic | Clamp to zero and retain the previous high-water clock |
| Fractional remainder | Exact rational units | Exact rational units; interpolation alpha in [0,1) |

The integer accumulator represents nanoseconds times the integral tick rate,
with 1,000,000,000 units per tick; 60 Hz is not rounded to an integer nanosecond
period. Checked arithmetic prevents time or index wrap. Overload retains its
offending debt snapshot but stops accepting further time, so debt cannot grow
indefinitely. The defaults are provisional policy values, not measured MMO
capacity. An owner must respond to a fault by controlled shutdown/recovery;
admission/replication load shedding and durable recovery arrive with their owners.

Cancellation stops before another callback. The scheduler retains its remaining
debt and becomes terminally cancelled; deliberately construct a new instance to
restart. It cannot preempt a callback already running. Callback work must be
bounded by its subsystem; the callback budget alone cannot bound arbitrary code.
Offline discarded time never advances indices over unexecuted callbacks. Offline
pause/step, window focus/input and presentation integration belong to 04; server
scheduling has no such pause input. Network snapshots, prediction and durable
wall-time policies remain separate later implementations.

## Bounded completions and backpressure

`CompletionQueue<T, Capacity>` holds a fixed inline ring of value envelopes:
1..4096 slots and at most 1 MiB of inline optional payload storage. Payloads must
have trivial, nonthrowing move construction and trivial destruction, preventing
user move/destructor callbacks under the lock. These type checks do not bound
memory reached through pointers; use owned bounded values, not pointers to mutable
scene state or dynamically growing graphs. Construction and standard-library
synchronization may allocate or throw; transfers perform no payload allocation
or logging.

`try_push` returns accepted/full/closed. `wait_push` also accepts a stop token and
returns cancelled; rejected pushes retain the caller's value. Observed cancellation
takes precedence over closure or free capacity, but cancellation racing the final
acceptance check can lose. Accepted values remain owned by the queue until popped.
FIFO is acceptance lock order, not a deterministic ordering between producers.
The owner applies popped results outside the queue lock; authority/epoch validation
for future network/storage results belongs to those subsystems.

`close` rejects admission and wakes blocked producers while retaining accepted
values. The owning lifecycle closes admission, requests producer stop, joins all
producers, then drains or invalidates completions before releasing the queue and
consumer. Destruction requires no live users; the queue cannot join borrowed
threads or forcibly interrupt producer computation. `waiting_producers()` is a
diagnostic snapshot, not an admission guarantee. The test fixture uses real
`std::jthread` producers and an owner consumer to exercise blocking, wakeup,
cancellation, bounded error publication and teardown. Production Server currently
owns no workers; this primitive does not invent one to serve an idle loop.

## Callback and thread rules

| Boundary | Allowed work and enforced behavior |
| --- | --- |
| Application/simulation owner | Serialized ordinary-thread operations may allocate and log. Scheduler bookkeeping on successful polls allocates no storage; user callbacks own their own budgets. Failed callbacks retain context and stop tick advancement. |
| Ordinary worker entry | Compute isolated values, check cancellation and publish bounded results. Catch exceptions before leaving the thread entry; tests translate a thrown operation into an inline failure completion. Never mutate owner/scene storage. |
| Completion transfer | Brief mutex locking and stop-aware blocking are allowed. No payload callbacks or logging occur under the lock. Not a signal/audio/real-time queue. |
| Signal bridge | Existing `sig_atomic_t` assignment only; no allocation, logging, locks or application pointer. |
| Future audio, physics, Lua and render callbacks | ADR 0001/0005 restrictions remain required at integration: audio gets preallocated nonblocking messages; physics workers never call Lua; scene mutation stays on its owner; GPU retirement waits for submitted use. These systems are not implemented here. |

The [03c handoff](handoffs/03c-time-and-work.md) records verification and the full
assignment-03 gate. External CPU/thread/process fixtures are appropriate before
FeatureWorld exists; no component, command or Lua coverage is invented.
