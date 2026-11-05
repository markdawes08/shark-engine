#include "shark/script_runtime.hpp"

#include "lua_vm.hpp"
#include "physics_runtime_access.hpp"
#include "runtime_prefab_internal.hpp"
#include "scene_script_access.hpp"
#include "shark/scene.hpp"
#include "shark/script_components.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <utility>
#include <variant>

namespace shark {
namespace {

Error runtime_error(ErrorCode code, const char* operation, const char* message) {
    return {code, message, {.operation = operation}};
}

struct ScriptRecord {
    EntityReference entity;
    Script script;
};

struct Inventory {
    SceneScope scope;
    SceneRevision revision;
    std::vector<ScriptRecord> scripts;
};

Result<Inventory> inventory(const Scene& scene, std::size_t maximum) {
    auto stats = scene.stats();
    if (!stats)
        return stats.error();
    if (stats.value().closed || stats.value().pending_mutations != 0)
        return runtime_error(ErrorCode::conflict, "script.inventory",
                             "Script runtime requires an open Scene with no pending mutations");
    SceneQueryOptions options;
    options.fields = {false, false, false, false, false, false, true};
    options.page_size = 256;
    options.scan_budget = 4096;
    options.expected_revision = SceneRevision{stats.value().instance, stats.value().revision};
    auto result = scene.query_entities(options);
    if (!result)
        return result.error();
    auto page = std::move(result).value();
    Inventory found{page.scope(), page.revision(), {}};
    for (;;) {
        for (const auto& item : page.entities()) {
            if (item.script && item.script->enabled) {
                if (found.scripts.size() == maximum)
                    return runtime_error(ErrorCode::invalid_argument, "script.inventory",
                                         "Enabled Script count exceeds the runtime limit");
                found.scripts.push_back({item.reference, *item.script});
            }
        }
        if (!page.next_cursor())
            break;
        result = scene.continue_query(*page.next_cursor());
        if (!result)
            return result.error();
        page = std::move(result).value();
    }
    std::sort(found.scripts.begin(), found.scripts.end(), [](const auto& left, const auto& right) {
        return left.entity.identity < right.entity.identity;
    });
    return found;
}

bool valid_profile(ScriptProfile profile) {
    return profile == ScriptProfile::server_gameplay ||
           profile == ScriptProfile::client_presentation;
}

bool admitted_scope(ScriptProfile profile, const SceneScope& scope) {
    if (profile == ScriptProfile::server_gameplay)
        return std::holds_alternative<RegionSceneScope>(scope);
    return std::holds_alternative<AuthoredSceneScope>(scope) ||
           std::holds_alternative<ReplicaSceneScope>(scope);
}

bool valid_delta(double delta) {
    return std::isfinite(delta) && delta >= 0 && delta <= 1;
}

detail::LuaProfile lua_profile(ScriptProfile profile) {
    return profile == ScriptProfile::server_gameplay ? detail::LuaProfile::server_gameplay
                                                     : detail::LuaProfile::client_presentation;
}

detail::LuaPhase lua_phase(ScriptPhase phase) {
    switch (phase) {
    case ScriptPhase::load:
        return detail::LuaPhase::load;
    case ScriptPhase::init:
        return detail::LuaPhase::init;
    case ScriptPhase::update:
        return detail::LuaPhase::update;
    case ScriptPhase::fixed_update:
        return detail::LuaPhase::fixed_update;
    case ScriptPhase::contact:
        return detail::LuaPhase::contact;
    case ScriptPhase::destroy:
        return detail::LuaPhase::destroy;
    }
    std::terminate();
}

Error contextualize(Error error, const ScriptRecord& record, const char* operation) {
    error.context.operation = operation;
    error.context.path = record.script.source;
    error.context.entity_id = std::visit(
        [](const auto& identity) { return identity.value().to_string(); }, record.entity.identity);
    return error;
}

} // namespace

struct ScriptRuntime::Impl {
    struct Entry {
        ScriptRecord record;
        std::unique_ptr<detail::LuaVm> vm;
        bool initialized{false};
        std::uint64_t mutation_watermark{0};
    };

    Impl(ScriptRuntimeConfig configured, SceneScope owner_scope, SceneRevision initial,
         scene_detail::ScriptLease owner_lease)
        : config(configured), scope(std::move(owner_scope)), revision(initial),
          owner(std::this_thread::get_id()), lease(std::move(owner_lease)) {}

    ~Impl() {
        if (!closed && owner != std::this_thread::get_id())
            std::terminate();
    }

    Result<void> thread(const char* operation) const {
        if (owner != std::this_thread::get_id())
            return runtime_error(ErrorCode::conflict, operation,
                                 "Script runtime belongs to its creating thread");
        return {};
    }

    Result<void> scene_identity(const Scene& scene, const char* operation) const {
        if (auto checked = scene_detail::SceneScriptAccess::check(scene, lease); !checked) {
            auto error = std::move(checked).error();
            error.context.operation = operation;
            return error;
        }
        auto stats = scene.stats();
        if (!stats)
            return stats.error();
        if (stats.value().closed || stats.value().instance != revision.scene_instance)
            return runtime_error(ErrorCode::conflict, operation,
                                 "Script runtime requires its original live Scene");
        return {};
    }

    Result<void> check_inventory(const Scene& scene, const char* operation) const {
        if (auto checked = scene_identity(scene, operation); !checked)
            return checked;
        auto captured = inventory(scene, config.max_scripts);
        if (!captured) {
            auto error = std::move(captured).error();
            error.context.operation = operation;
            return error;
        }
        if (captured.value().scope != scope || captured.value().scripts.size() != entries.size())
            return runtime_error(ErrorCode::conflict, operation,
                                 "Enabled Script inventory changed; recreate the runtime");
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& expected = entries[i].record;
            const auto current = std::find_if(
                captured.value().scripts.begin(), captured.value().scripts.end(),
                [&](const auto& candidate) { return candidate.entity == expected.entity; });
            if (current == captured.value().scripts.end() || expected.script != current->script)
                return runtime_error(ErrorCode::conflict, operation,
                                     "Script identity or settings changed; recreate the runtime");
        }
        return {};
    }

    Result<void> ready(const char* operation) const {
        if (auto checked = thread(operation); !checked)
            return checked;
        if (closed || !started || faulted)
            return runtime_error(ErrorCode::conflict, operation,
                                 "Script runtime is closed, not started or faulted");
        return {};
    }

    static bool log(void* user, std::string_view message) noexcept {
        auto& self = *static_cast<Impl*>(user);
        if (!self.active || message.empty() ||
            message.size() > script_runtime_maximum_log_message_bytes ||
            !detail::valid_utf8(message, true) || self.logs.size() >= script_runtime_maximum_logs ||
            message.size() > script_runtime_maximum_log_bytes - self.log_bytes)
            return false;
        try {
            self.logs.push_back({self.active->record.entity, self.active_phase, self.active_tick,
                                 std::string(message)});
            self.log_bytes += message.size();
            return true;
        } catch (...) {
            return false;
        }
    }

    struct Active {
        Impl& owner;
        Active(Impl& target, Entry& entry, ScriptPhase phase, std::uint64_t tick) : owner(target) {
            owner.active = &entry;
            owner.active_phase = phase;
            owner.active_tick = tick;
        }
        ~Active() {
            owner.active = nullptr;
        }
    };

    struct DeferredPhysics {
        detail::LuaPhysicsRequest request;
        PhysicsContactParticipant self;
        PhysicsContactParticipant other;
        RigidBody settings;
    };
    struct PendingEntity {
        detail::LuaEntityRequest request;
        EntityReference requester;
        std::size_t prefab_index{0};
        std::uint64_t tick{0};
        std::vector<WorldEntityId> identities{};
        std::vector<EntityReference> targets{};
        std::optional<PhysicsContactParticipant> contact_self{};
        std::optional<PhysicsContactParticipant> contact_peer{};
        std::shared_ptr<ScriptEntityMutationReceipt> receipt{};
    };
    struct EntityLedgerEntry {
        EntityReference requester;
        detail::LuaEntityRequest request;
        std::shared_ptr<ScriptEntityMutationReceipt> receipt;
    };
    using Deferred = std::variant<DeferredPhysics, std::shared_ptr<PendingEntity>>;
    struct Bridge {
        Impl* runtime;
        Scene* scene;
        PhysicsWorld* world;
        ScriptPhase phase;
        const PhysicsContactEvent* event;
    };
    std::vector<Deferred> deferred;
    bool deferred_overflow{false};
    std::weak_ptr<detail::RuntimeSpawnAuthorityState> spawn_authority;
    std::weak_ptr<const std::uint64_t> spawn_physics;
    std::vector<RuntimePrefab> prefabs;
    std::vector<ScriptSourceSnapshot> source_snapshots;
    std::vector<EntityLedgerEntry> entity_ledger;
    std::vector<std::shared_ptr<const ScriptEntityMutationReceipt>> entity_reports;
    std::vector<EntityReference> owned_roots;
    std::size_t queued_entities{0};
    std::size_t queued_scripts{0};
    bool spawning_enabled{false};

