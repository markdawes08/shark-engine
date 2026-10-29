#include <shark/client/gui_draw.hpp>

#include "gui_viewport.hpp"

#include <algorithm>
#include <cmath>

namespace shark::client {
namespace {
Error invalid(const char* message) {
    return {ErrorCode::invalid_argument, message, {.operation = "graphics.gui.validate"}};
}
bool extent_valid(WindowExtent extent, std::uint32_t maximum) {
    return extent.width > 0 && extent.height > 0 && extent.width <= maximum &&
           extent.height <= maximum;
}
} // namespace

Result<void> validate_gui_atlas(const GuiAtlas& atlas) {
    if (!extent_valid(atlas.extent, 2048) ||
        atlas.rgba8.size() !=
            static_cast<std::size_t>(atlas.extent.width) * atlas.extent.height * 4)
        return invalid("GUI atlas requires tightly packed RGBA8 dimensions in 1..2048");
    if (atlas.vertex_shader.stage != ShaderStage::vertex ||
        atlas.pixel_shader.stage != ShaderStage::pixel)
        return invalid("GUI atlas shader stages do not match the pipeline");
    if (auto result = validate_shader_artifact(atlas.vertex_shader, ShaderInterface::gui); !result)
        return result.error();
    return validate_shader_artifact(atlas.pixel_shader, ShaderInterface::gui);
}

Result<void> validate_gui_frame(const GuiFrame& frame) {
    if (!extent_valid(frame.extent, 4096))
        return invalid("GUI framebuffer dimensions must be in 1..4096");
    if (frame.vertices.size() > gui_max_vertices || frame.indices.size() > gui_max_indices ||
        frame.draws.size() > gui_max_draws || frame.indices.size() % 3 != 0)
        return invalid("GUI geometry exceeds the bounded triangle-list contract");
    for (const auto& vertex : frame.vertices) {
        if (!std::ranges::all_of(
                vertex.position,
                [](float v) { return std::isfinite(v) && std::abs(v) <= 10000000.0F; }) ||
            !std::ranges::all_of(
                vertex.uv, [](float v) { return std::isfinite(v) && v >= 0.0F && v <= 1.0F; }) ||
            !std::ranges::all_of(
                vertex.color, [](float v) { return std::isfinite(v) && v >= 0.0F && v <= 1.0F; }))
            return invalid("GUI vertices require bounded finite positions, UVs and linear RGBA");
    }
    for (const auto index : frame.indices)
        if (index >= frame.vertices.size())
            return invalid("GUI index references a vertex outside the frame");
    for (const auto& draw : frame.draws) {
        if (draw.texture != GuiTexture::atlas && draw.texture != GuiTexture::scene_viewport)
            return invalid("GUI draw references an unsupported texture");
        if (draw.texture == GuiTexture::scene_viewport && !frame.viewport)
            return invalid("GUI scene texture requires an owned viewport in the same frame");
        if (draw.index_count == 0 || draw.index_count % 3 != 0 ||
            draw.first_index > frame.indices.size() ||
            draw.index_count > frame.indices.size() - draw.first_index)
            return invalid("GUI draw range must contain complete in-bounds triangles");
        if (draw.clip[0] < 0 || draw.clip[1] < 0 || draw.clip[2] <= draw.clip[0] ||
            draw.clip[3] <= draw.clip[1] ||
            draw.clip[2] > static_cast<std::int32_t>(frame.extent.width) ||
            draw.clip[3] > static_cast<std::int32_t>(frame.extent.height))
            return invalid("GUI scissor must be a nonempty framebuffer rectangle");
    }
    if (frame.viewport)
        return validate_scene_viewport_request(*frame.viewport);
    return {};
}

bool detail::same_gui_viewport(const std::optional<SceneViewportRequest>& a,
                               const std::optional<SceneViewportRequest>& b) noexcept {
    if (a.has_value() != b.has_value())
        return false;
    if (!a)
        return true;
    const auto same_shader = [](const ShaderArtifact& left, const ShaderArtifact& right) {
        return left.stage == right.stage && left.entry_point == right.entry_point &&
               left.words == right.words;
    };
    return a->extent == b->extent && a->scene_revision == b->scene_revision &&
           a->origin == b->origin && a->indices == b->indices &&
           same_shader(a->vertex_shader, b->vertex_shader) &&
           same_shader(a->pixel_shader, b->pixel_shader) &&
           std::equal(a->vertices.begin(), a->vertices.end(), b->vertices.begin(),
                      b->vertices.end(),
                      [](const SceneViewportVertex& left, const SceneViewportVertex& right) {
                          return left.position == right.position && left.color == right.color;
                      }) &&
           std::equal(a->draws.begin(), a->draws.end(), b->draws.begin(), b->draws.end(),
                      [](const SceneViewportDraw& left, const SceneViewportDraw& right) {
                          return left.first_index == right.first_index &&
                                 left.index_count == right.index_count &&
                                 left.transform == right.transform &&
                                 left.base_color == right.base_color &&
                                 left.mirrored == right.mirrored &&
                                 left.double_sided == right.double_sided;
                      });
}
} // namespace shark::client
