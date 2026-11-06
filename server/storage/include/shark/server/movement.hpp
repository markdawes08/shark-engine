#pragma once

#include "shark/net/movement.hpp"
#include "shark/physics.hpp"
#include "shark/storage/characters.hpp"
#include "shark/world_position.hpp"

#include <span>

namespace shark::server {

inline constexpr std::size_t movement_maximum_characters = 32;
inline constexpr std::size_t movement_pending_inputs = 8;
inline constexpr double movement_speed = 6.0;
// Leaves headroom below the native 256 m/s total-vector ceiling even after
// horizontal input is packed to float. Ordinary falling velocity is preserved.
inline constexpr double movement_maximum_vertical_speed = 250.0;
inline constexpr std::uint32_t movement_tick_hz = 20;
inline constexpr std::uint32_t movement_collision_substeps = 3;

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

// Trusted creating-thread host adapter. Owns bounded input values and checked
// identities, never a borrowed Scene/physics/transport or a second session owner.
// A scheduler calls step exactly once per authoritative tick. Source authoring,
// client predictions, durable checkpoints and region transfer are separate.
class CharacterMovement final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<CharacterMovement>>
    create(const Scene& scene, const PhysicsWorld& physics, WorldPosition origin);
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
    [[nodiscard]] Result<MovementTick> step(storage::CharacterAuthority& authority, Scene& scene,
                                            PhysicsWorld& physics,
                                            std::span<net::TlsTransport* const> transports,
                                            MonotonicTime now);
    [[nodiscard]] Result<MovementStats> stats() const;
    // Trusted owned observation using the explicit local origin. Not a durable
    // save or a client-proposed checkpoint; only committed poses are converted.
    [[nodiscard]] Result<WorldPosition> position(const Scene& scene, const PhysicsWorld& physics,
                                                 net::MovementStreamId stream) const;

  private:
    struct Impl;
    explicit CharacterMovement(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::server
