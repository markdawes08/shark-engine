#include "shark/tools/project_files.hpp"

#include "project_files_native.hpp"
#include "shark/tools/asset_import.hpp"
#include "utf8.hpp"

#include <map>
#include <new>
#include <thread>
#include <utility>

namespace shark::tools {
namespace {

struct FileFailure {
    Error error;
};

[[noreturn]] void fail(ErrorCode code, const char* message, std::string path = {}) {
    throw FileFailure{Error{code, message, {.path = std::move(path)}}};
}

template <typename T> T checked(Result<T> result) {
    if (!result) {
        throw FileFailure{std::move(result).error()};
    }
    return std::move(result).value();
}

void checked(Result<void> result) {
    if (!result) {
        throw FileFailure{std::move(result).error()};
    }
}

template <typename T, typename Function>
Result<T> boundary(const char* operation, Function&& function) {
    try {
        return function();
    } catch (FileFailure& failure) {
        failure.error.context.operation = operation;
        return std::move(failure.error);
    } catch (const std::bad_alloc&) {
        return Error{
            ErrorCode::execution_failed, "source file allocation failed", {.operation = operation}};
    } catch (...) {
        return Error{
            ErrorCode::execution_failed, "source file operation failed", {.operation = operation}};
    }
}

void add_spelling(std::map<std::string, std::string>& spellings, const ProjectPath& path) {
    std::string folded;
    const auto& text = path.text();
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto value = text[i];
        folded.push_back(value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A'))
                                                      : value);
        if (i + 1 == text.size() || text[i + 1] == '/') {
            const auto prefix = text.substr(0, i + 1);
            const auto [found, inserted] = spellings.emplace(folded, prefix);
            if (!inserted && found->second != prefix) {
                fail(ErrorCode::conflict, "project paths have conflicting case spellings", text);
            }
        }
    }
}

const ProjectPath& scene_path(const ProjectDocument& project, SceneId scene) {
    for (const auto& entry : project.scenes) {
        if (entry.scene == scene) {
            return entry.path;
        }
    }
    fail(ErrorCode::invalid_configuration, "scene is missing from the project catalog",
         "/scene_id");
}

void validate_references(const ProjectDocument& project, const SceneDocument& scene) {
    if (scene.project != project.project) {
        fail(ErrorCode::invalid_configuration, "scene belongs to a different project",
             "/project_id");
    }
    static_cast<void>(scene_path(project, scene.scene));
    if (scene.world_origin && scene.world_origin->world != project.world) {
        fail(ErrorCode::invalid_configuration, "scene origin belongs to a different world",
             "/world_origin/world_id");
    }
}

void validate_text_path(const ProjectPath& path) {
    const auto& value = path.text();
    if (!value.starts_with("scripts/") || !value.ends_with(".lua")) {
        fail(ErrorCode::invalid_path, "Text access requires a scripts/ path ending in .lua", value);
    }
}

void validate_text(std::string_view text, const ProjectPath& path) {
    if (text.size() > project_text_maximum_bytes || text.find('\0') != std::string_view::npos ||
        !shark::detail::valid_utf8(text)) {
        fail(ErrorCode::invalid_argument, "Text must be at most 32768 UTF-8 bytes without NUL",
             path.text());
    }
}

} // namespace

struct FileRevision::Data {
    std::filesystem::path root;
    ProjectPath path;
    std::string bytes;
};

FileRevision::FileRevision(std::shared_ptr<const Data> data) noexcept : data_(std::move(data)) {}
bool FileRevision::valid() const noexcept {
    return static_cast<bool>(data_);
}
std::size_t FileRevision::byte_count() const noexcept {
    return data_ ? data_->bytes.size() : 0;
}
bool FileRevision::operator==(const FileRevision& other) const noexcept {
    return data_ == other.data_ ||
           (data_ && other.data_ && data_->root == other.data_->root &&
            data_->path == other.data_->path && data_->bytes == other.data_->bytes);
}

struct ProjectFiles::Impl {
    const std::thread::id owner{std::this_thread::get_id()};
    const ProjectRoot root;
    std::unique_ptr<detail::NativeProjectFiles> native;

