#pragma once

#include "shark/world_cell.hpp"

#include <functional>
#include <stop_token>

namespace shark {

inline constexpr std::size_t world_cell_maximum_jobs = 4;

struct WorldCellResidencyLimits {
    std::size_t maximum_resident_cells{8};
    std::size_t maximum_resident_bytes{128 * 1024 * 1024};
    std::size_t maximum_pending_bytes{160 * 1024 * 1024};
};

struct WorldCellJobId {
    std::uint64_t scope;
    std::uint64_t sequence;
    auto operator<=>(const WorldCellJobId&) const = default;
};
enum class WorldCellJobState { queued, running, ready, published, failed, cancelled };
struct WorldCellJobStatus {
    WorldCellJobId id;
    WorldCellKey key;
    std::uint64_t content_revision;
    WorldCellJobState state;
    bool cancellation_requested{false};
    std::optional<std::uint64_t> resident_generation{};
    std::optional<Error> error{};
};
struct WorldCellResident {
    WorldCellKey key;
    std::uint64_t content_revision;
    std::uint64_t generation;
    std::size_t charged_bytes;
};
struct WorldCellResidencyUsage {
    std::size_t resident_cells;
    std::size_t resident_bytes;
    std::size_t pending_bytes;
    std::size_t retained_jobs;
    // Returned reader bytes, including rejected results; not filesystem traffic.
    std::uint64_t bytes_read;
    std::size_t peak_resident_bytes;
    std::size_t peak_pending_bytes;
    bool closed;
};
struct WorldCellReadRequest {
    WorldCellKey key;
    std::uint64_t content_revision;
    std::size_t maximum_bytes{world_cell_maximum_payload_bytes};
};
// A trusted host callback. It runs serially on the private worker, returns owned
// bytes, cooperates with stop and bounds its own IO/transient allocation work.
// It cannot access Scene/owner state or reenter this service.
using WorldCellReader =
    std::function<Result<std::vector<std::byte>>(const WorldCellReadRequest&, std::stop_token)>;

// One creating-thread owner and one worker. Worker results never publish Scenes;
// explicit drain constructs complete owners and preserves last-valid residency.
// Retained terminal outcomes occupy their job slots until explicitly taken.
class WorldCellResidency final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<WorldCellResidency>>
    create(WorldCellReader reader, WorldCellResidencyLimits limits = {});
    ~WorldCellResidency();
    WorldCellResidency(const WorldCellResidency&) = delete;
    WorldCellResidency& operator=(const WorldCellResidency&) = delete;
    WorldCellResidency(WorldCellResidency&&) = delete;
    WorldCellResidency& operator=(WorldCellResidency&&) = delete;

    [[nodiscard]] Result<WorldCellJobId> submit(WorldCellKey key, std::uint64_t content_revision,
                                                std::shared_ptr<const PreparedAssets> assets);
    [[nodiscard]] Result<std::vector<WorldCellJobStatus>> jobs() const;
    [[nodiscard]] Result<std::size_t> drain();
    [[nodiscard]] Result<WorldCellJobStatus> cancel(WorldCellJobId id);
    [[nodiscard]] Result<WorldCellJobStatus> take(WorldCellJobId id);
    [[nodiscard]] Result<std::vector<WorldCellResident>> residents() const;
    // Borrowed until replacement, eviction or owner destruction. The pointer does
    // not extend residency and may only be queried on the creating thread.
    [[nodiscard]] Result<const LoadedWorldCell*> resident(WorldCellKey key) const;
    [[nodiscard]] Result<void> evict(WorldCellKey key, std::uint64_t expected_generation);
    [[nodiscard]] Result<WorldCellResidencyUsage> usage() const;
    // Permanent admission close; joins the reader before releasing pending work.
    // Existing residents and original terminal receipts remain available.
    [[nodiscard]] Result<void> close();

  private:
    struct Impl;
    explicit WorldCellResidency(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark
