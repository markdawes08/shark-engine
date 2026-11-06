#include <shark/combat_rules.hpp>

#include "physics_contact_scope.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <string>
#include <thread>

namespace {
using namespace shark;

template <typename T> T take(Result<T> result) {
    if (!result)
        INFO(result.error().context.operation << ": " << result.error().message);
    REQUIRE(result);
    return std::move(result).value();
}
CombatRuleInput input(CombatActionKind kind = CombatActionKind::strike) {
    return {kind, 17, 100, 75, 2};
}
std::unique_ptr<CombatRules> rules(const std::string& body) {
    return take(CombatRules::create(
        {"scripts/combat.lua",
         "function on_combat(kind,tick,actor,target,distance) " + body + " end"}));
}
} // namespace

TEST_CASE(
    "Server Lua combat decisions consume bounded authoritative values without engine services",
    "[combat-rules]") {
    auto owner = rules(R"lua(
        assert(shark == nil and io == nil and os == nil and package == nil)
        assert(kind == 1 or kind == 2)
        assert(tick == 17 and actor == 100 and target == 75 and distance == 2)
        if kind == 1 then return true, 25, 10 end
        return true, 0, 5
    )lua");
    REQUIRE(take(owner->stats()).evaluations == 0);
    REQUIRE(take(owner->evaluate(input())) == CombatRuleDecision{true, 25, 10});
    REQUIRE(take(owner->evaluate(input(CombatActionKind::interact))) ==
            CombatRuleDecision{true, 0, 5});
    REQUIRE(take(owner->stats()).evaluations == 2);
    REQUIRE_FALSE(take(owner->stats()).faulted);
    auto denial = rules("return false, 0, 0");
    REQUIRE(take(denial->evaluate(input())) == CombatRuleDecision{false, 0, 0});
}

TEST_CASE("Malformed Lua combat outputs fail closed and retain the first failed evaluation",
          "[combat-rules]") {
    for (const auto* body :
         {"return true, 0, 10", "return true, 26, 10", "return true, 25, 9", "return true, 25, 201",
          "return false, 1, 0", "return false, 0, 1", "return 1, 25, 10", "return true, '25', 10",
          "return true, 25.0, 10", "return true, 25, 10, 1", "return true, 25"}) {
        INFO(body);
        auto owner = rules(body);
        const auto result = owner->evaluate(input());
        REQUIRE_FALSE(result);
        REQUIRE(result.error().context.operation == "combat.rules.evaluate");
        REQUIRE(result.error().context.path == "scripts/combat.lua");
        REQUIRE_FALSE(result.error().message.empty());
        REQUIRE(take(owner->stats()).faulted);
        REQUIRE(take(owner->stats()).evaluations == 1);
        REQUIRE_FALSE(owner->evaluate(input()));
        REQUIRE(take(owner->stats()).evaluations == 1);
    }
    for (const auto* body : {"return true, 1, 5", "return true, 0, 4", "return true, 0, 201"}) {
        auto owner = rules(body);
        REQUIRE_FALSE(owner->evaluate(input(CombatActionKind::interact)));
        REQUIRE(take(owner->stats()).faulted);
    }
}

TEST_CASE("Invalid combat observations reject before Lua state or counters change",
          "[combat-rules]") {
    auto owner = rules("return true, 25, 10");
    for (unsigned variant = 0; variant < 10; ++variant) {
        auto invalid = input();
        switch (variant) {
        case 0:
            invalid.kind = static_cast<CombatActionKind>(0);
            break;
        case 1:
            invalid.tick = std::numeric_limits<std::uint64_t>::max();
            break;
        case 2:
            invalid.actor_health = 0;
            break;
        case 3:
            invalid.actor_health = 101;
            break;
        case 4:
            invalid.target_health = 0;
            break;
        case 5:
            invalid.target_health = 101;
            break;
        case 6:
            invalid.distance = -1;
            break;
        case 7:
            invalid.distance = 2.5001;
            break;
        case 8:
            invalid.distance = std::numeric_limits<double>::infinity();
            break;
        default:
            invalid.distance = std::numeric_limits<double>::quiet_NaN();
            break;
        }
        REQUIRE_FALSE(owner->evaluate(invalid));
    }
    REQUIRE(take(owner->stats()).evaluations == 0);
    REQUIRE_FALSE(take(owner->stats()).faulted);
    REQUIRE(take(owner->evaluate(input())) == CombatRuleDecision{true, 25, 10});
}

