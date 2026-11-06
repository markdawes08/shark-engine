#pragma once

#include "shark/math.hpp"
#include "shark/net/session.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace shark::net {

using MovementStreamId = TypedId<struct MovementStreamIdDomain>;
inline constexpr std::uint16_t movement_protocol_version = 1;
inline constexpr std::int32_t movement_axis_scale = 1000;
inline constexpr std::size_t movement_input_bytes = 80;
inline constexpr std::size_t movement_state_bytes = 108;

// Direction only, in bounded Scene X/Z axes. The authoritative host chooses
// speed and fixed duration. Session admission and the movement stream's input
// sequence are separate; a decoded value grants no authority.
struct MovementInput {
    SessionEnvelope authority;
    MovementStreamId stream;
    std::uint64_t input_sequence;
    std::int16_t x;
    std::int16_t z;
    bool operator==(const MovementInput&) const = default;
};

// Owned correction for this session's actor. No Scene/native handle, durable
// identity, source data or authority grant is serialized. Initial tick/input may
// be zero. Position is Scene-local; it is not a global WorldPosition.
struct MovementState {
    SessionId session;
    std::uint64_t ownership_epoch;
    MovementStreamId stream;
    std::uint64_t completed_tick;
    std::uint64_t last_input;
    math::Vec3 position;
    math::Vec3 velocity;
    bool operator==(const MovementState&) const = default;
};
struct MovementRejected {
    bool operator==(const MovementRejected&) const = default;
};
using MovementReply = std::variant<MovementState, MovementRejected>;

[[nodiscard]] Result<void> validate_movement_input(const MovementInput& input);
[[nodiscard]] Result<void> validate_movement_state(const MovementState& state);

// Protocol1 prefix: version u16 BE, kind u8, reserved-zero u8. Input kind1:
// SessionEnvelope, stream UUID, input u64, x/z signed two's-complement int16 BE.
// Reply kind1: session UUID, ownership u64, stream UUID, tick/input u64,
// position/velocity xyz IEEE754 binary64 BE. Reply kind2 is only the prefix and
// discloses no internal rejection detail. UUIDs use their canonical16 bytes;
// float zero must be positive. Exact lengths reject truncation/trailing data.
[[nodiscard]] Result<std::vector<std::byte>> encode_movement_input(const MovementInput& input);
[[nodiscard]] Result<MovementInput> decode_movement_input(std::span<const std::byte> bytes);
[[nodiscard]] Result<std::vector<std::byte>> encode_movement_reply(const MovementReply& reply);
[[nodiscard]] Result<MovementReply> decode_movement_reply(std::span<const std::byte> bytes);

} // namespace shark::net
