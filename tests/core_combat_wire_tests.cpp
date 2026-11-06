#include <shark/net/combat.hpp>
#include <shark/net/wire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace {
using namespace shark;
using namespace shark::net;
template <typename T> T id(const char* text) {
    auto parsed = StableId::parse(text);
    REQUIRE(parsed);
    return T{parsed.value()};
}
CombatAction action() {
    return {{id<SessionId>("00112233-4455-4677-8899-aabbccddeeff"), 1, 3,
             id<CharacterId>("20314253-6475-4869-8abc-ccddeeff0001")},
            id<CombatStreamId>("10213243-5465-4768-89ab-bbccddeeff01"),
            7,
            id<CombatTargetId>("30415263-7485-496a-8bcd-ddeeff000102"),
            CombatActionKind::strike};
}
CombatState state() {
    const auto request = action();
    MovementState movement{request.authority.session,
                           1,
                           id<MovementStreamId>("50617283-94a5-4b6c-8def-ff0001020304"),
                           9,
                           5,
                           {1, -2, 3},
                           {6, 0, -0.5}};
    return {request.authority.session,
            1,
            request.stream,
            9,
            id<CombatTargetId>("40516273-8495-4a6b-8cde-eeff00010203"),
            100,
            10,
            0,
            movement,
            {{request.target, 75, 0}},
            {{7, request.target, CombatActionKind::strike, CombatOutcome::hit, 25}}};
}
std::vector<std::byte> hex(std::string_view text) {
    REQUIRE(text.size() % 2 == 0);
    const auto digit = [](char value) -> unsigned {
        return value >= 'a' ? static_cast<unsigned>(value - 'a' + 10)
                            : static_cast<unsigned>(value - '0');
    };
    std::vector<std::byte> result;
    for (std::size_t offset = 0; offset < text.size(); offset += 2)
        result.push_back(
            static_cast<std::byte>((digit(text[offset]) << 4U) | digit(text[offset + 1])));
    return result;
}
} // namespace

TEST_CASE("Combat intent has an independent exact big endian layout", "[combat-wire]") {
    const auto golden = hex("00010100"
                            "00112233445546778899aabbccddeeff"
                            "0000000000000001"
                            "0000000000000003"
                            "20314253647548698abcccddeeff0001"
                            "102132435465476889abbbccddeeff01"
                            "0000000000000007"
                            "304152637485496a8bcdddeeff000102"
                            "01");
    REQUIRE(golden.size() == combat_action_bytes);
    REQUIRE(encode_combat_action(action()));
    CHECK(encode_combat_action(action()).value() == golden);
    REQUIRE(decode_combat_action(golden));
    CHECK(decode_combat_action(golden).value() == action());
    for (std::size_t count = 0; count < golden.size(); ++count)
        CHECK_FALSE(decode_combat_action(std::span{golden}.first(count)));
    auto trailing = golden;
    trailing.push_back(std::byte{0});
    CHECK_FALSE(decode_combat_action(trailing));
    CHECK_FALSE(decode_combat_reply(golden));
    auto interact = action();
    interact.kind = CombatActionKind::interact;
    CHECK(decode_combat_action(encode_combat_action(interact).value()).value() == interact);
}

TEST_CASE("Combat state and death rejection layouts preserve explicit bounded fields",
          "[combat-wire]") {
    const auto golden = hex("00010100"
                            "00112233445546778899aabbccddeeff"
                            "0000000000000001"
                            "102132435465476889abbbccddeeff01"
                            "0000000000000009"
                            "4051627384954a6b8cdeeeff00010203"
                            "0064"
                            "000000000000000a"
                            "0000000000000000"
                            "01"
                            "00010100"
                            "00112233445546778899aabbccddeeff"
                            "0000000000000001"
                            "5061728394a54b6c8defff0001020304"
                            "0000000000000009"
                            "0000000000000005"
                            "3ff0000000000000c0000000000000004008000000000000"
                            "40180000000000000000000000000000bfe0000000000000"
                            "01"
                            "304152637485496a8bcdddeeff000102004b0000000000000000"
                            "01"
                            "0000000000000007"
                            "304152637485496a8bcdddeeff000102"
                            "01010019");
    REQUIRE(golden.size() == 251);
    REQUIRE(encode_combat_reply(state()));
    CHECK(encode_combat_reply(state()).value() == golden);
    REQUIRE(decode_combat_reply(golden));
    CHECK(decode_combat_reply(golden).value() == CombatReply{state()});
    for (std::size_t count = 0; count < golden.size(); ++count)
        CHECK_FALSE(decode_combat_reply(std::span{golden}.first(count)));
    auto trailing = golden;
    trailing.push_back(std::byte{0});
    CHECK_FALSE(decode_combat_reply(trailing));
    auto dead = state();
    dead.health = 0;
    dead.ready_tick = 0;
    dead.respawn_tick = 12;
    dead.movement.reset();
    dead.targets.clear();
    dead.outcomes.clear();
    const auto dead_golden = hex("00010100"
                                 "00112233445546778899aabbccddeeff"
                                 "0000000000000001"
                                 "102132435465476889abbbccddeeff01"
                                 "0000000000000009"
                                 "4051627384954a6b8cdeeeff00010203"
                                 "0000"
                                 "0000000000000000"
                                 "000000000000000c"
                                 "000000");
    REQUIRE(dead_golden.size() == 89);
    CHECK(encode_combat_reply(dead).value() == dead_golden);
    CHECK(decode_combat_reply(dead_golden).value() == CombatReply{dead});
    const auto rejection = hex("00010200");
    CHECK(encode_combat_reply(CombatRejected{}).value() == rejection);
    CHECK(decode_combat_reply(rejection).value() == CombatReply{CombatRejected{}});
}

