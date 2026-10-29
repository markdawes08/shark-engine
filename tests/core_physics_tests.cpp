#include <shark/physics.hpp>

// This dedicated native-boundary fixture privately links Jolt to inspect its
// process registrations. Public consumers use only shark/physics.hpp.
#include <Jolt/Jolt.h>

// Keep the required base header above the native inspection headers.
#include <Jolt/Core/Factory.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <latch>
#include <limits>
#include <memory>
#include <thread>
#include <type_traits>

namespace {
using namespace shark;

template <typename T> T take(Result<T> result) {
    INFO((result ? std::string{} : result.error().message));
    INFO((result ? std::string{} : result.error().context.operation));
    REQUIRE(result);
    return std::move(result).value();
}
void accepted(Result<void> result) {
    INFO((result ? std::string{} : result.error().message));
    REQUIRE(result);
}
template <typename T> void rejected(const Result<T>& result, ErrorCode expected) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == expected);
    CHECK_FALSE(result.error().message.empty());
    CHECK_FALSE(result.error().context.operation.empty());
}

PhysicsWorldConfig small_config() {
    PhysicsWorldConfig config;
    config.max_bodies = 8;
    config.max_body_pairs = 32;
    config.max_contact_constraints = 16;
    config.temporary_bytes = physics_minimum_temporary_bytes;
    return config;
}

struct NativeGlobals {
    JPH::AllocateFunction allocate;
    JPH::ReallocateFunction reallocate;
    JPH::FreeFunction free;
    JPH::AlignedAllocateFunction aligned_allocate;
    JPH::AlignedFreeFunction aligned_free;
    JPH::TraceFunction trace;
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailedFunction assert_failed;
#endif
    bool operator==(const NativeGlobals&) const = default;
};
NativeGlobals capture() {
    return {JPH::Allocate,
            JPH::Reallocate,
            JPH::Free,
            JPH::AlignedAllocate,
            JPH::AlignedFree,
            JPH::Trace
#ifdef JPH_ENABLE_ASSERTS
            ,
            JPH::AssertFailed
#endif
    };
}
void restore(const NativeGlobals& saved) noexcept {
    JPH::Allocate = saved.allocate;
    JPH::Reallocate = saved.reallocate;
    JPH::Free = saved.free;
    JPH::AlignedAllocate = saved.aligned_allocate;
    JPH::AlignedFree = saved.aligned_free;
    JPH::Trace = saved.trace;
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailed = saved.assert_failed;
#endif
}
struct RestoreNativeGlobals {
    NativeGlobals previous{capture()};
    JPH::Factory* factory{JPH::Factory::sInstance};
    ~RestoreNativeGlobals() {
        JPH::Factory::sInstance = factory;
        restore(previous);
    }
};

NativeGlobals sentinel_targets{};
void* allocate_sentinel(std::size_t bytes) {
    return sentinel_targets.allocate(bytes);
}
void* reallocate_sentinel(void* pointer, std::size_t old_bytes, std::size_t new_bytes) {
    return sentinel_targets.reallocate(pointer, old_bytes, new_bytes);
}
void free_sentinel(void* pointer) {
    sentinel_targets.free(pointer);
}
void* aligned_allocate_sentinel(std::size_t bytes, std::size_t alignment) {
    return sentinel_targets.aligned_allocate(bytes, alignment);
}
void aligned_free_sentinel(void* pointer) {
    sentinel_targets.aligned_free(pointer);
}
void trace_sentinel([[maybe_unused]] const char* format, ...) {}
#ifdef JPH_ENABLE_ASSERTS
bool assert_sentinel([[maybe_unused]] const char* expression, [[maybe_unused]] const char* message,
                     [[maybe_unused]] const char* file, [[maybe_unused]] JPH::uint line) {
    return false;
}
#endif

static_assert(!std::is_copy_constructible_v<PhysicsWorld>);
static_assert(!std::is_move_constructible_v<PhysicsWorld>);
} // namespace

