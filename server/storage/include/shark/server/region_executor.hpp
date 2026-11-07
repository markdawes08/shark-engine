#pragma once

#include "shark/server/region_image.hpp"

namespace shark::server {

enum class RegionExecutorPhase { staged, active, frozen, faulted };
struct RegionCellUsage {
    WorldCellResidencyUsage usage;
    std::vector<WorldCellResident> residents;
    std::vector<WorldTraversalCell> active;
    std::vector<WorldCellJobStatus> jobs;
};

// Actual creating-thread Scene/native/gameplay owner. Closed parent-channel
// dispatch supplies verified GameplayAccess; this object creates no sessions,
// credentials, persistence owner or public player transport.
class RegionExecutor final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<RegionExecutor>>
    bootstrap(const RegionProcessImage& seeds, RegionInstanceId instance, GameplayAccess& access,
              MonotonicTime now);
    [[nodiscard]] static Result<std::unique_ptr<RegionExecutor>>
    stage(const RegionProcessImage& image, RegionInstanceId instance, MonotonicTime now);
    [[nodiscard]] static Result<std::unique_ptr<RegionExecutor>>
    stage(const RegionProcessImage& image, MonotonicTime now);
    ~RegionExecutor();
    RegionExecutor(const RegionExecutor&) = delete;
    RegionExecutor& operator=(const RegionExecutor&) = delete;
    [[nodiscard]] Result<void> activate();
    [[nodiscard]] Result<void> activate(std::span<const storage::RegionWriter> writers);
    [[nodiscard]] Result<void> set_context(std::span<const RegionPeer> peers,
                                           std::span<const storage::RegionWriter> writers,
                                           std::uint64_t route_watermark);
    [[nodiscard]] Result<void> freeze();
    [[nodiscard]] RegionExecutorPhase phase() const noexcept;
    [[nodiscard]] Result<void> submit_movement(GameplayAccess& access, std::uint64_t connection,
                                               const net::MovementInput& input, MonotonicTime now);
    [[nodiscard]] Result<void> submit_combat(GameplayAccess& access, std::uint64_t connection,
                                             const net::CombatAction& action, MonotonicTime now);
    [[nodiscard]] Result<void> submit_npc_velocity(WorldEntityId entity, math::Vec3 velocity);
    [[nodiscard]] Result<WorldCellJobId> prefetch_cell(WorldCellKey key);
    [[nodiscard]] Result<WorldCellJobStatus> cancel_cell(WorldCellJobId job);
    [[nodiscard]] Result<std::size_t> drain_cells();
    [[nodiscard]] Result<WorldCellJobStatus> take_cell(WorldCellJobId job);
    [[nodiscard]] Result<WorldTraversalCell> activate_cell(WorldCellKey key);
    [[nodiscard]] Result<void> deactivate_cell(WorldCellKey key);
    [[nodiscard]] Result<void> evict_cell(WorldCellKey key);
    [[nodiscard]] Result<RegionCellUsage> cell_usage() const;
    [[nodiscard]] Result<void> set_pursuit(WorldEntityId npc, std::uint64_t request,
                                           WorldEntityId target, net::CombatTargetId life,
                                           double speed, double stop_distance);
    [[nodiscard]] Result<void> cancel_pursuit(WorldEntityId npc, std::uint64_t request);
    [[nodiscard]] Result<RegionTick>
    step(GameplayAccess& access, std::span<const std::uint64_t> connections, MonotonicTime now);
    [[nodiscard]] Result<net::CombatState> view(GameplayAccess& access, std::uint64_t connection,
                                                std::span<const net::CombatTargetId> visible,
                                                MonotonicTime now) const;
    [[nodiscard]] Result<RegionProcessImage> capture(std::span<const RegionPeer> peers,
                                                     std::span<const storage::RegionWriter> writers,
                                                     std::uint64_t route_watermark,
                                                     bool allow_content_jobs = false) const;
    [[nodiscard]] Result<RegionProcessImage> capture(bool allow_content_jobs = false) const;

  private:
    struct Impl;
    explicit RegionExecutor(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::server
