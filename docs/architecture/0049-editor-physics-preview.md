# ADR 0049: Shared editor and terminal physics preview

Date: 2026-10-31. Status: accepted implementation decision for 18e.
Extends [ADR 0047](0047-fixed-step-physics.md) and
[ADR 0048](0048-physics-contacts.md). The [preview contract](../physics-preview.md)
defines the public operations and limits.

## Decision

The existing retained project session owns one optional CPU physics preview.
Both the native Editor and terminal `scene.physics` requests use this owner.
Starting captures a complete prepared runtime Scene with immutable client mesh
assets, then creates its native bodies and acquires the existing simulation
lease. No authored Scene, history entry, dirty baseline or saved file is changed
by play, stepping, runtime body removal or stop. There is no apply-play-changes
operation. Lua and gameplay execution remain future assignments.

Preview requires the project's declared startup Scene, an explicit Camera, current
source revision and client content target, using the existing export
preparation/asset freshness gates. This is the
connected scene preview profile, not a replacement for camera-free Core physics.
Its frozen runtime Scene has a fresh incarnation. Display extraction uses the
existing physics render poses and immutable geometry; it cannot authorize source
picking or transform manipulation. Source mutations reject while play owns the
preview. Stop releases runtime ownership before source editing resumes.

One engine-generated preview session ID fences every runtime operation. A
separate monotonically sequenced operation identity distinguishes removal from
steps even when no tick advances. Bounded retained original outcomes support
exact retry/reconciliation; request correlation alone cannot authorize repetition
of native work. Native faults preserve the last complete Scene and do not claim
rollback. Scoped stop remains available during error recovery and project close
destroys the preview before releasing the project writer.

The Editor owns only display observations and an existing OfflineScheduler.
Pause, blocked input/focus and single step use the established offline timing
policy. It does not introduce a second Scene writer or scheduling framework.
Human RigidBody drafts expose the same eleven schema fields and one retained
history batch as terminal component edits.

## Compatibility and qualification

`scene.physics` and the discovery capability are additive protocol-version-1
surfaces. Source/component/prepared formats retain their existing versions and
compatibility rules. Core/Runtime/Server receive no authoring dependency.
Native Jolt types and handles never cross the protocol boundary. Runtime
observations carry the preview session scope and owned entity identities.

CPU fixtures qualify shared ownership, exact retries, source preservation,
runtime poses/contact removal and lifetime cleanup. Actual input, rendered
motion, focus and connected saved-project continuation require the current-PC
GPU checks in [physics acceptance](../physics-acceptance.md). Headless results
alone do not close REQ-18 or the earlier editor/viewport UI gates.
