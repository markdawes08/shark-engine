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
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
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
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace shark {
namespace {

static_assert(std::is_nothrow_move_constructible_v<PhysicsContactEvent> &&
              std::is_nothrow_move_assignable_v<PhysicsContactEvent>);

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
// allocation paths. Discrete rigid primitives only, no external constraints/step-listeners/
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

// Bodies use private slot indices as native object layers. Immutable-per-body
// rules keep the full public mask out of the native 16-bit object-layer field.
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
class BodyLayerPairs final : public JPH::ObjectLayerPairFilter {
  public:
    struct Rule {
        std::uint32_t layer{0}, mask{0};
    };
    std::array<Rule, physics_maximum_bodies> rules{};
    bool ShouldCollide(JPH::ObjectLayer first, JPH::ObjectLayer second) const override {
        if (first >= rules.size() || second >= rules.size())
            return false;
        const auto& a = rules[first];
        const auto& b = rules[second];
        return (a.mask & (1U << b.layer)) != 0 && (b.mask & (1U << a.layer)) != 0;
    }
};

struct ObservedPair {
    JPH::BodyID native_first, native_second;
    std::uint32_t first, second;
    std::uint64_t first_generation, second_generation;
    bool operator==(const ObservedPair&) const = default;
};
bool pair_less(const ObservedPair& a, const ObservedPair& b) {
    return std::tie(a.first, a.first_generation, a.second, a.second_generation) <
           std::tie(b.first, b.first_generation, b.second, b.second_generation);
}
// Locked single-thread native jobs only. No allocation, Scene access, external
// callbacks, body locks or exceptions in this collector. Removed callbacks need
// no dead-body access: the owner compares completed-tick pair sets instead.
class ContactCollector final : public JPH::ContactListener {
  public:
    explicit ContactCollector(std::uint32_t maximum) : maximum_(maximum) {
        std::size_t capacity = 2;
        while (capacity < 2 * maximum)
            capacity *= 2;
        table_.resize(capacity);
        occupied_.reserve(maximum);
    }
    void clear() noexcept {
        for (auto index : occupied_)
            table_[index].reset();
        occupied_.clear();
        overflow = false;
    }
    void OnContactAdded(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold&,
                        JPH::ContactSettings&) noexcept override {
        observe(a, b);
    }
    void OnContactPersisted(const JPH::Body& a, const JPH::Body& b, const JPH::ContactManifold&,
                            JPH::ContactSettings&) noexcept override {
        observe(a, b);
    }
    template <typename F> void visit(F&& visitor) const {
        for (auto index : occupied_)
            visitor(*table_[index]);
    }
    bool overflow{false};

  private:
    void observe(const JPH::Body& a, const JPH::Body& b) noexcept {
        if (overflow)
            return;
        ObservedPair pair{a.GetID(),          b.GetID(),       a.GetObjectLayer(),
                          b.GetObjectLayer(), a.GetUserData(), b.GetUserData()};
        if (pair.second < pair.first) {
            std::swap(pair.first, pair.second);
            std::swap(pair.first_generation, pair.second_generation);
            std::swap(pair.native_first, pair.native_second);
        }
        auto hash = (pair.first_generation * 1099511628211ULL) ^ pair.second_generation ^
                    (static_cast<std::uint64_t>(pair.first) * 0x9e3779b185ebca87ULL) ^ pair.second;
        hash ^= hash >> 32U;
        auto index = static_cast<std::size_t>(hash) & (table_.size() - 1);
        while (table_[index]) {
            if (*table_[index] == pair)
                return;
            index = (index + 1) & (table_.size() - 1);
        }
        if (occupied_.size() == maximum_) {
            overflow = true;
            return;
        }
        table_[index] = pair;
        occupied_.push_back(index); // Reserved; trivial value, no callback allocation.
    }
    std::size_t maximum_;
    std::vector<std::optional<ObservedPair>> table_;
    std::vector<std::size_t> occupied_;
};

// Query callbacks run only within the synchronous owner-thread query. They
// retain bounded value copies; no Scene access, allocation or exceptions cross
// the native collision callback boundary.
struct QueryBody {
    JPH::BodyID native;
    std::uint32_t slot;
    std::uint64_t generation;
};
struct QueryRayBody {
    QueryBody body;
    float fraction;
    JPH::SubShapeID subshape;
};

class QueryLayerFilter final : public JPH::ObjectLayerFilter {
  public:
    explicit QueryLayerFilter(const std::array<bool, physics_maximum_bodies>& allowed)
        : allowed_(allowed) {}
    bool ShouldCollide(JPH::ObjectLayer layer) const override {
        return layer < allowed_.size() && allowed_[layer];
    }

  private:
    const std::array<bool, physics_maximum_bodies>& allowed_;
};

class QueryRayCollector final : public JPH::CastRayCollector {
  public:
    explicit QueryRayCollector(std::size_t maximum) : maximum_(maximum) {}
    void OnBody(const JPH::Body& body) noexcept override {
        current_ = {body.GetID(), body.GetObjectLayer(), body.GetUserData()};
    }
    void AddHit(const JPH::RayCastResult& hit) noexcept override {
        if (hit.mBodyID != current_.native || !std::isfinite(hit.mFraction) ||
            hit.mFraction < 0.0F || hit.mFraction > 1.0F) {
            invalid = true;
            ForceEarlyOut();
            return;
        }
        for (std::size_t i = 0; i < size; ++i) {
            if (values[i].body.native == hit.mBodyID) {
                if (hit.mFraction < values[i].fraction)
                    values[i] = {current_, hit.mFraction, hit.mSubShapeID2};
                return;
            }
        }
        if (size == maximum_) {
            overflow = true;
            ForceEarlyOut();
            return;
        }
        values[size++] = {current_, hit.mFraction, hit.mSubShapeID2};
    }
    std::array<QueryRayBody, physics_maximum_query_hits> values{};
    std::size_t size{0};
    bool overflow{false};
    bool invalid{false};

  private:
    std::size_t maximum_;
    QueryBody current_{};
};

class QueryOverlapCollector final : public JPH::CollideShapeCollector {
  public:
    explicit QueryOverlapCollector(std::size_t maximum) : maximum_(maximum) {}
    void OnBody(const JPH::Body& body) noexcept override {
        current_ = {body.GetID(), body.GetObjectLayer(), body.GetUserData()};
    }
    void AddHit(const JPH::CollideShapeResult& hit) noexcept override {
        if (hit.mBodyID2 != current_.native) {
            invalid = true;
            ForceEarlyOut();
            return;
        }
        for (std::size_t i = 0; i < size; ++i)
            if (values[i].native == hit.mBodyID2)
                return;
        if (size == maximum_) {
            overflow = true;
            ForceEarlyOut();
            return;
        }
        values[size++] = current_;
    }
    std::array<QueryBody, physics_maximum_query_hits> values{};
    std::size_t size{0};
    bool overflow{false};
    bool invalid{false};

  private:
    std::size_t maximum_;
    QueryBody current_{};
};

struct QueryAdmission {
    SceneRevision revision;
    std::array<bool, physics_maximum_bodies> allowed{};
};

Result<void> validate_query_position(const math::Vec3& value, const char* operation) {
    for (int axis = 0; axis < 3; ++axis) {
        const double component = value[axis];
        if (!std::isfinite(component) || std::abs(component) > physics_local_position_limit)
            return physics_error(ErrorCode::invalid_argument,
                                 "Query position must fit bounded normal Scene coordinates",
                                 operation);
        const auto packed = static_cast<float>(component);
        if (!std::isfinite(packed) || (component != 0 && packed == 0) ||
            std::fpclassify(packed) == FP_SUBNORMAL)
            return physics_error(ErrorCode::invalid_argument,
                                 "Query position must fit bounded normal Scene coordinates",
                                 operation);
    }
    return {};
}

Result<void> validate_query_extent(const math::Vec3& center, const math::Vec3& extent,
                                   const char* operation) {
    if (auto checked = validate_query_position(center, operation); !checked)
        return checked;
    for (int axis = 0; axis < 3; ++axis) {
        const auto value = extent[axis];
        if (!std::isfinite(value) || value < physics_minimum_dimension ||
            value > physics_maximum_dimension ||
            std::abs(center[axis]) + value > physics_local_position_limit)
            return physics_error(ErrorCode::invalid_argument,
                                 "Query shape exceeds bounded Scene coordinates or dimensions",
                                 operation);
        const auto packed_center = static_cast<float>(center[axis]);
        const auto packed_extent = static_cast<float>(value);
        if (!std::isfinite(packed_extent) ||
            std::abs(packed_center) + packed_extent > physics_local_position_limit)
            return physics_error(ErrorCode::invalid_argument,
                                 "Query shape exceeds bounded Scene coordinates or dimensions",
                                 operation);
    }
    return {};
}

Result<void> validate_control_vector(const math::Vec3& value, double maximum,
                                     const char* operation) {
    for (int axis = 0; axis < 3; ++axis) {
        const auto component = value[axis];
        if (!std::isfinite(component) || std::abs(component) > maximum)
            return physics_error(ErrorCode::invalid_argument,
                                 "Physics control vector exceeds its finite magnitude limit",
                                 operation);
        const auto packed = static_cast<float>(component);
        if (!std::isfinite(packed) || (component != 0.0 && packed == 0.0F) ||
            std::fpclassify(packed) == FP_SUBNORMAL)
            return physics_error(ErrorCode::invalid_argument,
                                 "Physics control vector must fit normal native floats", operation);
    }
    if (glm::length(value) > maximum)
        return physics_error(ErrorCode::invalid_argument,
                             "Physics control vector exceeds its finite magnitude limit",
                             operation);
    return {};
}

JPH::Vec3 pack_control_vector(const math::Vec3& value) {
    return {static_cast<float>(value.x), static_cast<float>(value.y), static_cast<float>(value.z)};
}

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
    if (config.max_contact_pairs == 0 || config.max_contact_pairs > 16384 ||
        config.max_queued_events < 2 * config.max_contact_pairs || config.max_queued_events > 65536)
        return physics_error(ErrorCode::invalid_configuration,
                             "Contact/event capacities exceed bounded policy",
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
    explicit Impl(PhysicsWorldConfig value, std::uint64_t token)
        : instance(token), config(value), contacts(value.max_contact_pairs) {}
    struct BodyRecord {
        EntityReference entity;
        RigidBody settings;
        Transform source_pose;
        JPH::BodyID native;
        Transform previous_pose{};
        std::optional<Transform> target{};
        math::Vec3 pending_force{};
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
    BodyLayerPairs pair_filter;
    ContactCollector contacts;
    std::vector<ObservedPair> active_pairs;
    std::vector<PhysicsContactEvent> events;
    std::uint64_t next_event{1};
    std::uint64_t discarded_events{0};
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
    Result<std::size_t> checked_dynamic(const Scene& scene, PhysicsBodyHandle body,
                                        const char* operation) const {
        if (auto checked = check_simulation(scene, operation); !checked) {
            auto error = std::move(checked).error();
            error.context.operation = operation;
            return error;
        }
        auto checked = checked_body(body);
        if (!checked) {
            auto error = std::move(checked).error();
            error.context.operation = operation;
            return error;
        }
        const auto& record = *bodies[checked.value()].value;
        if (record.settings.motion != BodyMotion::dynamic_body)
            return physics_error(ErrorCode::invalid_argument,
                                 "Physics control requires a dynamic body", operation);
        auto entity = scene.entity_state(record.entity.handle);
        if (!entity) {
            auto error = std::move(entity).error();
            error.context.operation = operation;
            return error;
        }
        if (entity.value().reference != record.entity ||
            entity.value().rigid_body != record.settings ||
            entity.value().local_transform != record.source_pose || entity.value().parent)
            return physics_error(ErrorCode::conflict,
                                 "Body source changed; remove and recreate explicitly", operation);
        return checked.value();
    }

    Result<PhysicsQueryHit> query_hit(const Scene& scene, const QueryBody& body,
                                      const char* operation) {
        if (body.slot >= bodies.size()) {
            faulted = true;
            return physics_error(ErrorCode::execution_failed,
                                 "Native query returned an unknown body slot", operation);
        }
        const auto& slot = bodies[body.slot];
        if (!slot.value || slot.generation != body.generation ||
            slot.value->native != body.native) {
            faulted = true;
            return physics_error(ErrorCode::execution_failed,
                                 "Native query returned a stale body generation", operation);
        }
        const auto& record = *slot.value;
        auto entity = scene.entity_state(record.entity.handle);
        if (!entity) {
            auto error = std::move(entity).error();
            error.context.operation = operation;
            return error;
        }
        if (entity.value().reference != record.entity ||
            entity.value().rigid_body != record.settings ||
            entity.value().local_transform != record.source_pose || entity.value().parent)
            return physics_error(ErrorCode::conflict,
                                 "Query body source changed; remove and recreate explicitly",
                                 operation);
        return PhysicsQueryHit{
            {instance, body.slot, body.generation}, record.entity, record.settings.is_trigger};
    }

    Result<QueryAdmission> admit_query(const Scene& scene, const PhysicsQueryFilter& filter,
                                       std::size_t maximum, const char* operation) {
        if (auto checked = check_simulation(scene, operation); !checked) {
            auto error = std::move(checked).error();
            error.context.operation = operation;
            return error;
        }
        if (maximum == 0 || maximum > physics_maximum_query_hits || filter.layer > 15 ||
            filter.mask > 0xffff)
            return physics_error(ErrorCode::invalid_argument, "Invalid query filter or hit limit",
                                 operation);
        std::optional<std::size_t> ignored;
        if (filter.ignore) {
            auto checked = checked_body(*filter.ignore);
            if (!checked) {
                auto error = std::move(checked).error();
                error.context.operation = operation;
                return error;
            }
            ignored = checked.value();
            const auto& slot = bodies[*ignored];
            auto valid = query_hit(
                scene, {slot.value->native, static_cast<std::uint32_t>(*ignored), slot.generation},
                operation);
            if (!valid)
                return valid.error();
        }
        auto stats = scene.stats();
        if (!stats)
            return stats.error();
        QueryAdmission admission{{stats.value().instance, stats.value().revision}, {}};
        for (std::size_t i = 0; i < bodies.size(); ++i) {
            const auto& slot = bodies[i];
            if (!slot.value || (ignored && *ignored == i))
                continue;
            const auto& settings = slot.value->settings;
            admission.allowed[i] = (filter.include_triggers || !settings.is_trigger) &&
                                   ((filter.mask & (1U << settings.collision_layer)) != 0) &&
                                   ((settings.collision_mask & (1U << filter.layer)) != 0);
        }
        return admission;
    }

    Result<PhysicsOverlapResult> overlap(const Scene& scene, const JPH::Shape& shape,
                                         math::Vec3 center, const QueryAdmission& admission,
                                         std::size_t maximum, const char* operation) {
        try {
            QueryLayerFilter layer_filter{admission.allowed};
            QueryOverlapCollector collector{maximum};
            const auto packed = pack_control_vector(center);
            const auto origin = JPH::RVec3{packed.GetX(), packed.GetY(), packed.GetZ()};
            JPH::CollideShapeSettings settings;
            native_diagnostic = {};
            system->GetNarrowPhaseQuery().CollideShape(&shape, JPH::Vec3::sReplicate(1),
                                                       JPH::RMat44::sTranslation(origin), settings,
                                                       origin, collector, {}, layer_filter);
            if (native_diagnostic.reported || collector.invalid) {
                faulted = true;
                if (native_diagnostic.reported)
                    return reported_error(operation);
                return physics_error(ErrorCode::execution_failed,
                                     "Native overlap returned an inconsistent body", operation);
            }
            if (collector.overflow)
                return physics_error(ErrorCode::conflict,
                                     "Query hit limit exceeded; no partial result is returned",
                                     operation);
            PhysicsOverlapResult result{instance, admission.revision, completed_ticks, {}};
            result.hits.reserve(collector.size);
            for (std::size_t i = 0; i < collector.size; ++i) {
                auto hit = query_hit(scene, collector.values[i], operation);
                if (!hit)
                    return hit.error();
                result.hits.push_back(std::move(hit).value());
            }
            std::sort(result.hits.begin(), result.hits.end(),
                      [](const auto& first, const auto& second) {
                          return first.entity.identity < second.entity.identity;
                      });
            return result;
        } catch (...) {
            return physics_error(ErrorCode::execution_failed,
                                 "Cannot complete bounded physics overlap", operation);
        }
    }
    Result<void> event_capacity(std::size_t count) const {
        if (count > config.max_queued_events - events.size())
            return physics_error(ErrorCode::conflict, "Take queued physics events before more work",
                                 "physics.events.capacity");
        if (count > std::numeric_limits<std::uint64_t>::max() - next_event)
            return physics_error(ErrorCode::conflict, "Physics event sequences exhausted",
                                 "physics.events.capacity");
        return {};
    }
    bool current_pair(const ObservedPair& pair) const {
        return pair.first < bodies.size() && pair.second < bodies.size() &&
               bodies[pair.first].value && bodies[pair.second].value &&
               bodies[pair.first].generation == pair.first_generation &&
               bodies[pair.second].generation == pair.second_generation &&
               bodies[pair.first].value->native == pair.native_first &&
               bodies[pair.second].value->native == pair.native_second;
    }
    PhysicsContactEvent event(const ObservedPair& pair, PhysicsContactPhase phase,
                              PhysicsContactCause cause, std::uint64_t tick) const {
        const auto& a = *bodies[pair.first].value;
        const auto& b = *bodies[pair.second].value;
        return {0,
                tick,
                phase,
                cause,
                {{instance, pair.first, pair.first_generation}, a.entity, a.settings.is_trigger},
                {{instance, pair.second, pair.second_generation}, b.entity, b.settings.is_trigger}};
    }
    void publish_events(std::vector<PhysicsContactEvent>& ready) {
        for (auto& value : ready) {
            value.sequence = next_event++;
            events.push_back(std::move(value)); // Complete queue storage reserved at begin.
        }
    }
    void release() noexcept {
        if (system) {
            system->SetContactListener(nullptr);
            auto& interface = system->GetBodyInterface();
            for (auto& slot : bodies)
                if (slot.value) {
                    interface.RemoveBody(slot.value->native);
                    interface.DestroyBody(slot.value->native);
                    slot.value.reset();
                }
        }
        system.reset();
        active_pairs.clear();
        discarded_events += events.size();
        events.clear();
        contacts.clear();
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
        impl->system->SetContactListener(&impl->contacts);
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
    return create_body_impl(scene, entity, nullptr, nullptr);
}
Result<PhysicsBodyHandle> PhysicsWorld::create_runtime_body(Scene& scene,
                                                            const EntityReference& entity,
                                                            const RigidBody& body) {
    if (auto checked = impl_->check_simulation(scene, "physics.body.create_runtime"); !checked)
        return checked.error();
    // Empty simulations may start with a smaller arena; live bodies require
    // the same populated-world admission as begin_simulation.
    if (temporary_requirement(impl_->config) > impl_->config.temporary_bytes)
        return physics_error(ErrorCode::conflict,
                             "Physics temporary arena cannot admit this configuration",
                             "physics.body.create_runtime");
    if (auto checked =
            scene_detail::ScenePhysicsAccess::check_attach(scene, impl_->simulation, entity, body);
        !checked)
        return checked.error();
    return create_body_impl(scene, entity.handle, &scene, &body);
}
Result<PhysicsBodyHandle> PhysicsWorld::create_body_impl(const Scene& scene, EntityHandle entity,
                                                         Scene* runtime,
                                                         const RigidBody* replacement) {
    if (auto checked = impl_->check_owner("physics.body.create"); !checked)
        return checked.error();
    if (impl_->closed || impl_->faulted || (impl_->simulation && runtime == nullptr) ||
        (runtime != nullptr && (!impl_->simulation || replacement == nullptr)))
        return physics_error(ErrorCode::conflict, "Physics owner is closed or faulted",
                             "physics.body.create");
    bool native_work = false;
    try {
        native_diagnostic = {};
        auto source = scene.entity_state(entity);
        if (!source)
            return source.error();
        const auto& value = source.value();
        const auto scene_stats = scene.stats();
        if (!scene_stats)
            return scene_stats.error();
        if (scene_stats.value().pending_mutations != 0 ||
            (runtime == nullptr && !value.rigid_body) || (runtime != nullptr && value.rigid_body) ||
            value.parent || std::holds_alternative<ReplicaSceneScope>(value.reference.scope))
            return physics_error(
                ErrorCode::conflict,
                "Body creation requires a committed authored or authoritative root with RigidBody",
                "physics.body.create");
        const auto& settings = runtime ? *replacement : *value.rigid_body;
        auto pose = validate_rigid_body_pose(settings, value.local_transform);
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
            motion, static_cast<JPH::ObjectLayer>(index)};
        native_settings.mFriction = static_cast<float>(settings.friction);
        native_settings.mRestitution = static_cast<float>(settings.restitution);
        native_settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
        native_settings.mMassPropertiesOverride.mMass = static_cast<float>(settings.mass);
        native_settings.mLinearDamping = 0;
        native_settings.mAngularDamping = 0;
        native_settings.mMaxLinearVelocity = static_cast<float>(physics_maximum_linear_speed);
        native_settings.mMaxAngularVelocity = static_cast<float>(physics_maximum_angular_speed);
        native_settings.mMotionQuality = JPH::EMotionQuality::Discrete;
        native_settings.mIsSensor = settings.is_trigger;
        native_settings.mCollideKinematicVsNonDynamic = true;
        native_settings.mAllowSleeping = false;
        native_settings.mUserData =
            index < impl_->bodies.size() ? impl_->bodies[index].generation : 1;
        impl_->pair_filter.rules[index] = {settings.collision_layer, settings.collision_mask};
        auto& interface = impl_->system->GetBodyInterface();
        auto* native = interface.CreateBody(native_settings);
        if (!native)
            return physics_error(ErrorCode::conflict, "Native physics body capacity exhausted",
                                 "physics.body.create");
        native_work = true;
        const auto id = native->GetID();
        NativeBodyCandidate candidate{interface, id};
        interface.AddBody(id, runtime && settings.motion != BodyMotion::static_body
                                  ? JPH::EActivation::Activate
                                  : JPH::EActivation::DontActivate);
        if (native_diagnostic.reported) {
            if (runtime)
                impl_->faulted = true;
            return reported_error("physics.body.create");
        }
        if (index == impl_->bodies.size())
            impl_->bodies.emplace_back();
        auto& slot = impl_->bodies[index];
        // Fence the exact observed source, not a second quaternion
        // normalization's rounded value used to prepare the native pose.
        slot.value.emplace(value.reference, settings, value.local_transform, id);
        slot.value->previous_pose = value.local_transform;
        if (runtime)
            scene_detail::ScenePhysicsAccess::attach(*runtime, entity, settings);
        candidate.retained = true;
        return PhysicsBodyHandle{impl_->instance, static_cast<std::uint32_t>(index),
                                 slot.generation};
    } catch (...) {
        if (runtime && native_work)
            impl_->faulted = true;
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
        impl_->events.reserve(impl_->config.max_queued_events);
        impl_->active_pairs.reserve(impl_->config.max_contact_pairs);
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
            slot.value->pending_force = {};
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
    if (auto checked = impl_->event_capacity(2 * impl_->config.max_contact_pairs); !checked)
        return checked;
    try {
        // Reserve the output before any native movement. Later Scene validation
        // can still fail; that faults the world and retains the last Scene state.
        std::vector<scene_detail::PhysicsPoseUpdate> output;
        output.reserve(impl_->system->GetNumBodies());
        impl_->contacts.clear();
        native_diagnostic = {};
        auto& interface = impl_->system->GetBodyInterface();
        for (auto& slot : impl_->bodies)
            if (slot.value && slot.value->settings.motion == BodyMotion::dynamic_body &&
                slot.value->pending_force != math::Vec3{})
                interface.AddForce(slot.value->native,
                                   pack_control_vector(slot.value->pending_force));
        if (native_diagnostic.reported) {
            impl_->faulted = true;
            return reported_error("physics.simulation.step");
        }
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
            impl_->contacts.overflow || !impl_->temporary->IsEmpty() ||
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
        std::vector<ObservedPair> pairs;
        pairs.reserve(impl_->config.max_contact_pairs);
        bool invalid_pair = false;
        impl_->contacts.visit([&](const ObservedPair& pair) {
            if (!impl_->current_pair(pair))
                invalid_pair = true;
            else if (impl_->system->WereBodiesInContact(pair.native_first, pair.native_second))
                pairs.push_back(pair);
        });
        if (invalid_pair) {
            impl_->faulted = true;
            return physics_error(ErrorCode::execution_failed,
                                 "Contact observation has a stale native owner",
                                 "physics.events.prepare");
        }
        std::sort(pairs.begin(), pairs.end(), pair_less);
        std::vector<PhysicsContactEvent> ready;
        ready.reserve(impl_->active_pairs.size() + pairs.size());
        std::size_t old_index = 0, new_index = 0;
        while (old_index < impl_->active_pairs.size() || new_index < pairs.size()) {
            if (new_index == pairs.size() ||
                (old_index < impl_->active_pairs.size() &&
                 pair_less(impl_->active_pairs[old_index], pairs[new_index]))) {
                ready.push_back(
                    impl_->event(impl_->active_pairs[old_index++], PhysicsContactPhase::exit,
                                 PhysicsContactCause::simulation, impl_->completed_ticks + 1));
            } else if (old_index == impl_->active_pairs.size() ||
                       pair_less(pairs[new_index], impl_->active_pairs[old_index])) {
                ready.push_back(impl_->event(pairs[new_index++], PhysicsContactPhase::enter,
                                             PhysicsContactCause::simulation,
                                             impl_->completed_ticks + 1));
            } else {
                ++old_index;
                ++new_index;
            }
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
                slot.value->pending_force = {};
            }
        ++impl_->completed_ticks;
        impl_->active_pairs.swap(pairs);
        impl_->publish_events(ready);
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
    record.pending_force = {};
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
Result<PhysicsBodyVelocity> PhysicsWorld::velocity(const Scene& scene,
                                                   PhysicsBodyHandle body) const {
    constexpr auto operation = "physics.body.velocity";
    auto index = impl_->checked_dynamic(scene, body, operation);
    if (!index)
        return index.error();
    const auto& record = *impl_->bodies[index.value()].value;
    native_diagnostic = {};
    JPH::Vec3 linear, angular;
    impl_->system->GetBodyInterface().GetLinearAndAngularVelocity(record.native, linear, angular);
    if (native_diagnostic.reported) {
        impl_->faulted = true;
        return reported_error(operation);
    }
    if (!std::isfinite(linear.GetX()) || !std::isfinite(linear.GetY()) ||
        !std::isfinite(linear.GetZ()) || !std::isfinite(angular.GetX()) ||
        !std::isfinite(angular.GetY()) || !std::isfinite(angular.GetZ())) {
        impl_->faulted = true;
        return physics_error(ErrorCode::execution_failed,
                             "Native body returned nonfinite velocity; replace this world",
                             operation);
    }
    return PhysicsBodyVelocity{{linear.GetX(), linear.GetY(), linear.GetZ()},
                               {angular.GetX(), angular.GetY(), angular.GetZ()}};
}

Result<void> PhysicsWorld::set_velocity(const Scene& scene, PhysicsBodyHandle body,
                                        PhysicsBodyVelocity value) {
    constexpr auto operation = "physics.body.set_velocity";
    auto index = impl_->checked_dynamic(scene, body, operation);
    if (!index)
        return index.error();
    if (auto checked =
            validate_control_vector(value.linear, physics_maximum_linear_speed, operation);
        !checked)
        return checked;
    if (auto checked =
            validate_control_vector(value.angular, physics_maximum_angular_speed, operation);
        !checked)
        return checked;
    const auto linear = pack_control_vector(value.linear);
    const auto angular = pack_control_vector(value.angular);
    // Reject native float rounding across the ceilings rather than silently clamp.
    constexpr auto max_linear = static_cast<float>(physics_maximum_linear_speed);
    constexpr auto max_angular = static_cast<float>(physics_maximum_angular_speed);
    if (linear.LengthSq() > max_linear * max_linear ||
        angular.LengthSq() > max_angular * max_angular)
        return physics_error(ErrorCode::invalid_argument,
                             "Packed velocity exceeds native speed limits", operation);
    native_diagnostic = {};
    impl_->system->GetBodyInterface().SetLinearAndAngularVelocity(
        impl_->bodies[index.value()].value->native, linear, angular);
    if (native_diagnostic.reported) {
        impl_->faulted = true;
        return reported_error(operation);
    }
    return {};
}

Result<void> PhysicsWorld::apply_force(const Scene& scene, PhysicsBodyHandle body,
                                       math::Vec3 force) {
    constexpr auto operation = "physics.body.apply_force";
    auto index = impl_->checked_dynamic(scene, body, operation);
    if (!index)
        return index.error();
    if (auto checked = validate_control_vector(force, physics_maximum_force, operation); !checked)
        return checked;
    auto& record = *impl_->bodies[index.value()].value;
    const auto candidate = record.pending_force + force;
    if (auto checked = validate_control_vector(candidate, physics_maximum_force, operation);
        !checked)
        return checked;
    record.pending_force = candidate;
    return {};
}

Result<void> PhysicsWorld::apply_impulse(const Scene& scene, PhysicsBodyHandle body,
                                         math::Vec3 impulse) {
    constexpr auto operation = "physics.body.apply_impulse";
    auto index = impl_->checked_dynamic(scene, body, operation);
    if (!index)
        return index.error();
    if (auto checked = validate_control_vector(impulse, physics_maximum_impulse, operation);
        !checked)
        return checked;
    const auto& record = *impl_->bodies[index.value()].value;
    auto& interface = impl_->system->GetBodyInterface();
    native_diagnostic = {};
    const auto linear = interface.GetLinearVelocity(record.native);
    float inverse_mass = 0.0F;
    {
        JPH::BodyLockRead locked{impl_->system->GetBodyLockInterfaceNoLock(), record.native};
        if (!locked.Succeeded())
            return physics_error(ErrorCode::conflict, "Native body disappeared", operation);
        inverse_mass = locked.GetBody().GetMotionProperties()->GetInverseMass();
    }
    if (native_diagnostic.reported) {
        impl_->faulted = true;
        return reported_error(operation);
    }
    const auto expected_inverse_mass = 1.0F / static_cast<float>(record.settings.mass);
    if (!std::isfinite(linear.GetX()) || !std::isfinite(linear.GetY()) ||
        !std::isfinite(linear.GetZ()) || !std::isfinite(inverse_mass) || inverse_mass <= 0.0F ||
        std::abs(inverse_mass - expected_inverse_mass) > expected_inverse_mass * 0.00001F) {
        impl_->faulted = true;
        return physics_error(ErrorCode::execution_failed,
                             "Native velocity or inverse mass is invalid; replace this world",
                             operation);
    }
    const auto packed = pack_control_vector(impulse);
    const auto candidate = linear + packed * inverse_mass;
    if (!std::isfinite(candidate.GetX()) || !std::isfinite(candidate.GetY()) ||
        !std::isfinite(candidate.GetZ())) {
        impl_->faulted = true;
        return physics_error(ErrorCode::execution_failed,
                             "Native impulse calculation is nonfinite; replace this world",
                             operation);
    }
    constexpr auto max_linear = static_cast<float>(physics_maximum_linear_speed);
    if (candidate.LengthSq() > max_linear * max_linear)
        return physics_error(ErrorCode::invalid_argument,
                             "Impulse would exceed the native linear speed limit", operation);
    interface.AddImpulse(record.native, packed);
    if (native_diagnostic.reported) {
        impl_->faulted = true;
        return reported_error(operation);
    }
    return {};
}

Result<PhysicsRaycastResult> PhysicsWorld::raycast(const Scene& scene, math::Vec3 from,
                                                   math::Vec3 to, PhysicsQueryFilter filter,
                                                   std::size_t max_hits) const {
    constexpr auto operation = "physics.query.raycast";
    auto admission = impl_->admit_query(scene, filter, max_hits, operation);
    if (!admission)
        return admission.error();
    if (auto checked = validate_query_position(from, operation); !checked)
        return checked.error();
    if (auto checked = validate_query_position(to, operation); !checked)
        return checked.error();
    const auto start = pack_control_vector(from);
    const auto direction = pack_control_vector(to) - start;
    const auto length_squared = direction.LengthSq();
    if (!std::isfinite(length_squared) || length_squared < std::numeric_limits<float>::min())
        return physics_error(ErrorCode::invalid_argument,
                             "Ray endpoints collapse or are too close in native coordinates",
                             operation);
    try {
        QueryLayerFilter layer_filter{admission.value().allowed};
        QueryRayCollector collector{max_hits};
        const JPH::RRayCast ray{JPH::RVec3{start.GetX(), start.GetY(), start.GetZ()}, direction};
        const JPH::RayCastSettings settings;
        native_diagnostic = {};
        impl_->system->GetNarrowPhaseQuery().CastRay(ray, settings, collector, {}, layer_filter);
        if (native_diagnostic.reported || collector.invalid) {
            impl_->faulted = true;
            if (native_diagnostic.reported)
                return reported_error(operation);
            return physics_error(ErrorCode::execution_failed,
                                 "Native ray returned an inconsistent body or fraction", operation);
        }
        if (collector.overflow)
            return physics_error(ErrorCode::conflict,
                                 "Query hit limit exceeded; no partial result is returned",
                                 operation);
        PhysicsRaycastResult result{
            impl_->instance, admission.value().revision, impl_->completed_ticks, {}};
        result.hits.reserve(collector.size);
        for (std::size_t i = 0; i < collector.size; ++i) {
            const auto& value = collector.values[i];
            auto hit = impl_->query_hit(scene, value.body, operation);
            if (!hit)
                return hit.error();
            const auto point = ray.GetPointOnRay(value.fraction);
            std::optional<math::Vec3> normal;
            if (value.fraction > 0) {
                JPH::BodyLockRead locked{impl_->system->GetBodyLockInterface(), value.body.native};
                if (!locked.Succeeded())
                    return physics_error(ErrorCode::conflict, "Native query body disappeared",
                                         operation);
                const auto surface =
                    locked.GetBody().GetWorldSpaceSurfaceNormal(value.subshape, point);
                if (!std::isfinite(surface.GetX()) || !std::isfinite(surface.GetY()) ||
                    !std::isfinite(surface.GetZ()) || surface.LengthSq() < 0.5F ||
                    surface.LengthSq() > 1.5F || native_diagnostic.reported) {
                    impl_->faulted = true;
                    if (native_diagnostic.reported)
                        return reported_error(operation);
                    return physics_error(ErrorCode::execution_failed,
                                         "Native ray surface normal is invalid", operation);
                }
                normal = math::Vec3{surface.GetX(), surface.GetY(), surface.GetZ()};
            }
            const auto& participant = hit.value();
            result.hits.push_back({participant.body,
                                   participant.entity,
                                   participant.is_trigger,
                                   value.fraction,
                                   {point.GetX(), point.GetY(), point.GetZ()},
                                   normal});
        }
        std::sort(result.hits.begin(), result.hits.end(),
                  [](const auto& first, const auto& second) {
                      if (first.fraction != second.fraction)
                          return first.fraction < second.fraction;
                      return first.entity.identity < second.entity.identity;
                  });
        return result;
    } catch (...) {
        return physics_error(ErrorCode::execution_failed, "Cannot complete bounded physics ray",
                             operation);
    }
}

Result<PhysicsOverlapResult> PhysicsWorld::overlap_sphere(const Scene& scene, math::Vec3 center,
                                                          double radius, PhysicsQueryFilter filter,
                                                          std::size_t max_hits) const {
    constexpr auto operation = "physics.query.overlap_sphere";
    auto admission = impl_->admit_query(scene, filter, max_hits, operation);
    if (!admission)
        return admission.error();
    if (auto checked = validate_query_extent(center, math::Vec3{radius}, operation); !checked)
        return checked.error();
    const JPH::SphereShape shape{static_cast<float>(radius)};
    return impl_->overlap(scene, shape, center, admission.value(), max_hits, operation);
}

Result<PhysicsOverlapResult> PhysicsWorld::overlap_box(const Scene& scene, math::Vec3 center,
                                                       math::Vec3 half_extents,
                                                       PhysicsQueryFilter filter,
                                                       std::size_t max_hits) const {
    constexpr auto operation = "physics.query.overlap_box";
    auto admission = impl_->admit_query(scene, filter, max_hits, operation);
    if (!admission)
        return admission.error();
    if (auto checked = validate_query_extent(center, half_extents, operation); !checked)
        return checked.error();
    const JPH::BoxShape shape{pack_control_vector(half_extents), 0.0F};
    return impl_->overlap(scene, shape, center, admission.value(), max_hits, operation);
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

Result<PhysicsBodyHandle> PhysicsWorld::find_body(const Scene& scene,
                                                  const EntityReference& entity) const {
    constexpr auto operation = "physics.body.find";
    if (auto checked = impl_->check_simulation(scene, operation); !checked)
        return checked.error();
    auto resolved = scene.resolve(entity);
    if (!resolved)
        return resolved.error();
    for (std::size_t i = 0; i < impl_->bodies.size(); ++i) {
        const auto& slot = impl_->bodies[i];
        if (slot.value && slot.value->entity == entity) {
            PhysicsBodyHandle handle{impl_->instance, static_cast<std::uint32_t>(i),
                                     slot.generation};
            auto state = body_state(scene, handle);
            if (!state)
                return state.error();
            return handle;
        }
    }
    return physics_error(ErrorCode::conflict, "Entity has no current physics body", operation);
}

Result<void> PhysicsWorld::remove_body(PhysicsBodyHandle body) {
    return remove_body_impl(nullptr, body);
}
Result<void> PhysicsWorld::remove_body(Scene& scene, PhysicsBodyHandle body) {
    return remove_body_impl(&scene, body);
}
Result<void> PhysicsWorld::remove_body_impl(Scene* scene, PhysicsBodyHandle body) {
    if (auto checked = impl_->check_owner("physics.body.remove"); !checked)
        return checked;
    if (impl_->closed || impl_->faulted)
        return physics_error(ErrorCode::conflict, "Physics owner is closed or faulted",
                             "physics.body.remove");
    if (scene) {
        if (auto checked = impl_->check_simulation(*scene, "physics.body.remove"); !checked)
            return checked;
    } else if (impl_->simulation) {
        return physics_error(ErrorCode::conflict, "Use the leased Scene to remove an active body",
                             "physics.body.remove");
    }
    auto checked = impl_->checked_body(body);
    if (!checked)
        return checked.error();
    auto& slot = impl_->bodies[checked.value()];
    const auto entity = slot.value->entity.handle;
    if (scene) {
        if (auto ready = scene_detail::ScenePhysicsAccess::check_detach(*scene, impl_->simulation,
                                                                        slot.value->entity);
            !ready)
            return ready;
    }
    bool native_work = false;
    try {
        std::vector<PhysicsContactEvent> exits;
        for (const auto& pair : impl_->active_pairs)
            if (pair.first == body.slot || pair.second == body.slot)
                exits.push_back(impl_->event(pair, PhysicsContactPhase::exit,
                                             PhysicsContactCause::body_removed,
                                             impl_->completed_ticks));
        if (auto ready = impl_->event_capacity(exits.size()); !ready)
            return ready;
        native_diagnostic = {};
        auto& interface = impl_->system->GetBodyInterface();
        native_work = true;
        interface.RemoveBody(slot.value->native);
        interface.DestroyBody(slot.value->native);
        slot.value.reset();
        impl_->pair_filter.rules[body.slot] = {};
        if (slot.generation == std::numeric_limits<std::uint64_t>::max())
            slot.retired = true;
        else
            ++slot.generation;
        std::erase_if(impl_->active_pairs, [&](const auto& pair) {
            return pair.first == body.slot || pair.second == body.slot;
        });
        if (native_diagnostic.reported) {
            impl_->faulted = true;
            return reported_error("physics.body.remove");
        }
        if (scene)
            scene_detail::ScenePhysicsAccess::detach(*scene, entity);
        impl_->publish_events(exits);
        return {};
    } catch (...) {
        if (native_work)
            impl_->faulted = true;
        return physics_error(ErrorCode::execution_failed, "Cannot prepare body removal",
                             "physics.body.remove");
    }
}
Result<std::vector<PhysicsContactEvent>> PhysicsWorld::take_events(const Scene& scene,
                                                                   std::size_t maximum) {
    if (auto checked = impl_->check_simulation(scene, "physics.events.take"); !checked)
        return checked.error();
    return take_events(maximum);
}

Result<std::vector<PhysicsContactEvent>> PhysicsWorld::take_events(std::size_t maximum) {
    if (auto checked = impl_->check_owner("physics.events.take"); !checked)
        return checked.error();
    if (impl_->closed || maximum == 0 || maximum > 65536)
        return physics_error(ErrorCode::invalid_argument,
                             "Event take requires an open owner and maximum in [1,65536]",
                             "physics.events.take");
    try {
        const auto count = std::min(maximum, impl_->events.size());
        std::vector<PhysicsContactEvent> result(
            impl_->events.begin(), impl_->events.begin() + static_cast<std::ptrdiff_t>(count));
        impl_->events.erase(impl_->events.begin(),
                            impl_->events.begin() + static_cast<std::ptrdiff_t>(count));
        return result;
    } catch (...) {
        return physics_error(ErrorCode::execution_failed, "Cannot take physics events",
                             "physics.events.take");
    }
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
                             impl_->temporary ? impl_->temporary->peak() : 0,
                             impl_->active_pairs.size(),
                             impl_->events.size(),
                             impl_->discarded_events};
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
