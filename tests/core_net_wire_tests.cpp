#include <shark/net/wire.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using shark::net::Frame;
using shark::net::FrameDecoder;
using shark::net::MessageKind;
using Bytes = std::vector<std::byte>;

Bytes payload(const std::size_t size) {
    Bytes bytes(size);
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = static_cast<std::byte>(index % 256);
    }
    return bytes;
}

Bytes encode(const Frame& frame) {
    auto result = shark::net::encode_frame(frame);
    REQUIRE(result);
    return std::move(result).value();
}

template <typename T> void invalid(const shark::Result<T>& result, const std::string& operation) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == shark::ErrorCode::invalid_argument);
    CHECK(result.error().context.operation == operation);
}

void conflict(const shark::Result<std::uint64_t>& result) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == shark::ErrorCode::conflict);
    CHECK(result.error().context.operation == "net.sequence");
}

void rejects_header(const Bytes& bytes) {
    invalid(shark::net::decode_frame(bytes), "net.wire.decode");
    FrameDecoder decoder;
    invalid(decoder.consume(bytes), "net.wire.decode");
    CHECK(decoder.failed());
    CHECK(decoder.buffered_bytes() == shark::net::wire_header_bytes);
    invalid(decoder.finish(), "net.wire.finish");
    invalid(decoder.consume({}), "net.wire.decode");
}

static_assert(shark::net::wire_header_bytes == 20);
static_assert(shark::net::wire_max_payload == 4096);
static_assert(shark::net::wire_max_frame_bytes == 4116);
static_assert(sizeof(FrameDecoder) <= shark::net::wire_max_frame_bytes + 64);

} // namespace

TEST_CASE("wire v1 matches independent network byte order golden bytes", "[net-wire]") {
    const Frame frame{MessageKind::probe,
                      0x0102030405060708ULL,
                      {std::byte{0xaa}, std::byte{0x00}, std::byte{0xff}}};
    constexpr std::array<std::uint8_t, 23> golden{
        0x53, 0x48, 0x52, 0x4b, // SHRK, never native struct storage
        0x00, 0x01, 0x03, 0x00, // version, kind, reserved flags
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x00, 0x00, 0x00, 0x03, // payload size
        0xaa, 0x00, 0xff};
    const auto bytes = encode(frame);
    REQUIRE(bytes.size() == golden.size());
    for (std::size_t index = 0; index < golden.size(); ++index) {
        CHECK(std::to_integer<std::uint8_t>(bytes[index]) == golden[index]);
    }
    Bytes independent;
    for (const auto byte : golden) {
        independent.push_back(static_cast<std::byte>(byte));
    }
    const auto decoded = shark::net::decode_frame(independent);
    REQUIRE(decoded);
    CHECK(decoded.value() == frame);
    CHECK(shark::net::wire_version == 1);
}

TEST_CASE("wire diagnostic kinds preserve empty and maximum owned payloads", "[net-wire]") {
    for (const auto kind : std::array{MessageKind::hello, MessageKind::hello_ack,
                                      MessageKind::probe, MessageKind::probe_reply}) {
        for (const auto sequence :
             std::array<std::uint64_t, 2>{1, std::numeric_limits<std::uint64_t>::max()}) {
            const Frame frame{kind, sequence, {}};
            const auto bytes = encode(frame);
            CHECK(bytes.size() == shark::net::wire_header_bytes);
            const auto result = shark::net::decode_frame(bytes);
            REQUIRE(result);
            CHECK(result.value() == frame);
        }
    }
    for (const auto kind : std::array{MessageKind::probe, MessageKind::probe_reply}) {
        for (const auto size : std::array<std::size_t, 4>{0, 1, 255, 4096}) {
            const Frame frame{kind, 2, payload(size)};
            auto bytes = encode(frame);
            CHECK(bytes.size() == shark::net::wire_header_bytes + size);
            const auto decoded = shark::net::decode_frame(bytes);
            REQUIRE(decoded);
            bytes.assign(bytes.size(), std::byte{0});
            CHECK(decoded.value() == frame);
        }
    }
}

TEST_CASE("wire encoder rejects unknown kinds zero sequence and illegal body lengths",
          "[net-wire]") {
    for (const auto kind : std::array{0U, 5U, 255U}) {
        invalid(shark::net::encode_frame({static_cast<MessageKind>(kind), 1, {}}),
                "net.wire.encode");
    }
    for (const auto kind : std::array{MessageKind::hello, MessageKind::hello_ack,
                                      MessageKind::probe, MessageKind::probe_reply}) {
        invalid(shark::net::encode_frame({kind, 0, {}}), "net.wire.encode");
    }
    for (const auto kind : std::array{MessageKind::hello, MessageKind::hello_ack}) {
        invalid(shark::net::encode_frame({kind, 1, payload(1)}), "net.wire.encode");
    }
    for (const auto kind : std::array{MessageKind::probe, MessageKind::probe_reply}) {
        invalid(shark::net::encode_frame({kind, 1, payload(4097)}), "net.wire.encode");
    }
}

