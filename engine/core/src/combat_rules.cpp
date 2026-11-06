#include "shark/combat_rules.hpp"

#include "lua_vm.hpp"
#include "physics_contact_scope.hpp"
#include "utf8.hpp"

#include <cmath>
#include <limits>
#include <optional>
#include <thread>
#include <utility>

namespace shark {
namespace {
Error rejected(const char* operation, const char* message,
               ErrorCode code = ErrorCode::invalid_argument) {
    return {code, message, {.operation = operation}};
}
bool valid_input(const CombatRuleInput& input) {
    if (input.kind != CombatActionKind::strike && input.kind != CombatActionKind::interact &&
        input.kind != CombatActionKind::projectile)
        return false;
    const double range = input.kind == CombatActionKind::strike       ? combat_strike_range
                         : input.kind == CombatActionKind::projectile ? combat_projectile_range
                                                                      : combat_interaction_range;
    return input.tick <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
           input.actor_health > 0 && input.actor_health <= combat_maximum_health &&
           input.target_health > 0 && input.target_health <= combat_maximum_health &&
           std::isfinite(input.distance) && input.distance >= 0 && input.distance <= range;
}
Error contextualize(Error error, const std::string& path, const char* operation) {
    error.context.operation = operation;
    error.context.path = path;
    return error;
}
} // namespace

struct CombatRules::Impl {
    std::thread::id owner{std::this_thread::get_id()};
    std::unique_ptr<detail::LuaVm> vm;
    std::string path;
    std::uint64_t evaluations{0};
    bool faulted{false};
    std::optional<CombatRulesPortableState> portable{};

    Impl(std::unique_ptr<detail::LuaVm> value, std::string source)
        : vm(std::move(value)), path(std::move(source)) {}
    Result<void> check(const char* operation) const {
        if (owner != std::this_thread::get_id())
            return rejected(operation, "Combat rules require their creating thread",
                            ErrorCode::conflict);
        return {};
    }
};

CombatRules::CombatRules(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
CombatRules::~CombatRules() = default;

Result<std::unique_ptr<CombatRules>> CombatRules::create(const ScriptSourceSnapshot& source,
                                                         ScriptProfile profile,
                                                         CombatRulesConfig config) {
    constexpr auto operation = "combat.rules.create";
    if (profile != ScriptProfile::server_gameplay ||
        config.memory_bytes > combat_rules_maximum_memory_bytes ||
        config.instruction_limit > combat_rules_maximum_instructions || source.path.empty() ||
        source.path.size() > script_maximum_source_path_bytes ||
        !detail::valid_utf8(source.path, true) ||
        source.text.size() > script_runtime_maximum_source_bytes)
        return rejected(operation,
                        "Combat rules require a server profile and bounded trusted source");
    auto vm = detail::LuaVm::create({config.memory_bytes, config.instruction_limit});
    if (!vm)
        return contextualize(vm.error(), source.path, operation);
    if (auto loaded = vm.value()->execute(source.text, source.path); !loaded)
        return contextualize(loaded.error(), source.path, operation);
    if (auto callback = vm.value()->validate_combat(); !callback)
        return contextualize(callback.error(), source.path, operation);
    try {
        return std::unique_ptr<CombatRules>{
            new CombatRules{std::make_unique<Impl>(std::move(vm).value(), source.path)}};
    } catch (...) {
        return rejected(operation, "Cannot allocate combat rule owner",
                        ErrorCode::execution_failed);
    }
}

Result<CombatRuleDecision> CombatRules::evaluate(const CombatRuleInput& input) {
    constexpr auto operation = "combat.rules.evaluate";
    if (auto owner = impl_->check(operation); !owner)
        return owner.error();
    if (impl_->faulted)
        return rejected(operation, "Combat rules faulted; replace the source owner explicitly",
                        ErrorCode::conflict);
    if (detail::NativeContactScope::active())
        return rejected(operation, "Combat rules cannot execute in a native contact callback",
                        ErrorCode::conflict);
    if (!valid_input(input))
        return rejected(operation, "Invalid bounded authoritative combat observation");
    if (impl_->evaluations == std::numeric_limits<std::uint64_t>::max())
        return rejected(operation, "Combat rule evaluation counter is exhausted",
                        ErrorCode::conflict);
    ++impl_->evaluations;
    auto decision = [&]() -> Result<CombatRuleDecision> {
        if (!impl_->portable)
            return impl_->vm->evaluate_combat(input);
        const auto& configured = *impl_->portable;
        auto vm = detail::LuaVm::create(
            {configured.config.memory_bytes, configured.config.instruction_limit});
        if (!vm)
            return vm.error();
        if (auto loaded = vm.value()->execute(configured.source.text, configured.source.path);
            !loaded)
            return loaded.error();
        auto candidate = configured.state;
        auto value = vm.value()->evaluate_combat(input, candidate);
        if (value)
            impl_->portable->state = candidate;
        return value;
    }();
    if (!decision) {
        impl_->faulted = true;
        return contextualize(decision.error(), impl_->path, operation);
    }
    return decision;
}

Result<std::unique_ptr<CombatRules>>
CombatRules::create_portable(const ScriptSourceSnapshot& source, CombatRulesConfig config,
                             CombatRuleValueState initial) {
    auto owner = create(source, ScriptProfile::server_gameplay, config);
    if (!owner)
        return owner.error();
    try {
        owner.value()->impl_->portable = CombatRulesPortableState{source, config, initial, 0};
        owner.value()->impl_->vm.reset();
        return std::move(owner).value();
    } catch (...) {
        return rejected("combat.rules.portable.create", "Cannot capture portable combat rules",
                        ErrorCode::execution_failed);
    }
}

Result<CombatRulesPortableState> CombatRules::capture_portable() const {
    constexpr auto operation = "combat.rules.portable.capture";
    if (auto valid = impl_->check(operation); !valid)
        return valid.error();
    if (impl_->faulted || !impl_->portable)
        return rejected(operation, "Rules require a healthy explicit portable value-state profile",
                        ErrorCode::conflict);
    try {
        auto snapshot = *impl_->portable;
        snapshot.evaluations = impl_->evaluations;
        return snapshot;
    } catch (...) {
        return rejected(operation, "Cannot capture bounded portable combat rules",
                        ErrorCode::execution_failed);
    }
}

Result<std::unique_ptr<CombatRules>>
CombatRules::restore_portable(const CombatRulesPortableState& snapshot) {
    auto owner = create_portable(snapshot.source, snapshot.config, snapshot.state);
    if (!owner)
        return owner.error();
    owner.value()->impl_->evaluations = snapshot.evaluations;
    return std::move(owner).value();
}

Result<CombatRulesStats> CombatRules::stats() const {
    if (auto owner = impl_->check("combat.rules.stats"); !owner)
        return owner.error();
    return CombatRulesStats{impl_->evaluations, impl_->faulted};
}

} // namespace shark