TEST_CASE("Combat rule construction and execution retain bounded server-only capabilities",
          "[combat-rules]") {
    const ScriptSourceSnapshot source{"scripts/combat.lua",
                                      "function on_combat() return true,25,10 end"};
    REQUIRE_FALSE(CombatRules::create(source, ScriptProfile::client_presentation));
    REQUIRE_FALSE(
        CombatRules::create(source, ScriptProfile::server_gameplay, {4 * 1024 * 1024 + 1, 100000}));
    REQUIRE_FALSE(
        CombatRules::create(source, ScriptProfile::server_gameplay, {4 * 1024 * 1024, 100001}));
    REQUIRE_FALSE(CombatRules::create(source, ScriptProfile::server_gameplay, {1, 100000}));
    REQUIRE_FALSE(
        CombatRules::create(source, ScriptProfile::server_gameplay, {4 * 1024 * 1024, 0}));
    REQUIRE_FALSE(CombatRules::create({"scripts/combat.lua", "on_combat = 3"}));
    REQUIRE_FALSE(CombatRules::create({"scripts/combat.lua", "function on_combat("}));
    REQUIRE_FALSE(CombatRules::create({"", source.text}));
    REQUIRE_FALSE(CombatRules::create(
        {source.path, std::string(script_runtime_maximum_source_bytes + 1, ' ')}));
    auto inaccessible = rules("shark.entity.set_name('forged'); return true,25,10");
    REQUIRE_FALSE(inaccessible->evaluate(input()));
    REQUIRE(take(inaccessible->stats()).faulted);
    auto bounded =
        take(CombatRules::create({source.path, "function on_combat() while true do end end"},
                                 ScriptProfile::server_gameplay, {4 * 1024 * 1024, 256}));
    const auto result = bounded->evaluate(input());
    REQUIRE_FALSE(result);
    REQUIRE(result.error().message.find("instruction limit") != std::string::npos);
    REQUIRE(take(bounded->stats()).faulted);
}

TEST_CASE(
    "Combat rules preserve partial Lua failure and creating-thread native-callback boundaries",
    "[combat-rules]") {
    auto owner = take(CombatRules::create({"scripts/combat.lua", R"lua(
        count = 0
        function on_combat()
            count = count + 1
            if count == 2 then error('second call failed after state changed') end
            return true, count, 10
        end
    )lua"}));
    bool foreign_evaluate = false;
    bool foreign_stats = false;
    std::thread foreign{[&] {
        foreign_evaluate = !owner->evaluate(input());
        foreign_stats = !owner->stats();
    }};
    foreign.join();
    REQUIRE(foreign_evaluate);
    REQUIRE(foreign_stats);
    {
        detail::NativeContactScope native;
        REQUIRE_FALSE(owner->evaluate(input()));
    }
    REQUIRE(take(owner->stats()).evaluations == 0);
    REQUIRE_FALSE(take(owner->stats()).faulted);
    REQUIRE(take(owner->evaluate(input())).damage == 1);
    const auto failed = owner->evaluate(input());
    REQUIRE_FALSE(failed);
    REQUIRE(failed.error().message.find("state changed") != std::string::npos);
    REQUIRE(take(owner->stats()).evaluations == 2);
    REQUIRE(take(owner->stats()).faulted);
    REQUIRE_FALSE(owner->evaluate(input()));
    REQUIRE(take(owner->stats()).evaluations == 2);
}
