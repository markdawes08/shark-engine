#pragma once

#include "shark/error.hpp"
#include "shark/math.hpp"
#include "shark/physics_components.hpp"
#include "shark/scene_identity.hpp"
#include "shark/scene_query.hpp"
#include "shark/time.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace shark {

class Scene;

struct PhysicsRenderPose {
    EntityReference entity;
    Transform local;
};
struct PhysicsRenderSnapshot {
    std::uint64_t world;
    SceneRevision revision;
    std::uint64_t completed_ticks;
    double alpha;
    std::vector<PhysicsRenderPose> poses;
};

inline constexpr double physics_maximum_linear_speed = 256;
inline constexpr double physics_maximum_angular_speed = 64;
// Magnitude bounds in N and kg*m/s, respectively, for one body/next tick.
inline constexpr double physics_maximum_force = 1'000'000;
inline constexpr double physics_maximum_impulse = 1'000'000;

struct PhysicsBodyVelocity {
    math::Vec3 linear{};
    math::Vec3 angular{};
    bool operator==(const PhysicsBodyVelocity&) const = default;
};

struct PhysicsBodyHandle {
    std::uint64_t world{0};
    std::uint32_t slot{0};
    std::uint64_t generation{0};
    bool operator==(const PhysicsBodyHandle&) const = default;
};

enum class PhysicsContactPhase { enter, exit };
enum class PhysicsContactCause { simulation, body_removed };
struct PhysicsContactParticipant {
    PhysicsBodyHandle body;
    EntityReference entity;
    bool is_trigger;
};
// Historical owned observations. Resolving either participant for a new action
// still requires its current Scene/authority and body generation.
struct PhysicsContactEvent {
    std::uint64_t sequence;
    std::uint64_t completed_tick;
    PhysicsContactPhase phase;
    PhysicsContactCause cause;
    PhysicsContactParticipant first;
    PhysicsContactParticipant second;
};

struct PhysicsBodyState {
    EntityReference entity;
    RigidBody settings;
    Transform pose;
    math::Vec3 collider_half_extents;
    BodyMotion motion;
    double inverse_mass;
    double friction;
    double restitution;
};

inline constexpr std::uint32_t physics_maximum_bodies = 4096;
inline constexpr std::uint32_t physics_minimum_body_pairs = 4;
inline constexpr std::uint32_t physics_maximum_body_pairs = 65536;
inline constexpr std::uint32_t physics_minimum_contact_constraints = 4;
inline constexpr std::uint32_t physics_maximum_contact_constraints = 65536;
inline constexpr std::uint32_t physics_maximum_collision_substeps = 16;
inline constexpr std::size_t physics_minimum_temporary_bytes = 64 * 1024;
inline constexpr std::size_t physics_maximum_temporary_bytes = 64 * 1024 * 1024;
inline constexpr double physics_gravity_component_limit = 1'000'000.0;

struct PhysicsWorldConfig {
    // The caller executes one step from its existing scheduler. Server tick
    // frequency and internal collision substeps remain independent policies.
    std::uint32_t tick_hz{60};
    std::uint32_t collision_substeps{1};
    std::uint32_t max_bodies{physics_maximum_bodies};
    std::uint32_t max_body_pairs{16384};
    std::uint32_t max_contact_constraints{8192};
    std::size_t temporary_bytes{8 * 1024 * 1024};
    math::Vec3 gravity{0.0, -9.81, 0.0};
    std::uint32_t max_contact_pairs{1024};
    std::uint32_t max_queued_events{16384};
    bool operator==(const PhysicsWorldConfig&) const = default;
};

struct PhysicsWorldStats {
    PhysicsWorldConfig config;
    std::uint64_t completed_ticks;
    std::size_t bodies;
    bool closed;
    bool faulted;
    bool simulating{false};
    std::size_t temporary_required_bytes{0};
    std::size_t temporary_high_water_bytes{0};
    std::size_t active_contact_pairs{0};
    std::size_t pending_events{0};
    std::uint64_t discarded_events{0};
};

