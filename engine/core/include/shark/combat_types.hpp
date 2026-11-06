#pragma once

#include <cstdint>

namespace shark {

enum class CombatActionKind : std::uint8_t { strike = 1, interact = 2 };
inline constexpr std::uint16_t combat_maximum_health = 100;
inline constexpr std::uint16_t combat_maximum_damage = 25;
inline constexpr std::uint32_t combat_strike_cooldown = 10;
inline constexpr std::uint32_t combat_interaction_cooldown = 5;
inline constexpr std::uint32_t combat_maximum_cooldown = 200;
inline constexpr std::uint32_t combat_respawn_ticks = 60;
inline constexpr double combat_strike_range = 2.5;
inline constexpr double combat_interaction_range = 3.0;

enum class CombatOutcome : std::uint8_t {
    hit = 1,
    interacted = 2,
    cooldown = 3,
    out_of_range = 4,
    blocked = 5,
    target_unavailable = 6,
    denied = 7,
    actor_unavailable = 8
};

} // namespace shark
