#include "shark/script_runtime.hpp"

#include "lua_vm.hpp"
#include "scene_script_access.hpp"
#include "shark/scene.hpp"
#include "shark/script_components.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <thread>
#include <utility>

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
            const auto& current = captured.value().scripts[i];
            if (expected.entity != current.entity || expected.script != current.script)
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

    struct Deferred {
        detail::LuaPhysicsRequest request;
        PhysicsContactParticipant self;
        PhysicsContactParticipant other;
        RigidBody settings;
    };
    struct Bridge {
        Impl* runtime;
        Scene* scene;
        PhysicsWorld* world;
        ScriptPhase phase;
        const PhysicsContactEvent* event;
    };
    std::vector<Deferred> deferred;
    bool deferred_overflow{false};

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
            deferred.push_back({request, a, other, state.value().settings});
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
        impl->entries.reserve(current.value().scripts.size());
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
        for (auto& entry : impl_->entries) {
            auto result =
                impl_->run(entry, scene, ScriptPhase::update, impl_->last_tick.value_or(0),
                           delta_seconds, "on_update", operation, physics);
            if (!result) {
                impl_->faulted = true;
                return result;
            }
        }
        return {};
    } catch (...) {
        impl_->faulted = true;
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
        for (auto& entry : impl_->entries) {
            auto result = impl_->run(entry, scene, ScriptPhase::fixed_update, tick, delta_seconds,
                                     "on_fixed_update", operation, physics);
            if (!result) {
                impl_->faulted = true;
                return result;
            }
        }
        impl_->last_tick = tick;
        return {};
    } catch (...) {
        impl_->faulted = true;
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
    auto stats = physics.stats();
    if (!stats)
        return stats.error();
    if (stats.value().closed || stats.value().faulted || !stats.value().simulating)
        return runtime_error(ErrorCode::conflict, operation, "Physics world is not simulating");
    try {
        impl_->deferred.clear();
        impl_->deferred_overflow = false;
        impl_->deferred.reserve(script_runtime_maximum_deferred_physics);
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
                    impl_->deferred.clear();
                    return report;
                }
            }
        }
        std::vector<PhysicsBodyHandle> removed;
        removed.reserve(impl_->deferred.size());
        for (std::size_t i = 0; i < impl_->deferred.size(); ++i) {
            const auto& command = impl_->deferred[i];
            auto peer = physics.body_state(scene, command.other.body);
            Result<void> result;
            if (!peer || !Impl::same(peer.value(), command.other)) {
                result = runtime_error(ErrorCode::conflict, operation,
                                       "Deferred contact peer generation changed");
            } else if (command.request.operation == detail::LuaPhysicsOperation::create_body) {
                if (std::find(removed.begin(), removed.end(), command.self.body) == removed.end())
                    result = runtime_error(ErrorCode::conflict, operation,
                                           "Body recreation requires a prior queued removal");
                else {
                    auto created =
                        physics.create_runtime_body(scene, command.self.entity, command.settings);
                    if (!created)
                        result = created.error();
                }
            } else {
                auto self = physics.body_state(scene, command.self.body);
                if (!self || !Impl::same(self.value(), command.self))
                    result = runtime_error(ErrorCode::conflict, operation,
                                           "Deferred contact body generation changed");
                else if (command.request.operation == detail::LuaPhysicsOperation::remove_body) {
                    result = physics.remove_body(scene, command.self.body);
                    if (result)
                        removed.push_back(command.self.body);
                } else
                    result = Impl::mutate(scene, physics, command.self.body, command.request);
            }
            if (!result) {
                impl_->faulted = true;
                auto error = std::move(result).error();
                const auto entry = std::find_if(
                    impl_->entries.begin(), impl_->entries.end(), [&](const auto& candidate) {
                        return candidate.record.entity == command.self.entity;
                    });
                report.error = entry != impl_->entries.end()
                                   ? contextualize(std::move(error), entry->record, operation)
                                   : std::move(error);
                report.discarded_commands = impl_->deferred.size() - i;
                impl_->deferred.clear();
                return report;
            }
            ++report.applied_commands;
        }
        impl_->deferred.clear();
        return report;
    } catch (...) {
        impl_->faulted = true;
        report.error = runtime_error(ErrorCode::execution_failed, operation,
                                     "Contact dispatch failed after consuming observations");
        report.undelivered_events = report.events.size() - current;
        report.discarded_commands = impl_->deferred.size() - report.applied_commands;
        impl_->deferred.clear();
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
        ScriptStopReport report;
        report.errors.reserve(impl_->entries.size());
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
    impl_->lease.reset();
    impl_->closed = true;
    return discarded;
}

} // namespace shark
