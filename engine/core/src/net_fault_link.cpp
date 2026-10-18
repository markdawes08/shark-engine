#include <shark/net/fault_link.hpp>

#include <algorithm>
#include <limits>
#include <thread>
#include <utility>

namespace shark::net {
namespace {

Error fault_error(ErrorCode code, const char* operation, const char* message) {
    return Error{code, message, ErrorContext{operation}};
}

// SplitMix64 uses defined unsigned arithmetic; the mapping is stable across STL
// implementations. Modulo mapping deliberately avoids unbounded rejection loops.
std::uint64_t next_random(std::uint64_t& state) noexcept {
    auto value = (state += UINT64_C(0x9e3779b97f4a7c15));
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

bool inject(std::uint64_t& state, std::uint32_t rate) noexcept {
    return next_random(state) % 10000 < rate;
}

void increment(std::uint64_t& value, std::uint64_t amount = 1) noexcept {
    const auto room = std::numeric_limits<std::uint64_t>::max() - value;
    value += std::min(room, amount);
}

Result<void> validate(const FaultLinkConfig& config, MonotonicTime origin) {
    constexpr auto operation = "net.fault-link.create";
    if (origin.count() < 0 || config.maximum_packet_bytes == 0 ||
        config.maximum_packet_bytes > fault_link_maximum_packet_bytes ||
        config.maximum_pending_packets == 0 ||
        config.maximum_pending_packets > fault_link_maximum_pending_packets ||
        config.maximum_pending_bytes == 0 ||
        config.maximum_pending_bytes > fault_link_maximum_pending_bytes ||
        config.maximum_drain_count == 0 ||
        config.maximum_drain_count > fault_link_maximum_drain_count ||
        config.loss_per_10000 > 10000 || config.duplication_per_10000 > 10000 ||
        config.reorder_per_10000 > 10000 || config.delay.count() < 0 || config.jitter.count() < 0 ||
        config.reorder_delay.count() < 0) {
        return fault_error(ErrorCode::invalid_configuration, operation,
                           "Invalid fault-link bounds, rates or nonnegative time");
    }
    const auto maximum = std::numeric_limits<MonotonicTime::rep>::max();
    if (config.delay.count() > maximum - config.jitter.count() ||
        config.delay.count() + config.jitter.count() > maximum - config.reorder_delay.count()) {
        return fault_error(ErrorCode::invalid_configuration, operation,
                           "Combined fault-link delay overflows monotonic time");
    }
    return {};
}

} // namespace

struct FaultLink::Impl {
    struct Queued {
        FaultDelivery delivery;
        std::uint64_t order;
    };

    Impl(FaultLinkConfig settings, MonotonicTime origin)
        : config(settings), random_state(settings.seed),
          metrics{origin, origin, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} {
        pending.reserve(config.maximum_pending_packets);
    }

    Result<void> check_owner(const char* operation) const {
        if (owner != std::this_thread::get_id()) {
            return fault_error(ErrorCode::conflict, operation,
                               "Fault link is restricted to its creating thread");
        }
        return {};
    }

    Result<void> check_time(MonotonicTime now, const char* operation) const {
        if (auto checked = check_owner(operation); !checked) {
            return checked.error();
        }
        if (now.count() < 0 || now < metrics.now) {
            return fault_error(ErrorCode::invalid_argument, operation,
                               "Fault-link time must be nonnegative and monotonic");
        }
        return {};
    }