TEST_CASE("Physics world performs explicit fixed empty updates and retains closed statistics",
          "[core-physics]") {
    const auto original_globals = capture();
    REQUIRE(JPH::Factory::sInstance == nullptr);
    auto world = take(PhysicsWorld::create());
    REQUIRE(JPH::Factory::sInstance != nullptr);
    const auto initial = take(world->stats());
    CHECK(initial.config == PhysicsWorldConfig{});
    CHECK(initial.completed_ticks == 0);
    CHECK(initial.bodies == 0);
    CHECK_FALSE(initial.closed);
    CHECK_FALSE(initial.faulted);
    for (std::uint64_t tick = 1; tick <= 60; ++tick) {
        accepted(world->step());
        const auto stats = take(world->stats());
        CHECK(stats.completed_ticks == tick);
        CHECK(stats.bodies == 0);
        CHECK_FALSE(stats.faulted);
    }
    accepted(world->close());
    const auto closed = take(world->stats());
    CHECK(closed.closed);
    CHECK_FALSE(closed.faulted);
    CHECK(closed.bodies == 0);
    CHECK(closed.completed_ticks == 60);
    CHECK(closed.config == initial.config);
    REQUIRE(JPH::Factory::sInstance == nullptr);
    CHECK(capture() == original_globals);
    accepted(world->close());
    rejected(world->step(), ErrorCode::conflict);
    CHECK(take(world->stats()).completed_ticks == 60);
}

TEST_CASE("Physics configuration rejects invalid capacities and unrepresentable gravity first",
          "[core-physics]") {
    const auto native_before = capture();
    const auto invalid = [&](PhysicsWorldConfig config) {
        rejected(PhysicsWorld::create(config), ErrorCode::invalid_configuration);
        CHECK(capture() == native_before);
        CHECK(JPH::Factory::sInstance == nullptr);
    };
    for (const auto frequency : std::array<std::uint32_t, 3>{0, 1001, UINT32_MAX}) {
        auto config = small_config();
        config.tick_hz = frequency;
        invalid(config);
    }
    for (const auto steps :
         std::array<std::uint32_t, 2>{0, physics_maximum_collision_substeps + 1}) {
        auto config = small_config();
        config.collision_substeps = steps;
        invalid(config);
    }
    for (const auto count : std::array<std::uint32_t, 2>{0, physics_maximum_bodies + 1}) {
        auto config = small_config();
        config.max_bodies = count;
        invalid(config);
    }
    for (const auto count : std::array<std::uint32_t, 3>{0, 3, physics_maximum_body_pairs + 1}) {
        auto config = small_config();
        config.max_body_pairs = count;
        invalid(config);
    }
    for (const auto count :
         std::array<std::uint32_t, 3>{0, 3, physics_maximum_contact_constraints + 1}) {
        auto config = small_config();
        config.max_contact_constraints = count;
        invalid(config);
    }
    for (const auto bytes : std::array<std::size_t, 3>{0, physics_minimum_temporary_bytes - 1,
                                                       physics_maximum_temporary_bytes + 1}) {
        auto config = small_config();
        config.temporary_bytes = bytes;
        invalid(config);
    }
    for (const auto component :
         std::array{std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::max(),
                    physics_gravity_component_limit + 1, std::numeric_limits<double>::denorm_min(),
                    static_cast<double>(std::numeric_limits<float>::denorm_min())}) {
        auto config = small_config();
        config.gravity.x = component;
        invalid(config);
    }
}

TEST_CASE("Physics tick policy and collision substeps remain independently configured",
          "[core-physics]") {
    for (const auto frequency : std::array<std::uint32_t, 4>{1, 20, 60, 1000}) {
        auto config = small_config();
        config.tick_hz = frequency;
        config.collision_substeps = frequency == 20 ? 3 : physics_maximum_collision_substeps;
        config.gravity = {0, 0, 0};
        auto world = take(PhysicsWorld::create(config));
        for (int tick = 0; tick < 4; ++tick)
            accepted(world->step());
        const auto stats = take(world->stats());
        CHECK(stats.config == config);
        CHECK(stats.completed_ticks == 4);
        CHECK(stats.bodies == 0);
        CHECK_FALSE(stats.faulted);
    }
    auto minimum = small_config();
    minimum.max_bodies = 1;
    minimum.max_body_pairs = physics_minimum_body_pairs;
    minimum.max_contact_constraints = physics_minimum_contact_constraints;
    auto world = take(PhysicsWorld::create(minimum));
    accepted(world->step());
    CHECK(take(world->stats()).config == minimum);
}

