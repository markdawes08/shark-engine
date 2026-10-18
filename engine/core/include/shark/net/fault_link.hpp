#pragma once

#include <shark/error.hpp>
#include <shark/time.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace shark::net {

inline constexpr std::size_t fault_link_maximum_packet_bytes = 8192;
inline constexpr std::size_t fault_link_maximum_pending_packets = 4096;
inline constexpr std::size_t fault_link_maximum_pending_bytes = 32 * 1024 * 1024;
inline constexpr std::size_t fault_link_maximum_drain_count = 1024;

struct FaultLinkConfig {
    std::uint64_t seed{1337};
    std::size_t maximum_packet_bytes{8192};
    std::size_t maximum_pending_packets{256};
    std::size_t maximum_pending_bytes{64 * 1024};
    std::size_t maximum_drain_count{64};
    MonotonicTime delay{0};
    // Additional nonnegative seeded jitter, inclusive of both endpoints.
    MonotonicTime jitter{0};
    MonotonicTime reorder_delay{std::chrono::milliseconds{10}};
    // Independent draws in 0..9999. 0 disables and 10000 always injects.
    std::uint32_t loss_per_10000{0};
    std::uint32_t duplication_per_10000{0};
    std::uint32_t reorder_per_10000{0};
};

enum class FaultSendOutcome { queued, dropped_loss, dropped_partition, backpressure };

struct FaultSendReport {
    FaultSendOutcome outcome;
    std::size_t queued_copies{0};
    std::optional<MonotonicTime> scheduled_at;
};

struct FaultDelivery {
    std::uint64_t message_tag;
    std::vector<std::byte> payload;
    MonotonicTime scheduled_at;
    bool duplicate;
    bool operator==(const FaultDelivery&) const = default;
};

struct FaultLinkStats {
    MonotonicTime now;
    MonotonicTime partition_end;
    std::size_t pending_packets;
    std::size_t pending_bytes;
    std::size_t peak_packets;
    std::size_t peak_bytes;
    // Cumulative counters saturate at UINT64_MAX; they never wrap.
    std::uint64_t submitted_messages;
    std::uint64_t admitted_messages;
    std::uint64_t queued_copies;
    std::uint64_t delivered_copies;
    std::uint64_t loss_drops;
    std::uint64_t partition_drops;
    std::uint64_t backpressure;
    std::uint64_t duplicated_messages;
    std::uint64_t reordered_messages;
    bool operator==(const FaultLinkStats&) const = default;
};

// Deterministic, unidirectional message/datagram fault model for semantic tests.
// This is not a reliable transport or a proxy for arbitrary TLS/TCP byte loss.
// One creating thread owns every method; no wall-clock waits or worker threads.
// Timestamped send/partition/drain calls form the reproducible injection schedule.
class FaultLink final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<FaultLink>>
    create(FaultLinkConfig config = {}, MonotonicTime origin = MonotonicTime{0});
    ~FaultLink();
    FaultLink(const FaultLink&) = delete;
    FaultLink& operator=(const FaultLink&) = delete;
    FaultLink(FaultLink&&) = delete;
    FaultLink& operator=(FaultLink&&) = delete;

    // Nonempty owned copies are enqueued atomically, including duplication.
    // Backpressure advances the virtual clock/counter but preserves RNG state;
    // validation, deadline and allocation errors preserve all state.
    [[nodiscard]] Result<FaultSendReport>
    send(MonotonicTime now, std::span<const std::byte> payload, std::uint64_t message_tag = 0);
    // Earliest scheduled time first, then admission order; duplicate immediately
    // follows its original at the same scheduled time. No hidden lookahead work.
    [[nodiscard]] Result<std::vector<FaultDelivery>> drain(MonotonicTime now,
                                                           std::size_t maximum_deliveries);
    // Partition [now, until): new sends drop and drain is empty. Existing queued
    // packets remain bounded and become drainable when the partition ends.
    // until == now explicitly clears a partition; no random state is consumed.
    [[nodiscard]] Result<void> partition_until(MonotonicTime now, MonotonicTime until);
    [[nodiscard]] Result<FaultLinkStats> stats() const;

  private:
    struct Impl;
    explicit FaultLink(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::net