    FaultLinkConfig config;
    const std::thread::id owner{std::this_thread::get_id()};
    std::uint64_t random_state;
    std::uint64_t next_order{0};
    FaultLinkStats metrics;
    std::vector<Queued> pending;
};

FaultLink::FaultLink(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FaultLink::~FaultLink() = default;

Result<std::unique_ptr<FaultLink>> FaultLink::create(FaultLinkConfig config, MonotonicTime origin) {
    if (auto checked = validate(config, origin); !checked) {
        return checked.error();
    }
    try {
        return std::unique_ptr<FaultLink>{new FaultLink{std::make_unique<Impl>(config, origin)}};
    } catch (...) {
        return fault_error(ErrorCode::execution_failed, "net.fault-link.create",
                           "Fault-link allocation failed");
    }
}

Result<FaultSendReport> FaultLink::send(MonotonicTime now, std::span<const std::byte> payload,
                                        std::uint64_t message_tag) {
    constexpr auto operation = "net.fault-link.send";
    if (auto checked = impl_->check_time(now, operation); !checked) {
        return checked.error();
    }
    if (payload.empty() || payload.size() > impl_->config.maximum_packet_bytes) {
        return fault_error(ErrorCode::invalid_argument, operation,
                           "Fault-link packet must be nonempty and within its byte limit");
    }
    auto& metrics = impl_->metrics;
    if (now < metrics.partition_end) {
        metrics.now = now;
        increment(metrics.submitted_messages);
        increment(metrics.partition_drops);
        return FaultSendReport{FaultSendOutcome::dropped_partition, 0, std::nullopt};
    }
    auto random = impl_->random_state;
    if (inject(random, impl_->config.loss_per_10000)) {
        impl_->random_state = random;
        metrics.now = now;
        increment(metrics.submitted_messages);
        increment(metrics.loss_drops);
        return FaultSendReport{FaultSendOutcome::dropped_loss, 0, std::nullopt};
    }
    const bool duplicate = inject(random, impl_->config.duplication_per_10000);
    const bool reordered = inject(random, impl_->config.reorder_per_10000);
    const auto jitter = static_cast<MonotonicTime::rep>(
        next_random(random) % (static_cast<std::uint64_t>(impl_->config.jitter.count()) + 1));
    const auto delay = impl_->config.delay.count() + jitter +
                       (reordered ? impl_->config.reorder_delay.count() : 0);
    if (now.count() > std::numeric_limits<MonotonicTime::rep>::max() - delay) {
        return fault_error(ErrorCode::invalid_argument, operation,
                           "Fault-link delivery deadline overflows monotonic time");
    }
    const MonotonicTime scheduled{now.count() + delay};
    const std::size_t copies = duplicate ? 2 : 1;
    const auto bytes = payload.size() * copies;
    if (copies > impl_->config.maximum_pending_packets - impl_->pending.size() ||
        bytes > impl_->config.maximum_pending_bytes - metrics.pending_bytes) {
        metrics.now = now;
        increment(metrics.submitted_messages);
        increment(metrics.backpressure);
        return FaultSendReport{FaultSendOutcome::backpressure, 0, std::nullopt};
    }
    if (impl_->next_order > std::numeric_limits<std::uint64_t>::max() - copies) {
        return fault_error(ErrorCode::conflict, operation, "Fault-link admission order exhausted");
    }
    try {
        std::vector<Impl::Queued> candidates;
        candidates.reserve(copies);
        for (std::size_t index = 0; index < copies; ++index) {
            candidates.push_back(
                {{message_tag, {payload.begin(), payload.end()}, scheduled, index != 0},
                 impl_->next_order + index});
        }
        // Capacity was reserved at creation; moves cannot allocate or throw.
        for (auto& candidate : candidates) {
            const auto position = std::lower_bound(
                impl_->pending.begin(), impl_->pending.end(), candidate,
                [](const Impl::Queued& left, const Impl::Queued& right) {
                    if (left.delivery.scheduled_at != right.delivery.scheduled_at) {
                        return left.delivery.scheduled_at < right.delivery.scheduled_at;
                    }
                    return left.order < right.order;
                });
            impl_->pending.insert(position, std::move(candidate));
        }
    } catch (...) {
        return fault_error(ErrorCode::execution_failed, operation,
                           "Fault-link payload allocation failed");
    }
    impl_->random_state = random;
    impl_->next_order += copies;
    metrics.now = now;
    metrics.pending_packets += copies;
    metrics.pending_bytes += bytes;
    metrics.peak_packets = std::max(metrics.peak_packets, metrics.pending_packets);
    metrics.peak_bytes = std::max(metrics.peak_bytes, metrics.pending_bytes);
    increment(metrics.submitted_messages);
    increment(metrics.admitted_messages);
    increment(metrics.queued_copies, copies);
    if (duplicate) {
        increment(metrics.duplicated_messages);
    }
    if (reordered) {
        increment(metrics.reordered_messages);
    }
    return FaultSendReport{FaultSendOutcome::queued, copies, scheduled};
}

Result<std::vector<FaultDelivery>> FaultLink::drain(MonotonicTime now,
                                                    std::size_t maximum_deliveries) {
    constexpr auto operation = "net.fault-link.drain";
    if (auto checked = impl_->check_time(now, operation); !checked) {
        return checked.error();
    }
    if (maximum_deliveries == 0 || maximum_deliveries > impl_->config.maximum_drain_count) {
        return fault_error(ErrorCode::invalid_argument, operation,
                           "Fault-link drain count is outside its configured bound");
    }
    std::vector<FaultDelivery> deliveries;
    auto& metrics = impl_->metrics;
    if (now < metrics.partition_end) {
        metrics.now = now;
        return deliveries;
    }
    std::size_t count = 0;
    while (count < impl_->pending.size() && count < maximum_deliveries &&
           impl_->pending[count].delivery.scheduled_at <= now) {
        ++count;
    }
    try {
        deliveries.reserve(count);
    } catch (...) {
        return fault_error(ErrorCode::execution_failed, operation,
                           "Fault-link drain allocation failed");
    }
    for (std::size_t index = 0; index < count; ++index) {
        metrics.pending_bytes -= impl_->pending[index].delivery.payload.size();
        deliveries.push_back(std::move(impl_->pending[index].delivery));
    }
    impl_->pending.erase(impl_->pending.begin(),
                         impl_->pending.begin() + static_cast<std::ptrdiff_t>(count));
    metrics.pending_packets -= count;
    metrics.now = now;
    increment(metrics.delivered_copies, count);
    return deliveries;
}

Result<void> FaultLink::partition_until(MonotonicTime now, MonotonicTime until) {
    constexpr auto operation = "net.fault-link.partition";
    if (auto checked = impl_->check_time(now, operation); !checked) {
        return checked.error();
    }
    if (until < now) {
        return fault_error(ErrorCode::invalid_argument, operation,
                           "Fault-link partition end precedes its start");
    }
    impl_->metrics.now = now;
    impl_->metrics.partition_end = until;
    return {};
}

Result<FaultLinkStats> FaultLink::stats() const {
    if (auto checked = impl_->check_owner("net.fault-link.stats"); !checked) {
        return checked.error();
    }
    return impl_->metrics;
}

} // namespace shark::net
