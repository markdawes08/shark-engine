#pragma once

#include "shark/world_cell.hpp"
#include "shark/world_geometry.hpp"

namespace shark {

inline constexpr std::size_t world_navigation_maximum_tiles = 8;
inline constexpr std::size_t world_navigation_maximum_triangles = 2048;
inline constexpr std::size_t world_navigation_maximum_waypoints = 2048;

struct WorldNavigationTile {
    WorldCellKey key;
    std::uint64_t resident_generation;
    WorldCellGeometry geometry;
};

class WorldNavigation;

// Immutable owned centerline route. Each consecutive segment lies within one
// authored walk triangle. It supplies no radius clearance or gameplay authority.
class WorldNavigationPath final {
  public:
    [[nodiscard]] std::span<const WorldPosition> waypoints() const& noexcept {
        return waypoints_;
    }
    std::span<const WorldPosition> waypoints() const&& = delete;
    [[nodiscard]] std::uint64_t navigation_instance() const noexcept {
        return instance_;
    }
    [[nodiscard]] std::uint64_t frame_generation() const noexcept {
        return frame_generation_;
    }

  private:
    friend class WorldNavigation;
    WorldNavigationPath(std::uint64_t instance, std::uint64_t frame_generation, std::uint64_t id,
                        std::vector<WorldPosition> waypoints)
        : instance_(instance), frame_generation_(frame_generation), id_(id),
          waypoints_(std::move(waypoints)) {}
    std::uint64_t instance_;
    std::uint64_t frame_generation_;
    std::uint64_t id_;
    std::vector<WorldPosition> waypoints_;
};

// Value-only progress candidate. A trusted host adopts it only after checking
// actual committed movement; this is not a teleport or a player command.
struct WorldNavigationCursor {
    std::uint64_t navigation_instance;
    std::uint64_t path_id;
    std::size_t next_waypoint;
    WorldPosition position;
};

// Complete immutable graph of explicitly selected, single-layer authored walk
// surfaces. Shared edges match exact canonical WorldPositions, without welding.
// Positive-area XZ overlap rejects, including stacked navigation surfaces.
class WorldNavigation final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<WorldNavigation>>
    create(WorldLocalFrame frame, std::span<const WorldNavigationTile> tiles);
    ~WorldNavigation();
    WorldNavigation(const WorldNavigation&) = delete;
    WorldNavigation& operator=(const WorldNavigation&) = delete;
    WorldNavigation(WorldNavigation&&) = delete;
    WorldNavigation& operator=(WorldNavigation&&) = delete;

    [[nodiscard]] std::uint64_t instance() const noexcept;
    // Vertical-only projection onto the authored single-layer walk surface.
    // No horizontal nearest-point search or clearance/collision authority.
    [[nodiscard]] Result<WorldPosition>
    project_walk_surface(WorldPosition point, double maximum_vertical_delta = 1.0 / 1024.0) const;
    [[nodiscard]] Result<WorldNavigationPath>
    path(const WorldPosition& start, const WorldPosition& goal,
         std::size_t maximum_expansions = world_navigation_maximum_triangles,
         std::size_t maximum_waypoints = world_navigation_maximum_waypoints) const;
    [[nodiscard]] Result<WorldNavigationCursor> begin(const WorldNavigationPath& path) const;
    [[nodiscard]] Result<WorldNavigationCursor> advance(const WorldNavigationPath& path,
                                                        const WorldNavigationCursor& cursor,
                                                        double maximum_distance) const;

  private:
    struct Impl;
    explicit WorldNavigation(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark
