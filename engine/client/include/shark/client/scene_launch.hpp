#pragma once

#include <shark/client/graphics.hpp>
#include <shark/client/scene_content.hpp>

#include <chrono>
#include <optional>
#include <span>

namespace shark::client {

inline constexpr std::size_t scene_launch_maximum_arguments = 274;

struct SceneLaunchOptions {
    // Mutually exclusive with explicit content/camera/shader paths. The package
    // descriptor selects those values and they are revalidated together on F5.
    std::filesystem::path package;
    SceneContentFiles content;
    std::optional<AuthoredEntityId> camera;
    std::filesystem::path vertex_shader;
    std::filesystem::path pixel_shader;
    std::filesystem::path capture_directory;
    WindowExtent extent{640, 480};
    std::optional<std::chrono::milliseconds> duration;
    std::optional<std::uint32_t> frames;
    std::string adapter_uuid;
    GraphicsValidation validation{GraphicsValidation::off};
};

[[nodiscard]] Result<SceneLaunchOptions>
parse_scene_arguments(std::span<const std::string_view> arguments);

// Explicit prepared input only. F5 reloads a fully validated candidate; F6/F7
// retain the existing offline controls. No authoring, import or server authority.
[[nodiscard]] Result<void> run_scene(std::string_view title, const SceneLaunchOptions& options);

} // namespace shark::client
