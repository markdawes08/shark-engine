#pragma once

#include "shark/scene_render.hpp"
#include "shark/tools/client_export.hpp"

#include <filesystem>
#include <functional>

namespace shark::tools {

inline constexpr std::uint32_t authoring_capture_maximum_extent = 2048;
inline constexpr std::uint64_t authoring_capture_maximum_pixels = 1024 * 1024;

struct AuthoringCaptureRequest {
    std::filesystem::path output;
    RenderViewport viewport{160, 120};
    RenderOrigin origin{};
};

struct AuthoringArtifactResult {
    // Bounded versioned artifact JSON, checked again by the protocol boundary.
    std::string json;
    bool published{false};
    bool succeeded{false};
};

// Optional application-owned Client adapter. Tools has no graphics dependency.
// Invoked synchronously on the project owner thread with immutable captured
// content. validate must pass immediately before publishing the fresh artifact.
class AuthoringCaptureProvider {
  public:
    virtual ~AuthoringCaptureProvider() = default;
    [[nodiscard]] virtual Result<AuthoringArtifactResult>
    capture(const PreparedClientExport& content, const AuthoringCaptureRequest& request,
            const std::function<Result<void>()>& validate) = 0;
};

} // namespace shark::tools
