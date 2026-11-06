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
    // A completed native tick was observed. False never promises rollback;
    // error/report prefixes and the owning movement fault still govern retry.
    bool movement_completed{false};
    std::vector<ScriptContactReport> script_contacts{};
    std::vector<ScriptEntityLifecycleReport> script_lifecycles{};
    ScriptEntityMutationReport script_mutations{};
    // Native movement and earlier effects are owned observations, not rollback.
    // On error the owner is permanently faulted; never repeat this tick.
    std::optional<Error> error{};
};
struct CombatStats {
    std::uint64_t completed_ticks;
    std::size_t actors;
    std::size_t pending_actions;
    bool faulted;
    std::size_t detached_actors{0};
};
struct CombatReconnectReport {
    std::optional<net::CombatState> state;
    std::vector<ScriptEntityLifecycleReport> scripts;
    std::size_t created_bodies{0};
    std::size_t teleported_bodies{0};
    std::optional<Error> error;
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
    // respawns in fixed order. Optional entity scripts use the same retained
    // owner: fixed update, owned contacts, then checked life transitions. The
    // first admitted step/reconnect fixes the optional runtime incarnation;
    // later omission, replacement or opt-in rejects before callbacks. Native
    // callbacks and client prediction never execute gameplay rules.
    [[nodiscard]] Result<CombatTick> step(storage::CharacterAuthority& authority,
                                          CharacterMovement& movement, Scene& scene,
                                          PhysicsWorld& physics,
                                          std::span<net::TlsTransport* const> transports,
                                          MonotonicTime now, ScriptRuntime* scripts = nullptr);
    // A detached record is required; a fresh ready session restores its original
    // health/cooldown/death deadline and exact entity, never a reset/heal intent.
    [[nodiscard]] Result<CombatReconnectReport>
    reconnect(storage::CharacterAuthority& authority, net::TlsTransport& transport,
              CharacterMovement& movement, Scene& scene, PhysicsWorld& physics, EntityHandle entity,
              MonotonicTime now, ScriptRuntime* scripts = nullptr);
    // Trusted inventory for explicit replication selection. Detached/dead actors
    // are absent; returned references are observations and resolve again at use.
    [[nodiscard]] Result<std::vector<EntityReference>>
    active_entities(const Scene& scene, const PhysicsWorld& physics) const;
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
