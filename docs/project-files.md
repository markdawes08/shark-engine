# Project files and external-edit conflicts

08c adds the device-free EngineTools [ProjectFiles API](../engine_tools/include/shark/tools/project_files.hpp)
over the existing [source codec](source-documents.md) and
[authored Scene owner](authored-scenes.md). It performs typed project/scene I/O;
15b adds scoped raw script text through the same native storage owner.
Core, Runtime and Server never load these documents. Follow the
[persistence checkpoints](roadmap/persistence-plan.md) for qualification status.

## Root ownership and paths

Open an existing native project directory. Its canonical location and native
identity are retained independently of the process working directory. Methods
require the creating thread, checked before mutable service access. Destruction
must not race operations. A persistent `.shark-write.lock` holds a nonblocking
native exclusive lock for the service lifetime. A second cooperating Shark
owner rejects with `conflict`; process death releases ownership without deleting
or interpreting a stale PID file. Do not remove or replace that lock while open.
Writers of overlapping paths must select the same canonical project root;
separate nested roots do not share this lock.
This storage lock does not implement the later shared editor/terminal project
owner, attachment protocol, unsaved history or undo.

Paths reuse bounded UTF-8 `ProjectPath`, including its traversal, device-name,
ADS and malformed-encoding rejection. The file service additionally limits paths
to 64 components and each inspected directory to 8192 entries. Every existing
component must be an ordinary directory or a single-link regular source file.
Symlinks, junctions, other reparse points, hard-linked files, device files, FIFOs
and directory targets reject, including links that remain inside the root.
Root and held ancestor identities are rechecked before publication. Windows
directory handles prevent rename/deletion while held; Linux uses descriptor-
relative operations with no-follow flags and identity checks.
Competing Windows opens may return a sharing conflict while an operation holds
its file handles; the API does not promise uninterrupted concurrent reader access.

Project catalogs reject ASCII case-folded whole-path and prefix spelling
collisions and overlapping scene file/directory paths. When a project file is
read or written, its known path must also not overlap any scene path. Scene-only
operations receive a catalog value without a project-file path; they cannot
infer that separate file's location. A revision binds bytes and location rather
than document kind, so an explicitly changed catalog plus the matching revision
can deliberately replace a path with another source document kind.
Native directory inspection rejects existing case aliases (including Windows
short names). Unicode and spaces are supported with exact spelling; this is not
a claim of universal Unicode normalization across arbitrary filesystems.
Catalog entries may reference missing files or missing descendants. Loading
requires a real source; saving requires existing parent directories. No directory
tree is created and no unrelated scene content is loaded by catalog validation.
Any component named `.shark-write.lock` or starting `.shark-stage-` is reserved
case-insensitively. Source format headers, rather than filename extensions,
identify document kinds.

## Content revisions and conflicts

The [08d integrated fixture](handoffs/08d-integrated-qualification.md) connects
these file guarantees to migrated documents and the existing authored owner.
Native fault cases preserve complete dirty snapshots; process interruptions are
followed by exact scene activation, an authored recovery save and another reopen.

Reads are bounded to 4 MiB before decoding and return owned documents, the
original source schema version and an opaque `FileRevision`. This token binds
the canonical root, relative path and exact original bytes. Copies share
immutable bytes. It is not a timestamp, file size, raw native identity or a
serialized protocol token. Same-size/same-timestamp edits and whitespace changes
conflict. Returning to exactly the original bytes is accepted: revisions describe
content, not the history of edits. A token can survive service close/reopen for
the same canonical root/path; a moved-from token is invalid.

Writes require either a revision from that path or `nullopt`, meaning the
destination must be absent. A token from another path or root rejects. Candidates
are completely validated and encoded before staging; scene saves also reuse
AuthoredScene's composed-transform validation. Existing bytes are checked both
before staging and immediately before publication. A mismatch returns `conflict`
and preserves the external file. The caller can reread for its current revision,
compare owned values and deliberately retry. Scene operations use the supplied
or captured project catalog snapshot; they do not automatically reload external
project-file changes or require its on-disk revision. Reload the project explicitly
when adopting catalog edits. The conflict boundary is one file, not a transaction
across the project and its scenes. No silent merge or forced overwrite
is provided. Codec errors retain JSON-pointer field context; filesystem errors
identify the relative path. Public operations use `source.file.open`,
`.read_project`, `.read_scene`, `.write_project`, `.write_scene`, `.load_scene`,
`.reload_scene` and `.save_scene` with owned `Result`/`Error`.

The lock coordinates Shark writers. External editors may modify documents
**between** operations, but must not race the final compare and native rename.
Ordinary filesystem APIs do not provide a portable compare-and-swap of arbitrary
file contents against an uncooperative writer. Likewise, hostile directory
topology changes must not race the final identity check and syscall. Retained
handles, no-follow opens, sharing restrictions and rechecks reject observed
changes; this service is not an adversarial OS sandbox or a distributed lock.

## Scoped script text

`read_text(path)` returns `ReadText { text, revision }`; `write_text(path, text,
expected)` returns the existing `FileCommit`. These operations use the same
creating-thread guard, root lock, native path checks, exact-content revisions and
atomic publication as typed documents. `nullopt` is create-only. They never create
parent directories or silently overwrite external changes.

