#include "shark/world_navigation.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace shark {
namespace {
constexpr double surface_tolerance = 1e-7;

struct Failure {
    Error error;
};
[[noreturn]] void fail(const char* message, ErrorCode code = ErrorCode::invalid_configuration) {
    throw Failure{Error{code, message, {}}};
}
template <typename T> T require_value(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
void require_value(Result<void> result) {
    if (!result)
        throw Failure{std::move(result).error()};
}
template <typename T, typename F> Result<T> boundary(const char* operation, F&& function) {
    try {
        return function();
    } catch (Failure& failure) {
        failure.error.context.operation = operation;
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed, "World navigation operation failed", {operation}};
    }
}
std::uint64_t fresh_id() {
    static std::atomic<std::uint64_t> next{1};
    auto value = next.load(std::memory_order_relaxed);
    while (value != std::numeric_limits<std::uint64_t>::max())
        if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
            return value;
    fail("World navigation identity exhausted", ErrorCode::conflict);
}
math::Vec3 local(const WorldPosition& value, const WorldLocalFrame& frame) {
    const auto result = require_value(
        world_position_relative_to(value, frame.origin, world_local_coordinate_limit));
    return {result[0], result[1], result[2]};
}
WorldPosition global(const math::Vec3& value, const WorldLocalFrame& frame) {
    return require_value(world_position_from_local(frame.origin, {value.x, value.y, value.z},
                                                   world_local_coordinate_limit));
}
double orient(const math::Vec3& a, const math::Vec3& b, const math::Vec3& c) {
    return (b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x);
}
bool strictly_inside(const math::Vec3& point, const std::array<math::Vec3, 3>& triangle) {
    const auto a = orient(triangle[0], triangle[1], point);
    const auto b = orient(triangle[1], triangle[2], point);
    const auto c = orient(triangle[2], triangle[0], point);
    return (a > 0.0 && b > 0.0 && c > 0.0) || (a < 0.0 && b < 0.0 && c < 0.0);
}
bool proper_crossing(const math::Vec3& a, const math::Vec3& b, const math::Vec3& c,
                     const math::Vec3& d) {
    const auto ab_c = orient(a, b, c);
    const auto ab_d = orient(a, b, d);
    const auto cd_a = orient(c, d, a);
    const auto cd_b = orient(c, d, b);
    return ((ab_c < 0.0 && ab_d > 0.0) || (ab_c > 0.0 && ab_d < 0.0)) &&
           ((cd_a < 0.0 && cd_b > 0.0) || (cd_a > 0.0 && cd_b < 0.0));
}
bool overlap(const std::array<math::Vec3, 3>& a, const std::array<math::Vec3, 3>& b) {
    for (std::size_t i = 0; i < 3; ++i) {
        if (strictly_inside(a[i], b) || strictly_inside(b[i], a))
            return true;
        for (std::size_t j = 0; j < 3; ++j)
            if (proper_crossing(a[i], a[(i + 1) % 3], b[j], b[(j + 1) % 3]))
                return true;
    }
    // Coincident projected boundaries (including different heights) need not
    // contain a strictly interior vertex. A centroid detects their positive area.
    return strictly_inside((a[0] + a[1] + a[2]) / 3.0, b) ||
           strictly_inside((b[0] + b[1] + b[2]) / 3.0, a);
}
bool contains(const std::array<math::Vec3, 3>& triangle, const math::Vec3& point) {
    const auto denominator = orient(triangle[0], triangle[1], triangle[2]);
    const auto a = orient(triangle[1], triangle[2], point) / denominator;
    const auto b = orient(triangle[2], triangle[0], point) / denominator;
    const auto c = 1.0 - a - b;
    // Only the fixed numerical membership allowance is admitted. It does not
    // weld topology, project the point onto a surface or choose another floor.
    const std::array weights{a, b, c};
    for (std::size_t index = 0; index < 3; ++index) {
        const auto edge = triangle[(index + 2) % 3] - triangle[(index + 1) % 3];
        const auto edge_length = std::hypot(edge.x, edge.z);
        if (weights[index] < -surface_tolerance * edge_length / std::abs(denominator))
            return false;
    }
    const auto height = a * triangle[0].y + b * triangle[1].y + c * triangle[2].y;
    return std::abs(point.y - height) <= surface_tolerance;
}
bool on_segment(const math::Vec3& point, const math::Vec3& a, const math::Vec3& b) {
    const auto delta = b - a;
    const auto length_squared = glm::dot(delta, delta);
    if (length_squared == 0.0)
        return glm::length(point - a) <= surface_tolerance;
    const auto fraction = glm::dot(point - a, delta) / length_squared;
    const auto length = std::sqrt(length_squared);
    if (fraction < -surface_tolerance / length || fraction > 1.0 + surface_tolerance / length)
        return false;
    return glm::length(point - (a + std::clamp(fraction, 0.0, 1.0) * delta)) <= surface_tolerance;
}
using EdgeKey = std::pair<WorldPosition, WorldPosition>;
struct EdgeUse {
    std::size_t node;
    std::size_t edge;
    bool reversed;
};
} // namespace

