#pragma once

#include "shark/server/combat.hpp"

#include <bit>
#include <type_traits>

namespace shark::server::detail {

// Explicit BE values, never padding, pointers or native ABI layouts. Used only
// to compare co-located cut observations; deliberately has no decoder/install.
struct CutEncoder {
    std::vector<std::byte> bytes;
    void integer(std::uint64_t number, unsigned width) {
        for (auto index = width; index > 0; --index)
            bytes.push_back(static_cast<std::byte>((number >> ((index - 1) * 8)) & 255));
    }
    void value(bool x) {
        integer(x ? 1 : 0, 1);
    }
    void value(std::uint64_t x) {
        integer(x, 8);
    }
    void value(std::uint32_t x) {
        integer(x, 4);
    }
    void value(std::uint16_t x) {
        integer(x, 2);
    }
    void value(std::int64_t x) {
        integer(static_cast<std::uint64_t>(x), 8);
    }
    void value(std::int16_t x) {
        integer(static_cast<std::uint16_t>(x), 2);
    }
    void value(double x) {
        value(std::bit_cast<std::uint64_t>(x));
    }
    template <class E>
        requires std::is_enum_v<E>
    void value(E x) {
        value(static_cast<std::uint32_t>(x));
    }
    template <class D> void value(const TypedId<D>& x) {
        bytes.insert(bytes.end(), x.value().bytes().begin(), x.value().bytes().end());
    }
    template <class T> void value(const std::optional<T>& x) {
        value(x.has_value());
        if (x)
            value(*x);
    }
    template <class T> void value(const std::vector<T>& x) {
        value(static_cast<std::uint64_t>(x.size()));
        for (const auto& element : x)
            value(element);
    }
    void value(MonotonicTime x) {
        value(static_cast<std::int64_t>(x.count()));
    }
    void value(const math::Vec3& x) {
        value(x.x);
        value(x.y);
        value(x.z);
    }
    void value(const Transform& x) {
        value(x.translation);
        value(x.rotation.x);
        value(x.rotation.y);
        value(x.rotation.z);
        value(x.rotation.w);
        value(x.scale);
    }
    void value(const RigidBody& x) {
        value(x.motion);
        value(x.shape);
        value(x.half_extents);
        value(x.radius);
        value(x.half_height);
        value(x.mass);
        value(x.friction);
        value(x.restitution);
        value(x.collision_layer);
        value(x.collision_mask);
        value(x.is_trigger);
    }
    void value(const EntityHandle& x) {
        value(x.scene_instance);
        value(x.slot);
        value(x.generation);
    }
    void value(const AuthorityStamp& x) {
        value(x.region);
        value(x.instance);
        value(x.epoch);
    }
    void value(const SceneScope& x) {
        integer(x.index(), 1);
        std::visit(
            [&](const auto& scope) {
                using T = std::decay_t<decltype(scope)>;
                if constexpr (std::is_same_v<T, AuthoredSceneScope>) {
                    value(scope.scene);
                } else if constexpr (std::is_same_v<T, RegionSceneScope>) {
                    value(scope.world);
                    value(scope.authority);
                } else {
                    value(scope.world);
                    value(scope.session);
                }
            },
            x);
    }
    void value(const EntityReference& x) {
        value(x.handle);
        integer(x.identity.index(), 1);
        std::visit([&](const auto& id) { value(id); }, x.identity);
        value(x.scope);
        value(x.authority);
    }
    void value(const PhysicsBodyHandle& x) {
        value(x.world);
        value(x.slot);
        value(x.generation);
    }
    void value(const PhysicsBodyVelocity& x) {
        value(x.linear);
        value(x.angular);
    }
    void value(const PhysicsBodyState& x) {
        value(x.entity);
        value(x.settings);
        value(x.pose);
        value(x.collider_half_extents);
        value(x.motion);
        value(x.inverse_mass);
        value(x.friction);
        value(x.restitution);
        value(x.geometry);
        value(x.collision_layer);
        value(x.collision_mask);
        value(static_cast<std::uint64_t>(x.triangle_count));
        value(x.is_trigger);
    }
    void value(const SceneRevision& x) {
        value(x.scene_instance);
        value(x.revision);
    }
    void value(const ExecutionPermit& x) {
        value(x.scope);
        value(x.generation);
    }
    void value(const WorldPosition& x) {
        value(x.world);
        value(x.cell.x);
        value(x.cell.y);
        value(x.cell.z);
        for (const auto coordinate : x.offset)
            value(coordinate);
    }
    void value(const MovementDomain& x) {
        value(x.min_x);
        value(x.max_x);
        value(x.min_z);
        value(x.max_z);
    }
    void value(const net::SessionSnapshot& x) {
        value(x.session);
        value(x.account);
        value(x.character);
        value(x.ownership_epoch);
        value(x.last_sequence);
        value(x.ready);
        value(x.active);
    }
    void value(const net::SessionEnvelope& x) {
        value(x.session);
        value(x.ownership_epoch);
        value(x.sequence);
        value(x.character);
    }
    void value(const net::MovementInput& x) {
        value(x.authority);
        value(x.stream);
        value(x.input_sequence);
        value(x.x);
        value(x.z);
    }
    void value(const net::MovementState& x) {
        value(x.session);
        value(x.ownership_epoch);
        value(x.stream);
        value(x.completed_tick);
        value(x.last_input);
        value(x.position);
        value(x.velocity);
    }
    void value(const net::CombatAction& x) {
        value(x.authority);
        value(x.stream);
        value(x.action_sequence);
        value(x.target);
        value(x.kind);
    }
    void value(const net::CombatActionResult& x) {
        value(x.action_sequence);
        value(x.target);
        value(x.kind);
        value(x.outcome);
        value(x.amount);
    }
    void value(const CombatRuleDecision& x) {
        value(x.allowed);
        value(x.damage);
        value(x.cooldown_ticks);
    }
    void value(const CombatRulesStats& x) {
        value(x.evaluations);
        value(x.faulted);
    }
    void value(const MovementActorCut& x) {
        value(x.connection);
        value(x.session);
        value(x.stream);
        value(x.entity);
        value(x.body);
        value(x.native);
        value(x.velocity);
        value(x.accepted);
        value(x.applied);
        value(x.domain);
        value(x.pending);
    }
    void value(const ProjectileCut& x) {
        value(x.request);
        value(x.decision);
        value(x.owner_life);
        value(x.owner_body);
        value(x.target_body);
        value(x.target_entity);
        value(x.position);
        value(x.direction);
        value(x.remaining_ticks);
        value(x.order);
    }
    void value(const CombatActorCut& x) {
        value(x.connection);
        value(x.session);
        value(x.stream);
        value(x.target);
        value(x.entity);
        value(x.body);
        value(x.native);
        value(x.velocity);
        value(x.settings);
        value(x.spawn);
        value(x.movement);
        value(x.health);
        value(x.ready_tick);
        value(x.respawn_tick);
        value(x.accepted);
        value(x.pending_order);
        value(x.pending);
        value(x.outcomes);
        value(x.fenced);
        value(x.detached);
        value(x.detached_pose);
        value(x.projectile);
    }
    void value(const MovementCut& x) {
        value(std::uint32_t{1}); // cut kind/version, not a public wire format
        value(x.revision);
        value(x.scope);
        value(x.physics_world);
        value(x.origin);
        value(x.traversal_instance);
        value(x.domains);
        value(x.completed_tick);
        value(x.last_time);
        value(x.execution);
        value(x.actors);
    }
    void value(const CombatCut& x) {
        value(std::uint32_t{2});
        value(x.revision);
        value(x.scope);
        value(x.physics_world);
        value(x.completed_tick);
        value(x.last_time);
        value(x.admission);
        value(x.script_mode_bound);
        value(x.script_instance);
        value(x.rules);
        value(x.execution);
        value(x.actors);
    }
};

} // namespace shark::server::detail
