#pragma once

#include "shark/error.hpp"
#include "shark/math.hpp"
#include "shark/physics_components.hpp"
#include "shark/scene_identity.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace shark {

class Scene;

struct PhysicsBodyHandle {
    std::uint64_t world{0};
    std::uint32_t slot{0};
    std::uint64_t generation{0};
    bool operator==(const PhysicsBodyHandle&) const = default;
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
    bool operator==(const PhysicsWorldConfig&) const = default;
};

struct PhysicsWorldStats {
    PhysicsWorldConfig config;
    std::uint64_t completed_ticks;
    std::size_t bodies;
    bool closed;
    bool faulted;
};

// CPU-only owner of a bounded native physics system and checked detached bodies.
// Empty-world fixed-duration stepping is supported; nonempty simulation, scene
// synchronization, contacts and queries remain later checkpoints.
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
    // Nonempty worlds reject before work until simulation admission lands in 18c.
    // A native update failure faults this owner instead of retrying unknown work.
    [[nodiscard]] Result<void> step();
    // Explicit detached body snapshots. No Scene pointer is retained and these
    // methods never write authored state. Reads recheck the source incarnation,
    // settings and pose; changed/deleted sources require remove and recreate.
    [[nodiscard]] Result<PhysicsBodyHandle> create_body(const Scene& scene, EntityHandle entity);
    [[nodiscard]] Result<PhysicsBodyState> body_state(const Scene& scene,
                                                      PhysicsBodyHandle body) const;
    [[nodiscard]] Result<void> remove_body(PhysicsBodyHandle body);
    [[nodiscard]] Result<PhysicsWorldStats> stats() const;
    // Idempotent, permanently closed. Releases native owners before their last
    // process-global registration lease. Closed values retain config/tick stats.
    [[nodiscard]] Result<void> close();

  private:
    struct Impl;
    explicit PhysicsWorld(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark
