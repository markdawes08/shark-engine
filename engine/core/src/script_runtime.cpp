#include "shark/script_runtime.hpp"

#include "lua_vm.hpp"
#include "scene_script_access.hpp"
#include "shark/scene.hpp"
#include "shark/script_components.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <cmath>
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

    Result<void> run(Entry& entry, Scene& scene, ScriptPhase phase, std::uint64_t tick,
                     double delta, std::string_view callback, const char* operation) {
        Active active_guard{*this, entry, phase, tick};
        detail::LuaCallContext context{
            &scene, entry.record.entity, lua_phase(phase), lua_profile(config.profile), tick, delta,
            this,   &Impl::log};
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
            auto result = impl_->run(entry, scene, ScriptPhase::init, 0, 0, "on_init", operation);
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
                           delta_seconds, "on_update", operation);
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
                                     "on_fixed_update", operation);
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

Result<ScriptStopReport> ScriptRuntime::stop(Scene& scene) {
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
                                     0, "on_destroy", operation);
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
