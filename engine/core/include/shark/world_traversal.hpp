#pragma once

#include "shark/physics.hpp"
#include "shark/world_cell_residency.hpp"
#include "shark/world_navigation.hpp"

namespace shark {

struct WorldTraversalCell {
    WorldCellKey key;
    std::uint64_t resident_generation;
    PhysicsBodyHandle collision;
    WorldEntityId proxy;
};

// Trusted region-host adapter over the existing residency/Scene/physics owners.
// Borrowed owners must outlive this object. Close it before ending simulation or
// releasing residency. A fixed frame is explicit; no live origin shift occurs.
class WorldTraversal final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<WorldTraversal>>
    create(WorldCellResidency& residency, PhysicsWorld& physics, Scene& scene,
           WorldLocalFrame frame);
    ~WorldTraversal();
    WorldTraversal(const WorldTraversal&) = delete;
    WorldTraversal& operator=(const WorldTraversal&) = delete;
    WorldTraversal(WorldTraversal&&) = delete;
    WorldTraversal& operator=(WorldTraversal&&) = delete;

    [[nodiscard]] std::uint64_t instance() const noexcept;
    [[nodiscard]] WorldLocalFrame frame() const;
    [[nodiscard]] Result<void> check_context(const Scene& scene, const PhysicsWorld& physics) const;
    // Conservative swept horizontal body footprint. All crossed content cells
    // must have live collision/navigation readiness before admitting motion.
    [[nodiscard]] Result<bool> allows_motion(WorldPosition from, WorldPosition to,
                                             double horizontal_radius) const;

    [[nodiscard]] Result<WorldTraversalCell> activate(WorldCellKey key,
                                                      std::uint64_t expected_generation,
                                                      WorldEntityId proxy,
                                                      PhysicsStaticMeshSettings settings = {});
    [[nodiscard]] Result<void> deactivate(WorldCellKey key, std::uint64_t expected_generation);
    [[nodiscard]] Result<std::vector<WorldTraversalCell>> cells() const;
    [[nodiscard]] Result<WorldNavigationPath> path(WorldPosition from, WorldPosition to,
                                                   std::size_t maximum_expansions = 2048,
                                                   std::size_t maximum_waypoints = 2048) const;
    [[nodiscard]] Result<WorldPosition>
    project_walk_surface(WorldPosition point, double maximum_vertical_delta = 1.0 / 1024.0) const;
    [[nodiscard]] Result<WorldNavigationCursor> begin(const WorldNavigationPath& path) const;
    // Pure steering candidate. Native committed movement must agree before a
    // host adopts this cursor. This does not move a character or grant a hit.
    [[nodiscard]] Result<WorldNavigationCursor> advance(const WorldNavigationPath& path,
                                                        const WorldNavigationCursor& cursor,
                                                        double maximum_distance) const;
    [[nodiscard]] Result<void> close();

  private:
    struct Impl;
    explicit WorldTraversal(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark
