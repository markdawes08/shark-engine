#include <shark/net/fault_link.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using shark::MonotonicTime;
using shark::net::FaultLink;
using shark::net::FaultLinkConfig;
using shark::net::FaultSendOutcome;
const std::array payload{std::byte{1}, std::byte{2}, std::byte{3}};

std::unique_ptr<FaultLink> link(FaultLinkConfig config = {}, MonotonicTime origin = 0ns) {
    auto result = FaultLink::create(config, origin);
    REQUIRE(result);
    return std::move(result).value();
}

shark::net::FaultLinkStats stats(const FaultLink& value) {
    auto result = value.stats();
    REQUIRE(result);
    return result.value();
}

void queued(FaultLink& value, MonotonicTime now, std::uint64_t tag) {
    auto result = value.send(now, payload, tag);
    REQUIRE(result);
    REQUIRE(result.value().outcome == FaultSendOutcome::queued);
}

} // namespace

TEST_CASE("Fault link rejects invalid bounds rates and combined delays", "[core][net-fault]") {
    auto invalid = [](const FaultLinkConfig& config) {
        const auto result = FaultLink::create(config);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == shark::ErrorCode::invalid_configuration);
        CHECK(result.error().context.operation == "net.fault-link.create");
    };
    for (const auto size : {std::size_t{0}, shark::net::fault_link_maximum_packet_bytes + 1}) {
        FaultLinkConfig config;
        config.maximum_packet_bytes = size;
        invalid(config);
    }
    for (const auto size : {std::size_t{0}, shark::net::fault_link_maximum_pending_packets + 1}) {
        FaultLinkConfig config;
        config.maximum_pending_packets = size;
        invalid(config);
    }
    for (const auto size : {std::size_t{0}, shark::net::fault_link_maximum_pending_bytes + 1}) {
        FaultLinkConfig config;
        config.maximum_pending_bytes = size;
        invalid(config);
    }
    for (const auto size : {std::size_t{0}, shark::net::fault_link_maximum_drain_count + 1}) {
        FaultLinkConfig config;
        config.maximum_drain_count = size;
        invalid(config);
    }
    FaultLinkConfig config;
    config.loss_per_10000 = 10001;
    invalid(config);
    config = {};
    config.duplication_per_10000 = 10001;
    invalid(config);
    config = {};
    config.reorder_per_10000 = 10001;
    invalid(config);
    config = {};
    config.delay = -1ns;
    invalid(config);
    config = {};
    config.jitter = -1ns;
    invalid(config);
    config = {};
    config.reorder_delay = -1ns;
    invalid(config);
    config = {};
    config.delay = MonotonicTime::max();
    invalid(config);
    config.reorder_delay = 0ns;
    REQUIRE(FaultLink::create(config));
    config.jitter = 1ns;
    invalid(config);
    CHECK_FALSE(FaultLink::create({}, -1ns));

    config = {};
    config.maximum_packet_bytes = shark::net::fault_link_maximum_packet_bytes;
    config.maximum_pending_packets = shark::net::fault_link_maximum_pending_packets;
    config.maximum_pending_bytes = shark::net::fault_link_maximum_pending_bytes;
    config.maximum_drain_count = shark::net::fault_link_maximum_drain_count;
    REQUIRE(FaultLink::create(config));
}

TEST_CASE("Fault link owns payloads and drains stable FIFO in bounded batches",
          "[core][net-fault]") {
    FaultLinkConfig config;
    config.delay = 10ns;
    config.maximum_drain_count = 2;
    auto value = link(config);
    auto input = payload;
    REQUIRE(value->send(0ns, input, 1));
    input[0] = std::byte{99};
    queued(*value, 0ns, 2);
    queued(*value, 0ns, 3);
    REQUIRE(value->drain(9ns, 2).value().empty());
    auto first = value->drain(10ns, 2);
    REQUIRE(first);
    REQUIRE(first.value().size() == 2);
    CHECK(first.value()[0].message_tag == 1);
    CHECK(first.value()[0].payload == std::vector<std::byte>(payload.begin(), payload.end()));
    CHECK(first.value()[1].message_tag == 2);
    CHECK(first.value()[0].scheduled_at == 10ns);
    CHECK_FALSE(first.value()[0].duplicate);
    auto second = value->drain(10ns, 1);
    REQUIRE(second);
    REQUIRE(second.value().size() == 1);
    CHECK(second.value()[0].message_tag == 3);
    CHECK(stats(*value).pending_bytes == 0);
    CHECK(stats(*value).pending_packets == 0);
    CHECK(stats(*value).delivered_copies == 3);
    value.reset();
    CHECK(first.value()[0].payload[0] == std::byte{1});
}