    Impl(ProjectRoot directory, std::unique_ptr<detail::NativeProjectFiles> files)
        : root(std::move(directory)), native(std::move(files)) {}

    void check_owner() const {
        if (owner != std::this_thread::get_id()) {
            fail(ErrorCode::conflict, "source file access requires its owner thread");
        }
    }

    ProjectDocument catalog(const ProjectDocument& candidate,
                            const ProjectPath* project_path = nullptr) const {
        auto project = checked(validate_project_document(candidate));
        std::map<std::string, std::string> spellings;
        if (project_path) {
            if (project_path->text() == asset_development_cache_name) {
                fail(ErrorCode::invalid_path, "Development cache filename is reserved",
                     project_path->text());
            }
            add_spelling(spellings, *project_path);
            checked(native->validate_path(*project_path));
        }
        for (const auto& entry : project.scenes) {
            if (entry.path.text() == asset_development_cache_name) {
                fail(ErrorCode::invalid_path, "Development cache filename is reserved",
                     entry.path.text());
            }
            if (project_path && entry.path == *project_path) {
                fail(ErrorCode::conflict, "project and scene paths must be distinct",
                     entry.path.text());
            }
            add_spelling(spellings, entry.path);
            checked(native->validate_path(entry.path));
        }
        // A document cannot also be an ancestor directory of another document,
        // even if neither has been created yet.
        for (const auto& entry : project.scenes) {
            const auto prefix = entry.path.text() + '/';
            for (const auto& other : project.scenes) {
                if (other.path.text().starts_with(prefix)) {
                    fail(ErrorCode::conflict, "source file paths overlap a directory",
                         entry.path.text());
                }
            }
            if (project_path && (project_path->text().starts_with(prefix) ||
                                 entry.path.text().starts_with(project_path->text() + '/'))) {
                fail(ErrorCode::conflict, "project path overlaps a scene directory",
                     entry.path.text());
            }
        }
        return project;
    }
};

