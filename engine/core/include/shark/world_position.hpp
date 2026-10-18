#pragma once

#include "shark/scene_identity.hpp"

#include <array>
#include <cstdint>

namespace shark {

// Representation contract, not a supported content extent or capacity claim.
inline constexpr std::uint32_t world_position_version = 1;
inline constexpr double world_cell_size_meters = 1024.0;
inline constexpr std::int64_t world_cell_min = -2'147'483'648LL;
inline constexpr std::int64_t world_cell_max = 2'147'483'647LL;

struct WorldCell {
    std::int64_t x{0};
    std::int64_t y{0};
    std::int64_t z{0};
    auto operator<=>(const WorldCell&) const = default;
};

// An owned value candidate: validate before use. Offsets are meters, in x/y/z
// order. Canonical offsets are finite [0, 1024), with positive zero only.
struct WorldPosition {
    WorldId world;
    WorldCell cell;
    std::array<double, 3> offset;
    auto operator<=>(const WorldPosition&) const = default;
};

[[nodiscard]] Result<void> validate_world_position(const WorldPosition& position);

// Input cells must already be within the representation bounds. Finite offsets
// carry into cells; a result outside those bounds rejects. Arithmetic requires
// the default round-to-nearest mode. A rounded remainder of 1024 carries to the
// next cell at +0, including negative offsets too small to represent near 1024.
[[nodiscard]] Result<WorldPosition> normalize_world_position(WorldPosition position);

// Requires canonical input. Splits delta into integer cells and a signed local
// remainder before adding, preserving local detail at large cell coordinates.
// Does not change the world ID or provide render/physics origin conversion.
[[nodiscard]] Result<WorldPosition>
translate_world_position(const WorldPosition& position, const std::array<double, 3>& delta_meters);

} // namespace shark
