#pragma once

#include "shark/tools/client_export.hpp"

#include <filesystem>
#include <nlohmann/json.hpp>

namespace shark::tools {

// Private package-adapter staging shared by saved-state and retained-owner
// exports. This value never crosses the public authoring protocol.
struct ExportContentStage {
    nlohmann::json content;
    std::filesystem::path vertex_path;
    std::filesystem::path pixel_path;
    std::vector<std::byte> vertex;
    std::vector<std::byte> pixel;
};

[[nodiscard]] Result<std::filesystem::path> checked_export_file(const std::filesystem::path& input);
[[nodiscard]] Result<ExportContentStage>
stage_client_export(const PreparedClientExport& candidate,
                    const std::filesystem::path& project_root,
                    const std::filesystem::path& vertex_shader,
                    const std::filesystem::path& pixel_shader, const std::filesystem::path& output);
[[nodiscard]] Result<void> validate_export_content_stage(const ExportContentStage& stage);

} // namespace shark::tools
