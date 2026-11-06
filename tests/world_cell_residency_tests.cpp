// Original Shark in-memory residency fixtures; project MIT license.
#include "shark/world_cell_residency.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace shark;
using State = WorldCellJobState;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.operation);
    }
    REQUIRE(result);
    return std::move(result).value();
}

template <typename Id> Id id(std::uint32_t value) {
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 0xfU];
        value >>= 4U;
    }
    return Id{take(StableId::parse(text))};
}

WorldCellKey key(std::int64_t x = 0) {
    return {id<WorldId>(91), {1'000'000'000 + x, 0, -1'000'000'000}};
}

PreparedWorldCell sample() {
    const auto cell = key();
    PreparedEntity root{id<AuthoredEntityId>(1)};
    root.name.value = "Ground";
    root.mesh_renderer = MeshRenderer{builtin_plane_asset_id(), 0, false};
    return {cell, 1, {id<SceneId>(2), WorldPosition{cell.world, cell.cell, {0, 0, 0}}, {root}}};
}

// The callback copies values only. No test assertion or Scene construction runs
// on the service's worker thread.
WorldCellReader reader() {
    return [base = sample()](const WorldCellReadRequest& request,
                             std::stop_token) mutable -> Result<std::vector<std::byte>> {
        auto value = base;
        value.key = request.key;
        value.content_revision = request.content_revision;
        value.scene.world_origin = WorldPosition{request.key.world, request.key.cell, {0, 0, 0}};
        return encode_world_cell(value);
    };
}

WorldCellJobStatus status(const WorldCellResidency& owner, WorldCellJobId job) {
    const auto jobs = take(owner.jobs());
    const auto found = std::find_if(jobs.begin(), jobs.end(),
                                    [job](const auto& candidate) { return candidate.id == job; });
    REQUIRE(found != jobs.end());
    return *found;
}

// A watchdog for observable job state, not a scheduling or performance target.
bool wait_state(const WorldCellResidency& owner, WorldCellJobId job, State expected) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto jobs = owner.jobs();
        if (!jobs)
            return false;
        const auto found =
            std::find_if(jobs.value().begin(), jobs.value().end(),
                         [job](const auto& candidate) { return candidate.id == job; });
        if (found == jobs.value().end())
            return false;
        if (found->state == expected)
            return true;
        std::this_thread::yield();
    }
    return false;
}

WorldCellJobStatus publish(WorldCellResidency& owner, WorldCellJobId job) {
    REQUIRE(wait_state(owner, job, State::ready));
    CHECK(take(owner.drain()) == 1);
    const auto result = status(owner, job);
    REQUIRE(result.state == State::published);
    REQUIRE(result.resident_generation);
    return result;
}

struct ReaderGate {
    std::mutex mutex;
    std::condition_variable wake;
    bool entered{false};
    bool released{false};

    bool block() {
        std::unique_lock lock{mutex};
        entered = true;
        wake.notify_all();
        return wake.wait_for(lock, std::chrono::seconds{5}, [&] { return released; });
    }
    bool wait_entered() {
        std::unique_lock lock{mutex};
        return wake.wait_for(lock, std::chrono::seconds{5}, [&] { return entered; });
    }
    void release() {
        const std::lock_guard lock{mutex};
        released = true;
        wake.notify_all();
    }
};

// Declare after the service so failure unwinding releases a blocked callback
// before the service's join. The reader itself also has a bounded watchdog.
struct ReleaseGate {
    ReaderGate& gate;
    ~ReleaseGate() {
        gate.release();
    }
};
} // namespace

