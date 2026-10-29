# ADR 0040: Human editor shell over the retained authoring owner

Date: 2026-10-29. Status: accepted design; qualification belongs to the
[16a handoff](../handoffs/16a-editor-shell.md). Owner: assignment 16.

The native Editor is another adapter to the existing assignment-15 authoring
service. An Editor-private `ProjectSession` owns one `AuthoringProtocol` instance
in process. That instance retains ProjectFiles, AuthoredScene, command history,
retry outcomes and import ownership. The editor caches owned display values only;
it has no second mutable Scene, direct registry path or alternate file writer.
Using the same bounded methods keeps component validation, source publication
and revision checks aligned with terminal clients without a subprocess or network
listener. Request correlation remains distinct from mutation retry identity.

Project opening adopts a complete validated owner. Save uses the observed scene
revision and preserves the original publication/durability receipt. Close is an
explicit save, discard or cancel decision when dirty; discard remains revision
bound. A successful save followed by a failed close must remain distinguishable
from a failed save. Another cooperating writer receives the existing conflict;
human/terminal handoff uses save/close/reopen. History stays with the live owner
and is not serialized for a later process.

Use pinned Dear ImGui privately in Editor, with Shark adapters for InputSnapshot
and GraphicsDevice. Upstream native/render backends do not own another GLFW loop
or Vulkan device. No ImGui type enters EngineCore, EngineTools or public Client
contracts. The dependency is populated only for native Editor builds; Runtime,
Server and independent CPU Tools never consume it. The
[dependency review](../dependencies/imgui.md) records exact inputs and notices.

Client exposes bounded engine-owned GUI atlas/frame values and a distinct offline
shader interface. One immutable atlas and per-frame geometry use the existing
presentation owner, submission completion, resize and retirement machinery.
GUI RGB is linear before alpha blending; the sRGB attachment encodes it once.
The Editor adapter converts UI colors and logical positions explicitly. Native
window handles remain private, and no shader compiler is loaded at startup.

The main thread owns the window, UI and authoring session. Native close requests
are cancellable before teardown so the UI can resolve dirty state. Focus loss
clears held/queued input. Text/modal capture suppresses global shortcuts; no
gameplay input consumer runs in this shell. Future viewport interaction must
preserve this routing contract.

This increment supplies project controls, not complete scene editing. The
[five checkpoints](../roadmap/editor-plan.md) retain hierarchy/selection,
inspector/history, asset/console and actual bidirectional UI/terminal acceptance.
Navigation, picking and gizmos remain assignment 17. Full REQ-16 stays NOT_RUN
until its connected acceptance is complete. No source/protocol format version
changes or migration are introduced by the shell.