TEST_CASE("wire parser fails closed on magic version kind flags and zero sequence", "[net-wire]") {
    const auto valid = encode({MessageKind::probe, 1, {}});
    for (std::size_t index = 0; index < 4; ++index) {
        auto bytes = valid;
        bytes[index] ^= std::byte{0xff};
        rejects_header(bytes);
    }
    for (const auto version : std::array<std::uint16_t, 5>{0, 2, 256, 257, 65535}) {
        auto bytes = valid;
        bytes[4] = static_cast<std::byte>(version >> 8U);
        bytes[5] = static_cast<std::byte>(version & 0xffU);
        rejects_header(bytes);
    }
    for (const auto kind : std::array{0U, 5U, 255U}) {
        auto bytes = valid;
        bytes[6] = static_cast<std::byte>(kind);
        rejects_header(bytes);
    }
    for (unsigned bit = 0; bit < 8; ++bit) {
        auto bytes = valid;
        bytes[7] = static_cast<std::byte>(1U << bit);
        rejects_header(bytes);
    }
    auto zero = valid;
    std::fill(zero.begin() + 8, zero.begin() + 16, std::byte{0});
    rejects_header(zero);
}

TEST_CASE("wire parser rejects oversized claims and hello bodies before buffering payload",
          "[net-wire]") {
    for (const auto size : std::array<std::uint32_t, 3>{4097, 0x80000000U, 0xffffffffU}) {
        auto bytes = encode({MessageKind::probe, 1, {}});
        for (std::size_t index = 0; index < 4; ++index) {
            bytes[16 + index] = static_cast<std::byte>((size >> ((3 - index) * 8)) & 0xffU);
        }
        // A large supplied span must still retain only the fixed header on failure.
        bytes.resize(1024 * 1024, std::byte{0x55});
        rejects_header(bytes);
    }
    for (const auto kind : std::array{MessageKind::hello, MessageKind::hello_ack}) {
        auto bytes = encode({MessageKind::probe, 1, payload(1)});
        bytes[6] = static_cast<std::byte>(kind);
        rejects_header(bytes);
    }
}

TEST_CASE("exact frame decoding rejects every truncation and trailing data", "[net-wire]") {
    const auto bytes = encode({MessageKind::probe_reply, 9, payload(17)});
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        invalid(shark::net::decode_frame(std::span{bytes}.first(size)), "net.wire.decode");
    }
    auto trailing = bytes;
    trailing.push_back(std::byte{0});
    invalid(shark::net::decode_frame(trailing), "net.wire.decode");
    trailing.insert(trailing.end(), bytes.begin(), bytes.end());
    invalid(shark::net::decode_frame(trailing), "net.wire.decode");
}

TEST_CASE("incremental wire decoding handles every two fragment split", "[net-wire]") {
    for (const auto& frame :
         std::array{Frame{MessageKind::hello, 1, {}}, Frame{MessageKind::probe, 2, payload(31)}}) {
        const auto bytes = encode(frame);
        for (std::size_t split = 0; split <= bytes.size(); ++split) {
            FrameDecoder decoder;
            const auto first = decoder.consume(std::span{bytes}.first(split));
            REQUIRE(first);
            CHECK(first.value().consumed == split);
            if (split == bytes.size()) {
                REQUIRE(first.value().frame);
                CHECK(*first.value().frame == frame);
            } else {
                CHECK_FALSE(first.value().frame);
                CHECK(decoder.buffered_bytes() == split);
            }
            const auto second = decoder.consume(std::span{bytes}.subspan(split));
            REQUIRE(second);
            CHECK(second.value().consumed == bytes.size() - split);
            if (split < bytes.size()) {
                REQUIRE(second.value().frame);
                CHECK(*second.value().frame == frame);
            } else {
                CHECK_FALSE(second.value().frame);
            }
            CHECK(decoder.buffered_bytes() == 0);
            CHECK_FALSE(decoder.failed());
            CHECK(decoder.finish());
        }
    }
}

TEST_CASE("incremental wire decoding bounds a maximum body across irregular fragments",
          "[net-wire]") {
    const Frame expected{MessageKind::probe, 1, payload(shark::net::wire_max_payload)};
    const auto bytes = encode(expected);
    constexpr std::array<std::size_t, 6> sizes{1, 2, 19, 3, 127, 509};
    FrameDecoder decoder;
    std::size_t offset = 0;
    std::size_t fragment = 0;
    while (offset < bytes.size()) {
        const auto count = std::min(sizes[fragment++ % sizes.size()], bytes.size() - offset);
        const auto step = decoder.consume(std::span{bytes}.subspan(offset, count));
        REQUIRE(step);
        REQUIRE(step.value().consumed == count);
        offset += count;
        CHECK(decoder.buffered_bytes() <= shark::net::wire_max_frame_bytes);
        if (offset == bytes.size()) {
            REQUIRE(step.value().frame);
            CHECK(*step.value().frame == expected);
        } else {
            CHECK_FALSE(step.value().frame);
            CHECK(decoder.buffered_bytes() == offset);
        }
        const auto idle = decoder.consume({});
        REQUIRE(idle);
        CHECK(idle.value().consumed == 0);
        CHECK_FALSE(idle.value().frame);
    }
    CHECK(decoder.finish());
}