    static void uuid(char* destination, const EntityReference& ref) {
        const auto text =
            std::visit([](const auto& id) { return id.value().to_string(); }, ref.identity);
        std::memcpy(destination, text.c_str(), 37);
    }
    static math::Vec3 vector(const double* p) {
        return {p[0], p[1], p[2]};
    }
    static bool same(const PhysicsBodyState& state, const PhysicsContactParticipant& p) {
        return state.entity == p.entity && state.settings.is_trigger == p.is_trigger;
    }
    static bool same_entity_request(const detail::LuaEntityRequest& a,
                                    const detail::LuaEntityRequest& b) noexcept {
        if (a.operation != b.operation || a.sequence != b.sequence ||
            std::memcmp(a.asset_id, b.asset_id, sizeof(a.asset_id)) != 0)
            return false;
        for (std::size_t axis = 0; axis < 3; ++axis)
            if (std::bit_cast<std::uint64_t>(a.position[axis]) !=
                std::bit_cast<std::uint64_t>(b.position[axis]))
                return false;
        return true;
    }
    Result<void> authority(const Scene& scene, const char* operation) const {
        const auto* region = std::get_if<RegionSceneScope>(&scope);
        if (!spawning_enabled || config.profile != ScriptProfile::server_gameplay || !region)
            return runtime_error(ErrorCode::conflict, operation,
                                 "Runtime entity mutation has no trusted server grant");
        return detail::check_runtime_spawn_authority(spawn_authority, scene, *region, operation);
    }
    Result<scene_detail::PhysicsLease> physics_identity(PhysicsWorld& physics,
                                                        const Scene& scene) const {
        auto current = scene_detail::PhysicsRuntimeAccess::lease(physics, scene);
        if (!current)
            return current.error();
        if (spawning_enabled && spawn_physics.lock() != current.value())
            return runtime_error(ErrorCode::conflict, "script.entities.physics",
                                 "Runtime entity operations require their original Physics owner");
        return current;
    }
    static void terminal_receipt(PendingEntity& pending, ScriptEntityMutationState state,
                                 const Error* error = nullptr) noexcept {
        pending.receipt->state = state;
        if (state == ScriptEntityMutationState::applied) {
            pending.receipt->error.reset();
        } else if (error) {
            // An owned fallback diagnostic was allocated at admission. Preserve
            // it if copying the more specific post-effect diagnostic fails.
            try {
                pending.receipt->error = *error;
            } catch (...) {
            }
        }
    }
    void discard_deferred(const Error* error = nullptr, std::size_t begin = 0) noexcept {
        for (std::size_t index = begin; index < deferred.size(); ++index)
            if (auto* pending = std::get_if<std::shared_ptr<PendingEntity>>(&deferred[index]))
                terminal_receipt(**pending, ScriptEntityMutationState::discarded, error);
        deferred.clear();
        queued_entities = 0;
        queued_scripts = 0;
    }
    void begin_deferred() {
        discard_deferred();
        deferred_overflow = false;
        deferred.reserve(script_runtime_maximum_deferred_physics);
    }
    Result<void> contact_identity(Scene& scene, PhysicsWorld& physics,
                                  const PendingEntity& pending) const {
        if (!pending.contact_self)
            return {};
        auto self = physics.body_state(scene, pending.contact_self->body);
        auto peer = physics.body_state(scene, pending.contact_peer->body);
        if (!self || !peer || !same(self.value(), *pending.contact_self) ||
            !same(peer.value(), *pending.contact_peer))
            return runtime_error(ErrorCode::conflict, "script.entities.apply",
                                 "Queued contact entity/body generations changed");
        return {};
    }
    Result<void> entity_request(Bridge& bridge, const EntityReference& self,
                                const detail::LuaEntityRequest& request,
                                detail::LuaEntityReply& reply) {
        constexpr auto operation = "script.entities.request";
        if (!active || active->record.entity != self || bridge.world == nullptr ||
            (bridge.phase != ScriptPhase::update && bridge.phase != ScriptPhase::fixed_update &&
             bridge.phase != ScriptPhase::contact))
            return runtime_error(ErrorCode::conflict, operation,
                                 "Entity mutation requires an active server update callback");
        if (auto checked = authority(*bridge.scene, operation); !checked)
            return checked;
        if (auto checked = scene_identity(*bridge.scene, operation); !checked)
            return checked;
        if (auto checked = bridge.scene->resolve(self); !checked)
            return checked.error();
        if (auto checked = physics_identity(*bridge.world, *bridge.scene); !checked)
            return checked.error();
        if (request.sequence == 0 ||
            request.sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            return runtime_error(ErrorCode::invalid_argument, operation,
                                 "Entity mutation sequence must be a positive Lua integer");
        const auto retained =
            std::find_if(entity_ledger.begin(), entity_ledger.end(), [&](const auto& value) {
                return value.requester == self && value.request.sequence == request.sequence;
            });
        if (retained != entity_ledger.end()) {
            if (!same_entity_request(retained->request, request))
                return runtime_error(ErrorCode::conflict, operation,
                                     "Entity mutation sequence has a different payload");
            if (retained->receipt->state == ScriptEntityMutationState::failed ||
                retained->receipt->state == ScriptEntityMutationState::discarded)
                return *retained->receipt->error;
            if (retained->receipt->root) {
                const auto text = retained->receipt->root->value().to_string();
                std::memcpy(reply.root_id, text.c_str(), 37);
            }
            return {};
        }
        if (request.sequence != active->mutation_watermark + 1)
            return runtime_error(ErrorCode::conflict, operation,
                                 "Entity mutation sequence expired or is not the next value");
        if (deferred.size() >= script_runtime_maximum_deferred_physics ||
            entity_reports.size() >= script_runtime_maximum_entity_mutations) {
            deferred_overflow = true;
            return runtime_error(ErrorCode::conflict, operation,
                                 "Runtime mutation queue or undrained receipt queue is full");
        }
        deferred.reserve(script_runtime_maximum_deferred_physics);
        entity_reports.reserve(script_runtime_maximum_entity_mutations);
        entity_ledger.reserve(script_runtime_maximum_entity_mutations);
        auto pending = std::make_shared<PendingEntity>(PendingEntity{request, self});
        pending->tick = active_tick;
        pending->receipt = std::make_shared<ScriptEntityMutationReceipt>(
            ScriptEntityMutationReceipt{self, request.sequence,
                                        request.operation == detail::LuaEntityOperation::spawn
                                            ? ScriptEntityMutationKind::spawn
                                            : ScriptEntityMutationKind::destroy});
        pending->receipt->error = contextualize(
            runtime_error(ErrorCode::execution_failed, operation,
                          "Runtime entity mutation did not complete; inspect its partial effects"),
            active->record, operation);
        if (bridge.event) {
            pending->contact_self =
                bridge.event->first.entity == self ? bridge.event->first : bridge.event->second;
            pending->contact_peer =
                bridge.event->first.entity == self ? bridge.event->second : bridge.event->first;
            if (auto checked = contact_identity(*bridge.scene, *bridge.world, *pending); !checked)
                return checked;
        }
        if (request.operation == detail::LuaEntityOperation::spawn) {
            const auto parsed = StableId::parse(request.asset_id);
            if (!parsed)
                return parsed.error();
            const AssetId asset{parsed.value()};
            const auto found = std::find_if(prefabs.begin(), prefabs.end(), [&](const auto& value) {
                return value.asset == asset;
            });
            if (found == prefabs.end())
                return runtime_error(ErrorCode::invalid_id, operation,
                                     "Prefab asset is not in the trusted runtime catalog");
            pending->prefab_index = static_cast<std::size_t>(found - prefabs.begin());
            const auto queued_roots = static_cast<std::size_t>(
                std::count_if(deferred.begin(), deferred.end(), [](const auto& command) {
                    const auto* pending = std::get_if<std::shared_ptr<PendingEntity>>(&command);
                    return pending &&
                           (*pending)->request.operation == detail::LuaEntityOperation::spawn;
                }));
            if (found->entities.size() > runtime_prefab_maximum_entities - queued_entities ||
                queued_roots >= runtime_prefab_maximum_entities - owned_roots.size()) {
                deferred_overflow = true;
                return runtime_error(ErrorCode::conflict, operation,
                                     "Runtime prefab expansion exceeds its bounded queue");
            }
            std::size_t scripts = 0;
            std::set<StableId> used;
            for (const auto& entity : found->entities) {
                used.insert(entity.id.value());
                if (entity.script && entity.script->enabled) {
                    ++scripts;
                    if (std::none_of(source_snapshots.begin(), source_snapshots.end(),
                                     [&](const auto& source) {
                                         return source.path == entity.script->source;
                                     }))
                        return runtime_error(ErrorCode::invalid_argument, operation,
                                             "Prefab Script is missing its captured source");
                }
                if (entity.id == found->root) {
                    auto pose = entity.transform;
                    pose.translation = vector(request.position);
                    if (auto checked = canonicalize_transform(pose); !checked)
                        return checked.error();
                    if (entity.rigid_body) {
                        if (auto checked = validate_rigid_body_pose(*entity.rigid_body, pose);
                            !checked)
                            return checked.error();
                    }
                }
            }
            for (const auto& command : deferred)
                if (const auto* previous = std::get_if<std::shared_ptr<PendingEntity>>(&command))
                    for (const auto& identity : (*previous)->identities)
                        used.insert(identity.value());
            if (scripts > config.max_scripts - entries.size() ||
                queued_scripts > config.max_scripts - entries.size() - scripts)
                return runtime_error(ErrorCode::conflict, operation,
                                     "Spawned Script count exceeds the runtime capacity");
            pending->identities.reserve(found->entities.size());
            pending->receipt->created_entities.reserve(found->entities.size());
            for (const auto& entity : found->entities) {
                std::optional<WorldEntityId> identity;
                for (std::size_t attempt = 0; attempt < 8; ++attempt) {
                    auto generated = generate_stable_id();
                    if (!generated)
                        return generated.error();
                    const WorldEntityId candidate{generated.value()};
                    auto existing = bridge.scene->find(candidate);
                    if (existing)
                        continue;
                    if (existing.error().code != ErrorCode::invalid_id)
                        return existing.error();
                    if (used.insert(candidate.value()).second) {
                        identity = candidate;
                        break;
                    }
                }
                if (!identity)
                    return runtime_error(ErrorCode::conflict, operation,
                                         "Fresh runtime entity ID collision limit exceeded");
                pending->identities.push_back(*identity);
                if (entity.id == found->root)
                    pending->receipt->root = *identity;
            }
        } else {
            pending->receipt->root = std::get<WorldEntityId>(self.identity);
            std::vector<EntityHandle> handles{self.handle};
            for (std::size_t index = 0; index < handles.size(); ++index) {
                auto state = bridge.scene->entity_state(handles[index]);
                if (!state)
                    return state.error();
                if (state.value().reference.scope != self.scope ||
                    state.value().reference.authority != self.authority)
                    return runtime_error(ErrorCode::conflict, operation,
                                         "Destroy subtree crosses runtime authority");
                if (state.value().children.size() >
                    runtime_prefab_maximum_entities - handles.size())
                    return runtime_error(ErrorCode::conflict, operation,
                                         "Destroy subtree exceeds the runtime entity bound");
                handles.insert(handles.end(), state.value().children.begin(),
                               state.value().children.end());
                pending->targets.push_back(state.value().reference);
            }
            pending->receipt->removed_entities.reserve(pending->targets.size());
        }
        char root_text[37]{};
        if (pending->receipt->root) {
            const auto text = pending->receipt->root->value().to_string();
            std::memcpy(root_text, text.c_str(), 37);
        }
        if (entity_ledger.size() == script_runtime_maximum_entity_mutations)
            entity_ledger.erase(entity_ledger.begin());
        entity_ledger.push_back({self, request, pending->receipt});
        entity_reports.push_back(pending->receipt);
        deferred.push_back(pending);
        if (request.operation == detail::LuaEntityOperation::spawn) {
            const auto& prefab = prefabs[pending->prefab_index];
            queued_entities += prefab.entities.size();
            for (const auto& entity : prefab.entities)
                if (entity.script && entity.script->enabled)
                    ++queued_scripts;
        }
        active->mutation_watermark = request.sequence;
        std::memcpy(reply.root_id, root_text, sizeof(root_text));
        return {};
    }
    static void entity_call(void* user, const EntityReference& self,
                            const detail::LuaEntityRequest& request,
                            detail::LuaEntityReply& reply) noexcept {
        try {
            auto& bridge = *static_cast<Bridge*>(user);
            auto result = bridge.runtime->entity_request(bridge, self, request, reply);
            if (!result)
                std::snprintf(reply.error, sizeof(reply.error), "%s",
                              result.error().message.c_str());
        } catch (...) {
            std::snprintf(reply.error, sizeof(reply.error), "Runtime entity host failed");
        }
    }
    std::vector<Entry*> callback_order() {
        std::vector<Entry*> ordered;
        ordered.reserve(entries.size());
        for (auto& entry : entries)
            ordered.push_back(&entry);
        std::sort(ordered.begin(), ordered.end(), [](const auto* a, const auto* b) {
            return a->record.entity.identity < b->record.entity.identity;
        });
        return ordered;
    }
    Result<void> load_entry(Entry& entry, Scene& scene, PhysicsWorld& physics,
                            std::string_view source, std::uint64_t tick) {
        Active active_guard{*this, entry, ScriptPhase::load, tick};
        detail::LuaCallContext context{&scene,
                                       entry.record.entity,
                                       detail::LuaPhase::load,
                                       lua_profile(config.profile),
                                       tick,
                                       0,
                                       this,
                                       &Impl::log};
        Bridge bridge{this, &scene, &physics, ScriptPhase::load, nullptr};
        context.physics_user = &bridge;
        context.physics_call = &Impl::physics_call;
        context.entity_user = &bridge;
        context.entity_call = &Impl::entity_call;
        auto result = entry.vm->execute(source, entry.record.script.source, context);
        if (!result)
            return contextualize(std::move(result).error(), entry.record, "script.entities.spawn");
        return {};
    }
    Result<void> spawn(Scene& scene, PhysicsWorld& physics,
                       const scene_detail::PhysicsLease& physics_lease, PendingEntity& pending) {
        const auto& prefab = prefabs[pending.prefab_index];
        std::map<AuthoredEntityId, std::size_t> indices;
        std::vector<std::vector<std::size_t>> children(prefab.entities.size());
        std::vector<std::size_t> ordered;
        ordered.reserve(prefab.entities.size());
        for (std::size_t index = 0; index < prefab.entities.size(); ++index)
            indices.emplace(prefab.entities[index].id, index);
        for (std::size_t index = 0; index < prefab.entities.size(); ++index) {
            const auto& entity = prefab.entities[index];
            if (entity.parent)
                children[indices.at(*entity.parent)].push_back(index);
            else
                ordered.push_back(index);
        }
        for (std::size_t cursor = 0; cursor < ordered.size(); ++cursor) {
            const auto& found = children[ordered[cursor]];
            ordered.insert(ordered.end(), found.begin(), found.end());
        }
        struct StagedScript {
            std::size_t index;
            Script settings;
            std::unique_ptr<detail::LuaVm> vm;
        };
        std::vector<StagedScript> scripts;
        scripts.reserve(config.max_scripts - entries.size());
        // Native VM owners and all variable-sized records exist before the
        // first Scene publication. Source execution waits for complete bodies.
        for (const auto index : ordered) {
            const auto& entity = prefab.entities[index];
            if (!entity.script || !entity.script->enabled)
                continue;
            if (scripts.size() == config.max_scripts - entries.size())
                return runtime_error(ErrorCode::conflict, "script.entities.spawn",
                                     "Spawned Script capacity changed before application");
            auto vm =
                detail::LuaVm::create({config.memory_bytes_per_script, config.instruction_limit});
            if (!vm)
                return vm.error();
            scripts.push_back({index, *entity.script, std::move(vm).value()});
        }
        std::vector<std::optional<EntityHandle>> handles(prefab.entities.size());
        std::vector<std::optional<EntityReference>> references(prefab.entities.size());
        for (const auto index : ordered) {
            const auto& entity = prefab.entities[index];
            auto transform = entity.transform;
            if (entity.id == prefab.root)
                transform.translation = vector(pending.request.position);
            const auto parent = entity.parent ? handles[indices.at(*entity.parent)] : std::nullopt;
            auto created = scene_detail::SceneScriptAccess::create_entity(
                scene, lease, physics_lease,
                {pending.identities[index], std::nullopt, entity.name, transform, parent,
                 entity.camera, entity.mesh_renderer, std::nullopt, entity.script});
            if (!created)
                return created.error();
            handles[index] = created.value();
            auto reference = scene.reference(created.value());
            if (!reference)
                return reference.error();
            references[index] = reference.value();
            pending.receipt->created_entities.push_back(reference.value());
            if (entity.id == prefab.root)
                owned_roots.push_back(reference.value());
        }
        for (const auto index : ordered) {
            if (!prefab.entities[index].rigid_body)
                continue;
            auto created = physics.create_runtime_body(scene, *references[index],
                                                       *prefab.entities[index].rigid_body);
            if (!created)
                return created.error();
            ++pending.receipt->created_bodies;
        }
        for (auto& staged : scripts) {
            const auto source = std::find_if(
                source_snapshots.begin(), source_snapshots.end(),
                [&](const auto& value) { return value.path == staged.settings.source; });
            if (source == source_snapshots.end())
                return runtime_error(ErrorCode::conflict, "script.entities.spawn",
                                     "Captured Script source disappeared");
            entries.push_back({{*references[staged.index], std::move(staged.settings)},
                               std::move(staged.vm),
                               false,
                               0});
            original_count = entries.size();
            auto& entry = entries.back();
            if (auto loaded = load_entry(entry, scene, physics, source->text, pending.tick);
                !loaded)
                return loaded;
            auto initialized = run(entry, scene, ScriptPhase::init, pending.tick, 0, "on_init",
                                   "script.entities.spawn", &physics);
            if (!initialized)
                return initialized;
            entry.initialized = true;
            ++pending.receipt->initialized_scripts;
        }
        return {};
    }
    Result<void> destroy_targets(Scene& scene, PhysicsWorld& physics,
                                 const scene_detail::PhysicsLease& physics_lease,
                                 const std::vector<EntityReference>& targets,
                                 ScriptEntityMutationReceipt& receipt, bool callbacks,
                                 std::uint64_t tick) {
        constexpr auto operation = "script.entities.destroy";
        if (targets.empty())
            return runtime_error(ErrorCode::invalid_argument, operation, "Empty destroy subtree");
        std::set<EntityHandle> handles;
        for (const auto& target : targets) {
            auto resolved = scene.resolve(target);
            if (!resolved)
                return resolved.error();
            handles.insert(target.handle);
        }
        std::optional<Error> first_error;
        if (callbacks) {
            for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
                if (!handles.contains(entry->record.entity.handle) || !entry->initialized)
                    continue;
                entry->initialized = false;
                ++receipt.attempted_destroy_callbacks;
                auto result = run(*entry, scene, ScriptPhase::destroy, tick, 0, "on_destroy",
                                  operation, &physics);
                if (!result && !first_error)
                    first_error = std::move(result).error();
            }
        }
        for (auto target = targets.rbegin(); target != targets.rend(); ++target) {
            auto current = scene.entity_state(target->handle);
            if (!current)
                return current.error();
            if (!current.value().rigid_body)
                continue;
            auto body = physics.find_body(scene, *target);
            if (!body)
                return body.error();
            auto removed = physics.remove_body(scene, body.value());
            if (!removed)
                return removed;
            ++receipt.removed_bodies;
        }
        std::erase_if(entries, [&](const auto& entry) {
            if (!handles.contains(entry.record.entity.handle))
                return false;
            ++receipt.destroyed_scripts;
            return true;
        });
        original_count = entries.size();
        auto erased = scene_detail::SceneScriptAccess::destroy_entity(scene, lease, physics_lease,
                                                                      targets.front());
        if (!erased)
            return erased;
        for (const auto& target : targets)
            receipt.removed_entities.push_back(target);
        std::erase_if(owned_roots, [&](const auto& root) { return handles.contains(root.handle); });
        if (first_error)
            return std::move(*first_error);
        return {};
    }
    Result<void> apply_entity(Scene& scene, PhysicsWorld& physics, PendingEntity& pending) {
        constexpr auto operation = "script.entities.apply";
        if (auto checked = authority(scene, operation); !checked)
            return checked;
        if (auto checked = scene_identity(scene, operation); !checked)
            return checked;
        if (auto checked = scene.resolve(pending.requester); !checked)
            return checked.error();
        if (auto checked = contact_identity(scene, physics, pending); !checked)
            return checked;
        auto physics_lease = physics_identity(physics, scene);
        if (!physics_lease)
            return physics_lease.error();
        if (pending.request.operation == detail::LuaEntityOperation::spawn)
            return spawn(scene, physics, physics_lease.value(), pending);
        return destroy_targets(scene, physics, physics_lease.value(), pending.targets,
                               *pending.receipt, true, pending.tick);
    }
    Result<void> drain_deferred(Scene& scene, PhysicsWorld& physics, std::size_t& applied,
                                std::size_t& discarded, const char* operation) {
        std::size_t current = 0;
        try {
            std::vector<PhysicsBodyHandle> removed;
            removed.reserve(deferred.size());
            for (; current < deferred.size(); ++current) {
                Result<void> result;
                if (auto* structural =
                        std::get_if<std::shared_ptr<PendingEntity>>(&deferred[current])) {
                    terminal_receipt(**structural, ScriptEntityMutationState::failed);
                    result = apply_entity(scene, physics, **structural);
                    if (!result) {
                        auto native = physics.stats();
                        (*structural)->receipt->native_progress_uncertain =
                            !native || native.value().faulted;
                    }
                    terminal_receipt(**structural,
                                     result ? ScriptEntityMutationState::applied
                                            : ScriptEntityMutationState::failed,
                                     result ? nullptr : &result.error());
                } else {
                    const auto& command = std::get<DeferredPhysics>(deferred[current]);
                    auto peer = physics.body_state(scene, command.other.body);
                    if (!peer || !same(peer.value(), command.other)) {
                        result = runtime_error(ErrorCode::conflict, operation,
                                               "Deferred contact peer generation changed");
                    } else if (command.request.operation ==
                               detail::LuaPhysicsOperation::create_body) {
                        if (std::find(removed.begin(), removed.end(), command.self.body) ==
                            removed.end())
                            result =
                                runtime_error(ErrorCode::conflict, operation,
                                              "Body recreation requires a prior queued removal");
                        else {
                            auto created = physics.create_runtime_body(scene, command.self.entity,
                                                                       command.settings);
                            if (!created)
                                result = created.error();
                        }
                    } else {
                        auto self = physics.body_state(scene, command.self.body);
                        if (!self || !same(self.value(), command.self))
                            result = runtime_error(ErrorCode::conflict, operation,
                                                   "Deferred contact body generation changed");
                        else if (command.request.operation ==
                                 detail::LuaPhysicsOperation::remove_body) {
                            result = physics.remove_body(scene, command.self.body);
                            if (result)
                                removed.push_back(command.self.body);
                        } else
                            result = mutate(scene, physics, command.self.body, command.request);
                    }
                    if (!result) {
                        const auto entry =
                            std::find_if(entries.begin(), entries.end(), [&](const auto& value) {
                                return value.record.entity == command.self.entity;
                            });
                        if (entry != entries.end())
                            result =
                                contextualize(std::move(result).error(), entry->record, operation);
                    }
                }
                if (!result) {
                    faulted = true;
                    discarded = deferred.size() - current;
                    auto error = std::move(result).error();
                    discard_deferred(&error, current + 1);
                    return error;
                }
                ++applied;
            }
            deferred.clear();
            queued_entities = 0;
            queued_scripts = 0;
            return {};
        } catch (...) {
            faulted = true;
            discarded = deferred.size() - current;
            if (current < deferred.size())
                if (auto* structural =
                        std::get_if<std::shared_ptr<PendingEntity>>(&deferred[current])) {
                    auto native = physics.stats();
                    (*structural)->receipt->native_progress_uncertain =
                        !native || native.value().faulted;
                }
            auto error =
                runtime_error(ErrorCode::execution_failed, operation,
                              "Deferred mutation failed; inspect owned partial-effect receipts");
            discard_deferred(&error, current + 1);
            return error;
        }
    }
    static Result<void> mutate(Scene& scene, PhysicsWorld& world, PhysicsBodyHandle body,
                               const detail::LuaPhysicsRequest& request) {
        using Op = detail::LuaPhysicsOperation;
        switch (request.operation) {
        case Op::set_velocity:
            return world.set_velocity(scene, body,
                                      {vector(request.arguments), vector(request.arguments + 3)});
        case Op::force:
            return world.apply_force(scene, body, vector(request.arguments));
        case Op::impulse:
            return world.apply_impulse(scene, body, vector(request.arguments));
        case Op::teleport:
        case Op::kinematic_target: {
            auto state = world.body_state(scene, body);
            if (!state)
                return state.error();
            auto pose = state.value().pose;
            pose.translation = vector(request.arguments);
            return request.operation == Op::teleport
                       ? world.teleport(scene, body, pose)
                       : world.set_kinematic_target(scene, body, pose);
        }
        default:
            return runtime_error(ErrorCode::invalid_argument, "script.physics",
                                 "Invalid physics mutation");
        }
    }
    Result<void> physics(Bridge& b, const EntityReference& self,
                         const detail::LuaPhysicsRequest& request, detail::LuaPhysicsReply& reply) {
        using Op = detail::LuaPhysicsOperation;
        if (!active || active->record.entity != self)
            return runtime_error(ErrorCode::conflict, "script.physics",
                                 "Physics call has no active Script owner");
        const auto op = request.operation;
        const PhysicsQueryFilter filter{request.layer, request.mask, request.include_triggers, {}};
        if (op == Op::raycast || op == Op::overlap_sphere || op == Op::overlap_box) {
            if (request.max_hits == 0 || request.max_hits > detail::lua_physics_maximum_hits)
                return runtime_error(ErrorCode::invalid_argument, "script.physics",
                                     "Invalid physics query limit");
            if (op == Op::raycast) {
                auto result =
                    b.world->raycast(*b.scene, vector(request.arguments),
                                     vector(request.arguments + 3), filter, request.max_hits);
                if (!result)
                    return result.error();
                for (const auto& hit : result.value().hits) {
                    auto& out = reply.hits[reply.count++];
                    uuid(out.entity_id, hit.entity);
                    out.is_trigger = hit.is_trigger;
                    out.fraction = hit.fraction;
                    out.position[0] = hit.position.x;
                    out.position[1] = hit.position.y;
                    out.position[2] = hit.position.z;
                    if (hit.normal) {
                        out.has_normal = true;
                        out.normal[0] = hit.normal->x;
                        out.normal[1] = hit.normal->y;
                        out.normal[2] = hit.normal->z;
                    }
                }
            } else {
                auto result =
                    op == Op::overlap_sphere
                        ? b.world->overlap_sphere(*b.scene, vector(request.arguments),
                                                  request.arguments[3], filter, request.max_hits)
                        : b.world->overlap_box(*b.scene, vector(request.arguments),
                                               vector(request.arguments + 3), filter,
                                               request.max_hits);
                if (!result)
                    return result.error();
                for (const auto& hit : result.value().hits) {
                    auto& out = reply.hits[reply.count++];
                    uuid(out.entity_id, hit.entity);
                    out.is_trigger = hit.is_trigger;
                }
            }
            return {};
        }
        auto body = b.world->find_body(*b.scene, self);
        if (!body)
            return body.error();
        if (op == Op::velocity) {
            auto v = b.world->velocity(*b.scene, body.value());
            if (!v)
                return v.error();
            reply.values[0] = v.value().linear.x;
            reply.values[1] = v.value().linear.y;
            reply.values[2] = v.value().linear.z;
            reply.values[3] = v.value().angular.x;
            reply.values[4] = v.value().angular.y;
            reply.values[5] = v.value().angular.z;
            return {};
        }
        if (config.profile != ScriptProfile::server_gameplay ||
            !std::holds_alternative<RegionSceneScope>(scope) ||
            (b.phase != ScriptPhase::fixed_update && b.phase != ScriptPhase::contact))
            return runtime_error(ErrorCode::conflict, "script.physics",
                                 "Physics mutation requires server fixed or contact authority");
        if (b.phase == ScriptPhase::contact) {
            if (!b.event || deferred.size() >= script_runtime_maximum_deferred_physics) {
                deferred_overflow = true;
                return runtime_error(ErrorCode::conflict, "script.physics",
                                     "Deferred physics queue is full or unavailable");
            }
            const auto& a = b.event->first.entity == self ? b.event->first : b.event->second;
            const auto& other = b.event->first.entity == self ? b.event->second : b.event->first;
            if (a.entity != self || a.body != body.value())
                return runtime_error(ErrorCode::conflict, "script.physics",
                                     "Contact participant changed");
            auto state = b.world->body_state(*b.scene, a.body);
            auto peer = b.world->body_state(*b.scene, other.body);
            if (!state || !peer || !same(state.value(), a) || !same(peer.value(), other))
                return runtime_error(ErrorCode::conflict, "script.physics",
                                     "Contact body generation changed");
            deferred.push_back(DeferredPhysics{request, a, other, state.value().settings});
            return {};
        }
        if (op == Op::remove_body || op == Op::create_body)
            return runtime_error(ErrorCode::conflict, "script.physics",
                                 "Body changes require a contact callback");
        return mutate(*b.scene, *b.world, body.value(), request);
    }
    static void physics_call(void* user, const EntityReference& self,
                             const detail::LuaPhysicsRequest& request,
                             detail::LuaPhysicsReply& reply) noexcept {
        try {
            auto& bridge = *static_cast<Bridge*>(user);
            auto result = bridge.runtime->physics(bridge, self, request, reply);
            if (!result) {
                const auto& message = result.error().message;
                std::snprintf(reply.error, sizeof(reply.error), "%s",
                              message.empty() ? "Physics request failed" : message.c_str());
            }
        } catch (...) {
            std::snprintf(reply.error, sizeof(reply.error), "Physics host failed");
        }
    }

