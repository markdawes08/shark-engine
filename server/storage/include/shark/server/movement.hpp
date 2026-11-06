#pragma once

#include "shark/net/movement.hpp"
#include "shark/physics.hpp"
#include "shark/server/execution_fence.hpp"
#include "shark/storage/characters.hpp"
#include "shark/world_position.hpp"

#include <span>

namespace shark {
class WorldTraversal;
}

namespace shark::server {

inline constexpr std::size_t movement_maximum_characters = 32;
inline constexpr std::size_t movement_pending_inputs = net::movement_maximum_pending_inputs;
inline constexpr double movement_speed = net::movement_profile_speed;
// Leaves headroom below the native 256 m/s total-vector ceiling even after
// horizontal input is packed to float. Ordinary falling velocity is preserved.
inline constexpr double movement_maximum_vertical_speed = net::movement_profile_vertical_speed;
inline constexpr std::uint32_t movement_tick_hz = net::movement_profile_tick_hz;
inline constexpr std::uint32_t movement_collision_substeps =
    net::movement_profile_collision_substeps;

struct MovementDelivery {
    std::uint64_t connection;
    net::MovementState state;
};
struct MovementTick {
    std::uint64_t completed_tick;
    std::size_t fenced;
    std::vector<MovementDelivery> deliveries;
    std::vector<PhysicsContactEvent> contacts;
};
struct MovementStats {
    std::uint64_t completed_ticks;
    std::size_t characters;
    std::size_t pending_inputs;
    bool faulted;
};

// Optional trusted-host coverage in the same Scene-local frame. The immutable
// union uses half-open XZ rectangles; it grants no regional/session authority.
struct MovementDomain {
    double min_x{0};
    double max_x{0};
    double min_z{0};
    double max_z{0};
};

struct MovementActorCut {
    std::uint64_t connection;
    net::SessionSnapshot session;
    net::MovementStreamId stream;
    EntityReference entity;
    PhysicsBodyHandle body;
    PhysicsBodyState native;
    PhysicsBodyVelocity velocity;
    std::uint64_t accepted;
    std::uint64_t applied;
    std::optional<MovementDomain> domain;
    // Original admitted values in FIFO order; at most eight.
    std::vector<net::MovementInput> pending;
};
struct MovementCut {
    SceneRevision revision;
    SceneScope scope;
    std::uint64_t physics_world;
    WorldPosition origin;
    std::optional<std::uint64_t> traversal_instance;
    std::vector<MovementDomain> domains;
    std::uint64_t completed_tick;
    MonotonicTime last_time;
    std::optional<ExecutionPermit> execution;
    std::vector<MovementActorCut> actors;
    // Canonical process-local fingerprint of every field above. This is not
    // portable native state or an installable authority credential.
    std::vector<std::byte> bytes;
};
[[nodiscard]] Result<bool> movement_coverage(std::span<const MovementDomain> domains,
                                             math::Vec3 from, math::Vec3 to);

// Trusted creating-thread host adapter. Owns bounded input values and checked
// identities, never a borrowed Scene/physics/transport or a second session owner.
// A scheduler calls step exactly once per authoritative tick. Source authoring,
// client predictions, durable checkpoints and region transfer are separate.
class CharacterMovement final {
  public:
    // Optional streamed-world mode fixes one trusted traversal incarnation.
    // Every tick must supply that same owner and exact local origin.
    [[nodiscard]] static Result<std::unique_ptr<CharacterMovement>>
    create(const Scene& scene, const PhysicsWorld& physics, WorldPosition origin,
           std::optional<std::uint64_t> traversal_instance = std::nullopt,
           std::span<const MovementDomain> domains = {},
           std::shared_ptr<ExecutionFence> fence = {});
    ~CharacterMovement();
    CharacterMovement(const CharacterMovement&) = delete;
    CharacterMovement& operator=(const CharacterMovement&) = delete;

    // Trusted host attaches an existing durable-owned root dynamic sphere.
    // Rebinding requires removal/fencing first and always issues a fresh stream.
    [[nodiscard]] Result<net::MovementState> bind(storage::CharacterAuthority& authority,
                                                  net::TlsTransport& transport, const Scene& scene,
                                                  const PhysicsWorld& physics, EntityHandle entity,
                                                  MonotonicTime now);
    // One accepted input is one future tick; no input means horizontal stop.
    // Shape/stream/queue checks precede the shared exact-next session admission.
    [[nodiscard]] Result<void> submit(storage::CharacterAuthority& authority,
                                      net::TlsTransport& transport, const Scene& scene,
                                      const PhysicsWorld& physics, const net::MovementInput& input,
                                      MonotonicTime now);
    // Borrow live server transports for this call only. Missing/replaced/revoked
    // participants lose queued input before native work. Returned contact values
    // belong to the host's existing ordered dispatch; this method invokes no Lua.
    // Failure after the first velocity write permanently faults this adapter;
    // native partial effects must never be replayed as an unexecuted tick.
    // Unready swept cells stop horizontal motion while consuming that input as
    // an applied stop; vertical native motion and committed corrections remain.
    [[nodiscard]] Result<MovementTick> step(storage::CharacterAuthority& authority, Scene& scene,
                                            PhysicsWorld& physics,
                                            std::span<net::TlsTransport* const> transports,
                                            MonotonicTime now,
                                            const WorldTraversal* traversal = nullptr);
    [[nodiscard]] Result<MovementStats> stats() const;
    // Trusted fixed-phase lifecycle controls. Release stops the exact old body
    // and discards queued input without acknowledging it as applied. Respawn
    // must bind again and issue a fresh stream; these are not player setters.
    [[nodiscard]] Result<void> release(const Scene& scene, PhysicsWorld& physics,
                                       net::MovementStreamId stream);
    [[nodiscard]] Result<net::MovementState>
    observe(const Scene& scene, const PhysicsWorld& physics, net::MovementStreamId stream) const;
    // Trusted owned observation using the explicit local origin. Not a durable
    // save or a client-proposed checkpoint; only committed poses are converted.
    [[nodiscard]] Result<WorldPosition> position(const Scene& scene, const PhysicsWorld& physics,
                                                 net::MovementStreamId stream) const;
    [[nodiscard]] Result<MovementCut> capture_cut(const Scene& scene,
                                                  const PhysicsWorld& physics) const;

  private:
    friend class RegionWorld;
    friend class CharacterCombat;
    [[nodiscard]] Result<void> adopt_execution(ExecutionPermit permit);
    [[nodiscard]] Result<void> set_actor_domain(net::MovementStreamId stream,
                                                std::optional<MovementDomain> domain);
    [[nodiscard]] Result<void> check_traversal(const WorldTraversal* traversal) const;
    struct Impl;
    explicit CharacterMovement(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::server