TEST_CASE("Combat decoders reject malformed headers UUIDs flags and oversized claims",
          "[combat-wire]") {
    const auto valid_action = encode_combat_action(action()).value();
    for (const auto offset : std::array<std::size_t, 6>{0, 1, 2, 3, 10, 12}) {
        auto bytes = valid_action;
        bytes[offset] = std::byte{0xff};
        CHECK_FALSE(decode_combat_action(bytes));
    }
    for (const auto offset : std::array<std::size_t, 3>{27, 35, 75}) {
        auto bytes = valid_action;
        std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(offset - 7),
                  bytes.begin() + static_cast<std::ptrdiff_t>(offset + 1), std::byte{0});
        CHECK_FALSE(decode_combat_action(bytes));
    }
    for (const auto kind : {0U, 3U, 255U}) {
        auto bytes = valid_action;
        bytes[92] = static_cast<std::byte>(kind);
        CHECK_FALSE(decode_combat_action(bytes));
    }
    const auto valid_state = encode_combat_reply(state()).value();
    for (const auto offset : std::array<std::size_t, 6>{0, 1, 2, 3, 34, 60}) {
        auto bytes = valid_state;
        bytes[offset] = std::byte{0xff};
        CHECK_FALSE(decode_combat_reply(bytes));
    }
    auto flag = valid_state;
    flag[86] = std::byte{2};
    CHECK_FALSE(decode_combat_reply(flag));
    auto target_count = valid_state;
    target_count[195] = std::byte{33};
    CHECK_FALSE(decode_combat_reply(target_count));
    auto outcome_count = valid_state;
    outcome_count[222] = std::byte{9};
    CHECK_FALSE(decode_combat_reply(outcome_count));
    auto oversized = valid_state;
    oversized.resize(combat_maximum_reply_bytes + 1);
    CHECK_FALSE(decode_combat_reply(oversized));
}

TEST_CASE("Combat values validate health movement lifecycle sorted targets and result semantics",
          "[combat-wire]") {
    const auto original = state();
    for (unsigned variant = 0; variant < 13; ++variant) {
        auto bad = original;
        switch (variant) {
        case 0:
            bad.health = 101;
            break;
        case 1:
            bad.health = 0;
            break;
        case 2:
            bad.respawn_tick = 1;
            break;
        case 3:
            bad.movement.reset();
            break;
        case 4:
            ++bad.movement->ownership_epoch;
            break;
        case 5:
            ++bad.movement->completed_tick;
            break;
        case 6:
            bad.targets.front().id = bad.self;
            break;
        case 7:
            bad.targets.push_back(bad.targets.front());
            break;
        case 8:
            bad.targets.front().health = 0;
            break;
        case 9:
            bad.outcomes.front().amount = 26;
            break;
        case 10:
            bad.outcomes.front().kind = CombatActionKind::interact;
            break;
        case 11:
            bad.outcomes.front().outcome = static_cast<CombatOutcome>(9);
            break;
        default:
            bad.outcomes.push_back(bad.outcomes.front());
            break;
        }
        CHECK_FALSE(validate_combat_state(bad));
        CHECK_FALSE(encode_combat_reply(bad));
    }
    auto live = original;
    live.targets.front().health = 0;
    live.targets.front().respawn_tick = 69;
    for (unsigned code = 1; code <= 8; ++code) {
        auto& result = live.outcomes.front();
        result.outcome = static_cast<CombatOutcome>(code);
        result.kind = code == 2 ? CombatActionKind::interact : CombatActionKind::strike;
        result.amount = static_cast<std::uint16_t>(code == 1 ? 25 : 0);
        CHECK(validate_combat_state(live));
    }
    live.outcomes.front().action_sequence = std::numeric_limits<std::uint64_t>::max();
    CHECK(validate_combat_state(live));
    live.outcomes.push_back(live.outcomes.front());
    CHECK_FALSE(validate_combat_state(live));
}

TEST_CASE("Combat payloads stay inside explicit list and outer wire boundaries", "[combat-wire]") {
    auto full = state();
    full.targets.clear();
    for (unsigned index = 0; index < 32; ++index) {
        std::string text = "a04c0000-0000-4000-8000-000000000000";
        constexpr char digits[] = "0123456789abcdef";
        text[34] = digits[index >> 4U];
        text[35] = digits[index & 15U];
        auto parsed = StableId::parse(text);
        REQUIRE(parsed);
        full.targets.push_back({CombatTargetId{parsed.value()}, 100, 0});
    }
    full.outcomes.clear();
    for (std::uint64_t sequence = 100; sequence < 108; ++sequence)
        full.outcomes.push_back(
            {sequence, full.targets.front().id, CombatActionKind::strike, CombatOutcome::hit, 25});
    REQUIRE(validate_combat_state(full));
    const auto payload = encode_combat_reply(full).value();
    CHECK(payload.size() == 1253);
    CHECK(payload.size() < combat_maximum_reply_bytes);
    CHECK(decode_combat_reply(payload).value() == CombatReply{full});
    full.targets.push_back(full.targets.back());
    CHECK_FALSE(validate_combat_state(full));
    full.targets.pop_back();
    full.outcomes.push_back(
        {108, full.targets.front().id, CombatActionKind::strike, CombatOutcome::hit, 25});
    CHECK_FALSE(validate_combat_state(full));
    for (const auto kind : {MessageKind::combat_action, MessageKind::combat_state}) {
        const Frame frame{kind, 1, payload};
        const auto encoded = encode_frame(frame);
        REQUIRE(encoded);
        CHECK(decode_frame(encoded.value()).value() == frame);
    }
}
