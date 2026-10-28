#pragma once

#include "shark/client/gpu_resources.hpp"
#include "shark/scene_render.hpp"

namespace shark::client {

inline constexpr std::uint32_t scene_viewport_maximum_extent = 2048;
inline constexpr std::size_t scene_viewport_maximum_draws = 4096;
inline constexpr std::size_t scene_viewport_maximum_vertices = 262144;
inline constexpr std::size_t scene_viewport_maximum_indices = 786432;
inline constexpr std::size_t scene_viewport_maximum_submitted_indices = 3'145'728;
inline constexpr double scene_viewport_clip_absolute_tolerance = 1e-6;
inline constexpr double scene_viewport_clip_relative_tolerance = 1e-3;

struct SceneViewportVertex {
    std::array<float, 3> position{};
    std::array<float, 4> color{1, 1, 1, 1};
};
struct SceneViewportDraw {
    std::uint32_t first_index{0};
    std::uint32_t index_count{0};
    // Column-major projection * view * world; projection already flips Y once.
    std::array<float, 16> transform{};
    std::array<float, 4> base_color{1, 1, 1, 1};
    // Combined model XOR camera reflection; double-sided disables face culling.
    bool mirrored{false};
    bool double_sided{false};
};
struct SceneViewportRequest {
    WindowExtent extent{};
    // Each distinct prepared primitive is packed once. Indices address this
    // shared vertex array directly; instances reuse index ranges.
    std::vector<SceneViewportVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<SceneViewportDraw> draws;
    ShaderArtifact vertex_shader;
    ShaderArtifact pixel_shader;
    SceneRevision scene_revision;
    RenderOrigin origin;
};

// CPU-only preparation. Finite representable float MVPs, complete draw/work
// budgets and the exact precompiled scene-unlit shader interface are required.
[[nodiscard]] Result<SceneViewportRequest>
make_scene_viewport_request(const RenderScene& scene, const ShaderArtifact& vertex_shader,
                            const ShaderArtifact& pixel_shader);
// Mutable public requests are revalidated at the device boundary. Empty scenes
// clear/read back the viewport; nonempty geometry requires complete draws.
[[nodiscard]] Result<void> validate_scene_viewport_request(const SceneViewportRequest& request);

struct SceneViewportStats {
    WindowExtent extent{};
    std::uint64_t generation{0};
    std::uint64_t retired_generations{0};
    std::uint64_t rendered_frames{0};
    std::uint32_t live_target_resources{0};
    bool pending{false};
};
struct SceneViewportReadback {
    // Linear RGBA8 + D32. Resource counts here describe transient frame handles;
    // the five persistent viewport target handles are reported separately.
    OffscreenReadback pixels;
    SceneViewportStats viewport;
    SceneRevision scene_revision;
    RenderOrigin origin;
    std::uint32_t draw_count{0};
};

} // namespace shark::client