TEST_CASE("coalesced stream decoding consumes one frame and retains independent results",
          "[net-wire]") {
    const std::array frames{Frame{MessageKind::hello, 1, {}},
                            Frame{MessageKind::hello_ack, 1, {}},
                            Frame{MessageKind::probe, 2, payload(4096)},
                            Frame{MessageKind::probe_reply, 2, payload(4096)},
                            Frame{MessageKind::probe, 3, {}},
                            Frame{MessageKind::probe_reply, 3, payload(1)}};
    Bytes coalesced;
    for (const auto& frame : frames) {
        const auto bytes = encode(frame);
        coalesced.insert(coalesced.end(), bytes.begin(), bytes.end());
    }
    FrameDecoder decoder;
    std::vector<Frame> owned;
    std::size_t offset = 0;
    for (const auto& frame : frames) {
        auto step = decoder.consume(std::span{coalesced}.subspan(offset));
        REQUIRE(step);
        REQUIRE(step.value().frame);
        CHECK(step.value().consumed == shark::net::wire_header_bytes + frame.payload.size());
        CHECK(*step.value().frame == frame);
        offset += step.value().consumed;
        owned.push_back(std::move(*step.value().frame));
        CHECK(decoder.buffered_bytes() == 0);
    }
    CHECK(offset == coalesced.size());
    CHECK(decoder.finish());
    coalesced.clear();
    CHECK(std::equal(owned.begin(), owned.end(), frames.begin(), frames.end()));
}

TEST_CASE("truncated EOF and malformed frames poison only their decoder", "[net-wire]") {
    const auto valid = encode({MessageKind::probe, 1, payload(8)});
    for (std::size_t size = 1; size < valid.size(); ++size) {
        FrameDecoder decoder;
        const auto step = decoder.consume(std::span{valid}.first(size));
        REQUIRE(step);
        CHECK_FALSE(step.value().frame);
        invalid(decoder.finish(), "net.wire.finish");
        CHECK(decoder.failed());
        invalid(decoder.consume(valid), "net.wire.decode");
    }
    auto bad = valid;
    bad[0] = std::byte{0};
    FrameDecoder failed;
    invalid(failed.consume(bad), "net.wire.decode");
    invalid(failed.consume(valid), "net.wire.decode");
    FrameDecoder fresh;
    CHECK(fresh.finish());
    const auto recovered = fresh.consume(valid);
    REQUIRE(recovered);
    REQUIRE(recovered.value().frame);
    CHECK(fresh.finish());
}

TEST_CASE("wire sequence directions require exact progression and reject zero gaps and replay",
          "[net-wire]") {
    std::uint64_t sender = 0;
    std::uint64_t receiver = 0;
    for (std::uint64_t sequence = 1; sequence <= 32; ++sequence) {
        const auto outgoing = shark::net::next_sequence(sender);
        REQUIRE(outgoing);
        CHECK(outgoing.value() == sequence);
        const auto incoming = shark::net::check_sequence(receiver, outgoing.value());
        REQUIRE(incoming);
        CHECK(incoming.value() == sequence);
        sender = outgoing.value();
        receiver = incoming.value();
        conflict(shark::net::check_sequence(receiver, sequence));
        conflict(shark::net::check_sequence(receiver, sequence + 2));
        if (sequence > 1) {
            conflict(shark::net::check_sequence(receiver, sequence - 1));
        }
        invalid(shark::net::check_sequence(receiver, 0), "net.sequence");
    }
    conflict(shark::net::check_sequence(0, 2));
    invalid(shark::net::check_sequence(0, 0), "net.sequence");
    CHECK(shark::net::check_sequence(0, 1));
}

TEST_CASE("wire sequence exhaustion never wraps and a new transport starts independently",
          "[net-wire]") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto last_outgoing = shark::net::next_sequence(maximum - 1);
    REQUIRE(last_outgoing);
    CHECK(last_outgoing.value() == maximum);
    const auto last_incoming = shark::net::check_sequence(maximum - 1, maximum);
    REQUIRE(last_incoming);
    CHECK(last_incoming.value() == maximum);
    conflict(shark::net::next_sequence(maximum));
    conflict(shark::net::check_sequence(maximum, maximum));
    conflict(shark::net::check_sequence(maximum, 1));
    invalid(shark::net::check_sequence(maximum, 0), "net.sequence");
    const auto fresh = shark::net::check_sequence(0, 1);
    REQUIRE(fresh);
    CHECK(fresh.value() == 1);
    // The helper is pure: this does not reset or authorize the exhausted stream.
    conflict(shark::net::next_sequence(maximum));
}
