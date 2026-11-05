#include "shark/runtime_prefab.hpp"

#include "runtime_prefab_internal.hpp"

#include <algorithm>
#include <exception>
#include <map>
#include <utility>

namespace shark {
namespace {

struct Failure {
    Error error;
};

[[noreturn]] void fail(const char* message, std::string path = {}, std::string entity = {}) {
    throw Failure{Error{ErrorCode::invalid_argument,
                        message,
                        {.path = std::move(path), .entity_id = std::move(entity)}}};
}

template <typename T> T take(Result<T> value) {
    if (!value)
        throw Failure{std::move(value).error()};
    return std::move(value).value();
}

void take(Result<void> value) {
    if (!value)
        throw Failure{std::move(value).error()};
}

void charge(std::size_t& total, std::size_t bytes) {
    if (bytes > runtime_prefab_maximum_decoded_bytes - total)
        fail("Runtime prefab exceeds its decoded byte bound", "/entities");
    total += bytes;
}

Result<RegionSceneScope> region_scope(const Scene& scene, const char* operation) {
    auto stats = scene.stats();
    if (!stats)
        return stats.error();
    if (stats.value().closed || stats.value().pending_mutations != 0)
        return Error{ErrorCode::conflict,
                     "Spawn authority requires a live committed Scene",
                     {.operation = operation}};
    SceneQueryOptions query;
    query.fields = {false, false, false, false, false, false, false};
    query.page_size = 1;
    query.scan_budget = 1;
    query.expected_revision = SceneRevision{stats.value().instance, stats.value().revision};
    auto page = scene.query_entities(query);
    if (!page)
        return page.error();
    const auto* region = std::get_if<RegionSceneScope>(&page.value().scope());
    if (!region || region->authority.epoch == 0)
        return Error{ErrorCode::conflict,
                     "Spawn authority requires an authoritative Region Scene",
                     {.operation = operation}};
    return *region;
}

} // namespace

Result<RuntimePrefab> validate_runtime_prefab(const RuntimePrefab& prefab) {
    constexpr auto operation = "runtime.prefab.validate";
    try {
        if (prefab.entities.empty() || prefab.entities.size() > runtime_prefab_maximum_entities)
            fail("Runtime prefab requires between 1 and 4096 entities", "/entities");
        std::size_t bytes = sizeof(RuntimePrefab);
        charge(bytes, prefab.entities.size() * sizeof(PreparedEntity));
        // Bound every owned string before copying any part of the candidate.
        for (const auto& entity : prefab.entities) {
            take(validate_name(entity.name));
            charge(bytes, entity.name.value.size() + 1);
            if (entity.script) {
                if (entity.script->source.size() > script_maximum_source_path_bytes)
                    fail("Runtime prefab script path exceeds its byte bound", "/entities",
                         entity.id.value().to_string());
                charge(bytes, entity.script->source.size() + 1);
            }
        }
        RuntimePrefab result{prefab.asset, prefab.root, {}};
        result.entities.reserve(prefab.entities.size());
        std::map<AuthoredEntityId, std::size_t> indices;
        for (std::size_t index = 0; index < prefab.entities.size(); ++index) {
            const auto& entity = prefab.entities[index];
            try {
                if (!indices.emplace(entity.id, index).second)
                    fail("Runtime prefab has duplicate entity IDs", "/entities");
                auto transform = take(canonicalize_transform(entity.transform));
                if (entity.camera)
                    static_cast<void>(take(validate_camera(*entity.camera)));
                if (entity.mesh_renderer)
                    static_cast<void>(take(validate_mesh_renderer(*entity.mesh_renderer)));
                if (entity.rigid_body) {
                    if (entity.parent)
                        fail("Runtime prefab rigid bodies must be roots", "/entities");
                    static_cast<void>(
                        take(validate_rigid_body_pose(*entity.rigid_body, transform)));
                }
                if (entity.script)
                    static_cast<void>(take(validate_script(*entity.script)));
                result.entities.push_back({entity.id, entity.parent, entity.name, transform,
                                           entity.camera, entity.mesh_renderer, entity.rigid_body,
                                           entity.script});
            } catch (Failure& failure) {
                failure.error.context.entity_id = entity.id.value().to_string();
                failure.error.context.path = "/entities/" + std::to_string(index);
                throw;
            }
        }
        const auto root = indices.find(result.root);
        if (root == indices.end())
            fail("Runtime prefab root is missing", "/root");
        std::vector<std::vector<std::size_t>> children(result.entities.size());
        std::vector<std::optional<std::size_t>> parents(result.entities.size());
        for (std::size_t index = 0; index < result.entities.size(); ++index) {
            const auto& entity = result.entities[index];
            if (entity.id == result.root) {
                if (entity.parent)
                    fail("Runtime prefab root must be parentless",
                         "/entities/" + std::to_string(index), entity.id.value().to_string());
            } else {
                if (!entity.parent)
                    fail("Only the runtime prefab root may be parentless",
                         "/entities/" + std::to_string(index), entity.id.value().to_string());
                const auto parent = indices.find(*entity.parent);
                if (parent == indices.end() || parent->second == index)
                    fail("Runtime prefab parent is missing or self-referential",
                         "/entities/" + std::to_string(index), entity.id.value().to_string());
                parents[index] = parent->second;
                children[parent->second].push_back(index);
            }
        }
        std::vector<std::size_t> order;
        order.reserve(result.entities.size());
        order.push_back(root->second);
        std::vector<math::Mat4> worlds(result.entities.size(), math::Mat4{1.0});
        std::vector<std::size_t> depths(result.entities.size());
        for (std::size_t cursor = 0; cursor < order.size(); ++cursor) {
            const auto index = order[cursor];
            const auto parent = parents[index];
            try {
                depths[index] = parent ? depths[*parent] + 1 : 1;
                if (depths[index] > prepared_scene_maximum_hierarchy_depth)
                    fail("Runtime prefab hierarchy exceeds depth 256");
                const auto local = take(transform_matrix(result.entities[index].transform));
                worlds[index] = parent ? worlds[*parent] * local : local;
                take(validate_transform_matrix(worlds[index]));
                order.insert(order.end(), children[index].begin(), children[index].end());
            } catch (Failure& failure) {
                failure.error.context.path = "/entities/" + std::to_string(index);
                failure.error.context.entity_id = result.entities[index].id.value().to_string();
                throw;
            }
        }
        if (order.size() != result.entities.size())
            fail("Runtime prefab hierarchy contains a cycle", "/entities");
        // Check the actual capacities retained by the returned owned value too.
        bytes = sizeof(RuntimePrefab);
        charge(bytes, result.entities.capacity() * sizeof(PreparedEntity));
        for (const auto& entity : result.entities) {
            charge(bytes, entity.name.value.capacity() + 1);
            if (entity.script)
                charge(bytes, entity.script->source.capacity() + 1);
        }
        return result;
    } catch (Failure& failure) {
        failure.error.context.operation = operation;
        failure.error.context.asset_id = prefab.asset.value().to_string();
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Runtime prefab validation failed",
                     {.operation = operation}};
    }
}

