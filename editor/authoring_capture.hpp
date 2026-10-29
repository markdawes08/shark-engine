#pragma once

#include "shark/tools/authoring_capture.hpp"

#include <memory>
#include <string>

namespace shark::editor {

struct AuthoringCaptureConfig {
    std::filesystem::path vertex_shader;
    std::filesystem::path pixel_shader;
    std::string adapter_uuid;
    std::string validation;
};

// Validates and retains shader inputs without opening a window or GPU. The
// application owns this adapter longer than its borrowed protocol service.
[[nodiscard]] Result<std::unique_ptr<tools::AuthoringCaptureProvider>>
make_authoring_capture(AuthoringCaptureConfig config);

} // namespace shark::editor
