#pragma once

#include "shark/tools/authoring_capture.hpp"

namespace shark::tools {

struct AuthoringExportRequest {
    std::filesystem::path build_dir;
    std::filesystem::path crt_dir;
    std::filesystem::path shaders;
    std::filesystem::path output;
    std::vector<std::pair<std::string, std::filesystem::path>> notices;
};

// Synchronous owner-thread adapter. A private helper packages immutable staged
// content and requests validation immediately before its final rename. The
// callback and project owner remain alive through helper exit.
[[nodiscard]] Result<AuthoringArtifactResult> export_authoring_scene(
    const PreparedClientExport& candidate, const std::filesystem::path& project_root,
    const std::filesystem::path& python, const std::filesystem::path& tool,
    const AuthoringExportRequest& request, const std::function<Result<void>()>& validate);

} // namespace shark::tools
