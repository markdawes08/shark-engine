#pragma once

#include "shark/server/region_child.hpp"
#include "shark/server/region_image.hpp"

namespace shark::server {
// Closed, parent-owned control channel. These are trusted host commands, never
// additions to the player protocol. One request is outstanding at a time.
class RegionProcess final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<RegionProcess>>
    launch(const std::filesystem::path& server);
    ~RegionProcess();
    RegionProcess(const RegionProcess&) = delete;
    RegionProcess& operator=(const RegionProcess&) = delete;
    [[nodiscard]] Result<void> stage(const RegionProcessImage& image, MonotonicTime now);
    [[nodiscard]] Result<void> activate(const RegionProcessImage& proof, MonotonicTime now);
    [[nodiscard]] Result<RegionProcessImage> inspect(MonotonicTime now);
    [[nodiscard]] Result<RegionProcessImage> freeze(MonotonicTime now);
    [[nodiscard]] Result<void> prepare(std::uint64_t route, std::uint64_t connection,
                                       const net::MovementInput& input,
                                       std::span<const RegionPeer> peers, MonotonicTime now);
    [[nodiscard]] Result<void> prepare(std::uint64_t route, std::uint64_t connection,
                                       const net::CombatAction& input,
                                       std::span<const RegionPeer> peers, MonotonicTime now);
    [[nodiscard]] Result<RegionProcessImage> commit(std::uint64_t route, std::uint64_t connection,
                                                    const net::MovementInput& input,
                                                    std::span<const RegionPeer> peers,
                                                    MonotonicTime now);
    [[nodiscard]] Result<RegionProcessImage> commit(std::uint64_t route, std::uint64_t connection,
                                                    const net::CombatAction& input,
                                                    std::span<const RegionPeer> peers,
                                                    MonotonicTime now);
    [[nodiscard]] Result<void> cancel(MonotonicTime now);
    [[nodiscard]] Result<RegionProcessImage> step(std::span<const RegionPeer> peers,
                                                  MonotonicTime now);
    [[nodiscard]] Result<void> terminate();
    [[nodiscard]] Result<void> retire(MonotonicTime now);
    [[nodiscard]] Result<bool> alive() const;
    [[nodiscard]] bool channel_healthy() const noexcept;
    [[nodiscard]] std::uint64_t process_id() const noexcept;
    // A lost response poisons the channel. Recovery confirms actual exit before
    // restoring a checkpoint; expiration alone never grants another writer.
    void set_timeout(std::chrono::milliseconds timeout);

  private:
    struct Impl;
    explicit RegionProcess(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// Server --region-worker: binary stdio only, no player listener, no autonomous
// ticks, no database and no source importers. EOF tears down native owners.
[[nodiscard]] int run_region_worker();
} // namespace shark::server
