# Script component and authoring

19b adds an optional engine-owned `Script` value with `source` and `enabled`.
`source` is a portable, project-relative UTF-8 path of at most 4096 bytes under
the exact lowercase `scripts/` directory, with a nonempty filename and lowercase
`.lua` suffix. It follows the existing `ProjectPath` rules, including rejection
of traversal, device names and controls. `enabled` defaults to true. The default
reference is `scripts/main.lua`. Validation does not read files, establish file
existence, compile source or authorize execution. Scoped terminal text operations
remain the existing 32 KiB, revision-bound `text.read`/`text.write` interface.

Scene setters, deferred edits, owned queries and schema discovery use the same
validator. The retained authoring owner supplies `SetScriptCommand`, optional
`script` on create, atomic batches, history, exact retries and source capture.
The terminal's `set_script` and the Editor inspector are adapters over those
same services. `null` removes the component. Inspector Apply remains one atomic
history group, and an unapplied draft is not saved. Native references and source
text are distinct: editing either does not implicitly mutate the other.

Source scene schema 1 adds optional `shark.script` component schema 1; its source
path is required and `enabled` defaults to true. Unknown versions/fields reject.
Earlier documents remain supported and an older reader rejects the unknown
component. Prepared scenes without Script retain their existing v1-v3 encoding;
v4 preserves this value as inert data for scene reconstruction, viewport and
physics preview. V4 is a new format and older decoders reject it. A reference is
not a cooked script or a dependency closure: standalone export and runtime
activation reject Script-bearing scenes until actual execution and cooking land.

The component can be stored in authored, authoritative or replica scenes; the
value itself grants no authority and is not replicated by this change. The [19c runtime](script-runtime.md) separates authoritative-server gameplay
and client-presentation capabilities. No player gains script upload or arbitrary
server execution, and rendering cannot grant gameplay outcomes. Source resolution
and connected execution and recovery remain later checkpoints; see the
[Lua plan](roadmap/lua-plan.md).
