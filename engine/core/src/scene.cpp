#include "shark/scene.hpp"

#include "scene_generation.hpp"
#include "utf8.hpp"

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <map>
#include <thread>
#include <type_traits>
#include <utility>

namespace shark {
namespace {

std::atomic<std::uint64_t> next_scene_instance{1};

Error scene_error(ErrorCode code, const char* message) {
    return Error{code, message, {.operation = "scene"}};
}

Error entity_error(Error error, const char* operation, const EntityIdentity& identity) {
    error.context.operation = operation;
    error.context.entity_id =
        std::visit([](const auto& id) { return id.value().to_string(); }, identity);
    return error;
}

bool valid_authority(const AuthorityStamp& authority) noexcept {
    return authority.epoch != 0;
}

} // namespace

struct Scene::Impl {
    struct Record {
        EntityIdentity identity;
        std::optional<AuthorityStamp> authority;
        Name name;
        Transform transform;
        std::optional<EntityHandle> parent;
        std::vector<EntityHandle> children;
        math::Mat4 world;
        std::size_t depth;
    };
    struct Slot {
        entt::entity entity{entt::null};
        std::uint64_t generation{1};
        bool retired{false};
    };
    struct Rename {
        EntityHandle handle;
        Name name;
    };
    struct ChangeTransform {
        EntityHandle handle;
        Transform transform;
    };
    struct ChangeParent {
        EntityHandle handle;
        std::optional<EntityHandle> parent;
        ReparentMode mode;
    };
    using Mutation = std::variant<EntitySeed, EntityHandle, Rename, ChangeTransform, ChangeParent>;
    struct Pending {
        SceneMutationTicket ticket;
        Mutation mutation;
    };
    struct WorldUpdate {
        EntityHandle handle;
        math::Mat4 world;
        std::size_t depth;
    };

    SceneScope scope;
    SceneLimits limits;
    const std::thread::id owner{std::this_thread::get_id()};
    const std::uint64_t instance;
    std::uint64_t revision{0};
    std::uint64_t next_ticket{1};
    bool closed{false};
    bool visiting{false};
    entt::registry registry;
    std::vector<Slot> slots;
    std::map<EntityIdentity, EntityHandle> identities;
    std::vector<Pending> pending;

    Impl(SceneScope value, SceneLimits bounds, std::uint64_t token)
        : scope(std::move(value)), limits(bounds), instance(token) {}

