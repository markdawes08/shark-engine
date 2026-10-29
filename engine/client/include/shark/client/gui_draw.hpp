#pragma once

#include <shark/client/gpu_resources.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace shark::client {

inline constexpr std::size_t gui_max_vertices = 131072;
inline constexpr std::size_t gui_max_indices = 393216;
inline constexpr std::size_t gui_max_draws = 4096;

// Framebuffer pixel positions, normalized atlas coordinates and linear RGBA.
// Indices are absolute within the frame; no native/UI-library types escape.
struct GuiVertex {
    std::array<float, 2> position{};
    std::array<float, 2> uv{};
    std::array<float, 4> color{};
};
struct GuiDraw {
    std::uint32_t first_index{0};
    std::uint32_t index_count{0};
    // Exclusive right/bottom bounds: x0, y0, x1, y1, inside the framebuffer.
    std::array<std::int32_t, 4> clip{};
};
struct GuiFrame {
    WindowExtent extent{};
    std::vector<GuiVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<GuiDraw> draws;
};
// One immutable RGBA8 atlas; RGB is linear, alpha is coverage. Shaders are
// offline GUI-profile artifacts. Configuration survives swapchain recreation.
struct GuiAtlas {
    WindowExtent extent{};
    std::vector<std::uint8_t> rgba8;
    ShaderArtifact vertex_shader;
    ShaderArtifact pixel_shader;
};

[[nodiscard]] Result<void> validate_gui_atlas(const GuiAtlas& atlas);
[[nodiscard]] Result<void> validate_gui_frame(const GuiFrame& frame);

} // namespace shark::client
