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
inline constexpr double world_local_coordinate_limit = 65536.0;

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

// Pure conversion through one explicit canonical origin. Inputs must share a
// world; no absolute global coordinate is formed. The caller's finite positive
// per-axis bound may not exceed world_local_coordinate_limit. Bounds are
// inclusive and arithmetic requires round-to-nearest. Returned zeros are +0.
// These values do not rebase a Scene or grant simulation authority.
[[nodiscard]] Result<std::array<double, 3>>
world_position_relative_to(const WorldPosition& position, const WorldPosition& origin,
                           double maximum_abs_meters);

// Finite local values within the same per-axis bound translate through the
// explicit origin. The result is canonical and must remain globally representable.
[[nodiscard]] Result<WorldPosition>
world_position_from_local(const WorldPosition& origin, const std::array<double, 3>& local_meters,
                          double maximum_abs_meters);

// A caller-owned frame generation must change whenever its origin changes.
struct WorldLocalFrame {
    WorldPosition origin;
    std::uint64_t generation{0};
    bool operator==(const WorldLocalFrame&) const = default;
};
struct LocalWorldPosition {
    std::array<float, 3> meters{};
    std::uint64_t origin_generation{0};
    bool operator==(const LocalWorldPosition&) const = default;
};

// Explicitly lossy float packing within world_local_coordinate_limit. Generation
// zero rejects; unpack also rejects a stale generation and noncanonical floats.
// A successful packed value stays globally representable through the same frame.
// The generation belongs to this local frame, not durable/network authority.
[[nodiscard]] Result<LocalWorldPosition> pack_world_position(const WorldPosition& position,
                                                             const WorldLocalFrame& frame);
[[nodiscard]] Result<WorldPosition> unpack_world_position(const LocalWorldPosition& position,
                                                          const WorldLocalFrame& frame);

} // namespace shark
