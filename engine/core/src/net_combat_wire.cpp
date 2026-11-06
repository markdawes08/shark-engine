#include "shark/net/combat.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace shark::net {
namespace {
Error invalid(const char* operation) {
    return {ErrorCode::invalid_argument, "Invalid combat protocol value", {.operation = operation}};
}
void append(std::vector<std::byte>& bytes, std::uint64_t value, unsigned width = 8) {
    for (unsigned index = 0; index < width; ++index)
        bytes.push_back(static_cast<std::byte>((value >> ((width - index - 1U) * 8U)) & 0xffU));
}
std::uint64_t read(std::span<const std::byte> bytes, std::size_t offset, unsigned width = 8) {
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
        const auto value = std::to_integer<unsigned>(bytes[offset + index]);
        text[position++] = digits[value >> 4U];
        text[position++] = digits[value & 15U];
    }
    return StableId::parse(std::string_view{text.data(), text.size()});
}
std::vector<std::byte> prefix(std::uint8_t kind, std::size_t capacity = 4,
                              std::uint16_t version = combat_legacy_protocol_version) {
    std::vector<std::byte> bytes;
    bytes.reserve(capacity);
    append(bytes, version, 2);
    append(bytes, kind, 1);
    append(bytes, 0, 1);
    return bytes;
}
bool header(std::span<const std::byte> bytes, std::byte kind) {
    return bytes.size() >= 4 && bytes[0] == std::byte{0} &&
           (bytes[1] == std::byte{combat_legacy_protocol_version} ||
            bytes[1] == std::byte{combat_protocol_version}) &&
           bytes[2] == kind && bytes[3] == std::byte{0};
}
bool action_kind(CombatActionKind kind) {
    return kind == CombatActionKind::strike || kind == CombatActionKind::interact ||
           kind == CombatActionKind::projectile;
}
bool health(std::uint16_t amount, std::uint64_t respawn_tick) {
    return amount <= combat_maximum_health && (amount == 0 ? respawn_tick != 0 : respawn_tick == 0);
}
bool result(const CombatActionResult& outcome) {
    const auto code = static_cast<std::uint8_t>(outcome.outcome);
    if (outcome.action_sequence == 0 || !action_kind(outcome.kind) || code < 1 || code > 8)
        return false;
    if (outcome.outcome == CombatOutcome::hit)
        return outcome.kind != CombatActionKind::interact && outcome.amount != 0 &&
               outcome.amount <= combat_maximum_damage;
    if (outcome.outcome == CombatOutcome::interacted)
        return outcome.kind == CombatActionKind::interact && outcome.amount == 0;
    return outcome.amount == 0;
}
} // namespace

Result<void> validate_combat_action(const CombatAction& action) {
    if (action.authority.ownership_epoch == 0 || action.authority.sequence == 0 ||
        action.action_sequence == 0 || !action_kind(action.kind))
        return invalid("net.combat.action");
    return {};
}

Result<void> validate_combat_state(const CombatState& state) {
    constexpr auto operation = "net.combat.state";
    if (state.ownership_epoch == 0 || !health(state.health, state.respawn_tick) ||
        state.targets.size() > combat_maximum_targets ||
        state.outcomes.size() > combat_maximum_outcomes ||
        (state.health == 0) == state.movement.has_value())
        return invalid(operation);
    if (state.movement &&
        (!validate_movement_state(*state.movement) || state.movement->session != state.session ||
         state.movement->ownership_epoch != state.ownership_epoch ||
         state.movement->completed_tick != state.completed_tick))
        return invalid(operation);
    const CombatTargetId* previous = nullptr;
    for (const auto& target : state.targets) {
        if (target.id == state.self || (previous && target.id <= *previous) ||
            !health(target.health, target.respawn_tick))
            return invalid(operation);
        previous = &target.id;
    }
    std::uint64_t prior_sequence = 0;
    for (const auto& outcome : state.outcomes) {
        if (!result(outcome) ||
            (prior_sequence != 0 && (prior_sequence == std::numeric_limits<std::uint64_t>::max() ||
                                     outcome.action_sequence != prior_sequence + 1)))
            return invalid(operation);
        prior_sequence = outcome.action_sequence;
    }
    return {};
}