TEST_CASE("cell residency publishes complete owners only through explicit drain",
          "[world-cell-residency]") {
    const auto assets = take(PreparedAssets::create());
    const auto owner = take(WorldCellResidency::create(reader()));
    const auto first = take(owner->submit(key(), 1, assets));
    REQUIRE(wait_state(*owner, first, State::ready));
    CHECK_FALSE(owner->resident(key()));
    CHECK(take(owner->residents()).empty());
    CHECK(take(owner->usage()).resident_cells == 0);
    CHECK_FALSE(owner->take(first));
    CHECK_FALSE(owner->submit(key(), 2, assets));
    CHECK_FALSE(owner->evict(key(), 1));

    const auto second = take(owner->submit(key(1), 1, assets));
    REQUIRE(wait_state(*owner, second, State::ready));
    CHECK(take(owner->drain()) == 1);
    CHECK(status(*owner, first).state == State::published);
    CHECK(status(*owner, second).state == State::ready);
    CHECK(take(owner->residents()).size() == 1);
    const auto original = status(*owner, first);
    REQUIRE(original.resident_generation);
    const auto retained = take(owner->cancel(first));
    CHECK(retained.state == State::published);
    CHECK(retained.resident_generation == original.resident_generation);
    CHECK_FALSE(retained.cancellation_requested);
    CHECK(take(owner->drain()) == 1);
    CHECK(take(owner->drain()) == 0);
    CHECK(take(owner->residents()).size() == 2);
    CHECK(take(owner->take(first)).resident_generation == original.resident_generation);
    CHECK_FALSE(owner->cancel(first));
    CHECK_FALSE(owner->take(first));

    const auto resident = take(owner->resident(key()));
    CHECK(resident->key() == key());
    CHECK(resident->content_revision() == 1);
    CHECK(take(resident->scene().stats()).live_entities == 1);
    CHECK_FALSE(owner->evict(key(), *original.resident_generation + 1));
    take(owner->evict(key(), *original.resident_generation));
    CHECK_FALSE(owner->resident(key()));
    CHECK(take(owner->residents()).size() == 1);
}

TEST_CASE("failed and cancelled replacements preserve the original cell until explicit retry",
          "[world-cell-residency]") {
    const auto assets = take(PreparedAssets::create());
    auto source = reader();
    const auto owner = take(WorldCellResidency::create(
        [source](const WorldCellReadRequest& request,
                 std::stop_token stop) mutable -> Result<std::vector<std::byte>> {
            if (request.content_revision == 2)
                return Error{ErrorCode::io_failure, "Fixture source unavailable", {"fixture.read"}};
            return source(request, stop);
        }));
    const auto initial = take(owner->submit(key(), 1, assets));
    const auto published = publish(*owner, initial);
    take(owner->take(initial));
    const auto original = take(owner->resident(key()));
    const auto original_stats = take(original->scene().stats());
    const auto reference =
        take(original->scene().reference(take(original->scene().find(id<AuthoredEntityId>(1)))));

    const auto failed = take(owner->submit(key(), 2, assets));
    REQUIRE(wait_state(*owner, failed, State::ready));
    CHECK(take(owner->drain()) == 1);
    const auto failure = status(*owner, failed);
    REQUIRE(failure.state == State::failed);
    REQUIRE(failure.error);
    CHECK(failure.error->code == ErrorCode::io_failure);
    CHECK(take(owner->resident(key())) == original);
    CHECK(take(original->scene().stats()).instance == original_stats.instance);
    CHECK(take(original->scene().resolve(reference)) == reference.handle);
    const auto original_failure = take(owner->cancel(failed));
    REQUIRE(original_failure.error);
    CHECK(original_failure.error->code == ErrorCode::io_failure);
    take(owner->take(failed));

    const auto cancelled = take(owner->submit(key(), 3, assets));
    REQUIRE(wait_state(*owner, cancelled, State::ready));
    CHECK(take(owner->cancel(cancelled)).state == State::cancelled);
    CHECK(take(owner->drain()) == 0);
    CHECK(take(owner->resident(key())) == original);
    CHECK(take(owner->take(cancelled)).state == State::cancelled);
    const auto retry = take(owner->submit(key(), 3, assets));
    CHECK(retry != cancelled);
    const auto replacement = publish(*owner, retry);
    CHECK(replacement.resident_generation != published.resident_generation);
    const auto current = take(owner->resident(key()));
    CHECK(current->content_revision() == 3);
    CHECK(take(current->scene().stats()).instance != original_stats.instance);
    CHECK_FALSE(current->scene().resolve(reference));
    CHECK_FALSE(owner->evict(key(), *published.resident_generation));
}

