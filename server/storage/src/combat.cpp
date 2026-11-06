#include "shark/server/combat.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <thread>

namespace shark::server {
namespace {
Error rejected(const char* operation, const char* message, ErrorCode code = ErrorCode::conflict) {
    return {code, message, {.operation = operation}};
}
bool same_session(const net::SessionSnapshot& a, const net::SessionSnapshot& b) {
    return b.active && b.ready && a.session == b.session && a.account == b.account &&
           a.character == b.character && a.ownership_epoch == b.ownership_epoch;
}
net::TlsTransport* connection(std::span<net::TlsTransport* const> transports, std::uint64_t id) {
    for (auto* transport : transports) {
        if (!transport)
            continue;
        const auto peer = transport->authenticated_peer();
        if (peer && peer.value().connection_id == id)
            return transport;
    }
    return nullptr;
}
} // namespace

struct CharacterCombat::Impl {
    struct Actor {
        std::uint64_t connection;
        net::SessionSnapshot session;
        net::CombatStreamId stream;
        net::CombatTargetId target;
        EntityReference entity;
        std::optional<PhysicsBodyHandle> body;
        RigidBody settings;
        Transform spawn;
        net::MovementState movement;
        std::uint16_t health{combat_maximum_health};
        std::uint64_t ready_tick{0};
        std::uint64_t respawn_tick{0};
        std::uint64_t accepted{0};
        std::uint64_t pending_order{0};
        std::optional<net::CombatAction> pending{};
        std::array<std::optional<net::CombatActionResult>, net::combat_maximum_outcomes> outcomes{};
        std::size_t outcome_count{0};
        bool fenced{false};
    };
    std::thread::id owner{std::this_thread::get_id()};
    std::uint64_t scene_instance;
    std::uint64_t world;
    std::uint64_t ticks;
    mutable MonotonicTime last_time{};
    std::unique_ptr<CombatRules> rules;
    std::vector<Actor> actors;
    std::uint64_t admission{0};
    bool faulted{false};

