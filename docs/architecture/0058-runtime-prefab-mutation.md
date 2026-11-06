# ADR 0058: deferred runtime prefab mutation

Status: accepted, 21c; extended by 21e. Date: 2026-11-05.

## Decision

Extend the creating-thread ScriptRuntime with an owned Core prefab catalog and
revocable trusted-host grant. Keep source JSON, history and file publication in
Tools. Runtime instances receive fresh WorldEntityIds; their checked generations
and Region authority remain distinct from prefab-local and durable identities.

Lua spawn and self-subtree destruction share one ordered bounded queue with
contact physics writes. Drain after the entire callback batch. A narrow private
structural port checks both Script and Physics leases without unbinding physics
or opening a public mutation bypass. Initialize complete transforms/hierarchy,
then native bodies, then Lua load/init. Reverse script teardown precedes body
removal, VM release and checked Scene erasure.

## Outcomes and compatibility

The [runtime spawning contract](../runtime-spawning.md) defines bounds, sequences,
fences and receipts. Exact retries return their original outcome; stale issuers,
expired outcomes and revoked/foreign grants cannot become a new mutation.
Receipts report partial native/script effects without promising rollback.

Trusted hosts supply the grant after actual ownership admission. Region metadata
alone is not authentication. This surface creates ephemeral actors and exposes
no reward or durable-record operation; economic deduplication and M04/M05 network
and handoff gates retain their separate owners.

No source, prepared-scene, wire or database version changes. Lua API version 1
gains documented functions with explicit rejection outside supported profiles.
Source importers stay excluded from Core/Runtime/Server. 21d supplies shared
Editor/terminal adapters. 21e adds a pure Tools source-to-Core value conversion
and [connected acceptance](../prefab-acceptance.md), preserving the existing
catalog, host admission and separate native Editor interaction gate. No new
cooked prefab format or source loader enters Runtime.