TEST_CASE("Physics native registrations outlive every world and restore prior callbacks",
          "[core-physics]") {
    REQUIRE(JPH::Factory::sInstance == nullptr);
    const RestoreNativeGlobals environment;
    JPH::RegisterDefaultAllocator();
    sentinel_targets = capture();
    JPH::Allocate = allocate_sentinel;
    JPH::Reallocate = reallocate_sentinel;
    JPH::Free = free_sentinel;
    JPH::AlignedAllocate = aligned_allocate_sentinel;
    JPH::AlignedFree = aligned_free_sentinel;
    JPH::Trace = trace_sentinel;
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailed = assert_sentinel;
#endif
    const auto sentinels = capture();
    auto first = take(PhysicsWorld::create(small_config()));
    auto* shared_factory = JPH::Factory::sInstance;
    auto second = take(PhysicsWorld::create(small_config()));
    REQUIRE(shared_factory != nullptr);
    CHECK(JPH::Factory::sInstance == shared_factory);
    CHECK_FALSE(capture() == sentinels);
    accepted(first->step());
    accepted(first->close());
    CHECK(JPH::Factory::sInstance == shared_factory);
    accepted(second->step());
    second.reset();
    CHECK(JPH::Factory::sInstance == nullptr);
    CHECK(capture() == sentinels);
    auto recreated = take(PhysicsWorld::create(small_config()));
    accepted(recreated->step());
    accepted(recreated->close());
    CHECK(JPH::Factory::sInstance == nullptr);
    CHECK(capture() == sentinels);
}

TEST_CASE("Physics initialization refuses a foreign factory without claiming its callbacks",
          "[core-physics]") {
    REQUIRE(JPH::Factory::sInstance == nullptr);
    const RestoreNativeGlobals environment;
    JPH::RegisterDefaultAllocator();
    const auto before = capture();
    const auto foreign = std::make_unique<JPH::Factory>();
    JPH::Factory::sInstance = foreign.get();
    rejected(PhysicsWorld::create(small_config()), ErrorCode::conflict);
    CHECK(JPH::Factory::sInstance == foreign.get());
    CHECK(capture() == before);
    JPH::Factory::sInstance = nullptr;
}

TEST_CASE("Physics owner guards reject foreign operations while separate worlds coexist",
          "[core-physics]") {
    auto world = take(PhysicsWorld::create(small_config()));
    std::latch ready{1};
    std::latch proceed{1};
    bool rejected_calls = false;
    bool secondary_created = false;
    bool secondary_stepped = false;
    bool secondary_closed = false;
    std::exception_ptr failure;
    std::thread worker([&] {
        std::unique_ptr<PhysicsWorld> secondary;
        try {
            const auto step = world->step();
            const auto stats = world->stats();
            const auto close = world->close();
            rejected_calls = !step && !stats && !close &&
                             step.error().code == ErrorCode::conflict &&
                             stats.error().code == ErrorCode::conflict &&
                             close.error().code == ErrorCode::conflict;
            auto created = PhysicsWorld::create(small_config());
            secondary_created = static_cast<bool>(created);
            if (created)
                secondary = std::move(created).value();
        } catch (...) {
            failure = std::current_exception();
        }
        ready.count_down();
        proceed.wait();
        try {
            if (secondary) {
                secondary_stepped = static_cast<bool>(secondary->step());
                secondary_closed = static_cast<bool>(secondary->close());
            }
        } catch (...) {
            failure = std::current_exception();
        }
    });
    ready.wait();
    const auto primary_step = world->step();
    proceed.count_down();
    worker.join();
    accepted(primary_step);
    CHECK_FALSE(failure);
    CHECK(rejected_calls);
    CHECK(secondary_created);
    CHECK(secondary_stepped);
    CHECK(secondary_closed);
    CHECK(take(world->stats()).completed_ticks == 1);
    accepted(world->close());
    // Like Scene, explicit owner-thread close permits later value destruction
    // elsewhere; a live owner cannot be transferred for native teardown.
    std::thread destroy_closed([closed = std::move(world)]() mutable { closed.reset(); });
    destroy_closed.join();
    CHECK(JPH::Factory::sInstance == nullptr);
}
