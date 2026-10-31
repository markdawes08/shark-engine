#include "shark/physics.hpp"
#include "shark/scene.hpp"

#include "scene_physics_access.hpp"

#include <Jolt/Jolt.h>

// Jolt's base header defines the macros required by every other native header.
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/ContactConstraintManager.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsUpdateContext.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace shark {
namespace {

Error physics_error(ErrorCode code, const char* message, const char* operation) {
    return {code, message, {.operation = operation}};
}

std::atomic<std::uint64_t> next_world{1};

struct NativeDiagnostic {
    std::array<char, 512> message{};
    bool reported{false};
};
thread_local NativeDiagnostic native_diagnostic;

class MeteredTemporary final : public JPH::TempAllocator {
  public:
    explicit MeteredTemporary(std::size_t capacity) : arena_(capacity) {}
    void* Allocate(JPH::uint size) override {
        auto* result = arena_.Allocate(size);
        used_ += aligned(size);
        peak_ = std::max(peak_, used_);
        return result;
    }
    void Free(void* address, JPH::uint size) override {
        arena_.Free(address, size);
        used_ -= aligned(size);
    }
    bool IsEmpty() const {
        return arena_.IsEmpty();
    }
    std::size_t peak() const {
        return peak_;
    }

  private:
    static std::size_t aligned(JPH::uint size) {
        return (static_cast<std::size_t>(size) + JPH_RVECTOR_ALIGNMENT - 1) &
               ~(static_cast<std::size_t>(JPH_RVECTOR_ALIGNMENT) - 1);
    }
    JPH::TempAllocatorImpl arena_;
    std::size_t used_{0};
    std::size_t peak_{0};
};

// Reviewed against the locked Update/IslandBuilder/ContactConstraintManager
// allocation paths. Discrete rigid primitives only, no constraints/listeners/
// soft bodies, one native job thread, large-island splitting disabled. Include
// simultaneous buffers, Array growth, alignment and a 64 KiB fixed allowance.
std::size_t temporary_requirement(const PhysicsWorldConfig& config) {
    return 64 * 1024 + 2 * config.collision_substeps * sizeof(JPH::PhysicsUpdateContext::Step) +
           config.max_body_pairs * sizeof(JPH::BodyPair) +
           config.max_contact_constraints *
               (static_cast<std::size_t>(JPH::ContactConstraintManager::cMaxConstraintSize) + 16) +
           config.max_bodies * std::size_t{64};
}

// Publication owns no native body until the complete record can be retained.
// A preparation failure removes a candidate without borrowing its Scene.
struct NativeBodyCandidate {
    JPH::BodyInterface& owner;
    JPH::BodyID id;
    bool retained{false};
    ~NativeBodyCandidate() {
        if (!retained) {
            if (owner.IsAdded(id))
                owner.RemoveBody(id);
            owner.DestroyBody(id);
        }
    }
};

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
    explicit Impl(PhysicsWorldConfig value, std::uint64_t token) : instance(token), config(value) {}
    struct BodyRecord {
        EntityReference entity;
        RigidBody settings;
        Transform source_pose;
        JPH::BodyID native;
        Transform previous_pose{};
        std::optional<Transform> target{};
    };
    struct BodySlot {
        std::uint64_t generation{1};
        bool retired{false};
        std::optional<BodyRecord> value;
    };
    const std::uint64_t instance;
    std::vector<BodySlot> bodies;
    Result<std::size_t> checked_body(PhysicsBodyHandle body) const {
        if (body.world != instance || body.slot >= bodies.size() || !bodies[body.slot].value ||
            body.generation != bodies[body.slot].generation)
            return physics_error(ErrorCode::conflict, "Physics body is stale or foreign",
                                 "physics.body");
        return body.slot;
    }
    PhysicsWorldConfig config;
    std::thread::id owner{std::this_thread::get_id()};
    std::uint64_t completed_ticks{0};
    bool closed{false};
    bool faulted{false};
    scene_detail::PhysicsLease simulation;
    SingleBroadPhaseLayer broad_phase;
    AllBroadPhaseLayers broad_phase_filter;
    AllLayerPairs pair_filter;
    // Declaration order preserves borrowed filters and global callbacks until
    // all native resources have been destroyed, including partial startup.
    std::unique_ptr<NativeLease> lease;
    std::unique_ptr<MeteredTemporary> temporary;
    std::unique_ptr<JPH::JobSystemSingleThreaded> jobs;
    std::unique_ptr<JPH::PhysicsSystem> system;

    ~Impl() {
        release();
    }
    Result<void> check_owner(const char* operation) const {
        if (owner != std::this_thread::get_id())
            return physics_error(ErrorCode::conflict,
                                 "Physics operations require the creating thread", operation);
        return {};
    }
    Result<void> check_simulation(const Scene& scene, const char* operation) const {
        if (auto checked = check_owner(operation); !checked)
            return checked;
        if (closed || faulted || !simulation)
            return physics_error(ErrorCode::conflict, "Physics simulation is not ready", operation);
        return scene_detail::ScenePhysicsAccess::check(scene, simulation);
    }
    void release() noexcept {
        if (system) {
            auto& interface = system->GetBodyInterface();
            for (auto& slot : bodies)
                if (slot.value) {
                    interface.RemoveBody(slot.value->native);
                    interface.DestroyBody(slot.value->native);
                    slot.value.reset();
                }
        }
        system.reset();
        jobs.reset();
        temporary.reset();
        simulation.reset();
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
        auto token = next_world.load();
        while (token != std::numeric_limits<std::uint64_t>::max() &&
               !next_world.compare_exchange_weak(token, token + 1)) {
        }
        if (token == std::numeric_limits<std::uint64_t>::max())
            return physics_error(ErrorCode::conflict, "Physics world identities exhausted",
                                 "physics.world.create");
        auto impl = std::make_unique<Impl>(config, token);
        impl->bodies.reserve(config.max_bodies);
        auto lease = NativeLease::acquire();
        if (!lease)
            return std::move(lease).error();
        impl->lease = std::move(lease).value();
        impl->temporary = std::make_unique<MeteredTemporary>(config.temporary_bytes);
        impl->jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);
        impl->system = std::make_unique<JPH::PhysicsSystem>();
        impl->system->Init(config.max_bodies, 1, config.max_body_pairs,
                           config.max_contact_constraints, impl->broad_phase,
                           impl->broad_phase_filter, impl->pair_filter);
        auto settings = impl->system->GetPhysicsSettings();
        settings.mMaxInFlightBodyPairs = static_cast<int>(config.max_body_pairs);
        settings.mUseLargeIslandSplitter = false;
        impl->system->SetPhysicsSettings(settings);
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
    // Nonempty stepping requires 18c's qualified temporary-work admission and
    // scene synchronization. Reject before native work, not after arena OOM.
    if (impl_->simulation || impl_->system->GetNumBodies() != 0)
        return physics_error(ErrorCode::conflict,
                             "Nonempty physics stepping requires scene simulation integration",
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

Result<PhysicsBodyHandle> PhysicsWorld::create_body(const Scene& scene, EntityHandle entity) {
    if (auto checked = impl_->check_owner("physics.body.create"); !checked)
        return checked.error();
    if (impl_->closed || impl_->faulted || impl_->simulation)
        return physics_error(ErrorCode::conflict, "Physics owner is closed or faulted",
                             "physics.body.create");
    try {
        native_diagnostic = {};
        auto source = scene.entity_state(entity);
        if (!source)
            return source.error();
        const auto& value = source.value();
        const auto scene_stats = scene.stats();
        if (!scene_stats)
            return scene_stats.error();
        if (scene_stats.value().pending_mutations != 0 || !value.rigid_body || value.parent ||
            std::holds_alternative<ReplicaSceneScope>(value.reference.scope))
            return physics_error(
                ErrorCode::conflict,
                "Body creation requires a committed authored or authoritative root with RigidBody",
                "physics.body.create");
        auto pose = validate_rigid_body_pose(*value.rigid_body, value.local_transform);
        if (!pose)
            return pose.error();
        for (const auto& slot : impl_->bodies)
            if (slot.value) {
                if (slot.value->entity.handle.scene_instance != entity.scene_instance ||
                    slot.value->entity.handle == entity)
                    return physics_error(
                        ErrorCode::conflict,
                        "Physics world already contains this entity or another scene",
                        "physics.body.create");
            }
        auto found = std::find_if(impl_->bodies.begin(), impl_->bodies.end(),
                                  [](const auto& slot) { return !slot.retired && !slot.value; });
        const auto index = static_cast<std::size_t>(found - impl_->bodies.begin());
        if (index == impl_->bodies.size() && index >= impl_->config.max_bodies)
            return physics_error(ErrorCode::conflict, "Physics body capacity exhausted",
                                 "physics.body.create");
        const auto& settings = *value.rigid_body;
        const auto scale = static_cast<float>(pose.value().scale.x);
        JPH::ShapeSettings::ShapeResult shape;
        if (settings.shape == ColliderShape::box) {
            const auto h = settings.half_extents * pose.value().scale.x;
            shape = JPH::BoxShapeSettings(
                        JPH::Vec3{static_cast<float>(h.x), static_cast<float>(h.y),
                                  static_cast<float>(h.z)},
                        static_cast<float>(std::min(0.05, std::min({h.x, h.y, h.z}) * 0.1)))
                        .Create();
        } else if (settings.shape == ColliderShape::sphere) {
            shape = JPH::SphereShapeSettings(static_cast<float>(settings.radius) * scale).Create();
        } else {
            shape = JPH::CapsuleShapeSettings(static_cast<float>(settings.half_height) * scale,
                                              static_cast<float>(settings.radius) * scale)
                        .Create();
        }
        if (shape.HasError())
            return physics_error(ErrorCode::invalid_argument, shape.GetError().c_str(),
                                 "physics.body.create");
        const auto& t = pose.value();
        const auto motion = settings.motion == BodyMotion::static_body ? JPH::EMotionType::Static
                            : settings.motion == BodyMotion::dynamic_body
                                ? JPH::EMotionType::Dynamic
                                : JPH::EMotionType::Kinematic;
        JPH::BodyCreationSettings native_settings{
            shape.Get(),
            JPH::RVec3{static_cast<float>(t.translation.x), static_cast<float>(t.translation.y),
                       static_cast<float>(t.translation.z)},
            JPH::Quat{static_cast<float>(t.rotation.x), static_cast<float>(t.rotation.y),
                      static_cast<float>(t.rotation.z), static_cast<float>(t.rotation.w)},
            motion, 0};
        native_settings.mFriction = static_cast<float>(settings.friction);
        native_settings.mRestitution = static_cast<float>(settings.restitution);
        native_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        native_settings.mMassPropertiesOverride.mMass = static_cast<float>(settings.mass);
        native_settings.mLinearDamping = 0;
        native_settings.mAngularDamping = 0;
        native_settings.mMaxLinearVelocity = static_cast<float>(physics_maximum_linear_speed);
        native_settings.mMaxAngularVelocity = static_cast<float>(physics_maximum_angular_speed);
        native_settings.mMotionQuality = JPH::EMotionQuality::Discrete;
        auto& interface = impl_->system->GetBodyInterface();
        auto* native = interface.CreateBody(native_settings);
        if (!native)
            return physics_error(ErrorCode::conflict, "Native physics body capacity exhausted",
                                 "physics.body.create");
        const auto id = native->GetID();
        NativeBodyCandidate candidate{interface, id};
        interface.AddBody(id, JPH::EActivation::DontActivate);
        if (native_diagnostic.reported)
            return reported_error("physics.body.create");
        if (index == impl_->bodies.size())
            impl_->bodies.emplace_back();
        auto& slot = impl_->bodies[index];
        // Fence the exact observed source, not a second quaternion
        // normalization's rounded value used to prepare the native pose.
        slot.value.emplace(value.reference, settings, value.local_transform, id);
        slot.value->previous_pose = value.local_transform;
        candidate.retained = true;
        return PhysicsBodyHandle{impl_->instance, static_cast<std::uint32_t>(index),
                                 slot.generation};
    } catch (...) {
        return physics_error(ErrorCode::execution_failed, "Cannot create native physics body",
                             "physics.body.create");
    }
}

Result<void> PhysicsWorld::begin_simulation(Scene& scene) {
    if (auto checked = impl_->check_owner("physics.simulation.begin"); !checked)
        return checked;
    if (impl_->closed || impl_->faulted || impl_->simulation)
        return physics_error(ErrorCode::conflict, "Physics owner cannot begin simulation",
                             "physics.simulation.begin");
    if (impl_->system->GetNumBodies() &&
        temporary_requirement(impl_->config) > impl_->config.temporary_bytes)
        return physics_error(ErrorCode::conflict,
                             "Physics temporary arena cannot admit this configuration",
                             "physics.simulation.begin");
    try {
        const auto stats = scene.stats();
        if (!stats)
            return stats.error();
        if (stats.value().live_entities > physics_maximum_bodies || stats.value().pending_mutations)
            return physics_error(ErrorCode::conflict, "Physics requires a bounded committed Scene",
                                 "physics.simulation.begin");
        std::size_t native_count = 0;
        for (std::size_t index = 0; index < impl_->bodies.size(); ++index) {
            const auto& slot = impl_->bodies[index];
            if (slot.value) {
                auto checked = body_state(
                    scene, {impl_->instance, static_cast<std::uint32_t>(index), slot.generation});
                if (!checked)
                    return checked.error();
                ++native_count;
            }
        }
        SceneQueryOptions query;
        query.fields = {false, false, false, false, false, true};
        query.page_size = scene_query_maximum_page_size;
        query.scan_budget = scene_query_maximum_page_size;
        query.expected_revision = SceneRevision{stats.value().instance, stats.value().revision};
        auto page = scene.query_entities(query);
        std::size_t scanned = 0, components = 0;
        while (page) {
            scanned += page.value().scanned_slots();
            for (const auto& entity : page.value().entities())
                if (entity.rigid_body) {
                    const auto found = std::find_if(
                        impl_->bodies.begin(), impl_->bodies.end(), [&](const auto& slot) {
                            return slot.value && slot.value->entity == entity.reference;
                        });
                    if (found == impl_->bodies.end())
                        return physics_error(ErrorCode::conflict,
                                             "RigidBody is missing its native snapshot",
                                             "physics.simulation.begin");
                    ++components;
                }
            if (!page.value().next_cursor())
                break;
            if (scanned + scene_query_maximum_page_size > 65536)
                return physics_error(ErrorCode::conflict,
                                     "Physics Scene exceeds its slot scan bound",
                                     "physics.simulation.begin");
            page = scene.continue_query(*page.value().next_cursor());
        }
        if (!page)
            return page.error();
        if (components != native_count)
            return physics_error(ErrorCode::conflict, "Native body inventory differs from Scene",
                                 "physics.simulation.begin");
        auto lease = std::make_shared<const std::uint64_t>(impl_->instance);
        if (auto checked = scene_detail::ScenePhysicsAccess::bind(scene, lease); !checked)
            return checked;
        impl_->simulation = std::move(lease);
        native_diagnostic = {};
        for (auto& slot : impl_->bodies)
            if (slot.value) {
                auto& record = *slot.value;
                record.previous_pose = record.source_pose;
                record.target.reset();
                if (record.settings.motion != BodyMotion::static_body)
                    impl_->system->GetBodyInterface().ActivateBody(record.native);
            }
        if (native_diagnostic.reported) {
            impl_->faulted = true;
            return reported_error("physics.simulation.begin");
        }
        return {};
    } catch (...) {
        if (impl_->simulation)
            impl_->faulted = true;
        return physics_error(ErrorCode::execution_failed, "Cannot begin physics simulation",
                             "physics.simulation.begin");
    }
}

Result<void> PhysicsWorld::end_simulation(Scene& scene) {
    if (auto checked = impl_->check_simulation(scene, "physics.simulation.end"); !checked)
        return checked;
    native_diagnostic = {};
    for (auto& slot : impl_->bodies)
        if (slot.value) {
            slot.value->target.reset();
            slot.value->previous_pose = slot.value->source_pose;
            if (slot.value->settings.motion == BodyMotion::kinematic_body)
                impl_->system->GetBodyInterface().SetLinearAndAngularVelocity(
                    slot.value->native, JPH::Vec3::sZero(), JPH::Vec3::sZero());
        }
    if (native_diagnostic.reported) {
        impl_->faulted = true;
        return reported_error("physics.simulation.end");
    }
    if (auto checked = scene_detail::ScenePhysicsAccess::unbind(scene, impl_->simulation); !checked)
        return checked;
    impl_->simulation.reset();
    return {};
}

Result<void> PhysicsWorld::step(Scene& scene) {
    if (auto checked = impl_->check_simulation(scene, "physics.simulation.step"); !checked)
        return checked;
    if (impl_->completed_ticks == std::numeric_limits<std::uint64_t>::max())
        return physics_error(ErrorCode::conflict, "Physics tick identities exhausted",
                             "physics.simulation.step");
    try {
        // Reserve the output before any native movement. Later Scene validation
        // can still fail; that faults the world and retains the last Scene state.
        std::vector<scene_detail::PhysicsPoseUpdate> output;
        output.reserve(impl_->system->GetNumBodies());
        native_diagnostic = {};
        auto& interface = impl_->system->GetBodyInterface();
        for (auto& slot : impl_->bodies)
            if (slot.value && slot.value->settings.motion == BodyMotion::kinematic_body) {
                auto& record = *slot.value;
                const auto& t = record.target ? *record.target : record.source_pose;
                interface.MoveKinematic(
                    record.native,
                    JPH::RVec3{static_cast<float>(t.translation.x),
                               static_cast<float>(t.translation.y),
                               static_cast<float>(t.translation.z)},
                    JPH::Quat{static_cast<float>(t.rotation.x), static_cast<float>(t.rotation.y),
                              static_cast<float>(t.rotation.z), static_cast<float>(t.rotation.w)},
                    1.0F / static_cast<float>(impl_->config.tick_hz));
            }
        const auto outcome =
            impl_->system->Update(1.0F / static_cast<float>(impl_->config.tick_hz),
                                  static_cast<int>(impl_->config.collision_substeps),
                                  impl_->temporary.get(), impl_->jobs.get());
        if (outcome != JPH::EPhysicsUpdateError::None || native_diagnostic.reported ||
            !impl_->temporary->IsEmpty() ||
            impl_->temporary->peak() > temporary_requirement(impl_->config)) {
            impl_->faulted = true;
            if (native_diagnostic.reported)
                return reported_error("physics.simulation.step");
            return physics_error(ErrorCode::execution_failed,
                                 "Native update failed; partial progress cannot be retried",
                                 "physics.simulation.step");
        }
        for (const auto& slot : impl_->bodies)
            if (slot.value) {
                const auto& record = *slot.value;
                JPH::RVec3 position;
                JPH::Quat rotation;
                interface.GetPositionAndRotation(record.native, position, rotation);
                auto pose = record.source_pose;
                pose.translation = {position.GetX(), position.GetY(), position.GetZ()};
                pose.rotation =
                    math::Quat{rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ()};
                output.push_back({record.entity.handle, pose});
            }
        auto candidate =
            scene_detail::ScenePhysicsAccess::prepare(scene, impl_->simulation, output);
        if (!candidate) {
            impl_->faulted = true;
            return candidate.error();
        }
        auto committed =
            scene_detail::ScenePhysicsAccess::commit(scene, impl_->simulation, candidate.value());
        if (!committed) {
            impl_->faulted = true;
            return committed;
        }
        std::size_t index = 0;
        for (auto& slot : impl_->bodies)
            if (slot.value) {
                slot.value->previous_pose = slot.value->source_pose;
                slot.value->source_pose = candidate.value().poses[index++].local;
                slot.value->target.reset();
            }
        ++impl_->completed_ticks;
        return {};
    } catch (...) {
        impl_->faulted = true;
        return physics_error(ErrorCode::execution_failed,
                             "Physics synchronization failed; replace this world",
                             "physics.simulation.step");
    }
}

Result<void> PhysicsWorld::reset_pose(Scene& scene, PhysicsBodyHandle body, Transform pose,
                                      BodyMotion mode) {
    if (auto checked = impl_->check_simulation(scene, "physics.simulation.reset"); !checked)
        return checked;
    auto index = impl_->checked_body(body);
    if (!index)
        return index.error();
    auto& record = *impl_->bodies[index.value()].value;
    if (record.settings.motion != mode)
        return physics_error(ErrorCode::invalid_argument,
                             "Pose operation does not match body motion",
                             "physics.simulation.reset");
    const scene_detail::PhysicsPoseUpdate update{record.entity.handle, pose};
    auto candidate =
        scene_detail::ScenePhysicsAccess::prepare(scene, impl_->simulation, {&update, 1});
    if (!candidate)
        return candidate.error();
    const auto& t = candidate.value().poses.front().local;
    native_diagnostic = {};
    auto& interface = impl_->system->GetBodyInterface();
    interface.SetPositionAndRotation(
        record.native,
        JPH::RVec3{static_cast<float>(t.translation.x), static_cast<float>(t.translation.y),
                   static_cast<float>(t.translation.z)},
        JPH::Quat{static_cast<float>(t.rotation.x), static_cast<float>(t.rotation.y),
                  static_cast<float>(t.rotation.z), static_cast<float>(t.rotation.w)},
        JPH::EActivation::DontActivate);
    if (mode == BodyMotion::dynamic_body)
        interface.SetLinearAndAngularVelocity(record.native, JPH::Vec3::sZero(),
                                              JPH::Vec3::sZero());
    for (const auto& slot : impl_->bodies)
        if (slot.value && slot.value->settings.motion != BodyMotion::static_body)
            interface.ActivateBody(slot.value->native);
    if (native_diagnostic.reported) {
        impl_->faulted = true;
        return reported_error("physics.simulation.reset");
    }
    auto committed =
        scene_detail::ScenePhysicsAccess::commit(scene, impl_->simulation, candidate.value());
    if (!committed) {
        impl_->faulted = true;
        return committed;
    }
    record.source_pose = t;
    record.previous_pose = t;
    record.target.reset();
    return {};
}
Result<void> PhysicsWorld::set_static_pose(Scene& scene, PhysicsBodyHandle body, Transform pose) {
    return reset_pose(scene, body, pose, BodyMotion::static_body);
}
Result<void> PhysicsWorld::teleport(Scene& scene, PhysicsBodyHandle body, Transform pose) {
    return reset_pose(scene, body, pose, BodyMotion::dynamic_body);
}
Result<void> PhysicsWorld::set_kinematic_target(const Scene& scene, PhysicsBodyHandle body,
                                                Transform target) {
    if (auto checked = impl_->check_simulation(scene, "physics.simulation.target"); !checked)
        return checked;
    auto index = impl_->checked_body(body);
    if (!index)
        return index.error();
    auto& record = *impl_->bodies[index.value()].value;
    record.target.reset();
    auto valid = validate_rigid_body_pose(record.settings, target);
    if (!valid)
        return valid.error();
    target = valid.value();
    const auto speed =
        glm::length(target.translation - record.source_pose.translation) * impl_->config.tick_hz;
    const auto angle =
        2 * std::acos(std::clamp(std::abs(glm::dot(target.rotation, record.source_pose.rotation)),
                                 0.0, 1.0));
    if (record.settings.motion != BodyMotion::kinematic_body ||
        target.scale != record.source_pose.scale || speed > physics_maximum_linear_speed ||
        angle * impl_->config.tick_hz > physics_maximum_angular_speed) {
        // A rejected newest request must not execute an older queued target.
        record.target.reset();
        return physics_error(ErrorCode::invalid_argument,
                             "Kinematic target exceeds mode, scale or speed policy",
                             "physics.simulation.target");
    }
    record.target = target;
    return {};
}
Result<PhysicsRenderSnapshot> PhysicsWorld::render_poses(const Scene& scene, double alpha) const {
    if (auto checked = impl_->check_simulation(scene, "physics.simulation.interpolate"); !checked)
        return checked.error();
    if (!std::isfinite(alpha) || alpha < 0 || alpha > 1)
        return physics_error(ErrorCode::invalid_argument, "Interpolation alpha must be in [0,1]",
                             "physics.simulation.interpolate");
    try {
        const auto stats = scene.stats();
        if (!stats)
            return stats.error();
        PhysicsRenderSnapshot result{impl_->instance,
                                     {stats.value().instance, stats.value().revision},
                                     impl_->completed_ticks,
                                     alpha,
                                     {}};
        result.poses.reserve(impl_->system->GetNumBodies());
        for (const auto& slot : impl_->bodies)
            if (slot.value) {
                const auto& record = *slot.value;
                auto pose = record.source_pose;
                pose.translation =
                    glm::mix(record.previous_pose.translation, pose.translation, alpha);
                pose.rotation = glm::slerp(record.previous_pose.rotation, pose.rotation, alpha);
                auto valid = canonicalize_transform(pose);
                if (!valid)
                    return valid.error();
                result.poses.push_back({record.entity, valid.value()});
            }
        return result;
    } catch (...) {
        return physics_error(ErrorCode::execution_failed, "Cannot prepare physics render poses",
                             "physics.simulation.interpolate");
    }
}
Result<StepReport> PhysicsWorld::poll_offline(Scene& scene, OfflineScheduler& schedule,
                                              MonotonicTime now, OfflineControl control) {
    if (auto checked = impl_->check_simulation(scene, "physics.simulation.offline"); !checked)
        return checked.error();
    if (impl_->config.tick_hz != 60 || schedule.next_tick() != impl_->completed_ticks)
        return physics_error(ErrorCode::conflict,
                             "Offline physics rate or tick differs from scheduler",
                             "physics.simulation.offline");
    return schedule.poll(now, [&](std::uint64_t) { return step(scene); }, control);
}
Result<StepReport> PhysicsWorld::poll_server(Scene& scene, ServerScheduler& schedule,
                                             MonotonicTime now) {
    if (auto checked = impl_->check_simulation(scene, "physics.simulation.server"); !checked)
        return checked.error();
    if (impl_->config.tick_hz != schedule.config().tick_rate_hz ||
        schedule.next_tick() != impl_->completed_ticks)
        return physics_error(ErrorCode::conflict,
                             "Server physics rate or tick differs from scheduler",
                             "physics.simulation.server");
    return schedule.poll(now, [&](std::uint64_t) { return step(scene); });
}

Result<PhysicsBodyState> PhysicsWorld::body_state(const Scene& scene,
                                                  PhysicsBodyHandle body) const {
    if (auto checked = impl_->check_owner("physics.body.state"); !checked)
        return checked.error();
    if (impl_->closed || impl_->faulted)
        return physics_error(ErrorCode::conflict, "Physics owner is closed or faulted",
                             "physics.body.state");
    auto checked = impl_->checked_body(body);
    if (!checked)
        return checked.error();
    const auto& record = *impl_->bodies[checked.value()].value;
    auto entity = scene.entity_state(record.entity.handle);
    if (!entity)
        return entity.error();
    if (entity.value().reference != record.entity || entity.value().rigid_body != record.settings ||
        entity.value().local_transform != record.source_pose || entity.value().parent)
        return physics_error(ErrorCode::conflict,
                             "Body source changed; remove and recreate explicitly",
                             "physics.body.state");
    const auto& interface = impl_->system->GetBodyInterface();
    JPH::RVec3 position;
    JPH::Quat rotation;
    interface.GetPositionAndRotation(record.native, position, rotation);
    auto pose = record.source_pose;
    pose.translation = {position.GetX(), position.GetY(), position.GetZ()};
    pose.rotation = math::Quat{rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ()};
    const auto extent = interface.GetShape(record.native)->GetLocalBounds().GetExtent();
    const auto motion = interface.GetMotionType(record.native);
    JPH::BodyLockRead locked{impl_->system->GetBodyLockInterfaceNoLock(), record.native};
    if (!locked.Succeeded())
        return physics_error(ErrorCode::conflict, "Native body disappeared", "physics.body.state");
    const auto inverse_mass =
        motion == JPH::EMotionType::Dynamic
            ? static_cast<double>(locked.GetBody().GetMotionProperties()->GetInverseMass())
            : 0.0;
    return PhysicsBodyState{record.entity,
                            record.settings,
                            pose,
                            {extent.GetX(), extent.GetY(), extent.GetZ()},
                            motion == JPH::EMotionType::Static    ? BodyMotion::static_body
                            : motion == JPH::EMotionType::Dynamic ? BodyMotion::dynamic_body
                                                                  : BodyMotion::kinematic_body,
                            inverse_mass,
                            interface.GetFriction(record.native),
                            interface.GetRestitution(record.native)};
}

Result<void> PhysicsWorld::remove_body(PhysicsBodyHandle body) {
    if (auto checked = impl_->check_owner("physics.body.remove"); !checked)
        return checked;
    if (impl_->simulation)
        return physics_error(ErrorCode::conflict, "End simulation before removing bodies",
                             "physics.body.remove");
    auto checked = impl_->checked_body(body);
    if (!checked)
        return checked.error();
    auto& slot = impl_->bodies[checked.value()];
    auto& interface = impl_->system->GetBodyInterface();
    interface.RemoveBody(slot.value->native);
    interface.DestroyBody(slot.value->native);
    slot.value.reset();
    if (slot.generation == std::numeric_limits<std::uint64_t>::max())
        slot.retired = true;
    else
        ++slot.generation;
    return {};
}

Result<PhysicsWorldStats> PhysicsWorld::stats() const {
    if (auto checked = impl_->check_owner("physics.world.stats"); !checked)
        return checked.error();
    return PhysicsWorldStats{impl_->config,
                             impl_->completed_ticks,
                             impl_->system ? impl_->system->GetNumBodies() : 0,
                             impl_->closed,
                             impl_->faulted,
                             static_cast<bool>(impl_->simulation),
                             temporary_requirement(impl_->config),
                             impl_->temporary ? impl_->temporary->peak() : 0};
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
