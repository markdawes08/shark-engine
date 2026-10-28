#include "shark/client/scene_viewport.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <new>

namespace shark::client {
namespace {
Error invalid(const char* message) {
    return {ErrorCode::invalid_argument, message, {.operation = "graphics.scene.prepare"}};
}
bool factor(const std::array<float, 4>& value) {
    return std::all_of(value.begin(), value.end(),
                       [](float v) { return std::isfinite(v) && v >= 0 && v <= 1; });
}
Result<void> shader_pair(const ShaderArtifact& vertex, const ShaderArtifact& pixel) {
    if (vertex.stage != ShaderStage::vertex || pixel.stage != ShaderStage::pixel)
        return invalid("Scene shaders must be vertex then pixel stages");
    if (auto result = validate_shader_artifact(vertex, ShaderInterface::scene_unlit); !result)
        return result;
    return validate_shader_artifact(pixel, ShaderInterface::scene_unlit);
}
Result<void> metadata(WindowExtent extent, SceneRevision revision, const RenderOrigin& origin) {
    if (extent.width == 0 || extent.height == 0 || extent.width > scene_viewport_maximum_extent ||
        extent.height > scene_viewport_maximum_extent)
        return invalid("Scene viewport extent must be 1..2048 in each dimension");
    if (revision.scene_instance == 0 || origin.generation == 0)
        return invalid("Scene and render origin must have nonzero identities");
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(origin.scene_local_offset[axis]))
            return invalid("Render origin offset must be finite");
    if (origin.world_anchor)
        return validate_world_position(*origin.world_anchor);
    return {};
}
Result<std::array<float, 16>> pack_matrix(const math::Mat4& matrix) {
    std::array<float, 16> result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            const double value = matrix[column][row];
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                return invalid("Scene clip transform cannot be represented as finite floats");
            const auto packed = static_cast<float>(value);
            if (value != 0 && (packed == 0 || std::fpclassify(packed) == FP_SUBNORMAL))
                return invalid(
                    "Scene clip transform would underflow or require GPU float denormals");
            result[static_cast<std::size_t>(column * 4 + row)] = packed;
        }
    }
    return result;
}
} // namespace

Result<void> validate_scene_viewport_request(const SceneViewportRequest& request) {
    if (auto valid = metadata(request.extent, request.scene_revision, request.origin); !valid)
        return valid;
    if (request.draws.size() > scene_viewport_maximum_draws ||
        request.vertices.size() > scene_viewport_maximum_vertices ||
        request.indices.size() > scene_viewport_maximum_indices)
        return invalid("Scene viewport exceeds its draw or geometry budget");
    if (request.draws.empty()) {
        if (!request.vertices.empty() || !request.indices.empty())
            return invalid("An empty scene draw must not contain unused geometry");
    } else if (request.vertices.size() < 3 || request.indices.size() < 3 ||
               request.indices.size() % 3 != 0) {
        return invalid("Scene viewport geometry must contain complete indexed triangles");
    }
    std::array<double, 3> maximum_position{};
    for (const auto& vertex : request.vertices) {
        for (std::size_t axis = 0; axis < vertex.position.size(); ++axis) {
            const auto position = vertex.position[axis];
            if (!std::isfinite(position) || std::abs(position) > render_coordinate_limit)
                return invalid("Scene viewport position is outside the bounded finite range");
            maximum_position[axis] =
                std::max(maximum_position[axis], std::abs(static_cast<double>(position)));
        }
        if (!factor(vertex.color))
            return invalid("Scene viewport vertex color must be finite in [0,1]");
    }
    for (const auto index : request.indices)
        if (index >= request.vertices.size())
            return invalid("Scene viewport index does not address an uploaded vertex");
    std::size_t submitted_indices = 0;
    for (const auto& draw : request.draws) {
        if (draw.index_count == 0 || draw.index_count % 3 != 0 || draw.first_index % 3 != 0 ||
            draw.first_index > request.indices.size() ||
            draw.index_count > request.indices.size() - draw.first_index)
            return invalid("Scene viewport draw range must contain complete uploaded triangles");
        if (draw.index_count > scene_viewport_maximum_submitted_indices - submitted_indices)
            return invalid("Scene viewport exceeds its submitted triangle work budget");
        submitted_indices += draw.index_count;
        for (const auto value : draw.transform)
            if (!std::isfinite(value) || std::fpclassify(value) == FP_SUBNORMAL)
                return invalid(
                    "Scene viewport transform must contain finite normal floats or zero");
        // Preclude overflow in shader products/sums, including cancellation in
        // the final value. Use actual uploaded coordinate maxima and a float
        // arithmetic margin; no per-draw vertex traversal is needed.
        constexpr double safe_sum =
            std::numeric_limits<float>::max() / (1.0 + 8.0 * std::numeric_limits<float>::epsilon());
        for (std::size_t row = 0; row < 4; ++row) {
            double magnitude = std::abs(static_cast<double>(draw.transform[12 + row]));
            for (std::size_t axis = 0; axis < 3; ++axis)
                magnitude += std::abs(static_cast<double>(draw.transform[axis * 4 + row])) *
                             maximum_position[axis];
            if (magnitude > safe_sum)
                return invalid("Scene viewport transform can overflow shader clip arithmetic");
        }
        if (!factor(draw.base_color))
            return invalid("Scene viewport base color must be finite in [0,1]");
    }
    return shader_pair(request.vertex_shader, request.pixel_shader);
}

