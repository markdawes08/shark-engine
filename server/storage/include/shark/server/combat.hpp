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
    std::size_t projectile_launches{0};
    std::size_t projectile_impacts{0};
};
struct CombatStats {
    std::uint64_t completed_ticks;
    std::size_t actors;
    std::size_t pending_actions;
    bool faulted;
    std::size_t detached_actors{0};
    std::size_t pending_projectiles{0};
};
struct CombatReconnectReport {
    std::optional<net::CombatState> state;
    std::vector<ScriptEntityLifecycleReport> scripts;
    std::size_t created_bodies{0};
    std::size_t teleported_bodies{0};
    std::optional<Error> error;
};

struct ProjectileCut {
    net::CombatAction request;
    CombatRuleDecision decision;
    net::CombatTargetId owner_life;
    PhysicsBodyHandle owner_body;
    PhysicsBodyHandle target_body;
    EntityReference target_entity;
    math::Vec3 position;
    math::Vec3 direction;
    std::uint32_t remaining_ticks;
    std::uint64_t order;
};
struct CombatActorCut {
    std::uint64_t connection;
    net::SessionSnapshot session;
    net::CombatStreamId stream;
    net::CombatTargetId target;
    EntityReference entity;
    std::optional<PhysicsBodyHandle> body;
    std::optional<PhysicsBodyState> native;
    std::optional<PhysicsBodyVelocity> velocity;
    RigidBody settings;
    Transform spawn;
    net::MovementState movement;
    std::uint16_t health;
    std::uint64_t ready_tick;
    std::uint64_t respawn_tick;
    std::uint64_t accepted;
    std::uint64_t pending_order;
    std::optional<net::CombatAction> pending;
    std::vector<net::CombatActionResult> outcomes;
    bool fenced;
    bool detached;
    Transform detached_pose;
    std::optional<ProjectileCut> projectile;
};
struct CombatCut {
    SceneRevision revision;
    SceneScope scope;
    std::uint64_t physics_world;
    std::uint64_t completed_tick;
    MonotonicTime last_time;
    std::uint64_t admission;
    bool script_mode_bound;
    std::optional<std::uint64_t> script_instance;
    CombatRulesStats rules;
    std::optional<ExecutionPermit> execution;
    std::vector<CombatActorCut> actors;
    // Fingerprints exposed values only. Exact native and Lua owners are retained.
    std::vector<std::byte> bytes;
};

// Creating-thread authoritative host over the retained movement/physics and
// durable session owners. Rules return bounded values and hold no Scene access.
// Actors and interaction outcomes are ephemeral; no item/reward endpoint exists.
class CharacterCombat final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<CharacterCombat>>
    create(const Scene& scene, const PhysicsWorld& physics, std::unique_ptr<CombatRules> rules,
           std::shared_ptr<ExecutionFence> fence = {});
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
    [[nodiscard]] Result<CombatTick>
    step(storage::CharacterAuthority& authority, CharacterMovement& movement, Scene& scene,
         PhysicsWorld& physics, std::span<net::TlsTransport* const> transports, MonotonicTime now,
         ScriptRuntime* scripts = nullptr, const WorldTraversal* traversal = nullptr);
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
    [[nodiscard]] Result<CombatCut> capture_cut(const Scene& scene,
                                                const PhysicsWorld& physics) const;
    [[nodiscard]] Result<CombatRulesPortableState> capture_portable_rules() const;
    [[nodiscard]] Result<CombatBinding> bind_host(GameplayAccess& access, std::uint64_t connection,
                                                  CharacterMovement& movement, const Scene& scene,
                                                  const PhysicsWorld& physics, EntityHandle entity,
                                                  MonotonicTime now);
    [[nodiscard]] Result<void> submit_host(GameplayAccess& access, std::uint64_t connection,
                                           const Scene& scene, const PhysicsWorld& physics,
                                           const net::CombatAction& action, MonotonicTime now);
    [[nodiscard]] Result<CombatTick> step_host(GameplayAccess& access, CharacterMovement& movement,
                                               Scene& scene, PhysicsWorld& physics,
                                               std::span<const std::uint64_t> connections,
                                               MonotonicTime now, ScriptRuntime* scripts = nullptr,
                                               const WorldTraversal* traversal = nullptr);
    [[nodiscard]] Result<net::CombatState> view_host(GameplayAccess& access,
                                                     std::uint64_t connection, const Scene& scene,
                                                     const PhysicsWorld& physics,
                                                     std::span<const net::CombatTargetId> visible,
                                                     MonotonicTime now) const;
    [[nodiscard]] static Result<std::unique_ptr<CharacterCombat>>
    stage_transfer(const Scene& scene, const PhysicsWorld& physics, const CombatCut& source,
                   std::span<const GameplayTransferBinding> bindings, MonotonicTime now,
                   std::unique_ptr<CombatRules> rules, std::shared_ptr<ExecutionFence> fence);

  private:
    friend class RegionWorld;
    friend class RegionExecutor;
    [[nodiscard]] Result<void> adopt_execution(ExecutionPermit permit);
    struct Impl;
    explicit CharacterCombat(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::server
