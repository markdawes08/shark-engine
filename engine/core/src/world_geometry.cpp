#include "shark/world_geometry.hpp"

#include "shark/world_position.hpp"

#include <algorithm>
#include <cmath>
#include <set>

namespace shark {
namespace {
using Point = std::array<std::int32_t, 3>;
using Face = std::array<Point, 3>;
using Triangle = std::array<std::uint32_t, 3>;

Error invalid(const char* message) {
    return {ErrorCode::invalid_configuration, message, {"world.geometry.validate"}};
}

Triangle oriented(Triangle triangle) {
    const auto minimum = std::min_element(triangle.begin(), triangle.end());
    std::rotate(triangle.begin(), minimum, triangle.end());
    return triangle;
}
} // namespace

Result<void> validate_world_cell_geometry(const WorldCellGeometry& geometry) {
    try {
        if (geometry.vertices.empty() || geometry.collision_triangles.empty() ||
            geometry.vertices.size() > world_geometry_maximum_vertices ||
            geometry.collision_triangles.size() > world_geometry_maximum_collision_triangles ||
            geometry.navigation_triangles.size() > world_geometry_maximum_navigation_triangles)
            return invalid("World geometry exceeds its nonempty bounded profile");
        std::vector<Point> points;
        points.reserve(geometry.vertices.size());
        for (const auto& vertex : geometry.vertices) {
            Point point{};
            for (int axis = 0; axis < 3; ++axis) {
                const auto value = vertex[axis];
                const auto units = value * world_geometry_grid_units_per_meter;
                if (!std::isfinite(value) || value < 0.0 || value > world_cell_size_meters ||
                    std::trunc(units) != units)
                    return invalid("World geometry vertices must fit the exact cell-local grid");
                point[static_cast<std::size_t>(axis)] = static_cast<std::int32_t>(units);
            }
            points.push_back(point);
        }
        std::set<Face> faces;
        std::set<Triangle> collision;
        for (const auto& triangle : geometry.collision_triangles) {
            for (const auto index : triangle)
                if (index >= geometry.vertices.size())
                    return invalid("World geometry triangle index is outside its vertex array");
            const auto& a = geometry.vertices[triangle[0]];
            const auto& b = geometry.vertices[triangle[1]];
            const auto& c = geometry.vertices[triangle[2]];
            const auto normal = glm::cross(b - a, c - a);
            const auto packed =
                glm::cross(glm::vec3{b} - glm::vec3{a}, glm::vec3{c} - glm::vec3{a});
            if (glm::dot(normal, normal) == 0.0 || glm::dot(packed, packed) == 0.0F)
                return invalid("World geometry triangle is degenerate before or after packing");
            Face face{points[triangle[0]], points[triangle[1]], points[triangle[2]]};
            std::sort(face.begin(), face.end());
            if (!faces.insert(face).second)
                return invalid("World geometry contains duplicate geometric triangles");
            collision.insert(oriented(triangle));
        }
        std::set<Triangle> selected;
        for (const auto& triangle : geometry.navigation_triangles) {
            const auto key = oriented(triangle);
            if (!collision.contains(key) || !selected.insert(key).second)
                return invalid("Navigation triangles must be unique oriented collision subsets");
            const auto normal =
                glm::cross(geometry.vertices[triangle[1]] - geometry.vertices[triangle[0]],
                           geometry.vertices[triangle[2]] - geometry.vertices[triangle[0]]);
            if (normal.y <= 0.0 || normal.y * normal.y < normal.x * normal.x + normal.z * normal.z)
                return invalid(
                    "Navigation triangles must face upward with slope at most 45 degrees");
        }
        return {};
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot validate bounded world geometry",
                     {"world.geometry.validate"}};
    }
}

} // namespace shark