TEST_CASE("Fault link validates calls without changing clock queue or random sequence",
          "[core][net-fault]") {
    FaultLinkConfig config;
    config.jitter = 100ns;
    config.maximum_packet_bytes = 3;
    auto value = link(config, 10ns);
    auto control = link(config, 10ns);
    const auto before = stats(*value);
    CHECK_FALSE(value->send(9ns, payload));
    CHECK_FALSE(value->send(-1ns, payload));
    CHECK_FALSE(value->send(11ns, {}));
    const std::array large{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    CHECK_FALSE(value->send(11ns, large));
    CHECK_FALSE(value->drain(11ns, 0));
    CHECK_FALSE(value->drain(11ns, config.maximum_drain_count + 1));
    CHECK_FALSE(value->drain(9ns, 1));
    CHECK_FALSE(value->partition_until(11ns, 10ns));
    CHECK_FALSE(value->partition_until(9ns, 12ns));
    CHECK(stats(*value) == before);
    queued(*value, 10ns, 7);
    queued(*control, 10ns, 7);
    CHECK(value->drain(1000ns, 4).value() == control->drain(1000ns, 4).value());
    CHECK(stats(*value) == stats(*control));
}

TEST_CASE("Fault link deadline overflow preserves the complete state and RNG",
          "[core][net-fault]") {
    FaultLinkConfig config;
    config.delay = 1ns;
    config.jitter = 500ns;
    auto value = link(config);
    auto control = link(config);
    const auto before = stats(*value);
    const auto failed = value->send(MonotonicTime::max(), payload, 2);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == shark::ErrorCode::invalid_argument);
    CHECK(stats(*value) == before);
    queued(*value, 0ns, 3);
    queued(*control, 0ns, 3);
    CHECK(value->drain(1000ns, 4).value() == control->drain(1000ns, 4).value());

    config.jitter = 0ns;
    config.reorder_delay = 0ns;
    auto edge = link(config, MonotonicTime::max() - 1ns);
    queued(*edge, MonotonicTime::max() - 1ns, 9);
    auto delivered = edge->drain(MonotonicTime::max(), 1);
    REQUIRE(delivered);
    REQUIRE(delivered.value().size() == 1);
    CHECK(delivered.value()[0].scheduled_at == MonotonicTime::max());
    CHECK_FALSE(edge->send(MonotonicTime::max(), payload));
}

TEST_CASE("Fault link duplicate admission is atomic under packet and byte pressure",
          "[core][net-fault]") {
    for (const bool byte_limit : {false, true}) {
        FaultLinkConfig config;
        config.duplication_per_10000 = 10000;
        config.maximum_pending_packets = byte_limit ? 8 : 3;
        config.maximum_pending_bytes = byte_limit ? 8 : 100;
        auto value = link(config);
        auto first = value->send(0ns, payload, 1);
        REQUIRE(first);
        CHECK(first.value().queued_copies == 2);
        const auto rejected = value->send(0ns, payload, 2);
        REQUIRE(rejected);
        CHECK(rejected.value().outcome == FaultSendOutcome::backpressure);
        CHECK(rejected.value().queued_copies == 0);
        CHECK_FALSE(rejected.value().scheduled_at);
        const auto snapshot = stats(*value);
        CHECK(snapshot.pending_packets == 2);
        CHECK(snapshot.pending_bytes == 6);
        CHECK(snapshot.admitted_messages == 1);
        CHECK(snapshot.backpressure == 1);
        CHECK(snapshot.duplicated_messages == 1);
        auto batch = value->drain(0ns, 2);
        REQUIRE(batch);
        REQUIRE(batch.value().size() == 2);
        CHECK(batch.value()[0].message_tag == 1);
        CHECK_FALSE(batch.value()[0].duplicate);
        CHECK(batch.value()[1].message_tag == 1);
        CHECK(batch.value()[1].duplicate);
        queued(*value, 0ns, 2);
    }
}