RuntimeSpawnAuthority::RuntimeSpawnAuthority(
    std::shared_ptr<detail::RuntimeSpawnAuthorityState> state)
    : state_(std::move(state)) {}

RuntimeSpawnAuthority::~RuntimeSpawnAuthority() {
    if (state_->owner != std::this_thread::get_id())
        std::terminate();
    state_->active = false;
}

Result<std::unique_ptr<RuntimeSpawnAuthority>>
RuntimeSpawnAuthority::create(const Scene& scene, RegionSceneScope expected) {
    constexpr auto operation = "runtime.spawn-authority.create";
    try {
        auto scope = region_scope(scene, operation);
        if (!scope)
            return scope.error();
        if (scope.value() != expected)
            return Error{ErrorCode::conflict,
                         "Spawn authority scope or ownership epoch differs",
                         {.operation = operation}};
        auto stats = scene.stats();
        if (!stats)
            return stats.error();
        auto state =
            std::make_shared<detail::RuntimeSpawnAuthorityState>(detail::RuntimeSpawnAuthorityState{
                stats.value().instance, expected, std::this_thread::get_id(), true});
        return std::unique_ptr<RuntimeSpawnAuthority>(new RuntimeSpawnAuthority(std::move(state)));
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot allocate runtime spawn authority",
                     {.operation = operation}};
    }
}

Result<void> RuntimeSpawnAuthority::revoke() {
    constexpr auto operation = "runtime.spawn-authority.revoke";
    if (state_->owner != std::this_thread::get_id())
        return Error{ErrorCode::conflict,
                     "Spawn authority belongs to its creating thread",
                     {.operation = operation}};
    state_->active = false;
    return {};
}

Result<void>
detail::check_runtime_spawn_authority(const std::weak_ptr<RuntimeSpawnAuthorityState>& authority,
                                      const Scene& scene, const RegionSceneScope& expected,
                                      const char* operation) {
    const auto state = authority.lock();
    if (!state || state->owner != std::this_thread::get_id() || !state->active ||
        state->scope != expected)
        return Error{ErrorCode::conflict,
                     "Runtime spawn authority is expired, revoked or foreign",
                     {.operation = operation}};
    auto stats = scene.stats();
    if (!stats)
        return stats.error();
    if (stats.value().closed || stats.value().instance != state->scene_instance)
        return Error{ErrorCode::conflict,
                     "Runtime spawn authority Scene incarnation changed",
                     {.operation = operation}};
    auto current = region_scope(scene, operation);
    if (!current)
        return current.error();
    if (current.value() != expected)
        return Error{ErrorCode::conflict,
                     "Runtime spawn authority ownership epoch changed",
                     {.operation = operation}};
    return {};
}

} // namespace shark