TEST_CASE("queued running and ready cell cancellation cannot publish or reenter the owner",
          "[world-cell-residency]") {
    const auto assets = take(PreparedAssets::create());
    ReaderGate gate;
    std::atomic<bool> reentry_rejected{false};
    WorldCellResidency* service{};
    auto source = reader();
    const auto owner = take(
        WorldCellResidency::create([&](const WorldCellReadRequest& request,
                                       std::stop_token stop) -> Result<std::vector<std::byte>> {
            const std::stop_callback callback{stop, [&] {
                                                  const auto observed = service->usage();
                                                  reentry_rejected.store(!observed &&
                                                                         observed.error().code ==
                                                                             ErrorCode::conflict);
                                              }};
            if (!gate.block())
                return Error{ErrorCode::execution_failed, "Reader gate watchdog expired", {}};
            return source(request, stop);
        }));
    service = owner.get();
    const ReleaseGate release{gate};
    const auto running = take(owner->submit(key(), 1, assets));
    REQUIRE(gate.wait_entered());
    CHECK(status(*owner, running).state == State::running);
    const auto queued = take(owner->submit(key(1), 1, assets));
    CHECK(status(*owner, queued).state == State::queued);
    CHECK(take(owner->cancel(queued)).state == State::cancelled);
    CHECK(take(owner->take(queued)).state == State::cancelled);
    const auto cancelled_running = take(owner->cancel(running));
    CHECK(cancelled_running.state == State::running);
    CHECK(cancelled_running.cancellation_requested);
    CHECK(reentry_rejected.load());
    CHECK_FALSE(owner->submit(key(), 2, assets));
    CHECK(take(owner->drain()) == 0);
    gate.release();
    REQUIRE(wait_state(*owner, running, State::ready));
    CHECK(take(owner->drain()) == 1);
    CHECK(status(*owner, running).state == State::cancelled);
    CHECK_FALSE(owner->resident(key()));
    CHECK(take(owner->usage()).pending_bytes == 0);
    CHECK(take(owner->take(running)).state == State::cancelled);
}

TEST_CASE("cell job slots retain terminal receipts until explicit take", "[world-cell-residency]") {
    const auto assets = take(PreparedAssets::create());
    const auto owner = take(WorldCellResidency::create(reader()));
    std::array<WorldCellJobId, 4> jobs{};
    for (std::size_t index = 0; index < jobs.size(); ++index)
        jobs[index] = take(owner->submit(key(static_cast<std::int64_t>(index)), 1, assets));
    for (const auto job : jobs)
        REQUIRE(wait_state(*owner, job, State::ready));
    CHECK_FALSE(owner->submit(key(4), 1, assets));
    for (const auto job : jobs) {
        CHECK(take(owner->drain()) == 1);
        CHECK(status(*owner, job).state == State::published);
    }
    CHECK(take(owner->usage()).retained_jobs == 4);
    CHECK(take(owner->usage()).pending_bytes == 0);
    CHECK_FALSE(owner->submit(key(4), 1, assets));
    CHECK(take(owner->take(jobs[0])).state == State::published);
    const auto admitted = take(owner->submit(key(4), 1, assets));
    CHECK(admitted.sequence > jobs.back().sequence);
    publish(*owner, admitted);
    CHECK(take(owner->residents()).size() == 5);
}

TEST_CASE("cell residency limits count and simultaneous replacement charges",
          "[world-cell-residency]") {
    const auto assets = take(PreparedAssets::create());
    const auto payload = take(encode_world_cell(sample()));
    const auto charge =
        prepared_scene_maximum_decoded_bytes + payload.size() + assets->retained_bytes();
    WorldCellResidencyLimits limits;
    limits.maximum_resident_cells = 1;
    SECTION("a full cell inventory rejects a second key but admits replacement") {
        const auto owner = take(WorldCellResidency::create(reader(), limits));
        const auto first = take(owner->submit(key(), 1, assets));
        publish(*owner, first);
        const auto second = take(owner->submit(key(1), 1, assets));
        REQUIRE(wait_state(*owner, second, State::ready));
        CHECK(take(owner->drain()) == 1);
        CHECK(status(*owner, second).state == State::failed);
        CHECK_FALSE(owner->resident(key(1)));
        const auto replacement = take(owner->submit(key(), 2, assets));
        publish(*owner, replacement);
        const auto usage = take(owner->usage());
        CHECK(usage.resident_cells == 1);
        CHECK(usage.resident_bytes == charge);
        CHECK(usage.peak_resident_bytes == 2 * charge);
        CHECK(usage.peak_resident_bytes <= limits.maximum_resident_bytes);
    }
    SECTION("last valid cell is retained if old plus candidate exceeds the byte limit") {
        limits.maximum_resident_bytes = 2 * charge - 1;
        const auto owner = take(WorldCellResidency::create(reader(), limits));
        const auto first = take(owner->submit(key(), 1, assets));
        const auto original = publish(*owner, first);
        const auto before = take(owner->resident(key()));
        const auto replacement = take(owner->submit(key(), 2, assets));
        REQUIRE(wait_state(*owner, replacement, State::ready));
        CHECK(take(owner->drain()) == 1);
        CHECK(status(*owner, replacement).state == State::failed);
        CHECK(take(owner->resident(key())) == before);
        CHECK(take(owner->residents()).front().generation == *original.resident_generation);
        CHECK(take(owner->usage()).peak_resident_bytes == charge);
    }
}