    Impl(std::uint64_t scene, std::uint64_t native, std::uint64_t completed,
         std::unique_ptr<CombatRules> configured)
        : scene_instance(scene), world(native), ticks(completed), rules(std::move(configured)) {
        actors.reserve(movement_maximum_characters);
    }
    Result<void> check(const char* operation) const {
        if (owner != std::this_thread::get_id())
            return rejected(operation, "Combat requires its creating thread");
        if (faulted)
            return rejected(operation, "Combat has partial effects; reconcile and replace owner");
        return {};
    }
    Result<void> context(const Scene& scene, const PhysicsWorld& physics, MonotonicTime now,
                         const char* operation) const {
        if (auto valid = check(operation); !valid)
            return valid;
        if (now < MonotonicTime::zero() || now < last_time)
            return rejected(operation, "Combat clock cannot regress");
        auto current = scene.stats();
        if (!current)
            return current.error();
        auto native = physics.stats();
        if (!native)
            return native.error();
        if (current.value().closed || current.value().instance != scene_instance ||
            current.value().pending_mutations != 0 || native.value().completed_ticks != ticks ||
            native.value().config.tick_hz != movement_tick_hz ||
            native.value().config.collision_substeps != movement_collision_substeps)
            return rejected(operation, "Combat requires its original fixed-step owners");
        auto poses = physics.render_poses(scene, 0);
        if (!poses)
            return poses.error();
        if (poses.value().world != world)
            return rejected(operation, "Combat cannot adopt a replacement native owner");
        // Reads also advance the owner's clock fence. Session observation may
        // expire leases; a later regressed call cannot turn that clock error
        // into participant fencing after another native tick.
        last_time = now;
        return {};
    }
    Result<PhysicsBodyState> live_body(const Actor& actor, const Scene& scene,
                                       const PhysicsWorld& physics) const {
        if (!actor.body || !scene.resolve(actor.entity))
            return rejected("server.combat.body", "Combat actor has no checked live body");
        auto current = physics.body_state(scene, *actor.body);
        if (!current)
            return current.error();
        if (current.value().entity != actor.entity || current.value().settings != actor.settings)
            return rejected("server.combat.body",
                            "Combat entity/body generation or profile changed");
        return current;
    }
    void remember(Actor& actor, net::CombatActionResult outcome) {
        if (actor.outcome_count == net::combat_maximum_outcomes) {
            for (std::size_t index = 1; index < actor.outcome_count; ++index)
                actor.outcomes[index - 1] = actor.outcomes[index];
            --actor.outcome_count;
        }
        actor.outcomes[actor.outcome_count++] = outcome;
    }
};

CharacterCombat::CharacterCombat(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
CharacterCombat::~CharacterCombat() = default;

Result<std::unique_ptr<CharacterCombat>>
CharacterCombat::create(const Scene& scene, const PhysicsWorld& physics,
                        std::unique_ptr<CombatRules> rules) {
    constexpr auto operation = "server.combat.create";
    if (!rules)
        return rejected(operation, "Combat requires explicit server rules",
                        ErrorCode::invalid_argument);
    auto rule_state = rules->stats();
    if (!rule_state)
        return rule_state.error();
    if (rule_state.value().faulted)
        return rejected(operation, "Combat rules are faulted");
    auto scene_state = scene.stats();
    if (!scene_state)
        return scene_state.error();
    auto poses = physics.render_poses(scene, 0);
    if (!poses)
        return poses.error();
    auto page = scene.query_entities();
    if (!page)
        return page.error();
    if (!std::holds_alternative<RegionSceneScope>(page.value().scope()))
        return rejected(operation, "Combat requires an authoritative Region Scene");
    auto owner = std::unique_ptr<CharacterCombat>{new CharacterCombat{
        std::make_unique<Impl>(scene_state.value().instance, poses.value().world,
                               poses.value().completed_ticks, std::move(rules))}};
    if (auto valid = owner->impl_->context(scene, physics, {}, operation); !valid)
        return valid.error();
    return owner;
}

Result<CombatBinding> CharacterCombat::bind(storage::CharacterAuthority& authority,
                                            net::TlsTransport& transport,
                                            CharacterMovement& movement, const Scene& scene,
                                            const PhysicsWorld& physics, EntityHandle entity,
                                            MonotonicTime now) {
    constexpr auto operation = "server.combat.bind";
    if (auto valid = impl_->context(scene, physics, now, operation); !valid)
        return valid.error();
    auto session = authority.authorize_entity(transport, scene, entity, now);
    if (!session)
        return session.error();
    auto peer = transport.authenticated_peer();
    if (!peer)
        return peer.error();
    if (impl_->actors.size() == movement_maximum_characters ||
        std::any_of(impl_->actors.begin(), impl_->actors.end(), [&](const auto& actor) {
            return actor.connection == peer.value().connection_id ||
                   actor.session.account == session.value().account ||
                   actor.entity.handle == entity;
        }))
        return rejected(operation, "Combat actor is already bound or capacity is full");
    auto reference = scene.reference(entity);
    if (!reference)
        return reference.error();
    auto body = physics.find_body(scene, reference.value());
    if (!body)
        return body.error();
    auto state = physics.body_state(scene, body.value());
    if (!state)
        return state.error();
    auto stream = generate_stable_id();
    auto target = generate_stable_id();
    if (!stream)
        return stream.error();
    if (!target)
        return target.error();
    auto bound = movement.bind(authority, transport, scene, physics, entity, now);
    if (!bound)
        return bound.error();
    impl_->actors.push_back({peer.value().connection_id, session.value(),
                             net::CombatStreamId{stream.value()},
                             net::CombatTargetId{target.value()}, reference.value(), body.value(),
                             state.value().settings, state.value().pose, bound.value()});
    impl_->last_time = now;
    return CombatBinding{net::CombatStreamId{stream.value()}, net::CombatTargetId{target.value()},
                         bound.value()};
}

Result<void> CharacterCombat::submit(storage::CharacterAuthority& authority,
                                     net::TlsTransport& transport, const Scene& scene,
                                     const PhysicsWorld& physics, const net::CombatAction& action,
                                     MonotonicTime now) {
    constexpr auto operation = "server.combat.submit";
    if (auto valid = impl_->context(scene, physics, now, operation); !valid)
        return valid;
    if (auto valid = net::validate_combat_action(action); !valid)
        return valid;
    auto actor = std::find_if(impl_->actors.begin(), impl_->actors.end(),
                              [&](const auto& value) { return value.stream == action.stream; });
    if (actor == impl_->actors.end() || actor->health == 0 || actor->fenced || actor->pending ||
        actor->accepted == std::numeric_limits<std::uint64_t>::max() ||
        action.action_sequence != actor->accepted + 1 ||
        impl_->admission == std::numeric_limits<std::uint64_t>::max())
        return rejected(operation,
                        "Combat action requires its live stream, next identity and capacity");
    auto session = authority.authorize_entity(transport, scene, actor->entity.handle, now);
    if (!session)
        return session.error();
    auto peer = transport.authenticated_peer();
    if (!peer)
        return peer.error();
    if (peer.value().connection_id != actor->connection ||
        !same_session(actor->session, session.value()))
        return rejected(operation, "Combat action belongs to a different session");
    auto target = std::find_if(impl_->actors.begin(), impl_->actors.end(),
                               [&](const auto& value) { return value.target == action.target; });
    if (target == impl_->actors.end() || target == actor || target->health == 0 || target->fenced)
        return rejected(operation, "Combat target is stale or unavailable");
    if (auto body = impl_->live_body(*actor, scene, physics); !body)
        return body.error();
    if (auto body = impl_->live_body(*target, scene, physics); !body)
        return body.error();
    if (auto admitted = authority.apply(transport, net::HeartbeatRequest{action.authority}, now);
        !admitted)
        return admitted.error();
    actor->pending = action;
    actor->accepted = action.action_sequence;
    actor->pending_order = ++impl_->admission;
    impl_->last_time = now;
    return {};
}

Result<CombatTick> CharacterCombat::step(storage::CharacterAuthority& authority,
                                         CharacterMovement& movement, Scene& scene,
                                         PhysicsWorld& physics,
                                         std::span<net::TlsTransport* const> transports,
                                         MonotonicTime now) {
    constexpr auto operation = "server.combat.step";
    if (auto valid = impl_->context(scene, physics, now, operation); !valid)
        return valid.error();
    if (impl_->ticks >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) -
                            combat_maximum_cooldown)
        return rejected(operation, "Combat tick domain is exhausted");
    CombatTick report{{impl_->ticks + 1, 0, {}, {}}, {}, {}};
    report.outcomes.reserve(impl_->actors.size());
    report.rebindings.reserve(impl_->actors.size());
    auto moved = movement.step(authority, scene, physics, transports, now);
    if (!moved) {
        impl_->faulted = true;
        // Movement can fail while publishing an already completed native tick.
        // Keep that observed milestone and the original failure, without claiming
        // an absent movement receipt or replaying the uncertain work.
        if (auto completed = movement.stats(); completed)
            impl_->ticks = completed.value().completed_ticks;
        return moved.error();
    }
    report.movement = std::move(moved).value();
    impl_->ticks = report.movement.completed_tick;
    impl_->last_time = now;
    // All later errors report the original committed movement and effect prefix.
    impl_->faulted = true;
    const auto fail = [&](Error error) -> Result<CombatTick> {
        report.error = std::move(error);
        return std::move(report);
    };
    std::array<Impl::Actor*, movement_maximum_characters> pending{};
    std::size_t pending_count = 0;
    for (auto& actor : impl_->actors) {
        auto* transport = connection(transports, actor.connection);
        const auto session =
            transport ? authority.authorize_entity(*transport, scene, actor.entity.handle, now)
                      : Result<net::SessionSnapshot>{rejected(operation, "Connection ended")};
        actor.fenced = !session || !same_session(actor.session, session.value());
        if (actor.health != 0) {
            const auto found =
                std::find_if(report.movement.deliveries.begin(), report.movement.deliveries.end(),
                             [&](const auto& delivery) {
                                 return delivery.connection == actor.connection &&
                                        delivery.state.stream == actor.movement.stream;
                             });
            if (found == report.movement.deliveries.end() ||
                !impl_->live_body(actor, scene, physics))
                actor.fenced = true;
            else
                actor.movement = found->state;
        } else {
            auto state = scene.entity_state(actor.entity.handle);
            if (!state || state.value().reference != actor.entity || state.value().rigid_body)
                actor.fenced = true;
        }
        if (actor.pending)
            pending[pending_count++] = &actor;
        if (actor.fenced)
            ++report.fenced;
    }
    std::sort(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(pending_count),
              [](const auto* a, const auto* b) { return a->pending_order < b->pending_order; });
    for (std::size_t index = 0; index < pending_count; ++index) {
        auto& actor = *pending[index];
        const auto request = *actor.pending;
        net::CombatActionResult outcome{request.action_sequence, request.target, request.kind,
                                        CombatOutcome::actor_unavailable, 0};
        bool recorded = false;
        auto target =
            std::find_if(impl_->actors.begin(), impl_->actors.end(),
                         [&](const auto& value) { return value.target == request.target; });
        if (!actor.fenced && actor.health != 0) {
            if (target == impl_->actors.end() || target->fenced || target->health == 0) {
                outcome.outcome = CombatOutcome::target_unavailable;
            } else if (actor.ready_tick > impl_->ticks) {
                outcome.outcome = CombatOutcome::cooldown;
            } else {
                auto source_body = impl_->live_body(actor, scene, physics);
                auto target_body = impl_->live_body(*target, scene, physics);
                if (!source_body || !target_body)
                    return fail(!source_body ? source_body.error() : target_body.error());
                const auto from = source_body.value().pose.translation;
                const auto to = target_body.value().pose.translation;
                const auto distance = glm::length(to - from);
                const auto range = request.kind == CombatActionKind::strike
                                       ? combat_strike_range
                                       : combat_interaction_range;
                if (distance > range || distance == 0) {
                    outcome.outcome = CombatOutcome::out_of_range;
                } else {
                    PhysicsQueryFilter filter{actor.settings.collision_layer,
                                              actor.settings.collision_mask, false, *actor.body};
                    const auto ray = physics.raycast(scene, from, to, filter, 32);
                    if (!ray || ray.value().hits.empty() ||
                        ray.value().hits.front().body != *target->body) {
                        outcome.outcome = CombatOutcome::blocked;
                    } else {
                        auto decision = impl_->rules->evaluate(
                            {request.kind, impl_->ticks, actor.health, target->health, distance});
                        if (!decision) {
                            auto error = decision.error();
                            error.context.request_id = std::to_string(request.action_sequence);
                            error.context.entity_id =
                                std::visit([](const auto& id) { return id.value().to_string(); },
                                           actor.entity.identity);
                            return fail(std::move(error));
                        }
                        if (!decision.value().allowed) {
                            outcome.outcome = CombatOutcome::denied;
                        } else {
                            actor.ready_tick = impl_->ticks + decision.value().cooldown_ticks;
                            if (request.kind == CombatActionKind::interact) {
                                outcome.outcome = CombatOutcome::interacted;
                            } else {
                                outcome.outcome = CombatOutcome::hit;
                                outcome.amount = std::min(target->health, decision.value().damage);
                                target->health =
                                    static_cast<std::uint16_t>(target->health - outcome.amount);
                                if (target->health == 0) {
                                    target->respawn_tick = impl_->ticks + combat_respawn_ticks;
                                    // Damage/death are already committed even if subsequent
                                    // native teardown fails. Preserve that exact prefix.
                                    ++report.deaths;
                                    impl_->remember(actor, outcome);
                                    report.outcomes.push_back({actor.stream, outcome});
                                    actor.pending.reset();
                                    recorded = true;
                                    if (auto released = movement.release(scene, physics,
                                                                         target->movement.stream);
                                        !released)
                                        return fail(released.error());
                                    ++report.released_movements;
                                    if (auto removed = physics.remove_body(scene, *target->body);
                                        !removed)
                                        return fail(removed.error());
                                    ++report.removed_bodies;
                                    target->body.reset();
                                }
                            }
                        }
                    }
                }
            }
        }
        if (!recorded) {
            impl_->remember(actor, outcome);
            report.outcomes.push_back({actor.stream, outcome});
            actor.pending.reset();
        }
    }
    for (auto& actor : impl_->actors) {
        if (actor.fenced || actor.health != 0 || actor.respawn_tick > impl_->ticks)
            continue;
        auto* transport = connection(transports, actor.connection);
        if (!transport)
            continue;
        auto stream = generate_stable_id();
        auto target = generate_stable_id();
        if (!stream || !target)
            return fail(!stream ? stream.error() : target.error());
        auto body = physics.create_runtime_body(scene, actor.entity, actor.settings);
        if (!body)
            return fail(body.error());
        actor.body = body.value();
        ++report.created_bodies;
        if (auto placed = physics.teleport(scene, *actor.body, actor.spawn); !placed)
            return fail(placed.error());
        ++report.teleported_bodies;
        auto bound = movement.bind(authority, *transport, scene, physics, actor.entity.handle, now);
        if (!bound)
            return fail(bound.error());
        const auto previous = actor.target;
        actor.stream = net::CombatStreamId{stream.value()};
        actor.target = net::CombatTargetId{target.value()};
        actor.movement = bound.value();
        actor.health = combat_maximum_health;
        actor.ready_tick = 0;
        actor.respawn_tick = 0;
        actor.accepted = 0;
        actor.pending.reset();
        actor.outcome_count = 0;
        for (auto& retained : actor.outcomes)
            retained.reset();
        report.rebindings.push_back({previous, {actor.stream, actor.target, actor.movement}});
        ++report.respawns;
    }
    // Preserve body-removal exits after the already-owned native tick contacts.
    auto exits = physics.take_events(scene, 65536);
    if (!exits)
        return fail(exits.error());
    report.movement.contacts.insert(report.movement.contacts.end(), exits.value().begin(),
                                    exits.value().end());
    std::erase_if(impl_->actors, [](const auto& actor) { return actor.fenced; });
    impl_->faulted = false;
    return report;
}

Result<net::CombatState> CharacterCombat::view(storage::CharacterAuthority& authority,
                                               net::TlsTransport& transport, const Scene& scene,
                                               const PhysicsWorld& physics,
                                               std::span<const net::CombatTargetId> visible,
                                               MonotonicTime now) const {
    constexpr auto operation = "server.combat.view";
    if (auto valid = impl_->context(scene, physics, now, operation); !valid)
        return valid.error();
    if (visible.size() > net::combat_maximum_targets)
        return rejected(operation, "Combat visibility selection exceeds its bound");
    auto peer = transport.authenticated_peer();
    if (!peer)
        return peer.error();
    const auto actor =
        std::find_if(impl_->actors.begin(), impl_->actors.end(), [&](const auto& value) {
            return value.connection == peer.value().connection_id;
        });
    if (actor == impl_->actors.end())
        return rejected(operation, "Combat observer has no live binding");
    auto session = authority.authorize_entity(transport, scene, actor->entity.handle, now);
    if (!session)
        return session.error();
    if (!same_session(actor->session, session.value()))
        return rejected(operation, "Combat observer session was replaced");
    net::CombatState result{actor->session.session,
                            actor->session.ownership_epoch,
                            actor->stream,
                            impl_->ticks,
                            actor->target,
                            actor->health,
                            actor->ready_tick,
                            actor->respawn_tick,
                            {},
                            {},
                            {}};
    if (actor->health != 0) {
        if (auto body = impl_->live_body(*actor, scene, physics); !body)
            return body.error();
        result.movement = actor->movement;
    }
    result.targets.reserve(visible.size());
    result.outcomes.reserve(actor->outcome_count);
    for (const auto id : visible) {
        const auto target = std::find_if(impl_->actors.begin(), impl_->actors.end(),
                                         [&](const auto& value) { return value.target == id; });
        if (target == impl_->actors.end() || target == actor || target->fenced)
            return rejected(operation, "Combat visibility selection is stale or invalid");
        result.targets.push_back({target->target, target->health, target->respawn_tick});
    }
    std::sort(result.targets.begin(), result.targets.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    for (std::size_t index = 0; index < actor->outcome_count; ++index)
        result.outcomes.push_back(*actor->outcomes[index]);
    if (auto valid = net::validate_combat_state(result); !valid)
        return valid.error();
    return result;
}

Result<CombatStats> CharacterCombat::stats() const {
    if (impl_->owner != std::this_thread::get_id())
        return rejected("server.combat.stats", "Combat requires its creating thread");
    std::size_t pending = 0;
    for (const auto& actor : impl_->actors)
        if (actor.pending)
            ++pending;
    return CombatStats{impl_->ticks, impl_->actors.size(), pending, impl_->faulted};
}

} // namespace shark::server