Result<std::vector<std::byte>> encode_combat_action(const CombatAction& action) {
    if (auto valid = validate_combat_action(action); !valid)
        return valid.error();
    auto bytes =
        prefix(1, combat_action_bytes,
               action.kind == CombatActionKind::projectile ? combat_protocol_version
                                                           : combat_legacy_protocol_version);
    append_id(bytes, action.authority.session.value());
    append(bytes, action.authority.ownership_epoch);
    append(bytes, action.authority.sequence);
    append_id(bytes, action.authority.character.value());
    append_id(bytes, action.stream.value());
    append(bytes, action.action_sequence);
    append_id(bytes, action.target.value());
    append(bytes, static_cast<std::uint8_t>(action.kind), 1);
    return bytes;
}

Result<CombatAction> decode_combat_action(std::span<const std::byte> bytes) {
    constexpr auto operation = "net.combat.decode-action";
    if (bytes.size() != combat_action_bytes || !header(bytes, std::byte{1}))
        return invalid(operation);
    auto session = read_id(bytes, 4);
    auto character = read_id(bytes, 36);
    auto stream = read_id(bytes, 52);
    auto target = read_id(bytes, 76);
    if (!session || !character || !stream || !target)
        return invalid(operation);
    CombatAction action{{SessionId{session.value()}, read(bytes, 20), read(bytes, 28),
                         CharacterId{character.value()}},
                        CombatStreamId{stream.value()},
                        read(bytes, 68),
                        CombatTargetId{target.value()},
                        static_cast<CombatActionKind>(read(bytes, 92, 1))};
    if (auto valid = validate_combat_action(action); !valid)
        return valid.error();
    if (action.kind == CombatActionKind::projectile && read(bytes, 0, 2) != combat_protocol_version)
        return invalid(operation);
    return action;
}

Result<std::vector<std::byte>> encode_combat_reply(const CombatReply& reply) {
    if (std::holds_alternative<CombatRejected>(reply))
        return prefix(2);
    const auto* state = std::get_if<CombatState>(&reply);
    if (!state)
        return invalid("net.combat.encode-reply");
    if (auto valid = validate_combat_state(*state); !valid)
        return valid.error();
    const auto version = std::any_of(state->outcomes.begin(), state->outcomes.end(),
                                     [](const auto& outcome) {
                                         return outcome.kind == CombatActionKind::projectile;
                                     })
                             ? combat_protocol_version
                             : combat_legacy_protocol_version;
    auto bytes = prefix(1,
                        89 + (state->movement ? movement_state_bytes : 0) +
                            state->targets.size() * 26 + state->outcomes.size() * 28,
                        version);
    append_id(bytes, state->session.value());
    append(bytes, state->ownership_epoch);
    append_id(bytes, state->stream.value());
    append(bytes, state->completed_tick);
    append_id(bytes, state->self.value());
    append(bytes, state->health, 2);
    append(bytes, state->ready_tick);
    append(bytes, state->respawn_tick);
    append(bytes, state->movement.has_value() ? 1 : 0, 1);
    if (state->movement) {
        auto movement = encode_movement_reply(MovementReply{*state->movement});
        if (!movement)
            return movement.error();
        bytes.insert(bytes.end(), movement.value().begin(), movement.value().end());
    }
    append(bytes, state->targets.size(), 1);
    for (const auto& target : state->targets) {
        append_id(bytes, target.id.value());
        append(bytes, target.health, 2);
        append(bytes, target.respawn_tick);
    }
    append(bytes, state->outcomes.size(), 1);
    for (const auto& outcome : state->outcomes) {
        append(bytes, outcome.action_sequence);
        append_id(bytes, outcome.target.value());
        append(bytes, static_cast<std::uint8_t>(outcome.kind), 1);
        append(bytes, static_cast<std::uint8_t>(outcome.outcome), 1);
        append(bytes, outcome.amount, 2);
    }
    return bytes;
}

