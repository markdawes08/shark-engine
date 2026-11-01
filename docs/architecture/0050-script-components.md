# ADR 0050: Inert authored Script settings

Date: 2026-11-01. Status: accepted for checkpoint 19b.

Store one optional bounded project-scoped Lua source reference and enabled flag
in Core Scene, with no file I/O or execution. Reuse Scene validation and the
existing retained authoring owner for both Editor and terminal. Source schema 1
adds a version-1 component; prepared scene v4 preserves it as inert data while
v1-v3 remain readable and byte-stable when their original inputs are encoded.
Standalone export and runtime activation must reject Script-bearing scenes until
they can actually package and run the intended content. Viewport and physics
preview may reconstruct the inert component without granting execution.

The server/client capability policy, callback lifecycle and cooked script closure
remain explicit 19c and later work. An inert component may be stored on a replica
but does not replicate itself or grant authority. See the
[component contract](../script-components.md), [Lua VM](../lua-vm.md) and
[checkpoints](../roadmap/lua-plan.md).