    Result<void> check_owner() const {
        if (owner != std::this_thread::get_id()) {
            return scene_error(ErrorCode::conflict, "scene access requires its owner thread");
        }
        return {};
    }
    Result<void> check_live() const {
        if (auto result = check_owner(); !result) {
            return result;
        }
        if (closed) {
            return scene_error(ErrorCode::conflict, "scene is closed");
        }
        return {};
    }
    Result<void> check_immediate() const {
        if (auto result = check_live(); !result) {
            return result;
        }
        if (visiting || !pending.empty()) {
            return scene_error(ErrorCode::conflict,
                               "immediate mutation requires a boundary without pending work");
        }
        return {};
    }
    Result<void> check_seed(const EntitySeed& seed) const {
        const bool authored = std::holds_alternative<AuthoredSceneScope>(scope);
        if (authored != std::holds_alternative<AuthoredEntityId>(seed.identity)) {
            return scene_error(ErrorCode::invalid_id,
                               "entity identity domain does not match scene scope");
        }
        const bool replica = std::holds_alternative<ReplicaSceneScope>(scope);
        if (replica != seed.replica_authority.has_value() ||
            (seed.replica_authority && !valid_authority(*seed.replica_authority))) {
            return scene_error(
                ErrorCode::invalid_argument,
                "replica authority must be present only for replicas and have a positive epoch");
        }
        return {};
    }
    std::optional<AuthorityStamp> seed_authority(const EntitySeed& seed) const {
        if (const auto* region = std::get_if<RegionSceneScope>(&scope)) {
            return region->authority;
        }
        return seed.replica_authority;
    }
    Result<std::size_t> checked_slot(EntityHandle handle) const {
        if (handle.scene_instance != instance || handle.generation == 0 ||
            handle.slot >= slots.size()) {
            return scene_error(ErrorCode::invalid_id,
                               "entity handle does not belong to this live scene");
        }
        const auto& slot = slots[handle.slot];
        if (slot.retired || slot.entity == entt::null || slot.generation != handle.generation) {
            return scene_error(ErrorCode::invalid_id, "entity handle is stale");
        }
        return static_cast<std::size_t>(handle.slot);
    }
    Record& record(EntityHandle handle) {
        return registry.get<Record>(slots[handle.slot].entity);
    }
    const Record& record(EntityHandle handle) const {
        return registry.get<Record>(slots[handle.slot].entity);
    }
    Result<void> check_revision() const {
        if (revision == std::numeric_limits<std::uint64_t>::max()) {
            return scene_error(ErrorCode::conflict, "scene revision capacity exhausted");
        }
        return {};
    }
    Error contextualize(Error error, EntityHandle handle, const char* operation) const {
        error.context.operation = operation;
        if (checked_slot(handle)) {
            return entity_error(std::move(error), operation, record(handle).identity);
        }
        return error;
    }
    Result<void> finish_change(Result<EntityHandle> result, EntityHandle handle,
                               const char* operation) const {
        if (!result) {
            return contextualize(std::move(result).error(), handle, operation);
        }
        return {};
    }
    Result<void> check_parent(std::optional<EntityHandle> parent,
                              const std::optional<AuthorityStamp>& authority) const {
        if (parent) {
            if (auto checked = checked_slot(*parent); !checked) {
                return checked.error();
            }
            if (record(*parent).authority != authority) {
                return scene_error(ErrorCode::conflict,
                                   "parent must have the same authority incarnation");
            }
        }
        return {};
    }
    Result<void> check_parent_request(const ChangeParent& request) const {
        if (auto checked = checked_slot(request.handle); !checked) {
            return checked.error();
        }
        if (request.mode != ReparentMode::keep_local && request.mode != ReparentMode::keep_world) {
            return scene_error(ErrorCode::invalid_argument, "unsupported reparent mode");
        }
        if (auto checked = check_parent(request.parent, record(request.handle).authority);
            !checked) {
            return checked;
        }
        for (auto ancestor = request.parent; ancestor; ancestor = record(*ancestor).parent) {
            if (*ancestor == request.handle) {
                return scene_error(ErrorCode::conflict, "reparent would create a hierarchy cycle");
            }
        }
        return {};
    }
    Result<WorldUpdate> prepare_seed(EntitySeed& seed) const {
        if (auto checked = check_seed(seed); !checked) {
            return checked.error();
        }
        if (auto checked = validate_name(seed.name); !checked) {
            return checked.error();
        }
        auto canonical = canonicalize_transform(seed.transform);
        if (!canonical) {
            return canonical.error();
        }
        seed.transform = canonical.value();
        if (auto checked = check_parent(seed.parent, seed_authority(seed)); !checked) {
            return checked.error();
        }
        const auto depth = seed.parent ? record(*seed.parent).depth + 1 : 1;
        if (depth > limits.max_hierarchy_depth) {
            return scene_error(ErrorCode::conflict, "scene hierarchy depth exhausted");
        }
        auto local = transform_matrix(seed.transform);
        if (!local) {
            return local.error();
        }
        auto world = seed.parent ? record(*seed.parent).world * local.value() : local.value();
        if (auto checked = validate_transform_matrix(world); !checked) {
            return checked.error();
        }
        return WorldUpdate{{}, world, depth};
    }
    // Compute every affected descendant before publishing any component/link.
    Result<std::vector<WorldUpdate>> prepare_subtree(EntityHandle root, const Transform& local,
                                                     std::optional<EntityHandle> parent) const {
        auto root_matrix = transform_matrix(local);
        if (!root_matrix) {
            return root_matrix.error();
        }
        try {
            std::vector<WorldUpdate> updates;
            updates.push_back(
                {root, parent ? record(*parent).world * root_matrix.value() : root_matrix.value(),
                 parent ? record(*parent).depth + 1 : 1});
            for (std::size_t index = 0; index < updates.size(); ++index) {
                const auto update = updates[index];
                if (update.depth > limits.max_hierarchy_depth) {
                    return scene_error(ErrorCode::conflict, "scene hierarchy depth exhausted");
                }
                if (auto checked = validate_transform_matrix(update.world); !checked) {
                    return checked.error();
                }
                for (const auto child : record(update.handle).children) {
                    auto child_local = transform_matrix(record(child).transform);
                    if (!child_local) {
                        return child_local.error();
                    }
                    updates.push_back(
                        {child, update.world * child_local.value(), update.depth + 1});
                }
            }
            return updates;
        } catch (...) {
            return scene_error(ErrorCode::execution_failed, "subtree validation allocation failed");
        }
    }
    void publish_worlds(const std::vector<WorldUpdate>& updates) {
        for (const auto& update : updates) {
            auto& entity = record(update.handle);
            entity.world = update.world;
            entity.depth = update.depth;
        }
    }
    void remove_child(EntityHandle parent, EntityHandle child) {
        auto& children = record(parent).children;
        children.erase(std::find(children.begin(), children.end(), child));
    }
    void append_child(EntityHandle parent, EntityHandle child) {
        auto& children = record(parent).children;
        children.push_back(child); // Capacity is reserved before publication.
        std::sort(children.begin(), children.end(),
                  [](EntityHandle left, EntityHandle right) { return left.slot < right.slot; });
    }
    EntityReference reference_at(std::size_t index) const {
        const auto& slot = slots[index];
        const auto& record = registry.get<Record>(slot.entity);
        return {{instance, static_cast<std::uint32_t>(index), slot.generation},
                record.identity,
                scope,
                record.authority};
    }
    Result<void> check_expected_revision(std::optional<SceneRevision> expected) const {
        if (expected && *expected != SceneRevision{instance, revision}) {
            return scene_error(ErrorCode::conflict,
                               "scene incarnation or committed revision differs from inspection");
        }
        return {};
    }
    SceneInspectedEntity inspect_at(std::size_t index, SceneQueryFields fields) const {
        const auto& entity = registry.get<Record>(slots[index].entity);
        SceneInspectedEntity result{reference_at(index), std::nullopt, std::nullopt, std::nullopt};
        if (fields.name) {
            result.name = entity.name;
        }
        if (fields.transform) {
            result.transform = SceneInspectedTransform{entity.transform, entity.world};
        }
        if (fields.hierarchy) {
            result.hierarchy =
                SceneInspectedHierarchy{entity.parent, entity.children.size(), entity.depth};
        }
        return result;
    }
    Result<EntityHandle> insert(EntitySeed seed) {
        auto prepared = prepare_seed(seed);
        if (!prepared) {
            return prepared.error();
        }
        if (identities.contains(seed.identity)) {
            return scene_error(ErrorCode::conflict,
                               "entity identity is already live in this scene");
        }
        if (identities.size() >= limits.max_entities ||
            revision == std::numeric_limits<std::uint64_t>::max()) {
            return scene_error(ErrorCode::conflict, "scene entity or revision capacity exhausted");
        }
        auto found = std::find_if(slots.begin(), slots.end(), [](const Slot& slot) {
            return !slot.retired && slot.entity == entt::null;
        });
        const auto index = static_cast<std::size_t>(found - slots.begin());
        const bool append = index == slots.size();
        // Retired slots also consume the bounded slot table.
        if (append && slots.size() >= limits.max_entities) {
            return scene_error(ErrorCode::conflict, "scene slot capacity exhausted");
        }
        entt::entity native = entt::null;
        bool appended = false;
        try {
            if (seed.parent) {
                auto& children = record(*seed.parent).children;
                children.reserve(children.size() + 1);
            }
            if (append) {
                slots.emplace_back();
                appended = true;
            }
            native = registry.create();
            registry.emplace<Record>(native, seed.identity, seed_authority(seed),
                                     std::move(seed.name), seed.transform, seed.parent,
                                     std::vector<EntityHandle>{}, prepared.value().world,
                                     prepared.value().depth);
            const EntityHandle handle{instance, static_cast<std::uint32_t>(index),
                                      slots[index].generation};
            identities.emplace(seed.identity, handle);
            slots[index].entity = native;
            if (seed.parent) {
                append_child(*seed.parent, handle);
            }
            ++revision;
            return handle;
        } catch (...) {
            if (native != entt::null) {
                registry.destroy(native);
            }
            if (appended) {
                slots.pop_back();
            }
            return scene_error(ErrorCode::execution_failed, "entity allocation failed");
        }
    }
    Result<EntityHandle> erase(EntityHandle handle) {
        const auto index = checked_slot(handle);
        if (!index) {
            return index.error();
        }
        if (auto checked = check_revision(); !checked) {
            return checked.error();
        }
        std::vector<EntityHandle> subtree;
        try {
            subtree.push_back(handle);
            for (std::size_t cursor = 0; cursor < subtree.size(); ++cursor) {
                const auto& children = record(subtree[cursor]).children;
                subtree.insert(subtree.end(), children.begin(), children.end());
            }
        } catch (...) {
            return scene_error(ErrorCode::execution_failed,
                               "subtree destruction allocation failed");
        }
        if (const auto parent = record(handle).parent) {
            remove_child(*parent, handle);
        }
        for (auto current = subtree.rbegin(); current != subtree.rend(); ++current) {
            auto& slot = slots[current->slot];
            identities.erase(registry.get<Record>(slot.entity).identity);
            registry.destroy(slot.entity);
            slot.entity = entt::null;
            slot.retired = !detail::advance_entity_generation(slot.generation);
        }
        ++revision;
        return handle;
    }
    Result<EntityHandle> rename(Rename change) {
        if (auto checked = checked_slot(change.handle); !checked) {
            return checked.error();
        }
        if (auto checked = validate_name(change.name); !checked) {
            return checked.error();
        }
        auto& entity = record(change.handle);
        if (entity.name == change.name) {
            return change.handle;
        }
        if (auto checked = check_revision(); !checked) {
            return checked.error();
        }
        entity.name = std::move(change.name);
        ++revision;
        return change.handle;
    }
    Result<EntityHandle> transform(ChangeTransform change) {
        if (auto checked = checked_slot(change.handle); !checked) {
            return checked.error();
        }
        auto& entity = record(change.handle);
        if (entity.transform == change.transform) {
            return change.handle;
        }
        auto canonical = canonicalize_transform(change.transform);
        if (!canonical) {
            return canonical.error();
        }
        if (entity.transform == canonical.value()) {
            return change.handle;
        }
        if (auto checked = check_revision(); !checked) {
            return checked.error();
        }
        auto updates = prepare_subtree(change.handle, canonical.value(), entity.parent);
        if (!updates) {
            return updates.error();
        }
        entity.transform = canonical.value();
        publish_worlds(updates.value());
        ++revision;
        return change.handle;
    }
    Result<EntityHandle> reparent(ChangeParent change) {
        if (auto checked = check_parent_request(change); !checked) {
            return checked.error();
        }
        auto& entity = record(change.handle);
        if (entity.parent == change.parent) {
            return change.handle;
        }
        if (auto checked = check_revision(); !checked) {
            return checked.error();
        }
        auto local = entity.transform;
        if (change.mode == ReparentMode::keep_world) {
            auto matrix = entity.world;
            if (change.parent) {
                auto inverse = inverse_transform_matrix(record(*change.parent).world);
                if (!inverse) {
                    return inverse.error();
                }
                matrix = inverse.value() * matrix;
            }
            auto decomposed = decompose_transform(matrix);
            if (!decomposed) {
                return decomposed.error();
            }
            local = decomposed.value();
        }
        auto updates = prepare_subtree(change.handle, local, change.parent);
        if (!updates) {
            return updates.error();
        }
        if (change.mode == ReparentMode::keep_world) {
            for (const auto& update : updates.value()) {
                if (!transform_matrices_equivalent(update.world, record(update.handle).world)) {
                    return scene_error(
                        ErrorCode::invalid_argument,
                        "keep-world reparent exceeds the world preservation tolerance");
                }
            }
        }
        try {
            if (change.parent) {
                auto& children = record(*change.parent).children;
                children.reserve(children.size() + 1);
            }
        } catch (...) {
            return scene_error(ErrorCode::execution_failed, "parent link allocation failed");
        }
        if (entity.parent) {
            remove_child(*entity.parent, change.handle);
        }
        if (change.parent) {
            append_child(*change.parent, change.handle);
        }
        entity.parent = change.parent;
        entity.transform = local;
        publish_worlds(updates.value());
        ++revision;
        return change.handle;
    }
    Result<SceneMutationTicket> enqueue(Mutation mutation) {
        if (pending.size() >= limits.max_pending_mutations || next_ticket == 0) {
            return scene_error(ErrorCode::conflict,
                               "scene deferred queue or ticket capacity exhausted");
        }
        try {
            const auto ticket = next_ticket;
            pending.push_back({ticket, std::move(mutation)});
            next_ticket = ticket == std::numeric_limits<std::uint64_t>::max() ? 0 : ticket + 1;
            return ticket;
        } catch (...) {
            return scene_error(ErrorCode::execution_failed, "deferred mutation allocation failed");
        }
    }
};

Result<std::unique_ptr<Scene>> Scene::create(SceneScope scope, SceneLimits limits) {
    if (limits.max_entities == 0 || limits.max_entities > 1'000'000 ||
        limits.max_pending_mutations == 0 || limits.max_pending_mutations > 65'536 ||
        limits.max_hierarchy_depth == 0 || limits.max_hierarchy_depth > 1024) {
        return scene_error(ErrorCode::invalid_configuration,
                           "scene bounds must be within supported storage limits");
    }
    if (const auto* region = std::get_if<RegionSceneScope>(&scope);
        region && !valid_authority(region->authority)) {
        return scene_error(ErrorCode::invalid_argument, "region authority epoch must be positive");
    }
    const auto instance = detail::claim_scene_instance(next_scene_instance);
    if (instance == 0) {
        return scene_error(ErrorCode::conflict, "scene instance capacity exhausted");
    }
    try {
        return std::unique_ptr<Scene>(
            new Scene(std::make_unique<Impl>(std::move(scope), limits, instance)));
    } catch (...) {
        return scene_error(ErrorCode::execution_failed, "scene allocation failed");
    }
}

Scene::Scene(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Scene::~Scene() {
    if (!impl_->closed && impl_->owner != std::this_thread::get_id()) {
        std::terminate();
    }
}

Result<EntityHandle> Scene::create_entity(EntitySeed seed) {
    if (auto result = impl_->check_immediate(); !result) {
        return entity_error(std::move(result).error(), "scene.create-entity", seed.identity);
    }
    const auto identity = seed.identity;
    auto result = impl_->insert(std::move(seed));
    if (!result) {
        return entity_error(std::move(result).error(), "scene.create-entity", identity);
    }
    return result;
}

Result<void> Scene::destroy_entity(EntityHandle handle) {
    if (auto result = impl_->check_immediate(); !result) {
        return result;
    }
    auto result = impl_->erase(handle);
    return result ? Result<void>{} : Result<void>{result.error()};
}

Result<void> Scene::set_name(EntityHandle handle, Name name) {
    if (auto checked = impl_->check_immediate(); !checked) {
        return checked;
    }
    return impl_->finish_change(impl_->rename({handle, std::move(name)}), handle, "scene.set-name");
}

Result<void> Scene::set_transform(EntityHandle handle, Transform transform) {
    if (auto checked = impl_->check_immediate(); !checked) {
        return checked;
    }
    return impl_->finish_change(impl_->transform({handle, transform}), handle,
                                "scene.set-transform");
}

Result<void> Scene::reparent(EntityHandle handle, std::optional<EntityHandle> parent,
                             ReparentMode mode) {
    if (auto checked = impl_->check_immediate(); !checked) {
        return checked;
    }
    return impl_->finish_change(impl_->reparent({handle, parent, mode}), handle, "scene.reparent");
}

Result<SceneEntityState> Scene::entity_state(EntityHandle handle) const {
    if (auto checked = impl_->check_live(); !checked) {
        return checked.error();
    }
    auto index = impl_->checked_slot(handle);
    if (!index) {
        return index.error();
    }
    try {
        const auto& entity = impl_->record(handle);
        return SceneEntityState{impl_->reference_at(index.value()),
                                entity.name,
                                entity.transform,
                                entity.parent,
                                entity.children,
                                entity.world,
                                entity.depth};
    } catch (...) {
        return impl_->contextualize(
            scene_error(ErrorCode::execution_failed, "entity state copy allocation failed"), handle,
            "scene.entity-state");
    }
}

Result<SceneQueryPage> Scene::query_entities(const SceneQueryOptions& options) const {
    return query_page(options, 0);
}

Result<SceneQueryPage> Scene::continue_query(const SceneQueryCursor& cursor) const {
    auto checked = impl_->check_live();
    if (checked) {
        checked = impl_->check_expected_revision(cursor.revision_);
    }
    if (!checked) {
        auto error = std::move(checked).error();
        error.context.operation = "scene.query";
        return error;
    }
    return query_page(cursor.options_, cursor.next_slot_);
}

Result<SceneQueryPage> Scene::query_page(const SceneQueryOptions& options,
                                         std::size_t next_slot) const {
    constexpr auto operation = "scene.query";
    auto contextual = [](Error error) {
        error.context.operation = operation;
        return error;
    };
    if (auto checked = impl_->check_live(); !checked) {
        return contextual(std::move(checked).error());
    }
    if (options.page_size == 0 || options.page_size > scene_query_maximum_page_size ||
        options.scan_budget == 0 || options.scan_budget > scene_query_maximum_scan_budget ||
        next_slot > impl_->slots.size()) {
        return contextual(scene_error(ErrorCode::invalid_argument,
                                      "query page size must be 1..256 and scan budget 1..4096"));
    }
    switch (options.selection) {
    case SceneQuerySelection::all:
    case SceneQuerySelection::roots:
        if (options.parent) {
            return contextual(
                scene_error(ErrorCode::invalid_argument, "only a children query accepts a parent"));
        }
        break;
    case SceneQuerySelection::children:
        if (!options.parent) {
            return contextual(scene_error(ErrorCode::invalid_argument,
                                          "a children query requires a checked parent"));
        }
        break;
    default:
        return contextual(scene_error(ErrorCode::invalid_argument, "unknown query selection"));
    }
    if (options.name_prefix.size() > maximum_name_bytes ||
        !detail::valid_utf8(options.name_prefix, true)) {
        return contextual(
            scene_error(ErrorCode::invalid_argument,
                        "query name prefix must be 0..256 UTF-8 bytes without controls"));
    }
    if (auto checked = impl_->check_expected_revision(options.expected_revision); !checked) {
        return contextual(std::move(checked).error());
    }
    if (options.parent) {
        if (auto checked = impl_->checked_slot(*options.parent); !checked) {
            return contextual(std::move(checked).error());
        }
    }
    try {
        std::vector<SceneInspectedEntity> entities;
        entities.reserve(std::min(options.page_size, options.scan_budget));
        std::size_t scanned = 0;
        while (next_slot < impl_->slots.size() && scanned < options.scan_budget &&
               entities.size() < options.page_size) {
            const auto index = next_slot++;
            ++scanned;
            const auto& slot = impl_->slots[index];
            if (slot.retired || slot.entity == entt::null) {
                continue;
            }
            const auto& entity = impl_->registry.get<Impl::Record>(slot.entity);
            if ((options.selection == SceneQuerySelection::roots && entity.parent) ||
                (options.selection == SceneQuerySelection::children &&
                 entity.parent != options.parent) ||
                !entity.name.value.starts_with(options.name_prefix)) {
                continue;
            }
            entities.push_back(impl_->inspect_at(index, options.fields));
        }
        const SceneRevision revision{impl_->instance, impl_->revision};
        std::optional<SceneQueryCursor> cursor;
        // No lookahead: holes and nonmatches also consume the explicit budget.
        if (next_slot < impl_->slots.size()) {
            cursor = SceneQueryCursor{revision, options, next_slot};
        }
        return SceneQueryPage{revision, impl_->scope, std::move(entities), scanned,
                              std::move(cursor)};
    } catch (...) {
        return contextual(
            scene_error(ErrorCode::execution_failed, "query snapshot allocation failed"));
    }
}

Result<SceneQueryPage> Scene::inspect_entity(EntityHandle handle, SceneQueryFields fields,
                                             std::optional<SceneRevision> expected_revision) const {
    constexpr auto operation = "scene.inspect-entity";
    auto contextual = [](Error error) {
        error.context.operation = operation;
        return error;
    };
    if (auto checked = impl_->check_live(); !checked) {
        return contextual(std::move(checked).error());
    }
    if (auto checked = impl_->check_expected_revision(expected_revision); !checked) {
        return contextual(std::move(checked).error());
    }
    const auto index = impl_->checked_slot(handle);
    if (!index) {
        return contextual(index.error());
    }
    try {
        std::vector<SceneInspectedEntity> entities;
        entities.push_back(impl_->inspect_at(index.value(), fields));
        return SceneQueryPage{
            {impl_->instance, impl_->revision}, impl_->scope, std::move(entities), 1, std::nullopt};
    } catch (...) {
        return impl_->contextualize(
            scene_error(ErrorCode::execution_failed, "entity inspection allocation failed"), handle,
            operation);
    }
}

Result<EntityReference> Scene::reference(EntityHandle handle) const {
    if (auto result = impl_->check_live(); !result) {
        return result.error();
    }
    const auto index = impl_->checked_slot(handle);
    if (!index) {
        return index.error();
    }
    return impl_->reference_at(index.value());
}

Result<EntityHandle> Scene::resolve(const EntityReference& value) const {
    auto current = reference(value.handle);
    if (!current) {
        return entity_error(std::move(current).error(), "scene.resolve", value.identity);
    }
    if (current.value() != value) {
        return entity_error(scene_error(ErrorCode::invalid_id,
                                        "entity reference scope, identity or authority is stale"),
                            "scene.resolve", value.identity);
    }
    return value.handle;
}

Result<EntityHandle> Scene::find(const EntityIdentity& identity) const {
    if (auto result = impl_->check_live(); !result) {
        return entity_error(std::move(result).error(), "scene.find", identity);
    }
    const auto found = impl_->identities.find(identity);
    if (found == impl_->identities.end()) {
        return entity_error(
            scene_error(ErrorCode::invalid_id, "entity identity is not live in this scene"),
            "scene.find", identity);
    }
    return found->second;
}

Result<SceneStats> Scene::stats() const {
    if (auto result = impl_->check_owner(); !result) {
        return result.error();
    }
    const auto retired = std::count_if(impl_->slots.begin(), impl_->slots.end(),
                                       [](const Impl::Slot& slot) { return slot.retired; });
    return SceneStats{impl_->instance,
                      impl_->revision,
                      impl_->identities.size(),
                      impl_->pending.size(),
                      static_cast<std::size_t>(retired),
                      impl_->closed};
}

Result<SceneMutationTicket> Scene::defer_create(EntitySeed seed) {
    if (auto result = impl_->check_live(); !result) {
        return entity_error(std::move(result).error(), "scene.defer-create", seed.identity);
    }
    if (auto result = impl_->prepare_seed(seed); !result) {
        return entity_error(std::move(result).error(), "scene.defer-create", seed.identity);
    }
    const auto identity = seed.identity;
    auto result = impl_->enqueue(std::move(seed));
    if (!result) {
        return entity_error(std::move(result).error(), "scene.defer-create", identity);
    }
    return result;
}

Result<SceneMutationTicket> Scene::defer_destroy(EntityHandle handle) {
    if (auto result = impl_->check_live(); !result) {
        return result.error();
    }
    if (auto result = impl_->checked_slot(handle); !result) {
        return result.error();
    }
    return impl_->enqueue(handle);
}

Result<SceneMutationTicket> Scene::defer_set_name(EntityHandle handle, Name name) {
    if (auto checked = impl_->check_live(); !checked) {
        return checked.error();
    }
    if (auto checked = impl_->checked_slot(handle); !checked) {
        return checked.error();
    }
    if (auto checked = validate_name(name); !checked) {
        return impl_->contextualize(std::move(checked).error(), handle, "scene.defer-set-name");
    }
    return impl_->enqueue(Impl::Rename{handle, std::move(name)});
}

Result<SceneMutationTicket> Scene::defer_set_transform(EntityHandle handle, Transform transform) {
    if (auto checked = impl_->check_live(); !checked) {
        return checked.error();
    }
    if (auto checked = impl_->checked_slot(handle); !checked) {
        return checked.error();
    }
    auto canonical = canonicalize_transform(transform);
    if (!canonical) {
        return impl_->contextualize(std::move(canonical).error(), handle,
                                    "scene.defer-set-transform");
    }
    return impl_->enqueue(Impl::ChangeTransform{handle, canonical.value()});
}

Result<SceneMutationTicket>
Scene::defer_reparent(EntityHandle handle, std::optional<EntityHandle> parent, ReparentMode mode) {
    if (auto checked = impl_->check_live(); !checked) {
        return checked.error();
    }
    if (auto checked = impl_->check_parent_request({handle, parent, mode}); !checked) {
        return impl_->contextualize(std::move(checked).error(), handle, "scene.defer-reparent");
    }
    return impl_->enqueue(Impl::ChangeParent{handle, parent, mode});
}

Result<SceneMutationReport> Scene::apply_deferred() {
    if (auto result = impl_->check_live(); !result) {
        return result.error();
    }
    if (impl_->visiting) {
        return scene_error(ErrorCode::conflict, "deferred mutations require an iteration boundary");
    }
    SceneMutationReport report{impl_->revision, impl_->revision, {}};
    try {
        report.outcomes.reserve(impl_->pending.size());
    } catch (...) {
        return scene_error(ErrorCode::execution_failed, "deferred outcome allocation failed");
    }
    for (const auto& pending : impl_->pending) {
        auto outcome = std::visit(
            [&](const auto& mutation) -> SceneMutationOutcome {
                using Type = std::decay_t<decltype(mutation)>;
                if constexpr (std::is_same_v<Type, EntitySeed>) {
                    auto result = impl_->insert(mutation);
                    if (!result) {
                        result = entity_error(std::move(result).error(),
                                              "scene.apply-deferred.create", mutation.identity);
                    }
                    return {pending.ticket, SceneMutationKind::create, std::move(result)};
                } else if constexpr (std::is_same_v<Type, EntityHandle>) {
                    return {pending.ticket, SceneMutationKind::destroy, impl_->erase(mutation)};
                } else {
                    const auto perform = [&]() -> SceneMutationOutcome {
                        if constexpr (std::is_same_v<Type, Impl::Rename>) {
                            return {pending.ticket, SceneMutationKind::set_name,
                                    impl_->rename(mutation)};
                        } else if constexpr (std::is_same_v<Type, Impl::ChangeTransform>) {
                            return {pending.ticket, SceneMutationKind::set_transform,
                                    impl_->transform(mutation)};
                        } else {
                            return {pending.ticket, SceneMutationKind::reparent,
                                    impl_->reparent(mutation)};
                        }
                    };
                    auto result = perform();
                    if (!result.result) {
                        result.result =
                            impl_->contextualize(std::move(result.result).error(), mutation.handle,
                                                 "scene.apply-deferred");
                    }
                    return result;
                }
            },
            pending.mutation);
        report.outcomes.push_back(std::move(outcome));
    }
    impl_->pending.clear();
    report.revision_after = impl_->revision;
    return report;
}

Result<void>
Scene::visit_entities(const std::function<Result<void>(const EntityReference&)>& visitor) {
    if (auto result = impl_->check_live(); !result) {
        return result;
    }
    if (impl_->visiting) {
        return scene_error(ErrorCode::conflict, "scene visits cannot nest");
    }
    if (!visitor) {
        return scene_error(ErrorCode::invalid_argument, "scene visitor is empty");
    }
    const auto initial_pending = impl_->pending.size();
    impl_->visiting = true;
    const auto cancel_visit = [&] {
        impl_->pending.erase(impl_->pending.begin() + static_cast<std::ptrdiff_t>(initial_pending),
                             impl_->pending.end());
        impl_->visiting = false;
    };
    try {
        for (std::size_t index = 0; index < impl_->slots.size(); ++index) {
            if (impl_->slots[index].entity != entt::null) {
                auto result = visitor(impl_->reference_at(index));
                if (!result) {
                    cancel_visit();
                    return result;
                }
            }
        }
    } catch (...) {
        cancel_visit();
        return scene_error(ErrorCode::execution_failed, "scene visitor threw an exception");
    }
    impl_->visiting = false;
    return {};
}

Result<void> Scene::close() {
    if (auto result = impl_->check_owner(); !result) {
        return result;
    }
    if (impl_->visiting) {
        return scene_error(ErrorCode::conflict, "scene close requires an iteration boundary");
    }
    if (!impl_->closed) {
        impl_->pending.clear();
        impl_->identities.clear();
        impl_->registry.clear();
        impl_->slots.clear();
        impl_->closed = true;
    }
    return {};
}

} // namespace shark
