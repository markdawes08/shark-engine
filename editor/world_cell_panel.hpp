#pragma once

#include "project_session.hpp"
#include "shark/tools/asset_import.hpp"
#include "shark/world_cell.hpp"

namespace shark::editor {
struct WorldCellExportOptions {
    WorldCellKey key;
    std::uint64_t content_revision;
    tools::AssetTargetProfile target;
    std::vector<AuthoredEntityId> collision;
    std::vector<AuthoredEntityId> navigation;
    std::string path;
};
struct WorldCellArtifact {
    std::string path;
    std::string digest;
    std::size_t bytes;
    std::size_t vertices;
    std::size_t collision_triangles;
    std::size_t navigation_triangles;
    bool published;
    std::optional<tools::FileDurability> durability{};
    // Matching current bytes do not recover the original publication receipt.
    std::optional<bool> matches_prepared{};
};
// Human adapter over the same retained terminal commands. It owns no Scene,
// imports nothing and publishes only after an explicit export action.
class WorldCellPanel final {
  public:
    explicit WorldCellPanel(ProjectSession& session) : session_(session) {}
    [[nodiscard]] Result<WorldCellArtifact> export_cell(const WorldCellExportOptions& options,
                                                        SceneRevision expected_revision);
    [[nodiscard]] Result<WorldCellArtifact> inspect(std::string_view path);
    [[nodiscard]] const std::optional<WorldCellArtifact>& prepared() const noexcept {
        return prepared_;
    }

  private:
    ProjectSession& session_;
    std::optional<WorldCellArtifact> prepared_;
};
} // namespace shark::editor
