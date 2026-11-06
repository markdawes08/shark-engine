#pragma once

#include "shark/net/interest.hpp"
#include "shark/server/combat.hpp"
#include "shark/world_traversal.hpp"

namespace shark::server {

inline constexpr std::size_t region_world_maximum_regions = 8;
inline constexpr std::size_t region_world_maximum_npcs = 16;

// Co-located logical ownership partitions in the shared island's local frame.
// A point belongs to exactly one half-open XZ rectangle. Scope on the shared
// Scene still identifies its single simulation owner, not these child writers.
struct RegionDefinition {
    RegionId id;
    double min_x;
    double max_x;
    double min_z;
    double max_z;
    // Coverage band must contain InterestLimits.exit_radius. Actual distance
    // selection retains InterestServer's shared entry/exit hysteresis.
    double interest_overlap{110};
};

// Trusted process-local routing capability. A copied/stale grant cannot admit
// another command after its entity's committed regional transition.
struct RegionGrant {
    std::uint64_t scope;
    WorldEntityId entity;
    RegionId region;
    std::uint64_t generation;
    bool operator==(const RegionGrant&) const = default;
};
struct RegionBinding {
    RegionGrant grant;
    CombatBinding gameplay;
};
struct RegionTransition {
    RegionGrant before;
    RegionGrant after;
};
struct RegionTick {
    CombatTick gameplay;
    std::vector<RegionTransition> transitions;
    // Native/combat progress and transitions above are original observations,
    // not rollback or permission to retry a partially executed tick.
    std::optional<Error> error{};
    std::size_t npc_velocity_writes{0};
};
struct RegionActor {
    RegionGrant grant;
    EntityReference entity;
    net::CharacterId character;
    net::CombatTargetId target;
    bool active;
};
struct RegionNpcActor {
    RegionGrant grant;
    EntityReference entity;
    PhysicsBodyHandle body;
};
struct RegionReconnect {
    RegionGrant grant;
    CombatReconnectReport gameplay;
};
struct RegionWorldStats {
    std::uint64_t completed_ticks;
    std::size_t regions;
    std::size_t actors;
    std::uint64_t transitions;
    bool faulted;
    std::size_t npcs{0};
};

// One creating-thread co-located island. The host owns the borrowed Scene,
// PhysicsWorld, traversal and actual session/storage authority. This adapter
// privately owns movement, combat and interest; regional gameplay ingress must
// use its exact grant gate. It is not distributed lease/persistence fencing.
class RegionWorld final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<RegionWorld>>
    create(Scene& scene, PhysicsWorld& physics, WorldLocalFrame frame,
           const WorldTraversal& traversal, std::unique_ptr<CombatRules> rules,
           std::span<const RegionDefinition> regions, net::InterestLimits interest = {});
    ~RegionWorld();
    RegionWorld(const RegionWorld&) = delete;
    RegionWorld& operator=(const RegionWorld&) = delete;
    RegionWorld(RegionWorld&&) = delete;
    RegionWorld& operator=(RegionWorld&&) = delete;

    [[nodiscard]] Result<RegionBinding> bind(storage::CharacterAuthority& authority,
                                             net::TlsTransport& transport, EntityHandle entity,
                                             MonotonicTime now);
    [[nodiscard]] Result<RegionGrant> current_grant(WorldEntityId entity) const;
    [[nodiscard]] Result<std::vector<RegionActor>> actors() const;
    // Trusted host registration of a precreated live root dynamic primitive.
    // NPCs have no player/session/economic identity. One horizontal intent per
    // tick is allowed; omission stops horizontal motion, preserving physics Y.
    [[nodiscard]] Result<RegionGrant> bind_npc(EntityHandle entity);
    [[nodiscard]] Result<void> submit_npc_velocity(const RegionGrant& grant,
                                                   math::Vec3 horizontal_velocity);
    [[nodiscard]] Result<std::vector<RegionNpcActor>> npcs() const;
    [[nodiscard]] Result<RegionId> region_at(WorldPosition position) const;
    [[nodiscard]] Result<void> submit_movement(const RegionGrant& grant,
                                               storage::CharacterAuthority& authority,
                                               net::TlsTransport& transport,
                                               const net::MovementInput& input, MonotonicTime now);
    [[nodiscard]] Result<void> submit_combat(const RegionGrant& grant,
                                             storage::CharacterAuthority& authority,
                                             net::TlsTransport& transport,
                                             const net::CombatAction& action, MonotonicTime now);
    [[nodiscard]] Result<RegionTick> step(storage::CharacterAuthority& authority,
                                          std::span<net::TlsTransport* const> transports,
                                          MonotonicTime now, ScriptRuntime* scripts = nullptr);
    [[nodiscard]] Result<net::CombatState> view(const RegionGrant& grant,
                                                storage::CharacterAuthority& authority,
                                                net::TlsTransport& transport,
                                                MonotonicTime now) const;
    [[nodiscard]] Result<RegionReconnect> reconnect(const RegionGrant& grant,
                                                    storage::CharacterAuthority& authority,
                                                    net::TlsTransport& transport, MonotonicTime now,
                                                    ScriptRuntime* scripts = nullptr);

    [[nodiscard]] Result<net::InterestPublishResult>
    publish_interest(const RegionGrant& grant, storage::CharacterAuthority& authority,
                     net::TlsTransport& transport, MonotonicTime now);
    [[nodiscard]] Result<net::ReplicationControlResult>
    control_interest(const RegionGrant& grant, storage::CharacterAuthority& authority,
                     net::TlsTransport& transport, const net::ReplicationControl& control,
                     MonotonicTime now);
    [[nodiscard]] Result<storage::RuntimeTicket>
    submit_transfer(const RegionGrant& grant, storage::CharacterAuthority& authority,
                    net::TlsTransport& transport, const net::SessionEnvelope& envelope,
                    const storage::TransferRequest& request, MonotonicTime deadline,
                    MonotonicTime now);
    [[nodiscard]] Result<storage::RuntimeCompletion>
    take_transfer(const RegionGrant& grant, storage::CharacterAuthority& authority,
                  net::TlsTransport& transport, storage::RuntimeTicket ticket, MonotonicTime now);
    [[nodiscard]] Result<RegionWorldStats> stats() const;

  private:
    struct Impl;
    explicit RegionWorld(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::server
