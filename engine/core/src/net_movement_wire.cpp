#include "shark/net/movement.hpp"

#include "shark/physics.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <string_view>

namespace shark::net {
namespace {
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

Error invalid(const char* operation) {
    return {
        ErrorCode::invalid_argument, "Invalid movement protocol value", {.operation = operation}};
}
void append_integer(std::vector<std::byte>& bytes, std::uint64_t value, unsigned width = 8) {
    for (unsigned index = 0; index < width; ++index)
        bytes.push_back(static_cast<std::byte>((value >> ((width - index - 1U) * 8U)) & 0xffU));
}
std::uint64_t read_integer(std::span<const std::byte> bytes, std::size_t offset,
                           unsigned width = 8) {
    std::uint64_t value = 0;
    for (unsigned index = 0; index < width; ++index)
        value = (value << 8U) | std::to_integer<std::uint64_t>(bytes[offset + index]);
    return value;
}
void append_id(std::vector<std::byte>& bytes, const StableId& id) {
    bytes.insert(bytes.end(), id.bytes().begin(), id.bytes().end());
}
Result<StableId> read_id(std::span<const std::byte> bytes, std::size_t offset) {
    constexpr char digits[] = "0123456789abcdef";
    std::array<char, 36> text{};
    std::size_t position = 0;
    for (std::size_t index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            text[position++] = '-';
        const auto byte = std::to_integer<unsigned>(bytes[offset + index]);
        text[position++] = digits[byte >> 4U];
        text[position++] = digits[byte & 0xfU];
    }
    return StableId::parse(std::string_view{text.data(), text.size()});
}
std::vector<std::byte> prefix(std::uint8_t kind, std::size_t capacity) {
    std::vector<std::byte> bytes;
    bytes.reserve(capacity);
    append_integer(bytes, movement_protocol_version, 2);
    bytes.push_back(static_cast<std::byte>(kind));
    bytes.push_back(std::byte{0});
    return bytes;
}
bool header(std::span<const std::byte> bytes, std::byte kind) {
    return bytes.size() >= 4 && bytes[0] == std::byte{0} && bytes[1] == std::byte{1} &&
           bytes[2] == kind && bytes[3] == std::byte{0};
}
bool canonical(double value, double bound) {
    return std::isfinite(value) && std::abs(value) <= bound && !(value == 0 && std::signbit(value));
}
void append_vector(std::vector<std::byte>& bytes, math::Vec3 value) {
    append_integer(bytes, std::bit_cast<std::uint64_t>(value.x));
    append_integer(bytes, std::bit_cast<std::uint64_t>(value.y));
    append_integer(bytes, std::bit_cast<std::uint64_t>(value.z));
}
math::Vec3 read_vector(std::span<const std::byte> bytes, std::size_t offset) {
    return {std::bit_cast<double>(read_integer(bytes, offset)),
            std::bit_cast<double>(read_integer(bytes, offset + 8)),
            std::bit_cast<double>(read_integer(bytes, offset + 16))};
}
} // namespace

Result<void> validate_movement_input(const MovementInput& input) {
    const auto x = static_cast<std::int64_t>(input.x);
    const auto z = static_cast<std::int64_t>(input.z);
    if (input.authority.ownership_epoch == 0 || input.authority.sequence == 0 ||
        input.input_sequence == 0 || x * x + z * z > movement_axis_scale * movement_axis_scale)
        return invalid("net.movement.input");
    return {};
}

Result<void> validate_movement_state(const MovementState& state) {
    if (state.ownership_epoch == 0 || !canonical(state.position.x, physics_local_position_limit) ||
        !canonical(state.position.y, physics_local_position_limit) ||
        !canonical(state.position.z, physics_local_position_limit) ||
        !canonical(state.velocity.x, physics_maximum_linear_speed) ||
        !canonical(state.velocity.y, physics_maximum_linear_speed) ||
        !canonical(state.velocity.z, physics_maximum_linear_speed) ||
        glm::dot(state.velocity, state.velocity) >
            physics_maximum_linear_speed * physics_maximum_linear_speed)
        return invalid("net.movement.state");
    return {};
}

Result<std::vector<std::byte>> encode_movement_input(const MovementInput& input) {
    if (auto checked = validate_movement_input(input); !checked)
        return checked.error();
    auto bytes = prefix(1, movement_input_bytes);
    append_id(bytes, input.authority.session.value());
    append_integer(bytes, input.authority.ownership_epoch);
    append_integer(bytes, input.authority.sequence);
    append_id(bytes, input.authority.character.value());
    append_id(bytes, input.stream.value());
    append_integer(bytes, input.input_sequence);
    append_integer(bytes, std::bit_cast<std::uint16_t>(input.x), 2);
    append_integer(bytes, std::bit_cast<std::uint16_t>(input.z), 2);
    return bytes;
}

Result<MovementInput> decode_movement_input(std::span<const std::byte> bytes) {
    constexpr auto operation = "net.movement.decode-input";
    if (bytes.size() != movement_input_bytes || !header(bytes, std::byte{1}))
        return invalid(operation);
    auto session = read_id(bytes, 4);
    auto character = read_id(bytes, 36);
    auto stream = read_id(bytes, 52);
    if (!session || !character || !stream)
        return invalid(operation);
    MovementInput input{
        {SessionId{session.value()}, read_integer(bytes, 20), read_integer(bytes, 28),
         CharacterId{character.value()}},
        MovementStreamId{stream.value()},
        read_integer(bytes, 68),
        std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(read_integer(bytes, 76, 2))),
        std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(read_integer(bytes, 78, 2)))};
    if (auto checked = validate_movement_input(input); !checked)
        return checked.error();
    return input;
}

Result<std::vector<std::byte>> encode_movement_reply(const MovementReply& reply) {
    if (std::holds_alternative<MovementRejected>(reply))
        return prefix(2, 4);
    const auto* state = std::get_if<MovementState>(&reply);
    if (!state)
        return invalid("net.movement.encode-reply");
    if (auto checked = validate_movement_state(*state); !checked)
        return checked.error();
    auto bytes = prefix(1, movement_state_bytes);
    append_id(bytes, state->session.value());
    append_integer(bytes, state->ownership_epoch);
    append_id(bytes, state->stream.value());
    append_integer(bytes, state->completed_tick);
    append_integer(bytes, state->last_input);
    append_vector(bytes, state->position);
    append_vector(bytes, state->velocity);
    return bytes;
}

Result<MovementReply> decode_movement_reply(std::span<const std::byte> bytes) {
    constexpr auto operation = "net.movement.decode-reply";
    if (bytes.size() == 4 && header(bytes, std::byte{2}))
        return MovementReply{MovementRejected{}};
    if (bytes.size() != movement_state_bytes || !header(bytes, std::byte{1}))
        return invalid(operation);
    auto session = read_id(bytes, 4);
    auto stream = read_id(bytes, 28);
    if (!session || !stream)
        return invalid(operation);
    MovementState state{
        SessionId{session.value()}, read_integer(bytes, 20), MovementStreamId{stream.value()},
        read_integer(bytes, 44),    read_integer(bytes, 52), read_vector(bytes, 60),
        read_vector(bytes, 84)};
    if (auto checked = validate_movement_state(state); !checked)
        return checked.error();
    return MovementReply{state};
}

} // namespace shark::net