// CPU-only owner of a bounded native physics system and checked detached bodies.
// Explicit simulation leases fence ordinary Scene writes. Bounded contact
// transitions are taken on the owner thread; queries remain assignment 20.
// Native types and process registrations remain private. Methods require the
// creating thread; this is not a concurrent registry or another scheduler.
class PhysicsWorld final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<PhysicsWorld>>
    create(PhysicsWorldConfig config = {});
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld(PhysicsWorld&&) = delete;
    PhysicsWorld& operator=(PhysicsWorld&&) = delete;

    // Executes exactly one native update of 1/tick_hz seconds with the configured
    // collision substeps. No wall clock, render delta, pause or dropped ticks.
    // This overload is empty/detached only; populated simulation requires Scene.
    // A native update failure faults this owner instead of retrying unknown work.
    [[nodiscard]] Result<void> step();
    // All authored body settings must have matching native snapshots. Acquires
    // one Scene writer after complete inventory and conservative arena admission.
    [[nodiscard]] Result<void> begin_simulation(Scene& scene);
    [[nodiscard]] Result<void> end_simulation(Scene& scene);
    [[nodiscard]] Result<void> step(Scene& scene);
    [[nodiscard]] Result<void> set_static_pose(Scene& scene, PhysicsBodyHandle body,
                                               Transform pose);
    [[nodiscard]] Result<void> teleport(Scene& scene, PhysicsBodyHandle body, Transform pose);
    [[nodiscard]] Result<void> set_kinematic_target(const Scene& scene, PhysicsBodyHandle body,
                                                    Transform target);
    // Dynamic bodies only; require the active Scene lease and creating thread.
    // Force accumulates for the next successful fixed update; end/teleport
    // discard queued force. Impulse and velocity change immediately in call order.
    [[nodiscard]] Result<PhysicsBodyVelocity> velocity(const Scene& scene,
                                                       PhysicsBodyHandle body) const;
    [[nodiscard]] Result<void> set_velocity(const Scene& scene, PhysicsBodyHandle body,
                                            PhysicsBodyVelocity velocity);
    [[nodiscard]] Result<void> apply_force(const Scene& scene, PhysicsBodyHandle body,
                                           math::Vec3 force);
    [[nodiscard]] Result<void> apply_impulse(const Scene& scene, PhysicsBodyHandle body,
                                             math::Vec3 impulse);
    [[nodiscard]] Result<PhysicsRenderSnapshot> render_poses(const Scene& scene,
                                                             double alpha) const;
    // Thin adapters for existing schedulers. Rate/index mismatch rejects before
    // consuming elapsed time. No new accumulator, focus or server pause policy.
    [[nodiscard]] Result<StepReport> poll_offline(Scene& scene, OfflineScheduler& schedule,
                                                  MonotonicTime now, OfflineControl control = {});
    [[nodiscard]] Result<StepReport> poll_server(Scene& scene, ServerScheduler& schedule,
                                                 MonotonicTime now);
    // Explicit detached body snapshots. No Scene pointer is retained and these
    // methods never write authored state. Reads recheck the source incarnation,
    // settings and pose; changed/deleted sources require remove and recreate.
    [[nodiscard]] Result<PhysicsBodyHandle> create_body(const Scene& scene, EntityHandle entity);
    [[nodiscard]] Result<PhysicsBodyState> body_state(const Scene& scene,
                                                      PhysicsBodyHandle body) const;
    [[nodiscard]] Result<void> remove_body(PhysicsBodyHandle body);
    // Runtime detach through the existing leased owner; removes RigidBody from
    // this Core Scene, keeps the visual entity and queues owned exit events.
    [[nodiscard]] Result<void> remove_body(Scene& scene, PhysicsBodyHandle body);
    [[nodiscard]] Result<std::vector<PhysicsContactEvent>> take_events(std::size_t maximum = 256);
    [[nodiscard]] Result<PhysicsWorldStats> stats() const;
    // Idempotent, permanently closed. Releases native owners before their last
    // process-global registration lease. Closed values retain config/tick stats.
    [[nodiscard]] Result<void> close();

  private:
    [[nodiscard]] Result<void> reset_pose(Scene& scene, PhysicsBodyHandle body, Transform pose,
                                          BodyMotion mode);
    [[nodiscard]] Result<void> remove_body_impl(Scene* scene, PhysicsBodyHandle body);
    struct Impl;
    explicit PhysicsWorld(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark
