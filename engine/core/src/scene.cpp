#include "shark/scene.hpp"

#include "scene_generation.hpp"

#include <entt/entity/registry.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <map>
#include <thread>
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
    };
    struct Slot {
        entt::entity entity{entt::null};
        std::uint64_t generation{1};
        bool retired{false};
    };
    struct Pending {
        SceneMutationTicket ticket;
        std::variant<EntitySeed, EntityHandle> mutation;
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
    EntityReference reference_at(std::size_t index) const {
        const auto& slot = slots[index];
        const auto& record = registry.get<Record>(slot.entity);
        return {{instance, static_cast<std::uint32_t>(index), slot.generation},
                record.identity,
                scope,
                record.authority};
    }
    Result<EntityHandle> insert(EntitySeed seed) {
        if (auto result = check_seed(seed); !result) {
            return result.error();
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
            if (append) {
                slots.emplace_back();
                appended = true;
            }
            auto authority = seed.replica_authority;
            if (const auto* region = std::get_if<RegionSceneScope>(&scope)) {
                authority = region->authority;
            }
            native = registry.create();
            registry.emplace<Record>(native, seed.identity, authority);
            const EntityHandle handle{instance, static_cast<std::uint32_t>(index),
                                      slots[index].generation};
            identities.emplace(seed.identity, handle);
            slots[index].entity = native;
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
        if (revision == std::numeric_limits<std::uint64_t>::max()) {
            return scene_error(ErrorCode::conflict, "scene revision capacity exhausted");
        }
        auto& slot = slots[index.value()];
        identities.erase(registry.get<Record>(slot.entity).identity);
        registry.destroy(slot.entity);
        slot.entity = entt::null;
        slot.retired = !detail::advance_entity_generation(slot.generation);
        ++revision;
        return handle;
    }
    Result<SceneMutationTicket> enqueue(std::variant<EntitySeed, EntityHandle> mutation) {
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
        limits.max_pending_mutations == 0 || limits.max_pending_mutations > 65'536) {
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
    if (auto result = impl_->check_seed(seed); !result) {
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
        if (const auto* seed = std::get_if<EntitySeed>(&pending.mutation)) {
            auto result = impl_->insert(*seed);
            if (!result) {
                result = entity_error(std::move(result).error(), "scene.apply-deferred.create",
                                      seed->identity);
            }
            report.outcomes.push_back(
                {pending.ticket, SceneMutationKind::create, std::move(result)});
        } else {
            report.outcomes.push_back({pending.ticket, SceneMutationKind::destroy,
                                       impl_->erase(std::get<EntityHandle>(pending.mutation))});
        }
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