Result<CombatReply> decode_combat_reply(std::span<const std::byte> bytes) {
    constexpr auto operation = "net.combat.decode-reply";
    if (bytes.size() == 4 && header(bytes, std::byte{2}))
        return CombatReply{CombatRejected{}};
    if (bytes.size() < 89 || bytes.size() > combat_maximum_reply_bytes ||
        !header(bytes, std::byte{1}))
        return invalid(operation);
    auto session = read_id(bytes, 4);
    auto stream = read_id(bytes, 28);
    auto self = read_id(bytes, 52);
    const auto movement_flag = read(bytes, 86, 1);
    if (!session || !stream || !self || movement_flag > 1)
        return invalid(operation);
    std::size_t offset = 87;
    std::optional<MovementState> movement;
    if (movement_flag == 1) {
        if (bytes.size() < offset + movement_state_bytes + 2)
            return invalid(operation);
        auto decoded = decode_movement_reply(bytes.subspan(offset, movement_state_bytes));
        if (!decoded || !std::holds_alternative<MovementState>(decoded.value()))
            return invalid(operation);
        movement = std::get<MovementState>(std::move(decoded).value());
        offset += movement_state_bytes;
    }
    const auto target_count = static_cast<std::size_t>(read(bytes, offset++, 1));
    if (target_count > combat_maximum_targets || bytes.size() - offset < target_count * 26 + 1)
        return invalid(operation);
    std::vector<CombatTargetState> targets;
    targets.reserve(target_count);
    for (std::size_t index = 0; index < target_count; ++index) {
        auto id = read_id(bytes, offset);
        if (!id)
            return invalid(operation);
        targets.push_back({CombatTargetId{id.value()},
                           static_cast<std::uint16_t>(read(bytes, offset + 16, 2)),
                           read(bytes, offset + 18)});
        offset += 26;
    }
    const auto outcome_count = static_cast<std::size_t>(read(bytes, offset++, 1));
    if (outcome_count > combat_maximum_outcomes || bytes.size() - offset != outcome_count * 28)
        return invalid(operation);
    std::vector<CombatActionResult> outcomes;
    outcomes.reserve(outcome_count);
    for (std::size_t index = 0; index < outcome_count; ++index) {
        auto target = read_id(bytes, offset + 8);
        if (!target)
            return invalid(operation);
        outcomes.push_back({read(bytes, offset), CombatTargetId{target.value()},
                            static_cast<CombatActionKind>(read(bytes, offset + 24, 1)),
                            static_cast<CombatOutcome>(read(bytes, offset + 25, 1)),
                            static_cast<std::uint16_t>(read(bytes, offset + 26, 2))});
        offset += 28;
    }
    CombatState state{SessionId{session.value()},
                      read(bytes, 20),
                      CombatStreamId{stream.value()},
                      read(bytes, 44),
                      CombatTargetId{self.value()},
                      static_cast<std::uint16_t>(read(bytes, 68, 2)),
                      read(bytes, 70),
                      read(bytes, 78),
                      std::move(movement),
                      std::move(targets),
                      std::move(outcomes)};
    if (auto valid = validate_combat_state(state); !valid)
        return valid.error();
    if (read(bytes, 0, 2) == combat_legacy_protocol_version &&
        std::any_of(state.outcomes.begin(), state.outcomes.end(), [](const auto& outcome) {
            return outcome.kind == CombatActionKind::projectile;
        }))
        return invalid(operation);
    return CombatReply{std::move(state)};
}

} // namespace shark::net
