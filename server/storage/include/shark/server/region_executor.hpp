#pragma once

#include "shark/server/region_image.hpp"

namespace shark::server {

enum class RegionExecutorPhase { staged, active, frozen, faulted };

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
    [[nodiscard]] Result<RegionTick>
    step(GameplayAccess& access, std::span<const std::uint64_t> connections, MonotonicTime now);
    [[nodiscard]] Result<net::CombatState> view(GameplayAccess& access, std::uint64_t connection,
                                                std::span<const net::CombatTargetId> visible,
                                                MonotonicTime now) const;
    [[nodiscard]] Result<RegionProcessImage> capture(std::span<const RegionPeer> peers,
                                                     std::span<const storage::RegionWriter> writers,
                                                     std::uint64_t route_watermark) const;
    [[nodiscard]] Result<RegionProcessImage> capture() const;

  private:
    struct Impl;
    explicit RegionExecutor(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::server
