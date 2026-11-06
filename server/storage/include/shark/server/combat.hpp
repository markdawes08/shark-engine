#pragma once

#include "shark/combat_rules.hpp"
#include "shark/net/combat.hpp"
#include "shark/server/movement.hpp"

namespace shark::server {

struct CombatBinding {
    net::CombatStreamId stream;
    net::CombatTargetId target;
    net::MovementState movement;
};
struct CombatResolution {
    net::CombatStreamId actor;
    net::CombatActionResult outcome;
};
struct CombatRebinding {
    net::CombatTargetId previous;
    CombatBinding binding;
};
struct CombatTick {
    MovementTick movement;
    std::vector<CombatResolution> outcomes;
    std::vector<CombatRebinding> rebindings;
    std::size_t deaths{0};
    std::size_t respawns{0};
    std::size_t fenced{0};
    std::size_t released_movements{0};
    std::size_t removed_bodies{0};
    std::size_t created_bodies{0};
    std::size_t teleported_bodies{0};
    // Native movement and earlier effects are owned observations, not rollback.
    // On error the owner is permanently faulted; never repeat this tick.
    std::optional<Error> error{};
};
struct CombatStats {
    std::uint64_t completed_ticks;
    std::size_t actors;
    std::size_t pending_actions;
    bool faulted;
};

// Creating-thread authoritative host over the retained movement/physics and
// durable session owners. Rules return bounded values and hold no Scene access.
// Actors and interaction outcomes are ephemeral; no item/reward endpoint exists.
class CharacterCombat final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<CharacterCombat>>
    create(const Scene& scene, const PhysicsWorld& physics, std::unique_ptr<CombatRules> rules);
    ~CharacterCombat();
    CharacterCombat(const CharacterCombat&) = delete;
    CharacterCombat& operator=(const CharacterCombat&) = delete;

    [[nodiscard]] Result<CombatBinding> bind(storage::CharacterAuthority& authority,
                                             net::TlsTransport& transport,
                                             CharacterMovement& movement, const Scene& scene,
                                             const PhysicsWorld& physics, EntityHandle entity,
                                             MonotonicTime now);
    [[nodiscard]] Result<void> submit(storage::CharacterAuthority& authority,
                                      net::TlsTransport& transport, const Scene& scene,
                                      const PhysicsWorld& physics, const net::CombatAction& action,
                                      MonotonicTime now);
    // Executes exactly one movement/native tick, then current-pose combat and
    // respawns in fixed order. Return owned contacts for explicit host processing;
    // this adapter does not invoke entity ScriptRuntime contact callbacks.
    // Neither native callbacks nor prediction execute combat rules.
    [[nodiscard]] Result<CombatTick>
    step(storage::CharacterAuthority& authority, CharacterMovement& movement, Scene& scene,
         PhysicsWorld& physics, std::span<net::TlsTransport* const> transports, MonotonicTime now);
    // Selection is a bounded trusted visibility allowlist, not a player list.
    [[nodiscard]] Result<net::CombatState> view(storage::CharacterAuthority& authority,
                                                net::TlsTransport& transport, const Scene& scene,
                                                const PhysicsWorld& physics,
                                                std::span<const net::CombatTargetId> visible,
                                                MonotonicTime now) const;
    [[nodiscard]] Result<CombatStats> stats() const;

  private:
    struct Impl;
    explicit CharacterCombat(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::server
