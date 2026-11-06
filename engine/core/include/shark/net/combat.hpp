#pragma once

#include "shark/combat_types.hpp"
#include "shark/net/movement.hpp"

#include <optional>

namespace shark::net {

using CombatStreamId = TypedId<struct CombatStreamIdDomain>;
using CombatTargetId = TypedId<struct CombatTargetIdDomain>;
inline constexpr std::uint16_t combat_protocol_version = 1;
inline constexpr std::size_t combat_action_bytes = 93;
inline constexpr std::size_t combat_maximum_reply_bytes = 2048;
inline constexpr std::size_t combat_maximum_targets = 32;
inline constexpr std::size_t combat_maximum_outcomes = 8;

// Intent only. Server time, rules, actor/target ownership and committed physics
// determine outcomes; no damage, pose, reward or client timestamp is supplied.
struct CombatAction {
    SessionEnvelope authority;
    CombatStreamId stream;
    std::uint64_t action_sequence;
    CombatTargetId target;
    CombatActionKind kind;
    bool operator==(const CombatAction&) const = default;
};
struct CombatTargetState {
    CombatTargetId id;
    std::uint16_t health;
    std::uint64_t respawn_tick;
    bool operator==(const CombatTargetState&) const = default;
};
struct CombatActionResult {
    std::uint64_t action_sequence;
    CombatTargetId target;
    CombatActionKind kind;
    CombatOutcome outcome;
    std::uint16_t amount;
    bool operator==(const CombatActionResult&) const = default;
};
struct CombatState {
    SessionId session;
    std::uint64_t ownership_epoch;
    CombatStreamId stream;
    std::uint64_t completed_tick;
    CombatTargetId self;
    std::uint16_t health;
    std::uint64_t ready_tick;
    std::uint64_t respawn_tick;
    std::optional<MovementState> movement;
    std::vector<CombatTargetState> targets;
    std::vector<CombatActionResult> outcomes;
    bool operator==(const CombatState&) const = default;
};
struct CombatRejected {
    bool operator==(const CombatRejected&) const = default;
};
using CombatReply = std::variant<CombatState, CombatRejected>;

[[nodiscard]] Result<void> validate_combat_action(const CombatAction& action);
[[nodiscard]] Result<void> validate_combat_state(const CombatState& state);
// Independent protocol1, exact BE fields, canonical UUID bytes. Input kind1:
// SessionEnvelope, stream UUID, action u64, target UUID, action kind u8 (93 bytes).
// State kind1 follows CombatState field order; movement flag0/1 optionally embeds
// one exact108-byte movement state. Lists use u8 counts, at most32 targets and8
// outcomes. Rejection kind2 is only the4-byte prefix. No trailing data/layout IO.
[[nodiscard]] Result<std::vector<std::byte>> encode_combat_action(const CombatAction& action);
[[nodiscard]] Result<CombatAction> decode_combat_action(std::span<const std::byte> bytes);
[[nodiscard]] Result<std::vector<std::byte>> encode_combat_reply(const CombatReply& reply);
[[nodiscard]] Result<CombatReply> decode_combat_reply(std::span<const std::byte> bytes);

} // namespace shark::net