Paths must start with exact lowercase `scripts/` and end with exact lowercase
`.lua`; nested directories are allowed. Content is at most 32,768 UTF-8 bytes,
permits empty text and line breaks, and rejects NUL or malformed UTF-8. Reads and
writes preserve exact bytes, including line endings. These are raw source-file
operations, not Lua parsing, validation, execution or a claim of an implemented
Lua API. The scripting subsystem remains assignment 19.

This path domain does not infer document kind from extension. A caller that owns
an active project must also exclude that project file and every declared scene
path, including case-folded and file/directory prefix overlaps. The terminal
adapter applies those checks and fences the retained project catalog before text
access. Typed source changes continue through the existing document/owner methods;
raw text is not a way to replace a live scene or clear its dirty/history state.
Text file publication is separate from Scene undo/redo and mutation retries.

`project_text_maximum_bytes` publishes the byte bound. Errors use
`source.file.read_text` and `source.file.write_text` with path context.
Before-publication failures preserve old bytes; a returned `FileCommit` always
means publication occurred, even when its durability confirmation is limited.

## Publication and durability

Writes create a random, exclusive sibling stage, write the complete encoded
document, flush its file contents, recheck the destination, then perform one
native rename. Create-only publication cannot replace an existing destination.
All allocations needed for success are prepared before the rename. An ordinary
failed Result means this operation did not publish. Before publication the old
destination remains intact; normal cleanup removes only this operation's own
verified stage. Abrupt process termination may leave an unpublished sibling.
Reopening never loads, promotes or globally deletes orphan stages.

Publication installs a new regular file: Linux mode 0600, Windows parent-inherited
ACL. Original inode identity, hard links, alternate streams and arbitrary file
metadata are not preserved. The service does not make multi-file transactions.
Saving a project and a scene are separate operations; no automatic migration
writeback occurs during a read.

| Platform/outcome | Reported guarantee |
| --- | --- |
| Windows `file_flushed` | Stage data was flushed and the native rename succeeded; directory-entry power-loss durability is not confirmed. |
| Linux `file_and_directory_flushed` | Stage data and parent directory were successfully fsynced around publication. |
| `directory_flush_failed` | The file was published, but the post-publication durability confirmation failed. This is a successful `FileCommit` with a limited durability result. |

Windows uses `SetFileInformationByHandle(FileRenameInfo)` with replacement chosen
from the expected state. Linux uses `renameat` for existing files and
`renameat2(RENAME_NOREPLACE)` for create-only files; an unavailable exclusive
rename capability rejects before publication. Linux parent `fsync` occurs after
rename. Windows also attempts a post-rename file flush; failure maps to the
limited post-publication durability result. No return value claims hardware
power-loss testing, arbitrary network filesystems or M03 account/economy durability.
The qualified scope is this PC's Windows filesystem and the retained native WSL
filesystem, with process interruption tests distinct from hardware failure tests.

These guarantees follow the native [Windows rename structure](https://learn.microsoft.com/en-us/windows/win32/api/winbase/ns-winbase-file_rename_info),
[handle information API](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfileinformationbyhandle),
[Linux open](https://man7.org/linux/man-pages/man2/open.2.html),
[rename](https://man7.org/linux/man-pages/man2/rename.2.html),
[fsync](https://man7.org/linux/man-pages/man2/fsync.2.html) and
[flock](https://man7.org/linux/man-pages/man2/flock.2.html) contracts.

## Scene save and reload

`load_scene` reads a selected catalog scene and constructs an AuthoredScene through
the existing validators. `reload_scene` captures the expected live revision,
reads and validates the external file, then uses the same transactional activation
with default unsaved-change rejection. Explicit discard remains subject to
revision, pending-work, visit, owner-thread and complete-candidate checks. Failed
reload leaves the old owner intact; successful reload creates a fresh instance
and invalidates old handles. Returned file revisions identify the bytes read.

`save_scene` captures one committed revision, publishes that complete document,
and acknowledges the clean baseline only if the owner still has that revision
and no pending work. The result contains the committed file revision/durability,
the saved Scene revision and `current_scene_clean`. A changed or closed owner
does not turn a committed file into an ordinary failure or incorrectly clear
new edits. Capture alone never acknowledges a save. These operations are C++
services shared by later human/agent adapters; there is no bootstrap CLI, editor
UI, command history or live-player protocol here.

This complete example opens a project, loads its startup scene and explicitly
saves the captured revision. The caller supplies an existing project path.

```cpp
#include <shark/tools/project_files.hpp>

shark::Result<shark::tools::SavedAuthoredScene>
load_and_save_startup(const std::filesystem::path& directory,
                      const shark::ProjectPath& project_path) {
    auto files = shark::tools::ProjectFiles::open(directory);
    if (!files) {
        return files.error();
    }
    auto project = files.value()->read_project(project_path);
    if (!project) {
        return project.error();
    }
    const auto& catalog = project.value().source.document;
    auto loaded = files.value()->load_scene(catalog, catalog.startup_scene);
    if (!loaded) {
        return loaded.error();
    }
    auto status = loaded.value().owner->status();
    if (!status) {
        return status.error();
    }
    return files.value()->save_scene(*loaded.value().owner, status.value().revision,
                                     loaded.value().file_revision);
}
```
