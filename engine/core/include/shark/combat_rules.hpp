#pragma once

#include "shark/combat_types.hpp"
#include "shark/script_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace shark {

struct CombatRuleInput {
    CombatActionKind kind;
    std::uint64_t tick;
    std::uint16_t actor_health;
    std::uint16_t target_health;
    double distance;
};
struct CombatRuleDecision {
    bool allowed;
    std::uint16_t damage;
    std::uint32_t cooldown_ticks;
    bool operator==(const CombatRuleDecision&) const = default;
};
inline constexpr std::size_t combat_rules_maximum_memory_bytes = 4 * 1024 * 1024;
inline constexpr std::size_t combat_rules_maximum_instructions = 100000;
struct CombatRulesConfig {
    std::size_t memory_bytes{combat_rules_maximum_memory_bytes};
    std::size_t instruction_limit{combat_rules_maximum_instructions};
};
struct CombatRulesStats {
    std::uint64_t evaluations;
    bool faulted;
};
inline constexpr std::size_t combat_rules_portable_slots = 8;
using CombatRuleValueState = std::array<std::int64_t, combat_rules_portable_slots>;
struct CombatRulesPortableState {
    ScriptSourceSnapshot source;
    CombatRulesConfig config;
    CombatRuleValueState state;
    std::uint64_t evaluations;
};

// Creating-thread value rules over a captured trusted Lua source. The caller
// owns source freshness and actual gameplay admission. No Scene, networking,
// physics, durable service or engine binding is exposed to these rule VMs.
class CombatRules final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<CombatRules>>
    create(const ScriptSourceSnapshot& source,
           ScriptProfile profile = ScriptProfile::server_gameplay, CombatRulesConfig config = {});
    // Fresh value-only VM per evaluation from the first call. The sixth Lua
    // argument is the only retained mutable state: exactly eight int64 slots.
    [[nodiscard]] static Result<std::unique_ptr<CombatRules>>
    create_portable(const ScriptSourceSnapshot& source, CombatRulesConfig config = {},
                    CombatRuleValueState initial = {});
    [[nodiscard]] Result<CombatRulesPortableState> capture_portable() const;
    [[nodiscard]] static Result<std::unique_ptr<CombatRules>>
    restore_portable(const CombatRulesPortableState& state);
    ~CombatRules();
    CombatRules(const CombatRules&) = delete;
    CombatRules& operator=(const CombatRules&) = delete;

    // on_combat(kind,tick,actor_health,target_health,distance) must return
    // exactly boolean allowed, integer damage, integer cooldown_ticks. The
    // server still validates all participants and constrains these values.
    // Failed evaluation faults the owner; Lua state changes are not rollback.
    [[nodiscard]] Result<CombatRuleDecision> evaluate(const CombatRuleInput& input);
    [[nodiscard]] Result<CombatRulesStats> stats() const;

  private:
    struct Impl;
    explicit CombatRules(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark
