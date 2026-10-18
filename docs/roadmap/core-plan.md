# Assignment 03 checkpoints

Assignment 02 is complete under the [current-machine scope](../architecture/0010-current-machine-scope.md).
Assignment 03 retains one owner and one overall requirement, `REQ-03`; the
checkpoints below provide independently reviewable implementation steps. The
whole assignment remains NOT_RUN until all three checkpoints pass. Native
Windows and the existing WSL Ubuntu CPU/headless environment provide the gates;
hosted CI is supplemental. No GPU run is needed for changes confined to Core.

1. **03a - Core contracts:** introduce owned structured errors/results and
   synchronous structured diagnostics, independently validated startup
   configuration, persistent UUIDv4 identity with injectable entropy, and
   portable project-relative paths anchored to a project root. Integrate errors
   with the existing executable boundaries. Test malformed input, lifetime of
   retained context, sink/entropy failures, Unicode/spaces and path escapes in
   Debug, Release and supported sanitizer configurations. Keep Core/Server free
   of presentation and authoring dependencies.
2. **03b - Application ownership and lifecycle:** give application roots explicit
   service ownership, startup ordering, reverse teardown and partial-start
   rollback. Exercise injected failure at each acquired stage, repeated cycles,
   ownership and cleanup failures using real owned resources where they exist.
   Extend the headless idle process without inventing unavailable game services.
3. **03c - Time, bounded work and qualification:** introduce controllable clocks,
   distinct offline/client and authoritative-server timing policies, and only
   the bounded completion queues needed by actual producers/consumers. Exercise
   monotonic scheduling, overload/backpressure, cancellation and stop/join/drain
   behavior. Server simulation must not depend on window focus or silently drop
   authoritative ticks. Complete callback/thread rules, regression qualification
   and the full `REQ-03` gate before assignment 04.

The [MMORPG plan](mmorpg-plan.md) and [human/agent plan](human-agent-plan.md)
continue to apply. UUID primitives do not implement authentication, durable
storage or runtime generational handles. Structured bootstrap diagnostics are
not the authoring JSON-RPC protocol. Project documents, transactions and CLI
authoring arrive in their existing owners; source schemas are not invented here.
Assertions diagnose programmer invariants, while validation returns errors in
both Debug and Release. Synchronous logging/entropy/filesystem calls are not
permitted in real-time callbacks or signal handlers.

**03a and 03b are complete within their checkpoint scopes.** The
[03a handoff](../handoffs/03a-core-contracts.md) and
[03b handoff](../handoffs/03b-application-lifecycle.md) record the delivered
interfaces, exact local evidence and limitations. **03c is next**; do not advance
directly to assignment 04. Full `REQ-03` remains NOT_RUN pending 03c.
