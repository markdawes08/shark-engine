#include "shark/physics.hpp"

#include <Jolt/Jolt.h>

// Jolt's base header defines the macros required by every other native header.
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>

namespace shark {
namespace {

Error physics_error(ErrorCode code, const char* message, const char* operation) {
    return {code, message, {.operation = operation}};
}

struct NativeDiagnostic {
    std::array<char, 512> message{};
    bool reported{false};
};
thread_local NativeDiagnostic native_diagnostic;

void native_trace(const char* format, ...) noexcept {
    if (native_diagnostic.reported)
        return;
    native_diagnostic.reported = true;
    va_list arguments;
    va_start(arguments, format);
    static_cast<void>(std::vsnprintf(native_diagnostic.message.data(),
                                     native_diagnostic.message.size(), format, arguments));
    va_end(arguments);
    native_diagnostic.message.back() = '\0';
    // Native diagnostics become bounded valid ASCII, including unexpected
    // platform/source text, without allocating or throwing in the callback.
    for (auto& byte : native_diagnostic.message) {
        if (byte == '\0')
            break;
        const auto value = static_cast<unsigned char>(byte);
        if (value < 0x20U || value >= 0x7fU)
            byte = '?';
    }
}

#ifdef JPH_ENABLE_ASSERTS
[[noreturn]] bool native_assert([[maybe_unused]] const char* expression,
                                [[maybe_unused]] const char* message,
                                [[maybe_unused]] const char* file,
                                [[maybe_unused]] JPH::uint line) noexcept {
    // Input is checked before native access. A Jolt assertion is an internal
    // programmer invariant, not an external-input Result or a debugger dialog.
    std::terminate();
}
#endif

Error reported_error(const char* operation) {
    return {ErrorCode::execution_failed,
            native_diagnostic.message.front() ? native_diagnostic.message.data()
                                              : "Native physics reported a diagnostic",
            {.operation = operation}};
}

struct SavedNativeGlobals {
    JPH::AllocateFunction allocate;
    JPH::ReallocateFunction reallocate;
    JPH::FreeFunction free;
    JPH::AlignedAllocateFunction aligned_allocate;
    JPH::AlignedFreeFunction aligned_free;
    JPH::TraceFunction trace;
#ifdef JPH_ENABLE_ASSERTS
    JPH::AssertFailedFunction assert_failed;
#endif
};

SavedNativeGlobals capture_globals() noexcept {
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
void restore_globals(const SavedNativeGlobals& saved) noexcept {
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

struct ProcessPhysics {
    std::mutex mutex;
    std::size_t leases{0};
    JPH::Factory* factory{nullptr};
    SavedNativeGlobals previous{};
};
ProcessPhysics& process_physics() {
    static ProcessPhysics process;
    return process;
}

class NativeLease final {
  public:
    static Result<std::unique_ptr<NativeLease>> acquire() {
        auto lease = std::unique_ptr<NativeLease>{new NativeLease};
        auto& process = process_physics();
        const std::lock_guard lock{process.mutex};
        if (process.leases == std::numeric_limits<std::size_t>::max())
            return physics_error(ErrorCode::conflict, "Physics registration leases are exhausted",
                                 "physics.world.create");
        if (process.leases == 0) {
            if (JPH::Factory::sInstance != nullptr)
                return physics_error(ErrorCode::conflict,
                                     "A foreign native physics factory already owns registration",
                                     "physics.world.create");
            if (!JPH::VerifyJoltVersionID())
                return physics_error(ErrorCode::invalid_configuration,
                                     "Native physics headers and library are incompatible",
                                     "physics.world.create");
            process.previous = capture_globals();
            JPH::RegisterDefaultAllocator();
            JPH::Trace = native_trace;
#ifdef JPH_ENABLE_ASSERTS
            JPH::AssertFailed = native_assert;
#endif
            try {
                process.factory = new JPH::Factory;
                JPH::Factory::sInstance = process.factory;
                JPH::RegisterTypes();
            } catch (...) {
                JPH::UnregisterTypes();
                delete process.factory;
                process.factory = nullptr;
                JPH::Factory::sInstance = nullptr;
                restore_globals(process.previous);
                throw;
            }
        }
        ++process.leases;
        lease->active_ = true;
        return lease;
    }

    ~NativeLease() {
        if (!active_)
            return;
        auto& process = process_physics();
        const std::lock_guard lock{process.mutex};
        if (process.leases == 0 || JPH::Factory::sInstance != process.factory)
            std::terminate();
        --process.leases;
        if (process.leases == 0) {
            JPH::UnregisterTypes();
            delete process.factory;
            process.factory = nullptr;
            JPH::Factory::sInstance = nullptr;
            restore_globals(process.previous);
        }
    }

  private:
    NativeLease() = default;
    bool active_{false};
};

// One private broadphase layer is sufficient for the empty 18a world. There is
// no public layer/filter API until its owning checkpoint supplies it.
class SingleBroadPhaseLayer final : public JPH::BroadPhaseLayerInterface {
  public:
    JPH::uint GetNumBroadPhaseLayers() const override {
        return 1;
    }
    JPH::BroadPhaseLayer
    GetBroadPhaseLayer([[maybe_unused]] JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer{0};
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName([[maybe_unused]] JPH::BroadPhaseLayer layer) const override {
        return "Shark";
    }
#endif
};
class AllBroadPhaseLayers final : public JPH::ObjectVsBroadPhaseLayerFilter {
  public:
    bool ShouldCollide([[maybe_unused]] JPH::ObjectLayer object,
                       [[maybe_unused]] JPH::BroadPhaseLayer broad_phase) const override {
        return true;
    }
};
class AllLayerPairs final : public JPH::ObjectLayerPairFilter {
  public:
    bool ShouldCollide([[maybe_unused]] JPH::ObjectLayer first,
                       [[maybe_unused]] JPH::ObjectLayer second) const override {
        return true;
    }
};

Result<void> validate(const PhysicsWorldConfig& config) {
    if (config.tick_hz == 0 || config.tick_hz > 1000 || config.collision_substeps == 0 ||
        config.collision_substeps > physics_maximum_collision_substeps || config.max_bodies == 0 ||
        config.max_bodies > physics_maximum_bodies ||
        config.max_body_pairs < physics_minimum_body_pairs ||
        config.max_body_pairs > physics_maximum_body_pairs ||
        config.max_contact_constraints < physics_minimum_contact_constraints ||
        config.max_contact_constraints > physics_maximum_contact_constraints ||
        config.temporary_bytes < physics_minimum_temporary_bytes ||
        config.temporary_bytes > physics_maximum_temporary_bytes)
        return physics_error(ErrorCode::invalid_configuration,
                             "Physics configuration exceeds its bounded capacities or tick policy",
                             "physics.world.create");
    for (int axis = 0; axis < 3; ++axis) {
        const auto component = config.gravity[axis];
        if (!std::isfinite(component) || std::abs(component) > physics_gravity_component_limit)
            return physics_error(ErrorCode::invalid_configuration,
                                 "Physics gravity must fit its finite normal float bounds",
                                 "physics.world.create");
        const auto packed = static_cast<float>(component);
        if (!std::isfinite(packed) || (component != 0.0 && packed == 0.0F) ||
            std::fpclassify(packed) == FP_SUBNORMAL)
            return physics_error(ErrorCode::invalid_configuration,
                                 "Physics gravity must fit its finite normal float bounds",
                                 "physics.world.create");
    }
    return {};
}

} // namespace

struct PhysicsWorld::Impl {
    explicit Impl(PhysicsWorldConfig value) : config(value) {}
    PhysicsWorldConfig config;
    std::thread::id owner{std::this_thread::get_id()};
    std::uint64_t completed_ticks{0};
    bool closed{false};
    bool faulted{false};
    SingleBroadPhaseLayer broad_phase;
    AllBroadPhaseLayers broad_phase_filter;
    AllLayerPairs pair_filter;
    // Declaration order preserves borrowed filters and global callbacks until
    // all native resources have been destroyed, including partial startup.
    std::unique_ptr<NativeLease> lease;
    std::unique_ptr<JPH::TempAllocatorImpl> temporary;
    std::unique_ptr<JPH::JobSystemSingleThreaded> jobs;
    std::unique_ptr<JPH::PhysicsSystem> system;

    Result<void> check_owner(const char* operation) const {
        if (owner != std::this_thread::get_id())
            return physics_error(ErrorCode::conflict,
                                 "Physics operations require the creating thread", operation);
        return {};
    }
    void release() noexcept {
        system.reset();
        jobs.reset();
        temporary.reset();
        lease.reset();
        closed = true;
    }
};

PhysicsWorld::PhysicsWorld(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
PhysicsWorld::~PhysicsWorld() {
    if (!impl_->closed && impl_->owner != std::this_thread::get_id())
        std::terminate();
}

Result<std::unique_ptr<PhysicsWorld>> PhysicsWorld::create(PhysicsWorldConfig config) {
    if (auto checked = validate(config); !checked)
        return checked.error();
    try {
        native_diagnostic = {};
        auto impl = std::make_unique<Impl>(config);
        auto lease = NativeLease::acquire();
        if (!lease)
            return std::move(lease).error();
        impl->lease = std::move(lease).value();
        impl->temporary = std::make_unique<JPH::TempAllocatorImpl>(config.temporary_bytes);
        impl->jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
        impl->system = std::make_unique<JPH::PhysicsSystem>();
        impl->system->Init(config.max_bodies, 1, config.max_body_pairs,
                           config.max_contact_constraints, impl->broad_phase,
                           impl->broad_phase_filter, impl->pair_filter);
        impl->system->SetGravity(JPH::Vec3{static_cast<float>(config.gravity.x),
                                           static_cast<float>(config.gravity.y),
                                           static_cast<float>(config.gravity.z)});
        if (native_diagnostic.reported)
            return reported_error("physics.world.create");
        return std::unique_ptr<PhysicsWorld>{new PhysicsWorld{std::move(impl)}};
    } catch (const std::bad_alloc&) {
        return physics_error(ErrorCode::execution_failed, "Physics startup allocation failed",
                             "physics.world.create");
    } catch (...) {
        return physics_error(ErrorCode::execution_failed, "Cannot initialize the physics owner",
                             "physics.world.create");
    }
}

Result<void> PhysicsWorld::step() {
    if (auto checked = impl_->check_owner("physics.world.step"); !checked)
        return checked;
    if (impl_->closed || impl_->faulted)
        return physics_error(ErrorCode::conflict, "Physics owner is closed or faulted",
                             "physics.world.step");
    if (impl_->completed_ticks == std::numeric_limits<std::uint64_t>::max())
        return physics_error(ErrorCode::conflict, "Physics tick identities are exhausted",
                             "physics.world.step");
    try {
        native_diagnostic = {};
        const auto outcome =
            impl_->system->Update(1.0F / static_cast<float>(impl_->config.tick_hz),
                                  static_cast<int>(impl_->config.collision_substeps),
                                  impl_->temporary.get(), impl_->jobs.get());
        if (outcome != JPH::EPhysicsUpdateError::None || native_diagnostic.reported ||
            !impl_->temporary->IsEmpty()) {
            impl_->faulted = true;
            if (native_diagnostic.reported)
                return reported_error("physics.world.step");
            return physics_error(ErrorCode::execution_failed,
                                 "Native physics update failed; partial progress cannot be retried",
                                 "physics.world.step");
        }
        ++impl_->completed_ticks;
        return {};
    } catch (...) {
        impl_->faulted = true;
        return physics_error(ErrorCode::execution_failed,
                             "Native physics update failed; partial progress cannot be retried",
                             "physics.world.step");
    }
}

Result<PhysicsWorldStats> PhysicsWorld::stats() const {
    if (auto checked = impl_->check_owner("physics.world.stats"); !checked)
        return checked.error();
    return PhysicsWorldStats{impl_->config, impl_->completed_ticks,
                             impl_->system ? impl_->system->GetNumBodies() : 0, impl_->closed,
                             impl_->faulted};
}

Result<void> PhysicsWorld::close() {
    if (auto checked = impl_->check_owner("physics.world.close"); !checked)
        return checked;
    if (impl_->closed)
        return {};
    native_diagnostic = {};
    impl_->release();
    if (native_diagnostic.reported) {
        impl_->faulted = true;
        return reported_error("physics.world.close");
    }
    return {};
}

} // namespace shark
