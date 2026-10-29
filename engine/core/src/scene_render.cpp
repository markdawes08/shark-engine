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

std::vector<SceneInspectedEntity> preview_entities(const Scene& scene, SceneRevision revision,
                                                   RenderTransformOverride& preview) {
    SceneQueryOptions query;
    query.fields = {false, true, true, false, true};
    query.page_size = scene_query_maximum_page_size;
    query.scan_budget = scene_query_maximum_page_size;
    query.expected_revision = revision;
    auto page = take(scene.query_entities(query));
    std::vector<SceneInspectedEntity> entities;
    std::size_t scanned = 0;
    while (true) {
        require(page.scanned_slots() <= render_scene_maximum_scanned_slots - scanned,
                "Render preview exceeds its slot scan limit");
        scanned += page.scanned_slots();
        require(page.entities().size() <= render_scene_maximum_entities - entities.size(),
                "Render preview exceeds its entity limit");
        entities.insert(entities.end(), page.entities().begin(), page.entities().end());
        if (!page.next_cursor())
            break;
        require(scene_query_maximum_page_size <= render_scene_maximum_scanned_slots - scanned,
                "Render preview exceeds its slot scan limit");
        page = take(scene.continue_query(*page.next_cursor()));
    }
    std::map<EntityHandle, std::size_t> indices;
    for (std::size_t index = 0; index < entities.size(); ++index)
        indices.emplace(entities[index].reference.handle, index);
    const auto selected = indices.find(preview.entity);
    require(selected != indices.end(), "Render preview entity is stale or foreign",
            ErrorCode::conflict);
    preview.local = take(canonicalize_transform(preview.local));
    std::vector<std::vector<std::size_t>> children(entities.size());
    for (std::size_t index = 0; index < entities.size(); ++index) {
        const auto& parent = entities[index].hierarchy->parent;
        if (parent) {
            const auto found = indices.find(*parent);
            require(found != indices.end(), "Render preview hierarchy is incomplete",
                    ErrorCode::conflict);
            children[found->second].push_back(index);
        }
    }
    std::vector<std::size_t> pending{selected->second};
    for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
        auto& entity = entities[pending[cursor]];
        const auto& local =
            pending[cursor] == selected->second ? preview.local : entity.transform->local;
        const auto local_matrix = take(transform_matrix(local));
        const auto& parent = entity.hierarchy->parent;
        const auto world =
            parent ? entities[indices.at(*parent)].transform->world_matrix * local_matrix
                   : local_matrix;
        // Validate every descendant, including hidden or non-renderable ones.
        // Never infer candidate worlds by multiplying an inverse old world: its
        // cancellation error could diverge from the real Scene mutation path.
        const auto valid = validate_transform_matrix(world);
        if (!valid) {
            auto error = valid.error();
            error.context.entity_id = std::visit(
                [](const auto& id) { return id.value().to_string(); }, entity.reference.identity);
            throw ExtractionFailure{std::move(error)};
        }
        entity.transform->world_matrix = world;
        const auto& descendants = children[pending[cursor]];
        require(descendants.size() <= render_scene_maximum_entities - pending.size(),
                "Render preview subtree exceeds its entity limit");
        pending.insert(pending.end(), descendants.begin(), descendants.end());
    }
    return entities;
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
        auto preview = options.transform_preview;
        std::vector<SceneInspectedEntity> preview_values;
        if (preview)
            preview_values = preview_entities(scene, revision, *preview);
        const auto& selected_camera =
            options.view_camera ? options.view_camera->camera : *camera_entity.camera;
        auto camera_world = options.view_camera ? options.view_camera->scene_local_world
                                                : camera_entity.transform->world_matrix;
        if (preview && !options.view_camera) {
            const auto found =
                std::find_if(preview_values.begin(), preview_values.end(), [&](const auto& value) {
                    return value.reference.handle == options.camera;
                });
            require(found != preview_values.end(), "Render preview camera is absent",
                    ErrorCode::conflict);
            camera_world = found->transform->world_matrix;
        }
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
        output.transform_preview_ = preview;
        output.records_.reserve(scene_query_maximum_page_size);
        std::map<std::pair<AssetId, std::uint32_t>, RenderBounds> bounds_cache;
        const auto append = [&](const SceneInspectedEntity& entity) {
            require(output.stats_.entities < render_scene_maximum_entities,
                    "Render scene exceeds its entity limit");
            ++output.stats_.entities;
            if (!entity.mesh_renderer)
                return;
            const auto& renderer = *entity.mesh_renderer;
            ++output.stats_.mesh_instances;
            try {
                // Core gameplay may have changed the reference since load.
                // Resolve hidden instances too; visibility cannot hide a bad ID.
                auto asset = take(assets.resolve(renderer));
                if (!renderer.visible) {
                    ++output.stats_.hidden;
                    return;
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
                    return;
                }
                output.records_.push_back({entity.reference, std::move(asset), renderer.mesh_index,
                                           transform.world, transform.normal, cached->second,
                                           world_bounds, transform.mirrored});
                ++output.stats_.visible;
            } catch (ExtractionFailure& failure) {
                failure.error.context.entity_id =
                    std::visit([](const auto& id) { return id.value().to_string(); },
                               entity.reference.identity);
                failure.error.context.asset_id = renderer.asset.value().to_string();
                throw;
            }
        };
        if (preview) {
            for (const auto& entity : preview_values)
                append(entity);
        } else {
            SceneQueryOptions query;
            query.fields = {false, true, false, false, true};
            query.page_size = scene_query_maximum_page_size;
            // Equal row/scan sizes ensure every nonterminal page scans exactly
            // one quantum. The preview path uses this same single-scan budget.
            query.scan_budget = scene_query_maximum_page_size;
            static_assert(render_scene_maximum_scanned_slots % scene_query_maximum_page_size == 0);
            query.expected_revision = revision;
            auto page = take(scene.query_entities(query));
            std::size_t scanned = 0;
            while (true) {
                require(page.scanned_slots() <= render_scene_maximum_scanned_slots - scanned,
                        "Render scene exceeds its slot scan limit");
                scanned += page.scanned_slots();
                for (const auto& entity : page.entities())
                    append(entity);
                if (!page.next_cursor())
                    break;
                require(scene_query_maximum_page_size <=
                            render_scene_maximum_scanned_slots - scanned,
                        "Render scene exceeds its slot scan limit");
                page = take(scene.continue_query(*page.next_cursor()));
            }
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
