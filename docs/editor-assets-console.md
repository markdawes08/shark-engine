# Editor assets, jobs and console

16d adds Assets and Console tabs beside the existing Scene workspace. The
[handoff](handoffs/16d-editor-assets-console.md) records selected verification;
actual UI acceptance remains NOT_RUN under the
[desktop pause](backlog.md#desktop-ui-deferral). Full REQ-16 stays NOT_RUN.

## Asset observation and metadata

AssetPanel borrows the same ProjectSession and public authoring methods as the
terminal. It creates no catalog, importer, worker, directory watcher or Scene.
The panel is bound to the original scene incarnation; close/reopen or activation
invalidates it. Its cached values are observations, not authoritative state.

Load inventory takes an explicit complete list of sidecar paths, bounded to
16 KiB of text and the existing catalog entry/path limits. It observes the
current catalog revision, then performs one refresh. Refresh catalog separately
loads the first page. Pages contain at most eight complete metadata records,
ordered by asset ID and tied to one catalog revision; stale paging rejects.
Changing pages or refreshing clears selection. Omitting a sidecar from inventory
removes only its derived catalog record, never a source file.

Records show source presence, stable ID, sidecar, importer, kind, audience,
provenance, license and dependency count. Source presence does not establish
import success. Importer discovery shows only the registered implementations.
The advanced metadata editor accepts the complete existing schema-1 JSON
envelope; a new template uses a selected registered importer and fresh asset ID,
leaving required provenance, license and audience for explicit completion.
This increment does not add a file picker, source copier or automatic type inference.

Read metadata retains one exact owner/path/file token. Repeated reads release the
previous token; writes replace only that observed path or explicitly create an
absent sidecar. Validation uses the existing metadata codec and protocol. Drafts
have the native 64 KiB limit; the protocol's smaller complete-request budget can
reject an envelope once framing/path overhead is included. A successful write
retains the actual publication/durability receipt and replacement token.
Rejected writes retain the draft. Metadata writes are immediate source edits,
outside scene undo, and do not refresh the catalog or import implicitly.

Scene Save applies neither metadata nor inspector drafts. Closing first resolves
unapplied authoring drafts, then the scene's existing save/discard/cancel decision.
Discarding a metadata draft releases its observation token. A release error keeps
the close decision open. Writing metadata is explicit and is not undone by
discarding an unsaved scene.

## Cache and import jobs

Target ID and client/server side are explicit. Cache observation reports
fresh/stale/missing, required and last-valid keys, and blocked-input diagnostics.
Inspecting a retained artifact shows its identity, versions, dependencies and
byte count; it does not certify current freshness. Existing privacy and fresh
dependency-closure checks remain authoritative. Cache maintenance/recovery remains
available through terminal methods; the UI does not advertise it as implemented.

Import selected submits captured inputs at the observed catalog/cache revisions
to the existing four-slot service. Pending deduplication and all original input
fences remain intact. Refresh jobs observes only. **Publish ready result** drains
at most one ready completion; a failed terminal job can be returned by a
successful drain call. The panel shows the original status, error, artifact key,
commit and durability. A directory-flush warning still means publication occurred.

Cancel requests cooperative stop; it cannot undo a committed result.
**Acknowledge result** takes a terminal status and releases its retained slot.
The most recent cancel, drain and take receipts remain separate from the observed
job list, so a later refresh failure cannot hide the original decision. Neither
drawing nor polling automatically drains or takes work. Publication rejects stale
captured inputs and preserves last-valid output. Project close joins/stops the
existing worker before releasing ownership, without publishing unfinished work.

Catalog/metadata/publication changes invalidate the cached viewport. Returning
to Scene requests fresh content through the existing explicit-camera service;
it does not import or silently authorize stale assets.

## Structured console and observation failures

The session console records actionable authoring replies, errors and job state
transitions. It uses the existing Error/LogLevel values, request correlation and
publication outcomes. It recognizes nested retained-mutation rejection and failed
job outcomes independently of RPC success. Successful read-only observations
are omitted; repeated observation errors are coalesced. Distinct actions retain
separate entries. Clearing the console changes display history only.

Retention is 128 entries with bounded text: 1,024 bytes for messages, 4,096 for
paths, and smaller method/identity fields. Display sanitation preserves UTF-8
boundaries and marks shortened/replaced text. Old or unavailable entries are
counted. The UI shows sixteen summaries per page and one selected detail view;
selection uses the entry sequence rather than a shifting row index. Raw requests,
source documents and binary payloads are not retained. This is a session display,
not a durable audit log, gameplay console or arbitrary command executor.

Recording is best-effort and cannot replace the original operation reply. If a
post-request project-status refresh fails, that reply remains available and
`state_current()` becomes false. The shell offers explicit status refresh and
blocks ordinary edits based on stale observations; bounded job observations and
scoped cancel/take/token-release remain available for cleanup. Open consumes its
own complete status response before constructing panels; confirmed close records
closed state. All operations still use the shared owner's fences.

No engine public API, terminal method, component/source format or FeatureWorld
case is added. Core, Client graphics, Runtime and Server remain independent of
these Editor-private panels. Connected human/terminal acceptance is 16e.