struct WorldNavigation::Impl {
    struct Node {
        std::array<WorldPosition, 3> positions;
        std::array<math::Vec3, 3> vertices;
        math::Vec3 center;
        std::array<std::optional<std::size_t>, 3> adjacent{};
        std::array<std::optional<WorldPosition>, 3> portals{};
    };
    WorldLocalFrame frame;
    const std::uint64_t id{fresh_id()};
    std::vector<Node> nodes;
    explicit Impl(WorldLocalFrame value) : frame(std::move(value)) {}
};

WorldNavigation::WorldNavigation(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
WorldNavigation::~WorldNavigation() = default;
std::uint64_t WorldNavigation::instance() const noexcept {
    return impl_->id;
}

Result<std::unique_ptr<WorldNavigation>>
WorldNavigation::create(WorldLocalFrame frame, std::span<const WorldNavigationTile> tiles) {
    return boundary<std::unique_ptr<WorldNavigation>>("world.navigation.create", [&] {
        require_value(validate_world_position(frame.origin));
        if (frame.generation == 0 || tiles.size() > world_navigation_maximum_tiles)
            fail("World navigation requires a valid frame and bounded tile inventory");
        auto impl = std::make_unique<Impl>(std::move(frame));
        std::set<WorldCellKey> keys;
        std::set<std::array<WorldPosition, 3>> triangles;
        for (const auto& tile : tiles) {
            if (tile.resident_generation == 0 || tile.key.world != impl->frame.origin.world ||
                !keys.insert(tile.key).second)
                fail("World navigation tile identity/generation is invalid");
            require_value(validate_world_cell_geometry(tile.geometry));
            if (tile.geometry.navigation_triangles.size() >
                world_navigation_maximum_triangles - impl->nodes.size())
                fail("World navigation exceeds its triangle bound");
            const WorldPosition origin{tile.key.world, tile.key.cell, {0.0, 0.0, 0.0}};
            require_value(validate_world_position(origin));
            std::vector<WorldPosition> vertices;
            vertices.reserve(tile.geometry.vertices.size());
            for (const auto& vertex : tile.geometry.vertices)
                vertices.push_back(require_value(world_position_from_local(
                    origin, {vertex.x, vertex.y, vertex.z}, world_cell_size_meters)));
            for (const auto& triangle : tile.geometry.navigation_triangles) {
                std::array positions{vertices[triangle[0]], vertices[triangle[1]],
                                     vertices[triangle[2]]};
                auto ordered = positions;
                std::sort(ordered.begin(), ordered.end());
                if (!triangles.insert(ordered).second)
                    fail("World navigation contains duplicate triangles");
                const std::array local_vertices{local(positions[0], impl->frame),
                                                local(positions[1], impl->frame),
                                                local(positions[2], impl->frame)};
                impl->nodes.push_back(
                    {std::move(positions), local_vertices,
                     (local_vertices[0] + local_vertices[1] + local_vertices[2]) / 3.0});
            }
        }
        std::map<EdgeKey, std::vector<EdgeUse>> edges;
        for (std::size_t node = 0; node < impl->nodes.size(); ++node) {
            const auto& value = impl->nodes[node];
            for (std::size_t previous = 0; previous < node; ++previous)
                if (overlap(value.vertices, impl->nodes[previous].vertices))
                    fail("World navigation surfaces overlap in XZ");
            for (std::size_t edge = 0; edge < 3; ++edge) {
                const auto& a = value.positions[edge];
                const auto& b = value.positions[(edge + 1) % 3];
                const bool reversed = b < a;
                const EdgeKey key = reversed ? EdgeKey{b, a} : EdgeKey{a, b};
                auto& uses = edges[key];
                if (uses.size() == 2)
                    fail("World navigation contains a nonmanifold edge");
                uses.push_back({node, edge, reversed});
            }
        }
        for (const auto& [edge, uses] : edges) {
            if (uses.size() != 2)
                continue;
            if (uses[0].reversed == uses[1].reversed)
                fail("World navigation shared edges have inconsistent orientation");
            const auto a = local(edge.first, impl->frame);
            const auto b = local(edge.second, impl->frame);
            const auto midpoint = global((a + b) * 0.5, impl->frame);
            for (std::size_t index = 0; index < 2; ++index) {
                auto& node = impl->nodes[uses[index].node];
                node.adjacent[uses[index].edge] = uses[1 - index].node;
                node.portals[uses[index].edge] = midpoint;
            }
        }
        return std::unique_ptr<WorldNavigation>(new WorldNavigation{std::move(impl)});
    });
}

Result<WorldNavigationPath> WorldNavigation::path(const WorldPosition& start,
                                                  const WorldPosition& goal,
                                                  std::size_t maximum_expansions,
                                                  std::size_t maximum_waypoints) const {
    return boundary<WorldNavigationPath>("world.navigation.path", [&] {
        if (maximum_expansions == 0 || maximum_expansions > world_navigation_maximum_triangles ||
            maximum_waypoints < 2 || maximum_waypoints > world_navigation_maximum_waypoints)
            fail("World navigation query budgets are invalid", ErrorCode::invalid_argument);
        const auto start_local = local(start, impl_->frame);
        const auto goal_local = local(goal, impl_->frame);
        const auto count = impl_->nodes.size();
        std::vector<double> cost(count, std::numeric_limits<double>::infinity());
        std::vector<bool> open(count, false), closed(count, false), goals(count, false);
        std::vector<std::optional<std::size_t>> parents(count), parent_edges(count);
        bool found_start = false, found_goal = false;
        for (std::size_t index = 0; index < count; ++index) {
            if (contains(impl_->nodes[index].vertices, start_local)) {
                found_start = true;
                open[index] = true;
                cost[index] = glm::length(start_local - impl_->nodes[index].center);
            }
            goals[index] = contains(impl_->nodes[index].vertices, goal_local);
            found_goal = found_goal || goals[index];
        }
        if (!found_start || !found_goal)
            fail("World navigation endpoints must lie on ready walk surfaces",
                 ErrorCode::invalid_argument);
        std::optional<std::size_t> reached;
        std::size_t expanded = 0;
        for (;;) {
            std::optional<std::size_t> current;
            double best = std::numeric_limits<double>::infinity();
            for (std::size_t index = 0; index < count; ++index) {
                if (!open[index])
                    continue;
                const auto estimate =
                    cost[index] + glm::length(impl_->nodes[index].center - goal_local);
                if (!current || estimate < best) {
                    current = index;
                    best = estimate;
                }
            }
            if (!current)
                fail("World navigation surfaces are disconnected", ErrorCode::conflict);
            if (expanded == maximum_expansions)
                fail("World navigation expansion budget exhausted", ErrorCode::conflict);
            ++expanded;
            const auto index = *current;
            open[index] = false;
            if (goals[index]) {
                reached = index;
                break;
            }
            closed[index] = true;
            const auto& node = impl_->nodes[index];
            for (std::size_t edge = 0; edge < 3; ++edge) {
                if (!node.adjacent[edge])
                    continue;
                const auto neighbor = *node.adjacent[edge];
                if (closed[neighbor])
                    continue;
                const auto candidate =
                    cost[index] + glm::length(node.center - impl_->nodes[neighbor].center);
                if (candidate < cost[neighbor]) {
                    cost[neighbor] = candidate;
                    parents[neighbor] = index;
                    parent_edges[neighbor] = edge;
                    open[neighbor] = true;
                }
            }
        }
        std::vector<WorldPosition> portals;
        for (auto index = *reached; parents[index]; index = *parents[index]) {
            if (portals.size() + 2 >= maximum_waypoints)
                fail("World navigation waypoint budget exhausted", ErrorCode::conflict);
            portals.push_back(*impl_->nodes[*parents[index]].portals[*parent_edges[index]]);
        }
        std::vector<WorldPosition> waypoints;
        waypoints.reserve(portals.size() + 2);
        waypoints.push_back(start);
        for (auto it = portals.rbegin(); it != portals.rend(); ++it)
            waypoints.push_back(*it);
        waypoints.push_back(goal);
        return WorldNavigationPath{impl_->id, impl_->frame.generation, fresh_id(),
                                   std::move(waypoints)};
    });
}

Result<WorldNavigationCursor> WorldNavigation::begin(const WorldNavigationPath& path) const {
    return boundary<WorldNavigationCursor>("world.navigation.begin", [&] {
        if (path.instance_ != impl_->id || path.frame_generation_ != impl_->frame.generation)
            fail("World navigation path belongs to a stale graph/frame", ErrorCode::conflict);
        return WorldNavigationCursor{impl_->id, path.id_, 1, path.waypoints_.front()};
    });
}

Result<WorldNavigationCursor> WorldNavigation::advance(const WorldNavigationPath& path,
                                                       const WorldNavigationCursor& cursor,
                                                       double maximum_distance) const {
    return boundary<WorldNavigationCursor>("world.navigation.advance", [&] {
        if (path.instance_ != impl_->id || path.frame_generation_ != impl_->frame.generation ||
            cursor.navigation_instance != impl_->id || cursor.path_id != path.id_)
            fail("World navigation progress belongs to a stale graph/path", ErrorCode::conflict);
        if (!std::isfinite(maximum_distance) || maximum_distance < 0.0 ||
            maximum_distance > world_local_coordinate_limit || cursor.next_waypoint == 0 ||
            cursor.next_waypoint >= path.waypoints_.size())
            fail("World navigation progress or distance is invalid", ErrorCode::invalid_argument);
        auto current = local(cursor.position, impl_->frame);
        auto next = cursor.next_waypoint;
        if (!on_segment(current, local(path.waypoints_[next - 1], impl_->frame),
                        local(path.waypoints_[next], impl_->frame)))
            fail("World navigation progress is outside its route segment",
                 ErrorCode::invalid_argument);
        if (maximum_distance == 0.0)
            return cursor;
        auto remaining = maximum_distance;
        while (next < path.waypoints_.size()) {
            const auto target = local(path.waypoints_[next], impl_->frame);
            const auto difference = target - current;
            const auto distance = glm::length(difference);
            if (distance > remaining) {
                current += difference * (remaining / distance);
                break;
            }
            current = target;
            remaining -= distance;
            if (next + 1 == path.waypoints_.size())
                break;
            ++next;
        }
        return WorldNavigationCursor{impl_->id, path.id_, next, global(current, impl_->frame)};
    });
}

} // namespace shark
