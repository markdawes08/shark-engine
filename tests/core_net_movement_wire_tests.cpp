#include <shark/net/movement.hpp>
#include <shark/net/wire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <string_view>

namespace {
using namespace shark::net;
shark::StableId id(const char* text) {
    auto parsed = shark::StableId::parse(text);
    REQUIRE(parsed);
    return parsed.value();
}
MovementInput input() {
    return {{SessionId{id("00112233-4455-4677-8899-aabbccddeeff")}, 1, 3,
             CharacterId{id("20314253-6475-4869-8abc-ccddeeff0001")}},
            MovementStreamId{id("10213243-5465-4768-89ab-bbccddeeff01")},
            5,
            600,
            -800};
}
MovementState state() {
    return {input().authority.session, 1, input().stream, 9, 5, {1, -2, 3}, {6, 0, -0.5}};
}
std::vector<std::byte> hex(std::string_view text) {
    std::vector<std::byte> bytes;
    REQUIRE(text.size() % 2 == 0);
    const auto digit = [](char c) -> unsigned {
        return c >= 'a' ? static_cast<unsigned>(c - 'a' + 10) : static_cast<unsigned>(c - '0');
    };
    for (std::size_t n = 0; n < text.size(); n += 2)
        bytes.push_back(static_cast<std::byte>((digit(text[n]) << 4U) | digit(text[n + 1])));
    return bytes;
}
} // namespace

TEST_CASE("Movement direction has an independent exact big endian signed-axis layout",
          "[net-movement-wire]") {
    const auto golden = hex("00010100"
                            "00112233445546778899aabbccddeeff"
                            "0000000000000001"
                            "0000000000000003"
                            "20314253647548698abcccddeeff0001"
                            "102132435465476889abbbccddeeff01"
                            "0000000000000005"
                            "0258fce0");
    REQUIRE(golden.size() == movement_input_bytes);
    REQUIRE(encode_movement_input(input()));
    CHECK(encode_movement_input(input()).value() == golden);
    REQUIRE(decode_movement_input(golden));
    CHECK(decode_movement_input(golden).value() == input());
    for (std::size_t size = 0; size < golden.size(); ++size)
        CHECK_FALSE(decode_movement_input(std::span{golden}.first(size)));
    auto trailing = golden;
    trailing.push_back(std::byte{0});
    CHECK_FALSE(decode_movement_input(trailing));
    CHECK_FALSE(decode_movement_reply(golden));
}

TEST_CASE("Movement corrections and generic rejection have independent exact layouts",
          "[net-movement-wire]") {
    const auto golden = hex("00010100"
                            "00112233445546778899aabbccddeeff"
                            "0000000000000001"
                            "102132435465476889abbbccddeeff01"
                            "0000000000000009"
                            "0000000000000005"
                            "3ff0000000000000"
                            "c000000000000000"
                            "4008000000000000"
                            "4018000000000000"
                            "0000000000000000"
                            "bfe0000000000000");
    REQUIRE(golden.size() == movement_state_bytes);
    REQUIRE(encode_movement_reply(state()));
    CHECK(encode_movement_reply(state()).value() == golden);
    REQUIRE(decode_movement_reply(golden));
    CHECK(decode_movement_reply(golden).value() == MovementReply{state()});
    for (std::size_t size = 0; size < golden.size(); ++size)
        CHECK_FALSE(decode_movement_reply(std::span{golden}.first(size)));
    auto trailing = golden;
    trailing.push_back(std::byte{0});
    CHECK_FALSE(decode_movement_reply(trailing));
    CHECK_FALSE(decode_movement_input(golden));
    const auto rejected = hex("00010200");
    CHECK(encode_movement_reply(MovementRejected{}).value() == rejected);
    CHECK(decode_movement_reply(rejected).value() == MovementReply{MovementRejected{}});
    for (std::size_t size = 0; size < rejected.size(); ++size)
        CHECK_FALSE(decode_movement_reply(std::span{rejected}.first(size)));
    auto initial = state();
    initial.completed_tick = 0;
    initial.last_input = 0;
    CHECK(decode_movement_reply(encode_movement_reply(initial).value()).value() ==
          MovementReply{initial});
}