    Result<void> run(Entry& entry, Scene& scene, ScriptPhase phase, std::uint64_t tick,
                     double delta, std::string_view callback, const char* operation,
                     PhysicsWorld* physics = nullptr,
                     const detail::LuaContactInfo* contact = nullptr,
                     const PhysicsContactEvent* event = nullptr) {
        Active active_guard{*this, entry, phase, tick};
        detail::LuaCallContext context{
            &scene, entry.record.entity, lua_phase(phase), lua_profile(config.profile), tick, delta,
            this,   &Impl::log};
        Bridge bridge{this, &scene, physics, phase, event};
        if (physics) {
            context.physics_user = &bridge;
            context.physics_call = &Impl::physics_call;
        }
        context.contact = contact;
        context.entity_user = &bridge;
        context.entity_call = &Impl::entity_call;
        auto result = entry.vm->invoke(callback, context);
        if (!result)
            return contextualize(std::move(result).error(), entry.record, operation);
        return {};
    }

    ScriptRuntimeConfig config;
    SceneScope scope;
    SceneRevision revision;
    std::thread::id owner;
    scene_detail::ScriptLease lease;
    std::vector<Entry> entries;
    std::size_t original_count{0};
    std::vector<ScriptLogEntry> logs;
    std::size_t log_bytes{0};
    Entry* active{nullptr};
    ScriptPhase active_phase{ScriptPhase::load};
    std::uint64_t active_tick{0};
    std::optional<std::uint64_t> last_tick;
    bool started{false};
    bool faulted{false};
    bool closed{false};
};