TEST_CASE("Fault link backpressure does not consume random fault decisions", "[core][net-fault]") {
    FaultLinkConfig config;
    config.maximum_pending_packets = 1;
    config.jitter = 100ns;
    config.reorder_per_10000 = 5000;
    config.reorder_delay = 100ns;
    auto value = link(config);
    auto control = link(config);
    queued(*value, 0ns, 1);
    queued(*control, 0ns, 1);
    for (int attempt = 0; attempt < 20; ++attempt) {
        auto result = value->send(0ns, payload, 99);
        REQUIRE(result);
        CHECK(result.value().outcome == FaultSendOutcome::backpressure);
    }
    REQUIRE(value->drain(1000ns, 1));
    REQUIRE(control->drain(1000ns, 1));
    queued(*value, 1000ns, 2);
    queued(*control, 1000ns, 2);
    CHECK(value->drain(2000ns, 1).value() == control->drain(2000ns, 1).value());
    CHECK(stats(*value).backpressure == 20);
}

TEST_CASE("Fault link explicit partitions block delivery and recover at the boundary",
          "[core][net-fault]") {
    auto value = link();
    queued(*value, 0ns, 1);
    REQUIRE(value->partition_until(0ns, 20ns));
    const auto dropped = value->send(1ns, payload, 2);
    REQUIRE(dropped);
    CHECK(dropped.value().outcome == FaultSendOutcome::dropped_partition);
    CHECK(value->drain(19ns, 5).value().empty());
    CHECK(stats(*value).pending_packets == 1);
    auto recovered = value->drain(20ns, 5);
    REQUIRE(recovered);
    REQUIRE(recovered.value().size() == 1);
    CHECK(recovered.value()[0].message_tag == 1);
    CHECK(recovered.value()[0].scheduled_at == 0ns);
    queued(*value, 20ns, 3);
    REQUIRE(value->partition_until(20ns, 50ns));
    REQUIRE(value->partition_until(21ns, 21ns));
    CHECK(value->drain(21ns, 5).value().size() == 1);
    CHECK(stats(*value).partition_drops == 1);
    CHECK(stats(*value).submitted_messages == 3);
}

TEST_CASE("Fault link all-loss does not consume queue capacity", "[core][net-fault]") {
    FaultLinkConfig config;
    config.loss_per_10000 = 10000;
    config.duplication_per_10000 = 10000;
    config.maximum_pending_bytes = 1;
    auto value = link(config);
    for (std::uint64_t index = 0; index < 100; ++index) {
        const auto result = value->send(0ns, payload, index);
        REQUIRE(result);
        CHECK(result.value().outcome == FaultSendOutcome::dropped_loss);
    }
    const auto snapshot = stats(*value);
    CHECK(snapshot.loss_drops == 100);
    CHECK(snapshot.pending_packets == 0);
    CHECK(snapshot.pending_bytes == 0);
    CHECK(snapshot.backpressure == 0);
    CHECK(value->drain(0ns, 1).value().empty());
}