ProjectFiles::ProjectFiles(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ProjectFiles::~ProjectFiles() = default;

Result<std::unique_ptr<ProjectFiles>> ProjectFiles::open(const std::filesystem::path& root) {
    return boundary<std::unique_ptr<ProjectFiles>>("source.file.open", [&] {
        auto directory = checked(ProjectRoot::open(root));
        auto native = checked(detail::NativeProjectFiles::open(directory));
        return std::unique_ptr<ProjectFiles>(
            new ProjectFiles(std::make_unique<Impl>(std::move(directory), std::move(native))));
    });
}

FileRevision ProjectFiles::make_revision(const ProjectPath& path, std::string bytes) const {
    return FileRevision(std::make_shared<const FileRevision::Data>(
        FileRevision::Data{impl_->root.native_path(), path, std::move(bytes)}));
}

std::optional<std::string_view>
ProjectFiles::expected_bytes(const ProjectPath& path,
                             const std::optional<FileRevision>& expected) const {
    if (!expected) {
        return std::nullopt;
    }
    if (!expected->data_ || expected->data_->root != impl_->root.native_path() ||
        expected->data_->path != path) {
        fail(ErrorCode::conflict, "file revision belongs to a different root or path", path.text());
    }
    return expected->data_->bytes;
}

Result<ReadSource<ProjectDocument>> ProjectFiles::read_project(const ProjectPath& path) {
    return boundary<ReadSource<ProjectDocument>>("source.file.read_project", [&] {
        impl_->check_owner();
        auto bytes = checked(impl_->native->read(path));
        auto source = checked(decode_project_document(bytes));
        source.document = impl_->catalog(source.document, &path);
        return ReadSource<ProjectDocument>{std::move(source),
                                           make_revision(path, std::move(bytes))};
    });
}

Result<ReadText> ProjectFiles::read_text(const ProjectPath& path) {
    return boundary<ReadText>("source.file.read_text", [&] {
        impl_->check_owner();
        validate_text_path(path);
        auto text = checked(impl_->native->read(path, project_text_maximum_bytes));
        validate_text(text, path);
        auto revision = make_revision(path, text);
        return ReadText{std::move(text), std::move(revision)};
    });
}

Result<FileCommit> ProjectFiles::write_text(const ProjectPath& path, std::string_view text,
                                            std::optional<FileRevision> expected) {
    return boundary<FileCommit>("source.file.write_text", [&] {
        impl_->check_owner();
        validate_text_path(path);
        validate_text(text, path);
        const auto prior = expected_bytes(path, expected);
        auto revision = make_revision(path, std::string{text});
        const auto durability = checked(
            impl_->native->replace(path, revision.data_->bytes, prior, project_text_maximum_bytes));
        return FileCommit{std::move(revision), durability};
    });
}

Result<ReadSource<AssetMetadata>> ProjectFiles::read_asset_metadata(const ProjectPath& sidecar) {
    return boundary<ReadSource<AssetMetadata>>("asset.file.read_metadata", [&] {
        impl_->check_owner();
        static_cast<void>(checked(asset_source_path(sidecar)));
        auto bytes = checked(impl_->native->read(sidecar, asset_metadata_maximum_bytes));
        auto metadata = checked(decode_asset_metadata(bytes));
        return ReadSource<AssetMetadata>{{std::move(metadata), asset_metadata_version},
                                         make_revision(sidecar, std::move(bytes))};
    });
}

Result<FileCommit> ProjectFiles::write_asset_metadata(const ProjectPath& sidecar,
                                                      const AssetMetadata& metadata,
                                                      std::optional<FileRevision> expected) {
    return boundary<FileCommit>("asset.file.write_metadata", [&] {
        impl_->check_owner();
        static_cast<void>(checked(asset_source_path(sidecar)));
        auto bytes = checked(encode_asset_metadata(metadata));
        const auto prior = expected_bytes(sidecar, expected);
        auto revision = make_revision(sidecar, std::move(bytes));
        const auto durability = checked(impl_->native->replace(
            sidecar, revision.data_->bytes, prior, asset_metadata_maximum_bytes));
        return FileCommit{std::move(revision), durability};
    });
}

Result<bool> ProjectFiles::asset_source_exists(const ProjectPath& source) {
    return boundary<bool>("asset.file.source_exists", [&] {
        impl_->check_owner();
        // Validate the same naming domain as sidecars before touching native files.
        static_cast<void>(checked(asset_sidecar_path(source)));
        return checked(impl_->native->exists(source));
    });
}

Result<FileRevision> ProjectFiles::read_asset_bytes(const ProjectPath& source) {
    return boundary<FileRevision>("asset.file.read_source", [&] {
        impl_->check_owner();
        static_cast<void>(checked(asset_sidecar_path(source)));
        return make_revision(
            source, checked(impl_->native->read(source, asset_import_maximum_source_bytes)));
    });
}
Result<std::optional<FileRevision>> ProjectFiles::read_development_cache() {
    return boundary<std::optional<FileRevision>>(
        "asset.file.read_cache", [&]() -> std::optional<FileRevision> {
            impl_->check_owner();
            const auto path = checked(ProjectPath::parse(asset_cache_filename));
            if (!checked(impl_->native->exists(path)))
                return {};
            return make_revision(
                path, checked(impl_->native->read(path, asset_cache_maximum_file_bytes)));
        });
}
Result<FileCommit> ProjectFiles::write_development_cache(std::string bytes,
                                                         std::optional<FileRevision> expected) {
    return boundary<FileCommit>("asset.file.write_cache", [&] {
        impl_->check_owner();
        const auto path = checked(ProjectPath::parse(asset_cache_filename));
        const auto prior = expected_bytes(path, expected);
        auto revision = make_revision(path, std::move(bytes));
        const auto durability = checked(impl_->native->replace(
            path, revision.data_->bytes, prior, asset_cache_maximum_file_bytes, true));
        return FileCommit{std::move(revision), durability};
    });
}
std::string_view ProjectFiles::revision_bytes(const FileRevision& revision) const {
    impl_->check_owner();
    if (!revision.data_ || revision.data_->root != impl_->root.native_path()) {
        fail(ErrorCode::conflict, "Asset file revision belongs to another root");
    }
    return revision.data_->bytes;
}
Result<std::size_t> ProjectFiles::cache_stages(bool discard) {
    return boundary<std::size_t>("asset.file.cache_stages", [&] {
        impl_->check_owner();
        return checked(impl_->native->cache_stages(discard));
    });
}

Result<ReadSource<SceneDocument>> ProjectFiles::read_scene(const ProjectDocument& project,
                                                           SceneId scene) {
    return boundary<ReadSource<SceneDocument>>("source.file.read_scene", [&] {
        impl_->check_owner();
        const auto catalog = impl_->catalog(project);
        const auto& path = scene_path(catalog, scene);
        auto bytes = checked(impl_->native->read(path));
        auto source = checked(decode_scene_document(bytes));
        validate_references(catalog, source.document);
        if (source.document.scene != scene) {
            fail(ErrorCode::invalid_configuration, "file contains a different scene", "/scene_id");
        }
        return ReadSource<SceneDocument>{std::move(source), make_revision(path, std::move(bytes))};
    });
}

Result<FileCommit> ProjectFiles::write_project(const ProjectPath& path,
                                               const ProjectDocument& project,
                                               std::optional<FileRevision> expected) {
    return boundary<FileCommit>("source.file.write_project", [&] {
        impl_->check_owner();
        const auto catalog = impl_->catalog(project, &path);
        auto bytes = checked(encode_project_document(catalog));
        const auto prior = expected_bytes(path, expected);
        auto revision = make_revision(path, std::move(bytes));
        const auto durability = checked(impl_->native->replace(path, revision.data_->bytes, prior));
        return FileCommit{std::move(revision), durability};
    });
}

Result<FileCommit> ProjectFiles::write_scene(const ProjectDocument& project,
                                             const SceneDocument& scene,
                                             std::optional<FileRevision> expected) {
    return boundary<FileCommit>("source.file.write_scene", [&] {
        impl_->check_owner();
        const auto catalog = impl_->catalog(project);
        validate_references(catalog, scene);
        // Reuse the activation validator, including composed transform feasibility.
        // This temporary candidate never replaces any caller-owned scene.
        const auto candidate = checked(AuthoredScene::create(catalog, scene));
        auto bytes = checked(encode_scene_document(scene));
        const auto& path = scene_path(catalog, scene.scene);
        const auto prior = expected_bytes(path, expected);
        auto revision = make_revision(path, std::move(bytes));
        const auto durability = checked(impl_->native->replace(path, revision.data_->bytes, prior));
        return FileCommit{std::move(revision), durability};
    });
}

Result<LoadedAuthoredScene> ProjectFiles::load_scene(const ProjectDocument& project, SceneId scene,
                                                     SceneLimits limits) {
    return boundary<LoadedAuthoredScene>("source.file.load_scene", [&] {
        impl_->check_owner();
        auto read = checked(read_scene(project, scene));
        auto owner = checked(AuthoredScene::create(project, read.source.document, limits));
        return LoadedAuthoredScene{std::move(owner), std::move(read.revision),
                                   read.source.source_schema_version};
    });
}

Result<ReloadedAuthoredScene> ProjectFiles::reload_scene(AuthoredScene& owner,
                                                         SceneRevision expected_scene,
                                                         UnsavedScenePolicy unsaved) {
    return boundary<ReloadedAuthoredScene>("source.file.reload_scene", [&] {
        impl_->check_owner();
        const auto captured = checked(owner.capture(expected_scene));
        auto read = checked(read_scene(captured.project, captured.scene.scene));
        const auto revision =
            checked(owner.activate(read.source.document, expected_scene, unsaved));
        return ReloadedAuthoredScene{revision, std::move(read.revision),
                                     read.source.source_schema_version};
    });
}

Result<SavedAuthoredScene> ProjectFiles::save_scene(AuthoredScene& owner,
                                                    SceneRevision expected_scene,
                                                    std::optional<FileRevision> expected_file) {
    return boundary<SavedAuthoredScene>("source.file.save_scene", [&] {
        impl_->check_owner();
        const auto captured = checked(owner.capture(expected_scene));
        auto committed =
            checked(write_scene(captured.project, captured.scene, std::move(expected_file)));
        const bool clean = owner.acknowledge_saved(captured.revision);
        return SavedAuthoredScene{std::move(committed), captured.revision, clean};
    });
}

} // namespace shark::tools