Result<SceneViewportRequest> make_scene_viewport_request(const RenderScene& scene,
                                                         const ShaderArtifact& vertex_shader,
                                                         const ShaderArtifact& pixel_shader) {
    try {
        const WindowExtent extent{scene.viewport().width, scene.viewport().height};
        if (auto valid = metadata(extent, scene.revision(), scene.origin()); !valid)
            return valid.error();
        if (auto valid = shader_pair(vertex_shader, pixel_shader); !valid)
            return valid.error();
        const auto projection = pack_matrix(scene.camera().projection);
        if (!projection)
            return projection.error();
        const auto& p = projection.value();
        const bool perspective = scene.camera().projection[2][3] == -1.0;
        const float near_clip = p[14] / p[10];
        const float far_clip = perspective ? p[14] / (1.0F + p[10]) : (p[14] - 1.0F) / p[10];
        if (!std::isfinite(near_clip) || !std::isfinite(far_clip) || far_clip <= near_clip ||
            (perspective && near_clip <= 0.0F))
            return invalid("Float projection cannot preserve distinct finite clipping planes");
        const auto& source = scene.camera().projection;
        const double source_near = source[3][2] / source[2][2];
        const double source_far =
            perspective ? source[3][2] / (1.0 + source[2][2]) : (source[3][2] - 1.0) / source[2][2];
        const auto close_clip = [](double actual, double expected) {
            return std::abs(actual - expected) <=
                   scene_viewport_clip_absolute_tolerance +
                       scene_viewport_clip_relative_tolerance * std::abs(expected);
        };
        if (!close_clip(near_clip, source_near) || !close_clip(far_clip, source_far))
            return invalid(
                "Float projection moves clip distances beyond the declared precision budget");
        struct Range {
            std::uint32_t first;
            std::uint32_t count;
        };
        std::map<const PreparedPrimitive*, Range> ranges;
        std::size_t vertices = 0;
        std::size_t indices = 0;
        std::size_t draws = 0;
        std::size_t submitted_indices = 0;
        // Charge every instance and distinct primitive before allocating output
        // geometry. Asset references come from an immutable checked RenderScene.
        for (const auto& record : scene.records()) {
            for (const auto& primitive : record.asset->meshes[record.mesh_index].primitives) {
                if (draws == scene_viewport_maximum_draws)
                    return invalid("Scene viewport exceeds its draw budget");
                ++draws;
                if (primitive.indices.size() >
                    scene_viewport_maximum_submitted_indices - submitted_indices)
                    return invalid("Scene viewport exceeds its submitted triangle work budget");
                submitted_indices += primitive.indices.size();
                if (ranges.contains(&primitive))
                    continue;
                if (primitive.vertices.size() > scene_viewport_maximum_vertices - vertices ||
                    primitive.indices.size() > scene_viewport_maximum_indices - indices)
                    return invalid("Scene viewport exceeds its unique geometry budget");
                ranges.emplace(&primitive,
                               Range{static_cast<std::uint32_t>(indices),
                                     static_cast<std::uint32_t>(primitive.indices.size())});
                vertices += primitive.vertices.size();
                indices += primitive.indices.size();
            }
        }
        SceneViewportRequest result;
        result.extent = extent;
        result.scene_revision = scene.revision();
        result.origin = scene.origin();
        result.vertex_shader = vertex_shader;
        result.pixel_shader = pixel_shader;
        result.vertices.reserve(vertices);
        result.indices.reserve(indices);
        result.draws.reserve(draws);
        // Pack in stable scene/dense-primitive order, not pointer-map order.
        ranges.clear();
        for (const auto& record : scene.records()) {
            auto transform = pack_matrix(scene.camera().view_projection * record.world_matrix);
            if (!transform) {
                auto error = std::move(transform).error();
                error.context.asset_id = record.asset->asset.value().to_string();
                error.context.entity_id = std::visit(
                    [](const auto& id) { return id.value().to_string(); }, record.entity.identity);
                return error;
            }
            for (const auto& primitive : record.asset->meshes[record.mesh_index].primitives) {
                auto found = ranges.find(&primitive);
                if (found == ranges.end()) {
                    const auto base_vertex = static_cast<std::uint32_t>(result.vertices.size());
                    const Range range{static_cast<std::uint32_t>(result.indices.size()),
                                      static_cast<std::uint32_t>(primitive.indices.size())};
                    for (const auto& vertex : primitive.vertices)
                        result.vertices.push_back({vertex.position, vertex.color});
                    for (const auto index : primitive.indices)
                        result.indices.push_back(base_vertex + index);
                    found = ranges.emplace(&primitive, range).first;
                }
                result.draws.push_back({found->second.first, found->second.count, transform.value(),
                                        primitive.material.base_color,
                                        record.mirrored != scene.camera().mirrored,
                                        primitive.material.double_sided});
            }
        }
        if (auto valid = validate_scene_viewport_request(result); !valid)
            return valid.error();
        return result;
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::execution_failed,
                     "Scene viewport preparation allocation failed",
                     {.operation = "graphics.scene.prepare"}};
    }
}
} // namespace shark::client