TEST_CASE("Fault link seed 1337 has a portable loss duplication jitter and reorder trace",
          "[core][net-fault]") {
    FaultLinkConfig config;
    config.delay = 10ns;
    config.jitter = 20ns;
    config.reorder_delay = 50ns;
    config.loss_per_10000 = 2500;
    config.duplication_per_10000 = 5000;
    config.reorder_per_10000 = 5000;
    auto value = link(config);
    for (std::uint64_t tag = 1; tag <= 10; ++tag) {
        REQUIRE(value->send(0ns, payload, tag));
    }
    const std::array<std::uint64_t, 6> tags{9, 5, 6, 4, 4, 3};
    const std::array<MonotonicTime, 6> deadlines{11ns, 60ns, 76ns, 78ns, 78ns, 80ns};
    auto batch = value->drain(100ns, 10);
    REQUIRE(batch);
    REQUIRE(batch.value().size() == tags.size());
    for (std::size_t index = 0; index < tags.size(); ++index) {
        CHECK(batch.value()[index].message_tag == tags[index]);
        CHECK(batch.value()[index].scheduled_at == deadlines[index]);
        CHECK(batch.value()[index].duplicate == (index == 4));
    }
    const auto snapshot = stats(*value);
    CHECK(snapshot.loss_drops == 5);
    CHECK(snapshot.admitted_messages == 5);
    CHECK(snapshot.duplicated_messages == 1);
    CHECK(snapshot.reordered_messages == 4);
}

TEST_CASE("Fault link repeated seeded schedules replay and retain strict memory bounds",
          "[core][net-fault]") {
    FaultLinkConfig config;
    config.seed = 0x4d303261;
    config.delay = 3ns;
    config.jitter = 19ns;
    config.reorder_delay = 11ns;
    config.loss_per_10000 = 1700;
    config.duplication_per_10000 = 3700;
    config.reorder_per_10000 = 4300;
    config.maximum_pending_packets = 7;
    config.maximum_pending_bytes = 17;
    config.maximum_drain_count = 3;
    auto first = link(config);
    auto second = link(config);
    for (MonotonicTime::rep tick = 0; tick < 1000; ++tick) {
        const MonotonicTime now{tick};
        if (tick % 97 == 0) {
            REQUIRE(first->partition_until(now, now + 7ns));
            REQUIRE(second->partition_until(now, now + 7ns));
        }
        const auto a = first->send(now, payload, static_cast<std::uint64_t>(tick));
        const auto b = second->send(now, payload, static_cast<std::uint64_t>(tick));
        REQUIRE(a);
        REQUIRE(b);
        CHECK(a.value().outcome == b.value().outcome);
        CHECK(a.value().scheduled_at == b.value().scheduled_at);
        auto a_batch = first->drain(now, 3);
        auto b_batch = second->drain(now, 3);
        REQUIRE(a_batch);
        REQUIRE(b_batch);
        CHECK(a_batch.value() == b_batch.value());
        CHECK(a_batch.value().size() <= 3);
        CHECK(stats(*first) == stats(*second));
        CHECK(stats(*first).pending_packets <= 7);
        CHECK(stats(*first).pending_bytes <= 17);
    }
    while (stats(*first).pending_packets != 0) {
        CHECK(first->drain(2000ns, 3).value() == second->drain(2000ns, 3).value());
    }
    const auto snapshot = stats(*first);
    CHECK(snapshot.queued_copies == snapshot.delivered_copies);
    CHECK(snapshot.submitted_messages == snapshot.admitted_messages + snapshot.loss_drops +
                                             snapshot.partition_drops + snapshot.backpressure);
    CHECK(snapshot.loss_drops > 0);
    CHECK(snapshot.partition_drops > 0);
    CHECK(snapshot.backpressure > 0);
}

TEST_CASE("Fault link rejects every method on another thread", "[core][net-fault]") {
    auto value = link();
    const auto before = stats(*value);
    std::array<bool, 4> rejected{};
    std::exception_ptr exception;
    std::thread worker([&] {
        try {
            const auto sent = value->send(0ns, payload);
            const auto drained = value->drain(0ns, 1);
            const auto partitioned = value->partition_until(0ns, 1ns);
            const auto snapshot = value->stats();
            rejected = {!sent && sent.error().code == shark::ErrorCode::conflict,
                        !drained && drained.error().code == shark::ErrorCode::conflict,
                        !partitioned && partitioned.error().code == shark::ErrorCode::conflict,
                        !snapshot && snapshot.error().code == shark::ErrorCode::conflict};
        } catch (...) {
            exception = std::current_exception();
        }
    });
    worker.join();
    REQUIRE_FALSE(exception);
    for (const bool result : rejected) {
        CHECK(result);
    }
    CHECK(stats(*value) == before);
}
