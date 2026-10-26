#pragma once

#include "shark/tools/asset_metadata.hpp"
#include "shark/tools/authored_scene.hpp"

#include <filesystem>
#include <memory>
#include <optional>

namespace shark::tools {

// Every value describes a published file. Windows confirms the file flush only;
// a failed directory flush after publication is not an uncommitted write error.
enum class FileDurability { file_flushed, file_and_directory_flushed, directory_flush_failed };

// Opaque exact-content revision, bound to a canonical project root and relative
// path. Copies share immutable bounded bytes. Returning to identical bytes is
// accepted: this is a content revision, not a history/event counter.
class FileRevision final {
  public:
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::size_t byte_count() const noexcept;
    bool operator==(const FileRevision& other) const noexcept;

  private:
    struct Data;
    explicit FileRevision(std::shared_ptr<const Data> data) noexcept;
    std::shared_ptr<const Data> data_;
    friend class ProjectFiles;
};

template <typename Document> struct ReadSource {
    DecodedSource<Document> source;
    FileRevision revision;
};

struct FileCommit {
    FileRevision revision;
    FileDurability durability;
};

struct LoadedAuthoredScene {
    std::unique_ptr<AuthoredScene> owner;
    FileRevision file_revision;
    std::uint32_t source_schema_version;
};

struct ReloadedAuthoredScene {
    SceneRevision revision;
    FileRevision file_revision;
    std::uint32_t source_schema_version;
};

struct SavedAuthoredScene {
    FileCommit file;
    SceneRevision saved_revision;
    // False if the owner changed after capture. Publication still succeeded.
    bool current_scene_clean;
};

// Creating-thread typed source storage with one cooperating writer per existing
// root. Parents must exist. External writers/topology changes must not race the
// final comparison and native rename; see docs/project-files.md. This is not the
// later shared editor/terminal project owner, undo history or player persistence.
class ProjectFiles final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<ProjectFiles>>
    open(const std::filesystem::path& root);
    ~ProjectFiles();
    ProjectFiles(const ProjectFiles&) = delete;
    ProjectFiles& operator=(const ProjectFiles&) = delete;
    ProjectFiles(ProjectFiles&&) = delete;
    ProjectFiles& operator=(ProjectFiles&&) = delete;

    [[nodiscard]] Result<ReadSource<ProjectDocument>> read_project(const ProjectPath& path);
    [[nodiscard]] Result<ReadSource<AssetMetadata>> read_asset_metadata(const ProjectPath& sidecar);
    [[nodiscard]] Result<FileCommit> write_asset_metadata(const ProjectPath& sidecar,
                                                          const AssetMetadata& metadata,
                                                          std::optional<FileRevision> expected);
    // Checks a regular source file without decoding or reading its contents.
    // Missing ancestors/files return false; unsafe paths and I/O faults fail.
    [[nodiscard]] Result<bool> asset_source_exists(const ProjectPath& source);
    [[nodiscard]] Result<ReadSource<SceneDocument>> read_scene(const ProjectDocument& project,
                                                               SceneId scene);
    // nullopt requires an absent destination; an existing file requires the
    // exact revision returned by a read or prior committed write of that path.
    [[nodiscard]] Result<FileCommit> write_project(const ProjectPath& path,
                                                   const ProjectDocument& project,
                                                   std::optional<FileRevision> expected);
    [[nodiscard]] Result<FileCommit> write_scene(const ProjectDocument& project,
                                                 const SceneDocument& scene,
                                                 std::optional<FileRevision> expected);
    [[nodiscard]] Result<LoadedAuthoredScene> load_scene(const ProjectDocument& project,
                                                         SceneId scene, SceneLimits limits = {});
    [[nodiscard]] Result<ReloadedAuthoredScene>
    reload_scene(AuthoredScene& owner, SceneRevision expected_scene,
                 UnsavedScenePolicy unsaved = UnsavedScenePolicy::reject);
    [[nodiscard]] Result<SavedAuthoredScene> save_scene(AuthoredScene& owner,
                                                        SceneRevision expected_scene,
                                                        std::optional<FileRevision> expected_file);

  private:
    friend class AssetImports;
    [[nodiscard]] Result<FileRevision> read_asset_bytes(const ProjectPath& source);
    [[nodiscard]] Result<std::optional<FileRevision>> read_development_cache();
    [[nodiscard]] Result<FileCommit> write_development_cache(std::string bytes,
                                                             std::optional<FileRevision> expected);
    [[nodiscard]] std::string_view revision_bytes(const FileRevision& revision) const;
    [[nodiscard]] Result<std::size_t> cache_stages(bool discard);
    struct Impl;
    explicit ProjectFiles(std::unique_ptr<Impl> impl) noexcept;
    [[nodiscard]] FileRevision make_revision(const ProjectPath& path, std::string bytes) const;
    [[nodiscard]] std::optional<std::string_view>
    expected_bytes(const ProjectPath& path, const std::optional<FileRevision>& expected) const;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::tools
