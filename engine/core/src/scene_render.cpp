#include "shark/scene_render.hpp"

#include "render_math.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <new>
#include <utility>

namespace shark {
namespace {
struct ExtractionFailure {
    Error error;
};
template <typename T> T take(Result<T> result) {
    if (!result)
        throw ExtractionFailure{std::move(result).error()};
    return std::move(result).value();
}
void require(bool condition, const char* message, ErrorCode code = ErrorCode::invalid_argument) {
    if (!condition)
        throw ExtractionFailure{Error{code, message, {.operation = "scene.render.extract"}}};
}

RenderBounds mesh_bounds(const PreparedMesh& mesh, std::size_t& visited_vertices) {
    // Charge the complete new mesh before traversal; repeated instances use the
    // cached bounds. Assets are already immutable and completely validated.
    std::size_t vertices = 0;
    for (const auto& primitive : mesh.primitives) {
        require(primitive.vertices.size() <=
                    render_scene_maximum_bounds_vertices - visited_vertices - vertices,
                "Render bounds vertex work exceeds its limit");
        vertices += primitive.vertices.size();
    }
    require(vertices != 0, "Render mesh has no vertices");
    visited_vertices += vertices;
    RenderBounds bounds{math::Vec3{std::numeric_limits<double>::infinity()},
                        math::Vec3{-std::numeric_limits<double>::infinity()}};
    for (const auto& primitive : mesh.primitives) {
        for (const auto& vertex : primitive.vertices) {
            for (int axis = 0; axis < 3; ++axis) {
                const double position = vertex.position[static_cast<std::size_t>(axis)];
                require(std::isfinite(position) && std::abs(position) <= render_coordinate_limit,
                        "Mesh position exceeds the bounded render coordinate range");
                bounds.minimum[axis] = std::min(bounds.minimum[axis], position);
                bounds.maximum[axis] = std::max(bounds.maximum[axis], position);
            }
        }
    }
    return bounds;
}
} // namespace

Result<RenderScene> extract_render_scene(const Scene& scene, const PreparedAssets& assets,
                                         const RenderExtractionOptions& options) {
    try {
        const auto camera_page = take(scene.inspect_entity(
            options.camera, {false, true, false, true, false}, options.expected_revision));
        const auto revision = camera_page.revision();
        const auto& camera_entity = camera_page.entities().front();
        require(camera_entity.camera.has_value(), "Selected render entity has no Camera");
        const auto& selected_camera =
            options.view_camera ? options.view_camera->camera : *camera_entity.camera;
        const auto& camera_world = options.view_camera ? options.view_camera->scene_local_world
                                                       : camera_entity.transform->world_matrix;
        auto camera =
            make_render_camera(selected_camera, camera_world, options.viewport, options.origin);
        if (!camera) {
            auto error = std::move(camera).error();
            error.context.entity_id =
                std::visit([](const auto& id) { return id.value().to_string(); },
                           camera_entity.reference.identity);
            throw ExtractionFailure{std::move(error)};
        }
        RenderScene output{revision, camera_entity.reference, std::move(camera).value(),
                           options.viewport, options.origin};
        output.records_.reserve(scene_query_maximum_page_size);
        std::map<std::pair<AssetId, std::uint32_t>, RenderBounds> bounds_cache;
        SceneQueryOptions query;
        query.fields = {false, true, false, false, true};
        query.page_size = scene_query_maximum_page_size;
        // Equal row/scan sizes ensure every nonterminal page scans exactly one
        // quantum. Precharge continuation below, without a whole-Scene stats scan.
        query.scan_budget = scene_query_maximum_page_size;
        static_assert(render_scene_maximum_scanned_slots % scene_query_maximum_page_size == 0);
        query.expected_revision = revision;
        auto page = take(scene.query_entities(query));
        std::size_t scanned = 0;
        while (true) {
            require(page.scanned_slots() <= render_scene_maximum_scanned_slots - scanned,
                    "Render scene exceeds its slot scan limit");
            scanned += page.scanned_slots();
            for (const auto& entity : page.entities()) {
                require(output.stats_.entities < render_scene_maximum_entities,
                        "Render scene exceeds its entity limit");
                ++output.stats_.entities;
                if (!entity.mesh_renderer)
                    continue;
                const auto& renderer = *entity.mesh_renderer;
                ++output.stats_.mesh_instances;
                try {
                    // Core gameplay may have changed the reference since load.
                    // Resolve hidden instances too; visibility cannot hide a bad ID.
                    auto asset = take(assets.resolve(renderer));
                    if (!renderer.visible) {
                        ++output.stats_.hidden;
                        continue;
                    }
                    const auto transform = take(render_detail::relative_transform(
                        entity.transform->world_matrix, options.origin));
                    const auto key = std::pair{renderer.asset, renderer.mesh_index};
                    auto cached = bounds_cache.find(key);
                    if (cached == bounds_cache.end()) {
                        const auto bounds = mesh_bounds(asset->meshes[renderer.mesh_index],
                                                        output.stats_.bounds_vertices);
                        cached = bounds_cache.emplace(key, bounds).first;
                    }
                    const auto world_bounds =
                        take(render_detail::transformed_bounds(cached->second, transform.world));
                    if (options.frustum_culling &&
                        !render_detail::visible(world_bounds, output.camera_)) {
                        ++output.stats_.culled;
                        continue;
                    }
                    output.records_.push_back(
                        {entity.reference, std::move(asset), renderer.mesh_index, transform.world,
                         transform.normal, cached->second, world_bounds, transform.mirrored});
                    ++output.stats_.visible;
                } catch (ExtractionFailure& failure) {
                    failure.error.context.entity_id =
                        std::visit([](const auto& id) { return id.value().to_string(); },
                                   entity.reference.identity);
                    failure.error.context.asset_id = renderer.asset.value().to_string();
                    throw;
                }
            }
            if (!page.next_cursor())
                break;
            require(scene_query_maximum_page_size <= render_scene_maximum_scanned_slots - scanned,
                    "Render scene exceeds its slot scan limit");
            page = take(scene.continue_query(*page.next_cursor()));
        }
        output.stats_.unique_meshes = bounds_cache.size();
        return output;
    } catch (ExtractionFailure& failure) {
        failure.error.context.operation = "scene.render.extract";
        return std::move(failure.error);
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::execution_failed,
                     "Render extraction allocation failed",
                     {.operation = "scene.render.extract"}};
    }
}
} // namespace shark