ScriptRuntime::ScriptRuntime(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
ScriptRuntime::~ScriptRuntime() = default;

Result<std::unique_ptr<ScriptRuntime>>
ScriptRuntime::create(Scene& scene, std::span<const ScriptSourceSnapshot> sources,
                      ScriptRuntimeConfig config) {
    constexpr auto operation = "script.create";
    try {
        if (!valid_profile(config.profile) || config.max_scripts == 0 ||
            config.max_scripts > script_runtime_maximum_scripts ||
            config.memory_bytes_per_script < 64 * 1024 ||
            config.memory_bytes_per_script > script_runtime_maximum_vm_bytes ||
            config.max_scripts > script_runtime_maximum_vm_bytes / config.memory_bytes_per_script ||
            config.instruction_limit == 0 ||
            config.instruction_limit > script_runtime_maximum_instructions ||
            config.max_scripts > script_runtime_maximum_instructions / config.instruction_limit ||
            sources.size() > script_runtime_maximum_sources)
            return runtime_error(ErrorCode::invalid_argument, operation,
                                 "Invalid Script runtime resource limits");
        std::map<std::string_view, std::string_view> captured_sources;
        std::size_t total_source_bytes = 0;
        for (const auto& source : sources) {
            if (source.path.size() > script_maximum_source_path_bytes)
                return runtime_error(ErrorCode::invalid_argument, operation,
                                     "Captured Script path exceeds its byte bound");
            auto path = validate_script(Script{source.path, true});
            if (!path)
                return std::move(path).error();
            if (source.text.size() > script_runtime_maximum_source_bytes ||
                source.text.size() >
                    script_runtime_maximum_total_source_bytes - total_source_bytes ||
                source.text.find('\0') != std::string::npos || !detail::valid_utf8(source.text))
                return runtime_error(ErrorCode::invalid_argument, operation,
                                     "Invalid or oversized captured Script source");
            total_source_bytes += source.text.size();
            if (!captured_sources.emplace(source.path, source.text).second)
                return runtime_error(ErrorCode::invalid_argument, operation,
                                     "Duplicate captured Script source path");
        }
        auto current = inventory(scene, config.max_scripts);
        if (!current)
            return current.error();
        if (!admitted_scope(config.profile, current.value().scope))
            return runtime_error(ErrorCode::conflict, operation,
                                 "Script profile is not permitted for this Scene scope");
        auto lease = std::make_shared<const std::uint64_t>(current.value().revision.scene_instance);
        if (auto bound = scene_detail::SceneScriptAccess::bind(scene, lease); !bound) {
            auto error = std::move(bound).error();
            error.context.operation = operation;
            return error;
        }
        auto impl =
            std::make_unique<Impl>(config, current.value().scope, current.value().revision, lease);
        impl->entries.reserve(config.max_scripts);
        impl->source_snapshots.assign(sources.begin(), sources.end());
        impl->original_count = current.value().scripts.size();
        for (auto& record : current.value().scripts) {
            const auto source = captured_sources.find(record.script.source);
            if (source == captured_sources.end()) {
                auto error = runtime_error(ErrorCode::invalid_argument, operation,
                                           "Enabled Script is missing its captured source");
                return contextualize(std::move(error), record, operation);
            }
            auto vm =
                detail::LuaVm::create({config.memory_bytes_per_script, config.instruction_limit});
            if (!vm)
                return contextualize(std::move(vm).error(), record, operation);
            impl->entries.push_back({std::move(record), std::move(vm).value(), false});
            auto& entry = impl->entries.back();
            Impl::Active active_guard{*impl, entry, ScriptPhase::load, 0};
            detail::LuaCallContext context{&scene,
                                           entry.record.entity,
                                           detail::LuaPhase::load,
                                           lua_profile(config.profile),
                                           0,
                                           0,
                                           impl.get(),
                                           &Impl::log};
            auto loaded = entry.vm->execute(source->second, entry.record.script.source, context);
            if (!loaded)
                return contextualize(std::move(loaded).error(), entry.record, operation);
        }
        return std::unique_ptr<ScriptRuntime>(new ScriptRuntime(std::move(impl)));
    } catch (...) {
        return runtime_error(ErrorCode::execution_failed, operation,
                             "Cannot allocate or initialize the Script runtime");
    }
}

Result<void> ScriptRuntime::start(Scene& scene) {
    return start_impl(scene, nullptr);
}

Result<void> ScriptRuntime::enable_spawning(Scene& scene, PhysicsWorld& physics,
                                            RuntimeSpawnAuthority& authority,
                                            std::span<const RuntimePrefab> prefabs) {
    constexpr auto operation = "script.spawning.enable";
    if (auto checked = impl_->thread(operation); !checked)
        return checked;
    if (impl_->closed || impl_->started || impl_->faulted || impl_->spawning_enabled ||
        impl_->config.profile != ScriptProfile::server_gameplay ||
        !std::holds_alternative<RegionSceneScope>(impl_->scope) ||
        prefabs.size() > runtime_prefab_maximum_prefabs)
        return runtime_error(ErrorCode::conflict, operation,
                             "Spawning needs one bounded trusted server catalog before start");
    try {
        if (auto checked = impl_->check_inventory(scene, operation); !checked)
            return checked;
        const auto& region = std::get<RegionSceneScope>(impl_->scope);
        std::weak_ptr<detail::RuntimeSpawnAuthorityState> grant = authority.state_;
        if (auto checked = detail::check_runtime_spawn_authority(grant, scene, region, operation);
            !checked)
            return checked;
        auto physics_lease = scene_detail::PhysicsRuntimeAccess::lease(physics, scene);
        if (!physics_lease)
            return physics_lease.error();
        std::vector<RuntimePrefab> captured;
        captured.reserve(prefabs.size());
        std::set<AssetId> assets;
        std::size_t entities = 0;
        std::size_t bytes = captured.capacity() * sizeof(RuntimePrefab);
        const auto charge = [&](std::size_t count) -> bool {
            if (count > runtime_prefab_maximum_decoded_bytes - bytes)
                return false;
            bytes += count;
            return true;
        };
        for (const auto& source : prefabs) {
            if (source.entities.size() > runtime_prefab_maximum_entities - entities ||
                !assets.insert(source.asset).second)
                return runtime_error(ErrorCode::invalid_argument, operation,
                                     "Runtime prefab catalog entity/identity bound exceeded");
            auto valid = validate_runtime_prefab(source);
            if (!valid)
                return valid.error();
            auto prefab = std::move(valid).value();
            if (prefab.entities.capacity() >
                    runtime_prefab_maximum_decoded_bytes / sizeof(PreparedEntity) ||
                !charge(prefab.entities.capacity() * sizeof(PreparedEntity)))
                return runtime_error(ErrorCode::invalid_argument, operation,
                                     "Runtime prefab catalog decoded byte bound exceeded");
            for (const auto& entity : prefab.entities) {
                if (!charge(entity.name.value.capacity() + 1) ||
                    (entity.script && !charge(entity.script->source.capacity() + 1)))
                    return runtime_error(ErrorCode::invalid_argument, operation,
                                         "Runtime prefab catalog text byte bound exceeded");
                if (entity.script && entity.script->enabled &&
                    std::none_of(impl_->source_snapshots.begin(), impl_->source_snapshots.end(),
                                 [&](const auto& snapshot) {
                                     return snapshot.path == entity.script->source;
                                 }))
                    return runtime_error(ErrorCode::invalid_argument, operation,
                                         "Enabled prefab Script lacks a captured source");
            }
            entities += prefab.entities.size();
            captured.push_back(std::move(prefab));
        }
        impl_->deferred.reserve(script_runtime_maximum_deferred_physics);
        impl_->entity_ledger.reserve(script_runtime_maximum_entity_mutations);
        impl_->entity_reports.reserve(script_runtime_maximum_entity_mutations);
        impl_->owned_roots.reserve(runtime_prefab_maximum_entities);
        impl_->prefabs = std::move(captured);
        impl_->spawn_authority = std::move(grant);
        impl_->spawn_physics = physics_lease.value();
        impl_->spawning_enabled = true;
        return {};
    } catch (...) {
        return runtime_error(ErrorCode::execution_failed, operation,
                             "Cannot capture the runtime spawning catalog");
    }
}
Result<void> ScriptRuntime::start(Scene& scene, PhysicsWorld& physics) {
    return start_impl(scene, &physics);
}
Result<void> ScriptRuntime::start_impl(Scene& scene, PhysicsWorld* physics) {
    constexpr auto operation = "script.start";
    if (auto checked = impl_->thread(operation); !checked)
        return checked;
    if (impl_->closed || impl_->started || impl_->faulted)
        return runtime_error(ErrorCode::conflict, operation, "Script runtime cannot start");
    try {
        if (auto checked = impl_->check_inventory(scene, operation); !checked)
            return checked;
        if (impl_->spawning_enabled) {
            if (auto checked = impl_->authority(scene, operation); !checked)
                return checked;
            if (physics == nullptr)
                return runtime_error(ErrorCode::conflict, operation,
                                     "Spawning runtime needs its supplied Physics owner");
            if (auto checked = impl_->physics_identity(*physics, scene); !checked)
                return checked.error();
        }
        impl_->started = true;
        for (auto& entry : impl_->entries) {
            auto result =
                impl_->run(entry, scene, ScriptPhase::init, 0, 0, "on_init", operation, physics);
            if (!result) {
                impl_->faulted = true;
                return result;
            }
            entry.initialized = true;
        }
        return {};
    } catch (...) {
        impl_->faulted = true;
        return runtime_error(ErrorCode::execution_failed, operation,
                             "Cannot complete Script initialization");
    }
}

Result<void> ScriptRuntime::update(Scene& scene, double delta_seconds) {
    return update_impl(scene, nullptr, delta_seconds);
}
Result<void> ScriptRuntime::update(Scene& scene, PhysicsWorld& physics, double delta_seconds) {
    return update_impl(scene, &physics, delta_seconds);
}
Result<void> ScriptRuntime::update_impl(Scene& scene, PhysicsWorld* physics, double delta_seconds) {
    constexpr auto operation = "script.update";
    if (auto checked = impl_->ready(operation); !checked)
        return checked;
    if (!valid_delta(delta_seconds))
        return runtime_error(ErrorCode::invalid_argument, operation, "Invalid Script delta");
    try {
        if (auto checked = impl_->check_inventory(scene, operation); !checked) {
            impl_->faulted = true;
            return checked;
        }
        impl_->begin_deferred();
        if (impl_->spawning_enabled) {
            if (auto checked = impl_->authority(scene, operation); !checked)
                return checked;
            if (physics == nullptr)
                return runtime_error(ErrorCode::conflict, operation,
                                     "Spawning runtime needs its supplied Physics owner");
            if (auto checked = impl_->physics_identity(*physics, scene); !checked)
                return checked.error();
        }
        for (auto* pointer : impl_->callback_order()) {
            auto& entry = *pointer;
            auto result =
                impl_->run(entry, scene, ScriptPhase::update, impl_->last_tick.value_or(0),
                           delta_seconds, "on_update", operation, physics);
            if (!result || impl_->deferred_overflow) {
                impl_->faulted = true;
                auto error = !result ? std::move(result).error()
                                     : runtime_error(ErrorCode::conflict, operation,
                                                     "Runtime deferred mutation queue overflow");
                impl_->discard_deferred(&error);
                return error;
            }
        }
        if (physics) {
            std::size_t applied = 0;
            std::size_t discarded = 0;
            return impl_->drain_deferred(scene, *physics, applied, discarded, operation);
        }
        return {};
    } catch (...) {
        impl_->faulted = true;
        impl_->discard_deferred();
        return runtime_error(ErrorCode::execution_failed, operation,
                             "Cannot complete Script update");
    }
}

Result<void> ScriptRuntime::fixed_update(Scene& scene, std::uint64_t tick, double delta_seconds) {
    return fixed_update_impl(scene, nullptr, tick, delta_seconds);
}
Result<void> ScriptRuntime::fixed_update(Scene& scene, PhysicsWorld& physics, std::uint64_t tick,
                                         double delta_seconds) {
    return fixed_update_impl(scene, &physics, tick, delta_seconds);
}
Result<void> ScriptRuntime::fixed_update_impl(Scene& scene, PhysicsWorld* physics,
                                              std::uint64_t tick, double delta_seconds) {
    constexpr auto operation = "script.fixed_update";
    if (auto checked = impl_->ready(operation); !checked)
        return checked;
    constexpr auto max_tick = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (!valid_delta(delta_seconds) || tick > max_tick ||
        (impl_->last_tick && (*impl_->last_tick == max_tick || tick != *impl_->last_tick + 1)))
        return runtime_error(ErrorCode::invalid_argument, operation,
                             "Invalid Script delta or nonconsecutive tick");
    try {
        if (auto checked = impl_->check_inventory(scene, operation); !checked) {
            impl_->faulted = true;
            return checked;
        }
        impl_->begin_deferred();
        if (impl_->spawning_enabled) {
            if (auto checked = impl_->authority(scene, operation); !checked)
                return checked;
            if (physics == nullptr)
                return runtime_error(ErrorCode::conflict, operation,
                                     "Spawning runtime needs its supplied Physics owner");
            if (auto checked = impl_->physics_identity(*physics, scene); !checked)
                return checked.error();
        }
        for (auto* pointer : impl_->callback_order()) {
            auto& entry = *pointer;
            auto result = impl_->run(entry, scene, ScriptPhase::fixed_update, tick, delta_seconds,
                                     "on_fixed_update", operation, physics);
            if (!result || impl_->deferred_overflow) {
                impl_->faulted = true;
                auto error = !result ? std::move(result).error()
                                     : runtime_error(ErrorCode::conflict, operation,
                                                     "Runtime deferred mutation queue overflow");
                impl_->discard_deferred(&error);
                return error;
            }
        }
        if (physics) {
            std::size_t applied = 0;
            std::size_t discarded = 0;
            if (auto drained =
                    impl_->drain_deferred(scene, *physics, applied, discarded, operation);
                !drained)
                return drained;
        }
        impl_->last_tick = tick;
        return {};
    } catch (...) {
        impl_->faulted = true;
        impl_->discard_deferred();
        return runtime_error(ErrorCode::execution_failed, operation,
                             "Cannot complete Script fixed update");
    }
}

Result<ScriptContactReport> ScriptRuntime::dispatch_contacts(Scene& scene, PhysicsWorld& physics,
                                                             std::size_t maximum) {
    constexpr auto operation = "script.contacts.dispatch";
    if (auto checked = impl_->ready(operation); !checked)
        return checked.error();
    if (maximum == 0 || maximum > script_runtime_maximum_contact_events)
        return runtime_error(ErrorCode::invalid_argument, operation, "Invalid contact batch limit");
    if (auto checked = impl_->check_inventory(scene, operation); !checked)
        return checked.error();
    if (impl_->spawning_enabled) {
        if (auto checked = impl_->authority(scene, operation); !checked)
            return checked.error();
        if (auto checked = impl_->physics_identity(physics, scene); !checked)
            return checked.error();
    }
    auto stats = physics.stats();
    if (!stats)
        return stats.error();
    if (stats.value().closed || stats.value().faulted || !stats.value().simulating)
        return runtime_error(ErrorCode::conflict, operation, "Physics world is not simulating");
    try {
        impl_->begin_deferred();
    } catch (...) {
        return runtime_error(ErrorCode::execution_failed, operation,
                             "Cannot allocate contact queue");
    }
    auto events = physics.take_events(scene, maximum);
    if (!events)
        return events.error();
    ScriptContactReport report;
    report.events = std::move(events).value();
    std::size_t current = 0;
    try {
        for (; current < report.events.size(); ++current) {
            const auto& event = report.events[current];
            auto first = physics.body_state(scene, event.first.body);
            auto second = physics.body_state(scene, event.second.body);
            if (event.completed_tick > stats.value().completed_ticks || !first || !second ||
                !Impl::same(first.value(), event.first) ||
                !Impl::same(second.value(), event.second)) {
                ++report.skipped_events;
                continue;
            }
            for (int side = 0; side < 2; ++side) {
                const auto& self = side == 0 ? event.first : event.second;
                const auto& other = side == 0 ? event.second : event.first;
                auto entry = std::find_if(
                    impl_->entries.begin(), impl_->entries.end(), [&](const auto& candidate) {
                        return candidate.initialized && candidate.record.entity == self.entity;
                    });
                if (entry == impl_->entries.end())
                    continue;
                detail::LuaContactInfo info{};
                Impl::uuid(info.other_entity_id, other.entity);
                info.self_trigger = self.is_trigger;
                info.other_trigger = other.is_trigger;
                info.completed_tick = event.completed_tick;
                ++report.attempted_callbacks;
                auto result =
                    impl_->run(*entry, scene, ScriptPhase::contact, event.completed_tick, 0,
                               event.phase == PhysicsContactPhase::enter ? "on_contact_enter"
                                                                         : "on_contact_exit",
                               operation, &physics, &info, &event);
                if (!result || impl_->deferred_overflow) {
                    impl_->faulted = true;
                    report.error =
                        !result ? std::move(result).error()
                                : contextualize(runtime_error(ErrorCode::conflict, operation,
                                                              "Deferred physics queue overflow"),
                                                entry->record, operation);
                    report.undelivered_events = report.events.size() - current - 1;
                    report.discarded_commands = impl_->deferred.size();
                    impl_->discard_deferred(&*report.error);
                    return report;
                }
            }
        }
        auto drained = impl_->drain_deferred(scene, physics, report.applied_commands,
                                             report.discarded_commands, operation);
        if (!drained)
            report.error = std::move(drained).error();
        return report;
    } catch (...) {
        impl_->faulted = true;
        report.error = runtime_error(ErrorCode::execution_failed, operation,
                                     "Contact dispatch failed after consuming observations");
        report.undelivered_events = report.events.size() - current;
        report.discarded_commands = impl_->deferred.size() - report.applied_commands;
        impl_->discard_deferred(&*report.error);
        return report;
    }
}

Result<ScriptStopReport> ScriptRuntime::stop(Scene& scene) {
    return stop_impl(scene, nullptr);
}
Result<ScriptStopReport> ScriptRuntime::stop(Scene& scene, PhysicsWorld& physics) {
    return stop_impl(scene, &physics);
}
Result<ScriptStopReport> ScriptRuntime::stop_impl(Scene& scene, PhysicsWorld* physics) {
    constexpr auto operation = "script.stop";
    if (auto checked = impl_->thread(operation); !checked)
        return checked.error();
    if (impl_->closed)
        return ScriptStopReport{};
    try {
        if (auto checked = impl_->scene_identity(scene, operation); !checked)
            return checked.error();
        if (!impl_->owned_roots.empty()) {
            if (physics == nullptr)
                return runtime_error(ErrorCode::conflict, operation,
                                     "Spawned actors need their Physics owner for explicit stop");
            if (auto checked = impl_->physics_identity(*physics, scene); !checked)
                return checked.error();
        }
        ScriptStopReport report;
        report.errors.reserve(impl_->entries.size() + 1);
        for (auto it = impl_->entries.rbegin(); it != impl_->entries.rend(); ++it) {
            if (!it->initialized)
                continue;
            auto handle = scene.resolve(it->record.entity);
            if (!handle) {
                ++report.discarded;
                report.errors.push_back(
                    contextualize(std::move(handle).error(), it->record, operation));
                it->initialized = false;
                continue;
            }
            auto current = scene.entity_state(handle.value());
            if (!current) {
                ++report.discarded;
                report.errors.push_back(
                    contextualize(std::move(current).error(), it->record, operation));
                it->initialized = false;
                continue;
            }
            if (current.value().script != it->record.script) {
                ++report.discarded;
                report.errors.push_back(
                    contextualize(runtime_error(ErrorCode::conflict, operation,
                                                "Script settings changed before destruction"),
                                  it->record, operation));
                it->initialized = false;
                continue;
            }
            ++report.attempted;
            // Never retry a callback if materializing its outcome fails after
            // it may already have produced effects.
            it->initialized = false;
            auto result = impl_->run(*it, scene, ScriptPhase::destroy, impl_->last_tick.value_or(0),
                                     0, "on_destroy", operation, physics);
            if (!result)
                report.errors.push_back(std::move(result).error());
        }
        // Revoking gameplay admission does not revoke cleanup of this runtime's
        // own ephemeral roots. Matching Scene/Physics leases remain mandatory.
        while (!impl_->owned_roots.empty()) {
            const auto root = impl_->owned_roots.back();
            std::vector<EntityReference> targets;
            std::vector<EntityHandle> pending{root.handle};
            for (std::size_t index = 0; index < pending.size(); ++index) {
                auto state = scene.entity_state(pending[index]);
                if (!state) {
                    report.errors.push_back(std::move(state).error());
                    break;
                }
                if (state.value().children.size() >
                    runtime_prefab_maximum_entities - pending.size()) {
                    report.errors.push_back(runtime_error(
                        ErrorCode::conflict, operation, "Spawn cleanup subtree exceeds its bound"));
                    break;
                }
                targets.push_back(state.value().reference);
                pending.insert(pending.end(), state.value().children.begin(),
                               state.value().children.end());
            }
            if (targets.size() != pending.size())
                break;
            ScriptEntityMutationReceipt cleanup{root, 0, ScriptEntityMutationKind::destroy};
            cleanup.removed_entities.reserve(targets.size());
            auto physics_lease = impl_->physics_identity(*physics, scene);
            if (!physics_lease) {
                report.errors.push_back(std::move(physics_lease).error());
                break;
            }
            auto removed = impl_->destroy_targets(scene, *physics, physics_lease.value(), targets,
                                                  cleanup, false, impl_->last_tick.value_or(0));
            report.removed_spawned_entities += cleanup.removed_entities.size();
            report.removed_spawned_bodies += cleanup.removed_bodies;
            if (!removed) {
                report.errors.push_back(std::move(removed).error());
                break;
            }
        }
        report.remaining_spawned_roots = impl_->owned_roots.size();
        impl_->discard_deferred();
        impl_->entries.clear();
        impl_->lease.reset();
        impl_->closed = true;
        return report;
    } catch (...) {
        impl_->faulted = true;
        return runtime_error(
            ErrorCode::execution_failed, operation,
            "Script teardown could not complete; use close to discard remaining callbacks");
    }
}

Result<std::vector<ScriptLogEntry>> ScriptRuntime::take_logs() {
    constexpr auto operation = "script.logs.take";
    if (auto checked = impl_->thread(operation); !checked)
        return checked.error();
    std::vector<ScriptLogEntry> result;
    result.swap(impl_->logs);
    impl_->log_bytes = 0;
    return result;
}

Result<ScriptEntityMutationReport> ScriptRuntime::take_entity_mutations() {
    constexpr auto operation = "script.entities.take";
    if (auto checked = impl_->thread(operation); !checked)
        return checked.error();
    if (impl_->active != nullptr)
        return runtime_error(ErrorCode::conflict, operation,
                             "Entity receipts cannot be taken during a Script callback");
    ScriptEntityMutationReport report;
    report.outcomes.swap(impl_->entity_reports);
    return report;
}

Result<ScriptRuntimeStats> ScriptRuntime::stats() const {
    constexpr auto operation = "script.stats";
    if (auto checked = impl_->thread(operation); !checked)
        return checked.error();
    const auto initialized = static_cast<std::size_t>(
        std::count_if(impl_->entries.begin(), impl_->entries.end(),
                      [](const auto& entry) { return entry.initialized; }));
    return ScriptRuntimeStats{impl_->config.profile, impl_->revision,    impl_->original_count,
                              initialized,           impl_->logs.size(), impl_->last_tick,
                              impl_->started,        impl_->faulted,     impl_->closed};
}

Result<std::size_t> ScriptRuntime::close() {
    constexpr auto operation = "script.close";
    if (auto checked = impl_->thread(operation); !checked)
        return checked.error();
    if (impl_->closed)
        return std::size_t{0};
    const auto discarded = static_cast<std::size_t>(
        std::count_if(impl_->entries.begin(), impl_->entries.end(),
                      [](const auto& entry) { return entry.initialized; }));
    impl_->entries.clear();
    impl_->discard_deferred();
    impl_->lease.reset();
    impl_->closed = true;
    return discarded;
}

} // namespace shark
