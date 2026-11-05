# Prefab authoring

Editor and terminal prefab operations use the retained project session and its
existing AuthoredScene. They share [source values](prefab-assets.md),
[instance commands](prefab-instances.md), file revisions, history and retained
mutation outcomes. No second Scene, asset catalog or source importer is created.

## Files and publication

`ProjectFiles::read_prefab` and `write_prefab` accept project-relative
`.sharkprefab` paths. An existing adjacent `.sharkprefab.sharkasset` must match
the source ProjectId and AssetId. The session also checks the open project and
rejects paths overlapping its project or declared scene sources, including
case-only aliases. The source codec remains bounded at 4 MiB.

Create the sidecar explicitly through `asset.metadata.write` before publishing
new prefab source. These are two separate file publications. If the source write
fails, the sidecar remains available for repair. Scene Save, undo and redo do not
write either file. A declared `shark.prefab` importer identifier is metadata;
there is no prefab cache importer or cooked prefab package support in this step.

Reads return opaque session/path/exact-content revision tokens. Writes require
that token, or explicit null for create-only, and return the actual publication
and durability receipt. A limited durability success still means published.
After uncertain completion, observe the file again and compare its contents;
do not assume rollback or recreate the asset identity. Tokens are released
explicitly or with the session.

## Terminal methods

`protocol.discover` advertises `prefabs` and the method schemas. The existing
64 KiB complete request and 1 MiB complete response bounds apply, including JSON
framing and escaping; large valid typed documents can exceed this transport.
Script text retains its separate 32 KiB bound. A read or preparation token is
admitted only after the complete response has been encoded and bounded.
The session retains at most 32 source tokens, eight prepared commands and 16 MiB
of charged source/preparation input. Release unused tokens instead of opening
unbounded observations. These charges are logical limits, not process memory measurements.

| Method | Operation |
| --- | --- |
| `prefab.read` | Read `path`; return canonical `document` text and exact `revision`, plus asset/root/count observations. |
| `prefab.write` | Publish `path` and `document` with `expected_revision` token or null. Return a new token and publication receipt. |
| `prefab.capture` | Capture `entity_id` and descendants at `expected_revision` into a source draft with the supplied `asset_id`. No file or Scene mutation. |
| `prefab.prepare` | Use a source `revision` token and expected Scene revision; retain one fresh remapping with optional `parent_id` and simple Name/Transform `overrides`. Return a `preparation` token and root/remap observations. |
| `prefab.instantiate` | Apply `preparation` using the existing `mutation_id`; return the normal three-level retained mutation outcome. |
| `prefab.release` | Release a source `revision` token. |
| `prefab.release_preparation` | Release a `preparation` token after its outcome is reconciled. |

Capture preserves selected subtree source order, IDs, components, local TRS and
entity extras, clearing only the selected root's external parent. It preserves
local values rather than baking the former parent's world transform. The new
source has its own asset identity and no live connection to the captured entities.

Preparation checks the observed source and matching sidecar again. The retained
command is then immutable: later source changes cannot silently retarget an
instance or its retry. Instantiation runs the same atomic candidate validation,
history and exact-input retry matching as the C++ command. A changed Scene can
reject a new mutation; an identical retained retry still returns its original
receipt. Request correlation IDs do not deduplicate writes.

Retain the preparation and mutation ID until completion is known. Query
`scene.mutation_outcome` after a lost reply, or replay the exact prepared intent.
Preparing again allocates new instance IDs and represents a new intent. Tokens
do not survive session close/reopen, and unknown/expired outcomes require explicit
reconciliation. Play blocks prefab writes, preparation and instantiation; stopping
preview does not publish a draft.

## Editor

The Prefabs tab adapts these same methods. Open a file or capture a selected
subtree into a new draft; edit and explicitly Write it. Create the matching
sidecar in Assets first. Prepare and Instantiate publish one Scene history group.
Save does not apply the source draft. File publication uncertainty and mutation
uncertainty remain separate and require their own reconciliation.

Unapplied or uncertain prefab drafts participate in Play and close/discard
decisions. Native interactive acceptance remains deferred under the current
[manual UI pause](testing-policy.md#manual-ui-testing-pause-2026-11-01); portable
panel tests establish adapter behavior, not actual desktop input qualification.
