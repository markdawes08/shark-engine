#pragma once

#include <shark/client/graphics.hpp>
#include <shark/error.hpp>

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace shark::editor {

struct EditorLaunch {
    std::filesystem::path root;
    std::string project;
    std::filesystem::path vertex_shader;
    std::filesystem::path pixel_shader;
    std::filesystem::path scene_vertex_shader;
    std::filesystem::path scene_pixel_shader;
    std::string adapter;
    client::GraphicsValidation validation{client::GraphicsValidation::off};
};

[[nodiscard]] Result<EditorLaunch>
parse_editor_arguments(std::span<const std::string_view> arguments);
[[nodiscard]] Result<void> run_editor(const EditorLaunch& launch);

} // namespace shark::editor
