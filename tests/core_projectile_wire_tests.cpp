// Original independent protocol/profile fixtures; project MIT license.
#include "shark/combat_rules.hpp"
#include "shark/net/combat_client.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace {
using namespace shark;
using namespace shark::net;
template <class T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().context.operation);
        INFO(result.error().message);
    }
    REQUIRE(result);
    return std::move(result).value();
}
template <class T> T id(const char* text) {
    return T{take(StableId::parse(text))};
}
CombatAction action() {
    return {{id<SessionId>("00112233-4455-4677-8899-aabbccddeeff"), 1, 3,
             id<CharacterId>("20314253-6475-4869-8abc-ccddeeff0001")},
            id<CombatStreamId>("10213243-5465-4768-89ab-bbccddeeff01"),
            1,
            id<CombatTargetId>("30415263-7485-496a-8bcd-ddeeff000102"),
            CombatActionKind::projectile};
}
std::vector<std::byte> hex(std::string_view text) {
    REQUIRE(text.size() % 2 == 0);
    const auto digit = [](char c) -> unsigned {
        return c >= 'a' ? static_cast<unsigned>(c - 'a' + 10) : static_cast<unsigned>(c - '0');
    };
    std::vector<std::byte> result;
    for (std::size_t i = 0; i < text.size(); i += 2)
        result.push_back(static_cast<std::byte>((digit(text[i]) << 4U) | digit(text[i + 1])));
    return result;
}
CombatState state() {
    const auto request = action();
    const auto self = id<CombatTargetId>("40516273-8495-4a6b-8cde-eeff00010203");
    MovementState movement{request.authority.session,
                           1,
                           id<MovementStreamId>("50617283-94a5-4b6c-8def-ff0001020304"),
                           0,
                           0,
                           {},
                           {}};
    return {request.authority.session,  1, request.stream, 0, self, 100, 0, 0, movement,
            {{request.target, 100, 0}}, {}};
}
} // namespace

TEST_CASE("projectile actions use explicit version two and preserve legacy encodings",
          "[projectile-wire]") {
    const auto expected = hex("00020100"
                              "00112233445546778899aabbccddeeff"
                              "00000000000000010000000000000003"
                              "20314253647548698abcccddeeff0001"
                              "102132435465476889abbbccddeeff01"
                              "0000000000000001"
                              "304152637485496a8bcdddeeff00010203");
    REQUIRE(expected.size() == combat_action_bytes);
    CHECK(take(encode_combat_action(action())) == expected);
    CHECK(take(decode_combat_action(expected)) == action());
    auto legacy = expected;
    legacy[1] = std::byte{1};
    CHECK_FALSE(decode_combat_action(legacy));
    for (const auto kind : {CombatActionKind::strike, CombatActionKind::interact}) {
        auto request = action();
        request.kind = kind;
        auto original = legacy;
        original.back() = static_cast<std::byte>(kind);
        CHECK(take(encode_combat_action(request)) == original);
        CHECK(take(decode_combat_action(original)) == request);
    }
    for (unsigned version : {0U, 3U, 255U}) {
        auto invalid = expected;
        invalid[1] = static_cast<std::byte>(version);
        CHECK_FALSE(decode_combat_action(invalid));
    }
    for (std::size_t size = 0; size < expected.size(); ++size)
        CHECK_FALSE(decode_combat_action(std::span{expected}.first(size)));
    auto trailing = expected;
    trailing.push_back(std::byte{0});
    CHECK_FALSE(decode_combat_action(trailing));
}

TEST_CASE("projectile results retain once-only effects and cannot hide in version one",
          "[projectile-wire]") {
    const auto initial = state();
    const SessionSnapshot session{initial.session,
                                  id<AccountId>("60718293-a4b5-4c6d-8ef0-001122334455"),
                                  action().authority.character,
                                  1,
                                  1,
                                  true,
                                  true};
    const auto client = take(CombatClient::create(session, initial));
    auto hit = initial;
    hit.completed_tick = 5;
    hit.movement->completed_tick = 5;
    hit.ready_tick = 11;
    hit.targets.front().health = 75;
    hit.outcomes = {{1, action().target, CombatActionKind::strike, CombatOutcome::hit, 25}};
    auto expected = take(encode_combat_reply(hit));
    CHECK(expected[1] == std::byte{1});
    expected[1] = std::byte{2};
    expected[expected.size() - 4] = std::byte{3};
    hit.outcomes.front().kind = CombatActionKind::projectile;
    CHECK(take(encode_combat_reply(hit)) == expected);
    CHECK(take(decode_combat_reply(expected)) == CombatReply{hit});
    const auto applied = take(client->apply(take(decode_combat_reply(expected))));
    CHECK(applied.status == CombatClientApplyResult::applied);
    REQUIRE(applied.effects.size() == 1);
    CHECK(applied.effects.front().kind == CombatActionKind::projectile);
    CHECK(take(client->apply(hit)).effects.empty());
    expected[1] = std::byte{1};
    CHECK_FALSE(decode_combat_reply(expected));
    hit.outcomes.front().amount = 0;
    CHECK_FALSE(validate_combat_state(hit));
    hit.outcomes.front().amount = 26;
    CHECK_FALSE(validate_combat_state(hit));
    hit.outcomes.front().amount = 0;
    hit.outcomes.front().outcome = CombatOutcome::blocked;
    CHECK(validate_combat_state(hit));
    CHECK(take(encode_combat_reply(CombatRejected{})) == hex("00010200"));
}

TEST_CASE("projectile rules retain bounded server-only damage range and cooldown",
          "[projectile-wire]") {
    const auto rules = take(CombatRules::create({"scripts/projectile.lua", R"(
function on_combat(kind,tick,actor,target,distance)
    assert(kind==3 and distance==10 and actor==100 and target==100)
    return true,25,10
end
)"}));
    const CombatRuleInput input{CombatActionKind::projectile, 1, 100, 100, 10};
    CHECK(take(rules->evaluate(input)) == CombatRuleDecision{true, 25, 10});
    auto invalid = input;
    invalid.distance = 10.5;
    CHECK_FALSE(rules->evaluate(invalid));
    CHECK(take(rules->stats()).evaluations == 1);
    for (const auto* result : {"return true,0,10", "return true,25,9", "return true,26,10"}) {
        const auto bad = take(CombatRules::create(
            {"scripts/projectile.lua", std::string{"function on_combat() "} + result + " end"}));
        CHECK_FALSE(bad->evaluate(input));
        CHECK(take(bad->stats()).faulted);
    }
}
