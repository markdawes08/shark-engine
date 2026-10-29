#pragma once

#include <shark/scene_query.hpp>
#include <shark/tools/authoring_protocol.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace shark::editor {

struct ProjectState {
    bool open{false};
    bool dirty{false};
    std::string path;
    std::string name;
    std::string project_id;
    std::string world_id;
    std::string scene_id;
    SceneRevision revision{};
    std::uint64_t pending_mutations{0};
    bool operator==(const ProjectState&) const = default;
};

struct SaveReceipt {
    bool published{false};
    bool current_scene_clean{false};
    SceneRevision saved_revision{};
    std::uint64_t bytes{0};
    // The original file publication outcome, including directory_flush_failed.
    std::string durability;
};

enum class CloseChoice { reject, cancel, save, discard };

struct CloseReceipt {
    bool closed{false};
    bool cancelled{false};
    std::optional<SaveReceipt> saved;
    // A close failure after a successful save must retain the publication above.
    std::optional<Error> error;
};

// Editor-private, creating-thread view of the existing shared protocol owner.
// Cached values are observations for display, never a second mutable scene.
// Destruction releases ownership without saving; normal UI exit must explicitly
// resolve dirty state through close before destroying this application owner.
class ProjectSession final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<ProjectSession>>
    create(std::filesystem::path root, std::string_view initial_project = {});
    ~ProjectSession();
    ProjectSession(const ProjectSession&) = delete;
    ProjectSession& operator=(const ProjectSession&) = delete;
    ProjectSession(ProjectSession&&) = delete;
    ProjectSession& operator=(ProjectSession&&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] const ProjectState& state() const noexcept;
    [[nodiscard]] Result<void> refresh();
    [[nodiscard]] Result<void> open(std::string_view relative_project);
    [[nodiscard]] Result<SaveReceipt> save(SceneRevision expected_revision);
    [[nodiscard]] Result<CloseReceipt> close(CloseChoice choice, SceneRevision expected_revision);

    // Panels use the same bounded public methods and retain all result levels.
    // Request correlation is generated locally; it is never mutation retry ID.
    // The reply remains the complete protocol envelope, including failed=true
    // for rejected nested decisions. Callers must inspect those owned outcomes.
    [[nodiscard]] Result<tools::AuthoringProtocolReply>
    request(std::string_view method, std::string_view parameters_json = "{}");

  private:
    struct Impl;
    explicit ProjectSession(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::editor
