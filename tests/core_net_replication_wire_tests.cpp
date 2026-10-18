#include <shark/net/replication.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace {
using namespace shark::net;
shark::StableId id(const char* text) {
    auto parsed = shark::StableId::parse(text);
    REQUIRE(parsed);
    return parsed.value();
}
ReplicationUpdate sample() {
    ReplicatedPose pose;
    pose.coefficients[3] = 10;
    return {SessionId{id("00112233-4455-4677-8899-aabbccddeeff")},
            1,
            shark::ReplicaSessionId{id("10213243-5465-4768-89ab-bbccddeeff01")},
            2,
            0,
            true,
            {{ReplicationChange::spawn, {7}, pose}}};
}
SessionEnvelope envelope() {
    return {sample().session, 1, 3, CharacterId{id("20314253-6475-4869-8abc-ccddeeff0001")}};
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

TEST_CASE("replication baseline has an independent big endian binary64 golden layout",
          "[net-replication-wire]") {
    const auto golden = hex("00010100"
                            "00112233445546778899aabbccddeeff"
                            "0000000000000001"
                            "102132435465476889abbbccddeeff01"
                            "0000000000000002"
                            "0000000000000000"
                            "0001"
                            "01"
                            "0000000000000007"
                            "01"
                            "3ff0000000000000"
                            "0000000000000000"
                            "0000000000000000"
                            "4024000000000000"
                            "0000000000000000"
                            "3ff0000000000000"
                            "0000000000000000"
                            "0000000000000000"
                            "0000000000000000"
                            "0000000000000000"
                            "3ff0000000000000"
                            "0000000000000000");
    REQUIRE(golden.size() == 168);
    REQUIRE(encode_replication_update(sample()));
    CHECK(encode_replication_update(sample()).value() == golden);
    REQUIRE(decode_replication_update(golden));
    CHECK(decode_replication_update(golden).value() == sample());
    for (std::size_t size = 0; size < golden.size(); ++size)
        CHECK_FALSE(decode_replication_update(std::span{golden}.first(size)));
    auto trailing = golden;
    trailing.push_back(std::byte{0});
    CHECK_FALSE(decode_replication_update(trailing));
}

TEST_CASE("replication controls have independent layouts and exact bounded envelopes",
          "[net-replication-wire]") {
    auto golden = hex("00010100"
                      "00112233445546778899aabbccddeeff"
                      "0000000000000001"
                      "0000000000000003"
                      "20314253647548698abcccddeeff0001"
                      "102132435465476889abbbccddeeff01"
                      "0000000000000002");
    REQUIRE(golden.size() == replication_control_bytes);
    ReplicationControl control = ReplicationAck{envelope(), sample().stream, 2};
    REQUIRE(encode_replication_control(control));
    CHECK(encode_replication_control(control).value() == golden);
    REQUIRE(decode_replication_control(golden));
    CHECK(decode_replication_control(golden).value() == control);
    for (std::size_t size = 0; size < golden.size(); ++size)
        CHECK_FALSE(decode_replication_control(std::span{golden}.first(size)));
    golden[2] = std::byte{2};
    golden[75] = std::byte{0};
    control = ReplicationResync{envelope(), sample().stream, 0};
    CHECK(encode_replication_control(control).value() == golden);
    CHECK(decode_replication_control(golden).value() == control);
    for (const auto offset : {0U, 1U, 2U, 3U, 10U, 12U, 42U, 44U, 58U, 60U}) {
        auto changed = golden;
        changed[offset] = std::byte{0xff};
        CHECK_FALSE(decode_replication_control(changed));
    }
    for (const auto offset : {20U, 28U}) {
        auto changed = golden;
        for (std::size_t n = offset; n < offset + 8U; ++n)
            changed[n] = std::byte{0};
        CHECK_FALSE(decode_replication_control(changed));
    }
    golden.push_back(std::byte{0});
    CHECK_FALSE(decode_replication_control(golden));
    auto invalid = envelope();
    invalid.sequence = 0;
    CHECK_FALSE(encode_replication_control(ReplicationAck{invalid, sample().stream, 2}));
    CHECK_FALSE(encode_replication_control(ReplicationAck{envelope(), sample().stream, 0}));
}

TEST_CASE("replication rejects unknown flags masks invalid identities and malformed records",
          "[net-replication-wire]") {
    const auto bytes = encode_replication_update(sample()).value();
    for (const auto offset : {0U, 1U, 2U, 3U, 10U, 12U, 34U, 36U, 60U, 61U, 62U, 71U}) {
        auto changed = bytes;
        changed[offset] = std::byte{0xff};
        CHECK_FALSE(decode_replication_update(changed));
    }
    for (const auto offset : {20U, 44U, 63U}) {
        auto changed = bytes;
        for (std::size_t n = offset; n < offset + 8U; ++n)
            changed[n] = std::byte{0};
        CHECK_FALSE(decode_replication_update(changed));
    }
    for (const auto bits :
         {"7ff0000000000000", "fff0000000000000", "7ff8000000000001", "8000000000000000"}) {
        auto changed = bytes;
        const auto value = hex(bits);
        std::copy(value.begin(), value.end(), changed.begin() + 80);
        CHECK_FALSE(decode_replication_update(changed));
    }
    auto changed = bytes;
    changed[71] = std::byte{0};
    CHECK_FALSE(decode_replication_update(changed));
    auto update = sample();
    update.changes.push_back(update.changes.front());
    CHECK_FALSE(encode_replication_update(update));
    update = sample();
    update.changes.front().pose.reset();
    CHECK_FALSE(encode_replication_update(update));
    update = sample();
    update.changes.front().kind = ReplicationChange::update;
    CHECK_FALSE(encode_replication_update(update));
    update.changes.front().kind = static_cast<ReplicationChange>(255);
    CHECK_FALSE(encode_replication_update(update));
}

TEST_CASE(
    "replication accepts affine shear and canonicalizes zero while rejecting degenerate poses",
    "[net-replication-wire]") {
    ReplicatedPose pose;
    pose.coefficients[1] = 0.5;
    pose.coefficients[2] = -0.0;
    auto normalized = canonicalize_replicated_pose(pose);
    REQUIRE(normalized);
    CHECK_FALSE(std::signbit(normalized.value().coefficients[2]));
    auto update = sample();
    update.changes.front().pose = pose;
    CHECK_FALSE(encode_replication_update(update));
    update.changes.front().pose = normalized.value();
    CHECK(decode_replication_update(encode_replication_update(update).value()).value() == update);
    for (double bad :
         {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        pose.coefficients[3] = bad;
        CHECK_FALSE(canonicalize_replicated_pose(pose));
    }
    pose = {};
    pose.coefficients[0] = 0;
    CHECK_FALSE(canonicalize_replicated_pose(pose));
    pose.coefficients = {1, 1, 0, 0, 0, 1e-15, 0, 0, 0, 0, 1, 0};
    CHECK_FALSE(canonicalize_replicated_pose(pose));
}

TEST_CASE("replication enforces separate pose and despawn bounds within one transport frame",
          "[net-replication-wire]") {
    auto update = sample();
    update.changes.clear();
    CHECK(encode_replication_update(update).value().size() == replication_update_header_bytes);
    for (std::uint64_t n = 1; n <= 32; ++n)
        update.changes.push_back({ReplicationChange::spawn, {n}, ReplicatedPose{}});
    auto bytes = encode_replication_update(update);
    REQUIRE(bytes);
    CHECK(bytes.value().size() == 3454);
    CHECK(decode_replication_update(bytes.value()).value() == update);
    update.changes.push_back({ReplicationChange::spawn, {33}, ReplicatedPose{}});
    CHECK_FALSE(encode_replication_update(update));
    update.changes.clear();
    update.baseline = false;
    update.base_revision = 1;
    CHECK_FALSE(encode_replication_update(update));
    for (std::uint64_t n = 1; n <= 32; ++n)
        update.changes.push_back({ReplicationChange::despawn, {n}, {}});
    for (std::uint64_t n = 33; n <= 64; ++n)
        update.changes.push_back({ReplicationChange::spawn, {n}, ReplicatedPose{}});
    bytes = encode_replication_update(update);
    REQUIRE(bytes);
    CHECK(bytes.value().size() == 3742);
    CHECK(decode_replication_update(bytes.value()).value() == update);
    update.changes.back().kind = ReplicationChange::update;
    CHECK(encode_replication_update(update));
    update.changes.back().kind = ReplicationChange::despawn;
    update.changes.back().pose.reset();
    CHECK_FALSE(encode_replication_update(update));
    update.changes.resize(1);
    update.changes.front().pose = ReplicatedPose{};
    CHECK_FALSE(encode_replication_update(update));
    update.base_revision = update.revision;
    CHECK_FALSE(encode_replication_update(update));
}