TEST_CASE("Movement input rejects malformed identities envelopes and overlength direction",
          "[net-movement-wire]") {
    const auto bytes = encode_movement_input(input()).value();
    for (const auto offset : {0U, 1U, 2U, 3U, 10U, 12U, 42U, 44U, 58U, 60U}) {
        auto changed = bytes;
        changed[offset] = std::byte{0xff};
        CHECK_FALSE(decode_movement_input(changed));
    }
    for (const auto offset : {20U, 28U, 68U}) {
        auto changed = bytes;
        std::fill_n(changed.begin() + offset, 8, std::byte{0});
        CHECK_FALSE(decode_movement_input(changed));
    }
    auto changed = bytes;
    changed[76] = std::byte{0x7f};
    changed[77] = std::byte{0xff};
    CHECK_FALSE(decode_movement_input(changed));
    for (const auto axes : std::array<std::array<std::int16_t, 2>, 7>{
             {{0, 0}, {1000, 0}, {-1000, 0}, {0, 1000}, {0, -1000}, {600, -800}, {707, 707}}}) {
        auto valid = input();
        valid.x = axes[0];
        valid.z = axes[1];
        CHECK(encode_movement_input(valid));
    }
    for (const auto axes : std::array<std::array<std::int16_t, 2>, 5>{
             {{1001, 0}, {-1001, 0}, {708, 708}, {1000, 1}, {-32768, -32768}}}) {
        auto invalid = input();
        invalid.x = axes[0];
        invalid.z = axes[1];
        CHECK_FALSE(encode_movement_input(invalid));
    }
    auto invalid = input();
    invalid.authority.sequence = 0;
    CHECK_FALSE(encode_movement_input(invalid));
    invalid = input();
    invalid.authority.ownership_epoch = 0;
    CHECK_FALSE(encode_movement_input(invalid));
    invalid = input();
    invalid.input_sequence = 0;
    CHECK_FALSE(encode_movement_input(invalid));
    invalid = input();
    invalid.input_sequence = std::numeric_limits<std::uint64_t>::max();
    CHECK(encode_movement_input(invalid)); // Exhaustion is a stream-owner policy, not a wire wrap.
}

TEST_CASE("Movement state rejects noncanonical floats out-of-bounds values and unknown headers",
          "[net-movement-wire]") {
    const auto bytes = encode_movement_reply(state()).value();
    for (const auto offset : {0U, 1U, 2U, 3U, 10U, 12U, 34U, 36U}) {
        auto changed = bytes;
        changed[offset] = std::byte{0xff};
        CHECK_FALSE(decode_movement_reply(changed));
    }
    auto changed = bytes;
    std::fill_n(changed.begin() + 20, 8, std::byte{0});
    CHECK_FALSE(decode_movement_reply(changed));
    for (const auto bits :
         {"7ff0000000000000", "fff0000000000000", "7ff8000000000001", "8000000000000000"}) {
        const auto value = hex(bits);
        for (const auto offset : {60U, 68U, 76U, 84U, 92U, 100U}) {
            auto malformed = bytes;
            std::copy(value.begin(), value.end(), malformed.begin() + offset);
            CHECK_FALSE(decode_movement_reply(malformed));
        }
    }
    for (double bad : {8192.01, -8192.01, std::numeric_limits<double>::infinity(),
                       std::numeric_limits<double>::quiet_NaN(), -0.0}) {
        auto invalid = state();
        invalid.position.y = bad;
        CHECK_FALSE(encode_movement_reply(invalid));
    }
    auto boundary = state();
    boundary.position = {-8192, 8192, 0};
    boundary.velocity = {256, 0, 0};
    CHECK(encode_movement_reply(boundary));
    boundary.velocity = {256, 1, 0};
    CHECK_FALSE(encode_movement_reply(boundary));
    boundary.velocity = {0, 0, -256.01};
    CHECK_FALSE(encode_movement_reply(boundary));
    boundary.velocity = {0, -0.0, 0};
    CHECK_FALSE(encode_movement_reply(boundary));
    boundary = state();
    boundary.ownership_epoch = 0;
    CHECK_FALSE(encode_movement_reply(boundary));
    auto rejected = hex("00010200");
    rejected.push_back(std::byte{0});
    CHECK_FALSE(decode_movement_reply(rejected));
    rejected = hex("00010300");
    CHECK_FALSE(decode_movement_reply(rejected));
}

TEST_CASE("Movement payloads fit independently sequenced bounded transport frames",
          "[net-movement-wire]") {
    const Frame request{MessageKind::movement_input, 7, encode_movement_input(input()).value()};
    const Frame reply{MessageKind::movement_state, 11, encode_movement_reply(state()).value()};
    for (const auto& frame : {request, reply}) {
        const auto encoded = encode_frame(frame);
        REQUIRE(encoded);
        CHECK(decode_frame(encoded.value()).value() == frame);
        CHECK(encoded.value().size() <= wire_max_frame_bytes);
    }
    CHECK(static_cast<std::uint8_t>(MessageKind::movement_input) == 9);
    CHECK(static_cast<std::uint8_t>(MessageKind::movement_state) == 10);
    CHECK(input().authority.sequence != request.sequence);
    CHECK(input().input_sequence != request.sequence);
}
