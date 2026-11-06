#pragma once

#include "shark/error.hpp"
#include "shark/math.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace shark {

inline constexpr std::size_t world_geometry_maximum_vertices = 4096;
inline constexpr std::size_t world_geometry_maximum_collision_triangles = 2048;
inline constexpr std::size_t world_geometry_maximum_navigation_triangles = 256;
inline constexpr std::size_t world_geometry_maximum_decoded_bytes = 256 * 1024;
inline constexpr double world_geometry_grid_units_per_meter = 1024.0;

// Explicit engine-owned cell-local surfaces. Navigation selects oriented
// collision triangles; it is a point/centerline surface, not a clearance bake.
// Vertices use the exact 1/1024-meter grid inside the inclusive cell cube.
struct WorldCellGeometry {
    std::vector<math::Vec3> vertices;
    std::vector<std::array<std::uint32_t, 3>> collision_triangles;
    std::vector<std::array<std::uint32_t, 3>> navigation_triangles;
    bool operator==(const WorldCellGeometry&) const = default;
};

[[nodiscard]] Result<void> validate_world_cell_geometry(const WorldCellGeometry& geometry);

} // namespace shark