TEST_CASE("cell admission reserves pending payload and asset bytes until completion",
          "[world-cell-residency]") {
    const auto assets = take(PreparedAssets::create());
    const auto reservation = world_cell_maximum_payload_bytes + assets->retained_bytes();
    ReaderGate gate;
    auto source = reader();
    WorldCellResidencyLimits limits;
    limits.maximum_pending_bytes = reservation;
    const auto owner = take(WorldCellResidency::create(
        [&](const WorldCellReadRequest& request,
            std::stop_token stop) -> Result<std::vector<std::byte>> {
            if (!gate.block())
                return Error{ErrorCode::execution_failed, "Reader gate watchdog expired", {}};
            return source(request, stop);
        },
        limits));
    const ReleaseGate release{gate};
    const auto first = take(owner->submit(key(), 1, assets));
    REQUIRE(gate.wait_entered());
    CHECK(take(owner->usage()).pending_bytes == reservation);
    CHECK_FALSE(owner->submit(key(1), 1, assets));
    CHECK(take(owner->cancel(first)).cancellation_requested);
    CHECK(take(owner->usage()).pending_bytes == reservation);
    gate.release();
    REQUIRE(wait_state(*owner, first, State::ready));
    CHECK(take(owner->drain()) == 1);
    CHECK(take(owner->usage()).pending_bytes == 0);
    const auto retry = take(owner->submit(key(1), 1, assets));
    publish(*owner, retry);
    const auto usage = take(owner->usage());
    CHECK(usage.peak_pending_bytes == reservation);
    CHECK(usage.bytes_read == 2 * take(encode_world_cell(sample())).size());
    CHECK(usage.retained_jobs == 2);
}

TEST_CASE("invalid cell completions and reader failures never acquire residency",
          "[world-cell-residency]") {
    const auto assets = take(PreparedAssets::create());
    auto base = sample();
    const auto owner = take(WorldCellResidency::create(
        [base](const WorldCellReadRequest& request,
               std::stop_token) mutable -> Result<std::vector<std::byte>> {
            if (request.content_revision == 1)
                throw std::runtime_error{"Fixture reader exception"};
            if (request.content_revision == 2)
                return std::vector<std::byte>{std::byte{7}};
            if (request.content_revision == 3)
                return std::vector<std::byte>(request.maximum_bytes + 1, std::byte{0});
            if (request.content_revision == 7) {
                Error error{ErrorCode::io_failure, std::string(1025, 'm'), {}};
                error.context.request_id.assign(1025, 'r');
                error.context.path.reserve(4096);
                error.context.path.assign("short text with excessive retained capacity");
                return error;
            }
            if (request.content_revision == 8) {
                std::vector<std::byte> excessive_capacity;
                excessive_capacity.reserve(request.maximum_bytes + 1);
                excessive_capacity.resize(1);
                return excessive_capacity;
            }
            auto value = base;
            value.key = request.key;
            value.content_revision = request.content_revision;
            value.scene.world_origin =
                WorldPosition{request.key.world, request.key.cell, {0, 0, 0}};
            if (request.content_revision == 4) {
                ++value.key.cell.x;
                value.scene.world_origin->cell = value.key.cell;
            }
            if (request.content_revision == 5)
                ++value.content_revision;
            if (request.content_revision == 6)
                value.scene.entities[0].mesh_renderer->mesh_index = 1;
            return encode_world_cell(value);
        }));
    for (std::uint64_t revision = 1; revision <= 8; ++revision) {
        const auto job = take(owner->submit(key(), revision, assets));
        REQUIRE(wait_state(*owner, job, State::ready));
        CHECK(take(owner->drain()) == 1);
        const auto failed = status(*owner, job);
        CHECK(failed.state == State::failed);
        REQUIRE(failed.error);
        if (revision == 7) {
            CHECK(failed.error->code == ErrorCode::io_failure);
            CHECK(failed.error->message.empty());
            CHECK(failed.error->context.request_id.empty());
            CHECK(failed.error->context.path.empty());
        } else {
            CHECK_FALSE(failed.error->message.empty());
        }
        CHECK_FALSE(owner->resident(key()));
        CHECK(take(owner->usage()).pending_bytes == 0);
        CHECK(take(owner->take(job)).state == State::failed);
    }
    const auto healthy = take(owner->submit(key(), 9, assets));
    publish(*owner, healthy);
    CHECK(take(owner->resident(key()))->content_revision() == 9);
    CHECK_FALSE(owner->submit(key(1), 0, assets));
    CHECK_FALSE(owner->submit(key(1), 1, nullptr));
    auto invalid = key();
    invalid.cell.x = world_cell_max + 1;
    CHECK_FALSE(owner->submit(invalid, 1, assets));
}

