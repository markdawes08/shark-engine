# Application ownership and lifecycle in 03b

The [Core checkpoint plan](roadmap/core-plan.md) implements lifecycle ownership
for existing resources before adding scheduling or game services. EngineCore
contains the signal resource and `ServerApplication`; `Server` links only Core.
The process root owns its logger and application. Output/diagnostic streams are
borrowed and must outlive their consumers. Editor and Runtime currently own only
their explicit bootstrap logger; they have no game services to start or tear down.

## Acquisition, activation and cleanup

`ServerApplication` validates a complete `ServerConfig` before claiming process
signal resources.
Its `ShutdownSignals` member exclusively owns a `SignalBackend`. The native
backend leases the process-wide signal registrations, saves the previous SIGINT
handler, then saves the previous SIGTERM handler. Only after both acquisitions
and successful readiness output does the application become running.

Acquisition failure unwinds successful acquisitions. Failure to publish readiness
also rolls back both registrations. Cleanup attempts SIGTERM restoration before
SIGINT restoration, even if the first restore fails. Each acquired registration
receives one explicit restoration attempt; retries cannot overwrite a saved
previous handler. Signal-resource restoration is nonthrowing and allocates no memory.
Expected platform failures use fixed flags until cleanup is complete, when the
application constructs owned diagnostics.

An explicit stop releases registrations before printing successful completion.
Failed cleanup suppresses that success line and leaves the application faulted.
A destructor performs bounded, silent cleanup for an active object. Call explicit
stop to obtain diagnostic results; destructors do not log, write output or throw.
There is no worker, network connection, window, GPU or audio lifetime to join or
drain yet. The existing idle deadline/poll algorithm is unchanged; controllable
clocks, bounded asynchronous work and deadlines for future producers belong to 03c.

## State and error behavior

Startup and shutdown transitions are explicit: stopped, starting, running,
stopping and faulted. Operations on a running or transitioning object cannot
silently reacquire resources. Reentrant start/stop calls are rejected without
changing the outer operation's ownership. Calls require serialized access by
the owning thread; this is not a concurrent service API.

Clean stop is idempotent and does not print another completion line. A clean
startup failure can be repaired and retried on the same object. Each new native
lease resets the previous stop request; another active owner cannot reset it.
Failed handler restoration faults the object and retains the native process
lease as unavailable. Restart the process after such a failure; claiming clean
reuse could lose the original saved handlers. Injected test backends isolate
these failures from the test process's real registrations.

`LifecycleReport` retains an optional primary error and up to two cleanup errors
in restoration order. A readiness/configuration error is not replaced by an
error encountered while unwinding it. Server reports all retained diagnostics
through the 03a logger and exits nonzero. Configuration rejection remains exit 2;
startup, cleanup, output or diagnostic-sink failure exits 1. Help/version and
successful readiness/completion text retain their established form. No new
command flags or public authoring protocol are introduced.

## Process-global signal boundary

OS signal registrations are inherently process-wide. The native backend has a
small explicit global bridge: an atomic ownership guard and a volatile
`sig_atomic_t` request value. This is the only state used by its signal handler,
which performs one assignment. It never logs, allocates, takes locks or accesses
an application pointer. The backend lifetime and previous handlers are owned by
the application, not by a service locator or a global application singleton.

The injectable backend exposes only claim, handler exchange, request inspection
and release. It must be nonthrowing and must not allocate during lifecycle calls.
The application converts its fixed outcomes into the existing structured errors
outside resource cleanup. The native adapter's signal restoration failure is
reported as a failed cleanup, not proof that the OS released the registration.

## Verification scope

`shark.core.lifecycle` uses seed 1337 and a 15-second CTest timeout. Fixtures cover
each acquisition failure, readiness/output failures, reverse cleanup despite
errors, retained primary/cleanup context, ownership, repeated cycles and actual
native signal installation/restoration. Existing Server process tests separately
exercise timed operation, POSIX signals and I/O failures under WSL. Native
Windows and WSL evidence is recorded in the
[03b handoff](handoffs/03b-application-lifecycle.md); these tests do not establish
MMO persistence, gameplay, authoring or a graphical editor.