TEST_CASE("cell residency rejects invalid limits and foreign job owners",
          "[world-cell-residency]") {
    CHECK_FALSE(WorldCellResidency::create({}));
    for (const auto cells : std::array<std::size_t, 2>{0, 9}) {
        WorldCellResidencyLimits limits;
        limits.maximum_resident_cells = cells;
        CHECK_FALSE(WorldCellResidency::create(reader(), limits));
    }
    for (const auto bytes : std::array<std::size_t, 2>{0, 256 * 1024 * 1024 + 1}) {
        WorldCellResidencyLimits limits;
        limits.maximum_resident_bytes = bytes;
        CHECK_FALSE(WorldCellResidency::create(reader(), limits));
    }
    for (const auto bytes : std::array<std::size_t, 2>{0, 160 * 1024 * 1024 + 1}) {
        WorldCellResidencyLimits limits;
        limits.maximum_pending_bytes = bytes;
        CHECK_FALSE(WorldCellResidency::create(reader(), limits));
    }
    const auto assets = take(PreparedAssets::create());
    const auto first = take(WorldCellResidency::create(reader()));
    const auto second = take(WorldCellResidency::create(reader()));
    const auto first_job = take(first->submit(key(), 1, assets));
    const auto second_job = take(second->submit(key(), 1, assets));
    CHECK(first_job.sequence == second_job.sequence);
    CHECK(first_job.scope != second_job.scope);
    REQUIRE(wait_state(*first, first_job, State::ready));
    REQUIRE(wait_state(*second, second_job, State::ready));
    CHECK_FALSE(first->cancel(second_job));
    CHECK_FALSE(first->take(second_job));
    CHECK_FALSE(second->cancel(first_job));
    CHECK_FALSE(second->take(first_job));
    CHECK(status(*first, first_job).state == State::ready);
    CHECK(status(*second, second_job).state == State::ready);
    publish(*first, first_job);
    publish(*second, second_job);
}

TEST_CASE("cell closure joins readers and retains published owners and original outcomes",
          "[world-cell-residency]") {
    const auto assets = take(PreparedAssets::create());
    ReaderGate entered;
    std::atomic<bool> exited{false};
    std::atomic<bool> observed_stop{false};
    auto source = reader();
    const auto owner = take(
        WorldCellResidency::create([&](const WorldCellReadRequest& request,
                                       std::stop_token stop) -> Result<std::vector<std::byte>> {
            if (request.content_revision == 2) {
                std::mutex wait_mutex;
                std::condition_variable_any wake;
                std::unique_lock lock{wait_mutex};
                {
                    const std::lock_guard entered_lock{entered.mutex};
                    entered.entered = true;
                    entered.wake.notify_all();
                }
                static_cast<void>(
                    wake.wait_for(lock, stop, std::chrono::seconds{5}, [] { return false; }));
                observed_stop.store(stop.stop_requested());
                exited.store(true);
            }
            return source(request, stop);
        }));
    const auto first = take(owner->submit(key(), 1, assets));
    const auto published = publish(*owner, first);
    const auto blocked = take(owner->submit(key(1), 2, assets));
    REQUIRE(entered.wait_entered());
    bool wrong_thread_rejected{};
    std::thread other([&] {
        const auto observed = owner->usage();
        wrong_thread_rejected = !observed && observed.error().code == ErrorCode::conflict;
    });
    other.join();
    CHECK(wrong_thread_rejected);
    take(owner->close());
    CHECK(exited.load());
    CHECK(observed_stop.load());
    CHECK(take(owner->usage()).closed);
    CHECK(take(owner->usage()).pending_bytes == 0);
    CHECK(status(*owner, blocked).state == State::cancelled);
    CHECK(take(owner->cancel(first)).resident_generation == published.resident_generation);
    CHECK(take(owner->resident(key()))->content_revision() == 1);
    CHECK_FALSE(owner->submit(key(2), 1, assets));
    take(owner->close());
    CHECK(take(owner->take(blocked)).state == State::cancelled);
    CHECK(take(owner->take(first)).state == State::published);
    take(owner->evict(key(), *published.resident_generation));
    CHECK(take(owner->residents()).empty());
}
