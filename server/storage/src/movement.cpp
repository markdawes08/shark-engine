#include "shark/server/movement.hpp"
#include "execution_cut_encoding.hpp"
#include "shark/world_traversal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <thread>

namespace shark::server {
namespace {
Error rejected(const char* operation, const char* message, ErrorCode code = ErrorCode::conflict) {
    return {code, message, {.operation = operation}};
}
math::Vec3 canonical(math::Vec3 value) {
    for (int axis = 0; axis < 3; ++axis)
        if (value[axis] == 0)
            value[axis] = 0;
    return value;
}
bool same_session(const net::SessionSnapshot& a, const net::SessionSnapshot& b) {
    return a.session == b.session && a.account == b.account && a.character == b.character &&
           a.ownership_epoch == b.ownership_epoch && b.active && b.ready;
}
bool character_body(const PhysicsBodyState& body) {
    return body.settings && body.geometry == PhysicsBodyGeometry::primitive &&
           body.motion == BodyMotion::dynamic_body &&
           body.settings->shape == ColliderShape::sphere && body.settings->radius == 0.5 &&
           body.settings->mass == 1 && body.settings->friction == 0 &&
           body.settings->restitution == 0 && !body.settings->is_trigger &&
           body.settings->collision_mask != 0 && body.pose.scale == math::Vec3{1};
}
} // namespace

Result<bool> movement_coverage(std::span<const MovementDomain> domains, math::Vec3 from,
                               math::Vec3 to) {
    constexpr auto operation = "server.movement.coverage";
    if (domains.size() > 8)
        return rejected(operation, "Movement coverage exceeds eight domains");
    for (const auto& domain : domains) {
        for (const auto value : {domain.min_x, domain.max_x, domain.min_z, domain.max_z})
            if (!std::isfinite(value) || std::abs(value) > physics_local_position_limit)
                return rejected(operation, "Invalid movement coverage bounds");
        if (domain.min_x >= domain.max_x || domain.min_z >= domain.max_z)
            return rejected(operation, "Empty movement coverage domain");
    }
    for (int axis = 0; axis < 3; ++axis)
        if (!std::isfinite(from[axis]) || !std::isfinite(to[axis]) ||
            std::abs(from[axis]) > physics_local_position_limit ||
            std::abs(to[axis]) > physics_local_position_limit)
            return rejected(operation, "Movement coverage requires bounded local positions");
    const auto inside = [&](const math::Vec3& point) {
        return domains.empty() || std::any_of(domains.begin(), domains.end(), [&](const auto& d) {
                   return point.x >= d.min_x && point.x < d.max_x && point.z >= d.min_z &&
                          point.z < d.max_z;
               });
    };
    if (!inside(from) || !inside(to))
        return false;
    std::array<double, 34> cuts{};
    std::size_t count = 2;
    cuts[1] = 1;
    const auto delta = to - from;
    for (const auto& domain : domains)
        for (const auto& axis : {std::array{from.x, delta.x, domain.min_x, domain.max_x},
                                 std::array{from.z, delta.z, domain.min_z, domain.max_z}}) {
            if (axis[1] == 0)
                continue;
            for (const auto edge : {axis[2], axis[3]}) {
                const auto at = (edge - axis[0]) / axis[1];
                if (at > 0 && at < 1)
                    cuts[count++] = at;
            }
        }
    std::sort(cuts.begin(), cuts.begin() + static_cast<std::ptrdiff_t>(count));
    for (std::size_t i = 1; i < count; ++i) {
        if (!inside(from + delta * cuts[i]))
            return false;
        if (cuts[i] > cuts[i - 1] && !inside(from + delta * ((cuts[i] + cuts[i - 1]) * .5)))
            return false;
    }
    return true;
}

struct CharacterMovement::Impl {
    struct Actor {
        std::uint64_t connection;
        net::SessionSnapshot session;
        net::MovementStreamId stream;
        EntityReference entity;
        PhysicsBodyHandle body;
        std::array<std::optional<net::MovementInput>, movement_pending_inputs> queue{};
        std::size_t head{0};
        std::size_t count{0};
        std::uint64_t accepted{0};
        std::uint64_t applied{0};
        std::optional<MovementDomain> domain{};
    };
    std::thread::id owner{std::this_thread::get_id()};
    std::uint64_t scene_instance;
    std::uint64_t physics_world;
    WorldPosition origin;
    std::optional<std::uint64_t> traversal_instance;
    std::array<MovementDomain, 8> domains{};
    std::size_t domain_count{0};
    std::uint64_t ticks;
    MonotonicTime last_time{};
    bool faulted{false};
    std::vector<Actor> actors;
    std::shared_ptr<ExecutionFence> fence;
    std::optional<ExecutionPermit> execution;

    Impl(std::uint64_t scene, std::uint64_t world, WorldPosition anchor, std::uint64_t completed,
         std::optional<std::uint64_t> traversal, std::span<const MovementDomain> coverage,
         std::shared_ptr<ExecutionFence> execution_fence, std::optional<ExecutionPermit> permit)
        : scene_instance(scene), physics_world(world), origin(anchor),
          traversal_instance(traversal), ticks(completed), fence(std::move(execution_fence)),
          execution(permit) {
        actors.reserve(movement_maximum_characters);
        domain_count = coverage.size();
        std::copy(coverage.begin(), coverage.end(), domains.begin());
    }
    Result<void> executing(bool allow_frozen = false) const {
        if (fence)
            return fence->check(*execution, allow_frozen);
        return {};
    }
    Result<void> check(const char* operation, bool allow_frozen = false) const {
        if (owner != std::this_thread::get_id())
            return rejected(operation, "Movement requires its creating thread");
        if (faulted)
            return rejected(operation, "Movement tick has uncertain native effects; replace owner");
        return executing(allow_frozen);
    }
    Result<void> context(const Scene& scene, const PhysicsWorld& physics, MonotonicTime now,
                         const char* operation, bool allow_frozen = false) const {
        if (auto checked = check(operation, allow_frozen); !checked)
            return checked;
        if (now < MonotonicTime::zero() || now < last_time)
            return rejected(operation, "Movement clock cannot regress");
        auto state = scene.stats();
        if (!state)
            return state.error();
        auto native = physics.stats();
        if (!native)
            return native.error();
        if (state.value().closed || state.value().instance != scene_instance ||
            state.value().pending_mutations != 0 || native.value().closed ||
            native.value().faulted || !native.value().simulating ||
            native.value().completed_ticks != ticks ||
            native.value().config.tick_hz != movement_tick_hz ||
            native.value().config.collision_substeps != movement_collision_substeps)
            return rejected(operation, "Movement requires its original live fixed-step owners");
        auto poses = physics.render_poses(scene, 0);
        if (!poses)
            return poses.error();
        if (poses.value().world != physics_world)
            return rejected(operation, "Movement cannot adopt a replacement physics owner");
        return {};
    }
    Result<PhysicsBodyState> body(const Actor& actor, const Scene& scene,
                                  const PhysicsWorld& physics) const {
        auto entity = scene.resolve(actor.entity);
        if (!entity)
            return entity.error();
        auto state = physics.body_state(scene, actor.body);
        if (!state)
            return state.error();
        if (actor.body.world != physics_world || state.value().entity != actor.entity ||
            !character_body(state.value()))
            return rejected("server.movement.body", "Character body profile or identity changed");
        return state;
    }
    Result<net::MovementState> snapshot(const Actor& actor, const Scene& scene,
                                        const PhysicsWorld& physics) const {
        auto state = body(actor, scene, physics);
        if (!state)
            return state.error();
        auto velocity = physics.velocity(scene, actor.body);
        if (!velocity)
            return velocity.error();
        net::MovementState result{actor.session.session,
                                  actor.session.ownership_epoch,
                                  actor.stream,
                                  ticks,
                                  actor.applied,
                                  canonical(state.value().pose.translation),
                                  canonical(velocity.value().linear)};
        if (auto valid = net::validate_movement_state(result); !valid)
            return valid.error();
        if (auto global = translate_world_position(
                origin, {result.position.x, result.position.y, result.position.z});
            !global)
            return global.error();
        return result;
    }
};

CharacterMovement::CharacterMovement(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
CharacterMovement::~CharacterMovement() = default;

Result<std::unique_ptr<CharacterMovement>>
CharacterMovement::create(const Scene& scene, const PhysicsWorld& physics, WorldPosition origin,
                          std::optional<std::uint64_t> traversal_instance,
                          std::span<const MovementDomain> domains,
                          std::shared_ptr<ExecutionFence> fence) {
    constexpr auto operation = "server.movement.create";
    if (auto valid = validate_world_position(origin); !valid)
        return valid.error();
    std::optional<ExecutionPermit> execution;
    if (fence) {
        auto current = fence->permit();
        if (!current)
            return current.error();
        if (auto checked = fence->check(current.value()); !checked)
            return checked.error();
        execution = current.value();
    }
    if (traversal_instance && *traversal_instance == 0)
        return rejected(operation, "Movement traversal instance must be nonzero",
                        ErrorCode::invalid_configuration);
    if (domains.size() > 8)
        return rejected(operation, "Movement coverage exceeds its bounded region profile",
                        ErrorCode::invalid_configuration);
    for (const auto& domain : domains)
        if (!std::isfinite(domain.min_x) || !std::isfinite(domain.max_x) ||
            !std::isfinite(domain.min_z) || !std::isfinite(domain.max_z) ||
            domain.min_x >= domain.max_x || domain.min_z >= domain.max_z ||
            domain.min_x < -physics_local_position_limit ||
            domain.max_x > physics_local_position_limit ||
            domain.min_z < -physics_local_position_limit ||
            domain.max_z > physics_local_position_limit)
            return rejected(operation, "Movement coverage is invalid in this local frame",
                            ErrorCode::invalid_configuration);
    auto scene_state = scene.stats();
    if (!scene_state)
        return scene_state.error();
    // This leased read proves both the exact native owner and Region Scene.
    auto poses = physics.render_poses(scene, 0);
    if (!poses)
        return poses.error();
    auto stats = physics.stats();
    if (!stats)
        return stats.error();
    auto page = scene.query_entities();
    if (!page)
        return page.error();
    const auto* region = std::get_if<RegionSceneScope>(&page.value().scope());
    if (!region || region->world != origin.world ||
        stats.value().config.tick_hz != movement_tick_hz ||
        stats.value().config.collision_substeps != movement_collision_substeps ||
        stats.value().pending_events != 0 || scene_state.value().pending_mutations != 0)
        return rejected(operation,
                        "Movement requires its Region, origin and 20 Hz/3-substep profile",
                        ErrorCode::invalid_configuration);
    return std::unique_ptr<CharacterMovement>{new CharacterMovement{std::make_unique<Impl>(
        scene_state.value().instance, poses.value().world, origin, stats.value().completed_ticks,
        traversal_instance, domains, std::move(fence), execution)}};
}

Result<void> CharacterMovement::check_traversal(const WorldTraversal* traversal) const {
    constexpr auto operation = "server.movement.traversal";
    if (auto valid = impl_->check(operation); !valid)
        return valid;
    if (impl_->traversal_instance) {
        if (!traversal || traversal->instance() != *impl_->traversal_instance ||
            traversal->frame().origin != impl_->origin)
            return rejected(operation, "Movement requires its declared traversal owner/origin");
    } else if (traversal) {
        return rejected(operation, "Movement cannot opt into traversal after creation");
    }
    return {};
}

Result<net::MovementState> CharacterMovement::bind(storage::CharacterAuthority& authority,
                                                   net::TlsTransport& transport, const Scene& scene,
                                                   const PhysicsWorld& physics, EntityHandle entity,
                                                   MonotonicTime now) {
    constexpr auto operation = "server.movement.bind";
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
            return actor.entity.handle == entity ||
                   actor.connection == peer.value().connection_id ||
                   actor.session.account == session.value().account;
        }))
        return rejected(operation, "Character is already bound or movement capacity is full");
    auto reference = scene.reference(entity);
    if (!reference)
        return reference.error();
    auto body = physics.find_body(scene, reference.value());
    if (!body)
        return body.error();
    auto stream = generate_stable_id();
    if (!stream)
        return stream.error();
    Impl::Actor candidate{peer.value().connection_id, session.value(),
                          net::MovementStreamId{stream.value()}, reference.value(), body.value()};
    auto state = impl_->snapshot(candidate, scene, physics);
    if (!state)
        return state.error();
    impl_->actors.push_back(std::move(candidate));
    impl_->last_time = now;
    return state;
}

Result<void> CharacterMovement::submit(storage::CharacterAuthority& authority,
                                       net::TlsTransport& transport, const Scene& scene,
                                       const PhysicsWorld& physics, const net::MovementInput& input,
                                       MonotonicTime now) {
    constexpr auto operation = "server.movement.submit";
    if (auto checked = impl_->context(scene, physics, now, operation); !checked)
        return checked;
    if (auto valid = net::validate_movement_input(input); !valid)
        return valid;
    const auto found =
        std::find_if(impl_->actors.begin(), impl_->actors.end(),
                     [&](const auto& actor) { return actor.stream == input.stream; });
    if (found == impl_->actors.end() || found->count == movement_pending_inputs ||
        found->accepted == std::numeric_limits<std::uint64_t>::max() ||
        input.input_sequence != found->accepted + 1)
        return rejected(operation, "Unknown stream, nonconsecutive input or full movement queue");
    auto session = authority.authorize_entity(transport, scene, found->entity.handle, now);
    if (!session)
        return session.error();
    auto peer = transport.authenticated_peer();
    if (!peer)
        return peer.error();
    if (peer.value().connection_id != found->connection ||
        !same_session(found->session, session.value()))
        return rejected(operation, "Movement belongs to a different live character session");
    if (auto valid = impl_->body(*found, scene, physics); !valid)
        return valid.error();
    // Existing Heartbeat admission checks the exact envelope and shares its rate
    // budget with readiness, replication and durable operations. No storage IO.
    if (auto valid = impl_->executing(); !valid)
        return valid;
    auto admitted = authority.apply(transport, net::HeartbeatRequest{input.authority}, now);
    if (!admitted)
        return admitted.error();
    found->queue[(found->head + found->count) % movement_pending_inputs] = input;
    ++found->count;
    found->accepted = input.input_sequence;
    impl_->last_time = now;
    return {};
}

Result<MovementTick> CharacterMovement::step(storage::CharacterAuthority& authority, Scene& scene,
                                             PhysicsWorld& physics,
                                             std::span<net::TlsTransport* const> transports,
                                             MonotonicTime now, const WorldTraversal* traversal) {
    constexpr auto operation = "server.movement.step";
    if (auto valid = check_traversal(traversal); !valid)
        return valid.error();
    if (traversal) {
        if (auto valid = traversal->check_context(scene, physics); !valid)
            return valid.error();
    }
    if (auto valid = impl_->context(scene, physics, now, operation); !valid)
        return valid.error();
    if (transports.size() > movement_maximum_characters ||
        impl_->ticks == std::numeric_limits<std::uint64_t>::max())
        return rejected(operation, "Movement connection or tick bound exceeded");
    const auto native = physics.stats();
    if (!native)
        return native.error();
    if (native.value().pending_events != 0)
        return rejected(operation, "Dispatch existing contact events before another movement tick");
    // Borrowing is scoped to this call; connection identities are never reused.
    std::array<std::uint64_t, movement_maximum_characters> connections{};
    for (std::size_t index = 0; index < transports.size(); ++index) {
        if (!transports[index])
            return rejected(operation, "Null transport in movement tick");
        auto peer = transports[index]->authenticated_peer();
        if (peer)
            connections[index] = peer.value().connection_id;
        if (connections[index] != 0 &&
            std::find(connections.begin(), connections.begin() + static_cast<std::ptrdiff_t>(index),
                      connections[index]) !=
                connections.begin() + static_cast<std::ptrdiff_t>(index))
            return rejected(operation, "Duplicate transport in movement tick");
    }
    struct Plan {
        bool live;
        bool body_valid;
        PhysicsBodyVelocity velocity;
    };
    std::array<Plan, movement_maximum_characters> plans{};
    MovementTick result{impl_->ticks + 1, 0, {}, {}};
    result.deliveries.reserve(impl_->actors.size());
    for (std::size_t index = 0; index < impl_->actors.size(); ++index) {
        const auto& actor = impl_->actors[index];
        auto body = impl_->body(actor, scene, physics);
        auto& plan = plans[index];
        plan.body_valid = static_cast<bool>(body);
        const auto connection = std::find(
            connections.begin(),
            connections.begin() + static_cast<std::ptrdiff_t>(transports.size()), actor.connection);
        if (body &&
            connection != connections.begin() + static_cast<std::ptrdiff_t>(transports.size())) {
            const auto offset = static_cast<std::size_t>(connection - connections.begin());
            auto session =
                authority.authorize_entity(*transports[offset], scene, actor.entity.handle, now);
            plan.live = session && same_session(actor.session, session.value());
        }
        if (body) {
            auto velocity = physics.velocity(scene, actor.body);
            if (!velocity)
                return velocity.error();
            plan.velocity.linear.y =
                std::clamp(velocity.value().linear.y, -movement_maximum_vertical_speed,
                           movement_maximum_vertical_speed);
            if (plan.live && actor.count != 0) {
                const auto& input = *actor.queue[actor.head];
                plan.velocity.linear.x = movement_speed * input.x / net::movement_axis_scale;
                plan.velocity.linear.z = movement_speed * input.z / net::movement_axis_scale;
            }
            if (plan.live && impl_->domain_count != 0) {
                const auto intended = body.value().pose.translation +
                                      plan.velocity.linear / static_cast<double>(movement_tick_hz);
                const auto covered = movement_coverage(
                    std::span<const MovementDomain>{impl_->domains}.first(impl_->domain_count),
                    body.value().pose.translation, intended);
                if (!covered)
                    return covered.error();
                if (!covered.value()) {
                    plan.velocity.linear.x = 0;
                    plan.velocity.linear.z = 0;
                }
            }
            if (plan.live && actor.domain) {
                const auto intended = body.value().pose.translation +
                                      plan.velocity.linear / static_cast<double>(movement_tick_hz);
                const auto covered =
                    movement_coverage(std::span<const MovementDomain>{&*actor.domain, 1},
                                      body.value().pose.translation, intended);
                if (!covered)
                    return covered.error();
                if (!covered.value()) {
                    plan.velocity.linear.x = 0;
                    plan.velocity.linear.z = 0;
                }
            }
            if (plan.live && traversal) {
                const auto from =
                    translate_world_position(impl_->origin, {body.value().pose.translation.x,
                                                             body.value().pose.translation.y,
                                                             body.value().pose.translation.z});
                if (!from)
                    return from.error();
                const auto intended = body.value().pose.translation +
                                      plan.velocity.linear / static_cast<double>(movement_tick_hz);
                const auto to =
                    translate_world_position(impl_->origin, {intended.x, intended.y, intended.z});
                if (!to)
                    return to.error();
                const auto allowed = traversal->allows_motion(from.value(), to.value(), 0.5);
                if (!allowed)
                    return allowed.error();
                if (!allowed.value()) {
                    plan.velocity.linear.x = 0.0;
                    plan.velocity.linear.z = 0.0;
                }
            }
        }
        if (!plan.live)
            ++result.fenced;
    }
    // From this point errors can retain immediate native effects. Fault first,
    // then clear only after the complete tick and publication values succeed.
    impl_->faulted = true;
    for (std::size_t index = 0; index < impl_->actors.size(); ++index) {
        if (plans[index].body_valid) {
            if (auto valid = impl_->executing(); !valid)
                return valid.error();
            auto changed =
                physics.set_velocity(scene, impl_->actors[index].body, plans[index].velocity);
            if (!changed)
                return changed.error();
        }
    }
    if (auto valid = impl_->executing(); !valid)
        return valid.error();
    if (auto stepped = physics.step(scene); !stepped)
        return stepped.error();
    ++impl_->ticks;
    for (std::size_t index = 0; index < impl_->actors.size(); ++index) {
        auto& actor = impl_->actors[index];
        if (!plans[index].live)
            continue;
        if (actor.count != 0) {
            actor.applied = actor.queue[actor.head]->input_sequence;
            actor.queue[actor.head].reset();
            actor.head = (actor.head + 1) % movement_pending_inputs;
            --actor.count;
        }
        auto state = impl_->snapshot(actor, scene, physics);
        if (!state)
            return state.error();
        result.deliveries.push_back({actor.connection, state.value()});
    }
    auto events = physics.take_events(scene, 65536);
    if (!events)
        return events.error();
    result.contacts = std::move(events).value();
    for (std::size_t index = impl_->actors.size(); index > 0; --index)
        if (!plans[index - 1].live)
            impl_->actors.erase(impl_->actors.begin() + static_cast<std::ptrdiff_t>(index - 1));
    impl_->last_time = now;
    impl_->faulted = false;
    return result;
}

Result<MovementStats> CharacterMovement::stats() const {
    if (impl_->owner != std::this_thread::get_id())
        return rejected("server.movement.stats", "Movement requires its creating thread");
    std::size_t pending = 0;
    for (const auto& actor : impl_->actors)
        pending += actor.count;
    return MovementStats{impl_->ticks, impl_->actors.size(), pending, impl_->faulted};
}

Result<net::MovementState> CharacterMovement::observe(const Scene& scene,
                                                      const PhysicsWorld& physics,
                                                      net::MovementStreamId stream) const {
    if (auto valid = impl_->context(scene, physics, impl_->last_time, "server.movement.observe");
        !valid)
        return valid.error();
    const auto found = std::find_if(impl_->actors.begin(), impl_->actors.end(),
                                    [&](const auto& actor) { return actor.stream == stream; });
    if (found == impl_->actors.end())
        return rejected("server.movement.observe", "Unknown movement stream");
    return impl_->snapshot(*found, scene, physics);
}

Result<void> CharacterMovement::release(const Scene& scene, PhysicsWorld& physics,
                                        net::MovementStreamId stream) {
    if (auto valid = impl_->context(scene, physics, impl_->last_time, "server.movement.release");
        !valid)
        return valid;
    const auto found = std::find_if(impl_->actors.begin(), impl_->actors.end(),
                                    [&](const auto& actor) { return actor.stream == stream; });
    if (found == impl_->actors.end())
        return rejected("server.movement.release", "Unknown movement stream");
    if (auto valid = impl_->body(*found, scene, physics); !valid)
        return valid.error();
    if (auto valid = impl_->executing(); !valid)
        return valid;
    if (auto stopped = physics.set_velocity(scene, found->body, {}); !stopped) {
        impl_->faulted = true;
        return stopped.error();
    }
    impl_->actors.erase(found);
    return {};
}

Result<WorldPosition> CharacterMovement::position(const Scene& scene, const PhysicsWorld& physics,
                                                  net::MovementStreamId stream) const {
    if (auto valid = impl_->context(scene, physics, impl_->last_time, "server.movement.position");
        !valid)
        return valid.error();
    const auto found = std::find_if(impl_->actors.begin(), impl_->actors.end(),
                                    [&](const auto& actor) { return actor.stream == stream; });
    if (found == impl_->actors.end())
        return rejected("server.movement.position", "Unknown movement stream");
    auto state = impl_->snapshot(*found, scene, physics);
    if (!state)
        return state.error();
    return translate_world_position(
        impl_->origin,
        {state.value().position.x, state.value().position.y, state.value().position.z});
}

Result<void> CharacterMovement::adopt_execution(ExecutionPermit permit) {
    constexpr auto operation = "server.movement.execution.adopt";
    if (impl_->owner != std::this_thread::get_id() || impl_->faulted || !impl_->fence)
        return rejected(operation, "Execution adoption requires its healthy fenced owner");
    if (auto valid = impl_->fence->check(permit, true); !valid)
        return valid;
    impl_->execution = permit;
    return {};
}

Result<void> CharacterMovement::set_actor_domain(net::MovementStreamId stream,
                                                 std::optional<MovementDomain> domain) {
    constexpr auto operation = "server.movement.domain";
    if (auto valid = impl_->check(operation); !valid)
        return valid;
    if (domain) {
        // Validation has fixed storage and no native or session effects.
        auto valid = movement_coverage(std::span<const MovementDomain>{&*domain, 1}, {}, {});
        if (!valid)
            return valid.error();
    }
    const auto actor = std::find_if(impl_->actors.begin(), impl_->actors.end(),
                                    [&](const auto& value) { return value.stream == stream; });
    if (actor == impl_->actors.end())
        return rejected(operation, "Unknown movement stream for regional domain");
    actor->domain = domain;
    return {};
}

Result<MovementCut> CharacterMovement::capture_cut(const Scene& scene,
                                                   const PhysicsWorld& physics) const {
    constexpr auto operation = "server.movement.cut";
    if (auto valid = impl_->context(scene, physics, impl_->last_time, operation, true); !valid)
        return valid.error();
    auto scene_state = scene.stats();
    if (!scene_state)
        return scene_state.error();
    auto page = scene.query_entities();
    if (!page)
        return page.error();
    auto native = physics.stats();
    if (!native)
        return native.error();
    if (native.value().pending_events != 0)
        return rejected(operation, "Movement cut requires drained native contact events");
    try {
        MovementCut cut{{scene_state.value().instance, scene_state.value().revision},
                        page.value().scope(),
                        impl_->physics_world,
                        impl_->origin,
                        impl_->traversal_instance,
                        {},
                        impl_->ticks,
                        impl_->last_time,
                        impl_->execution,
                        {},
                        {}};
        cut.domains.assign(impl_->domains.begin(),
                           impl_->domains.begin() +
                               static_cast<std::ptrdiff_t>(impl_->domain_count));
        cut.actors.reserve(impl_->actors.size());
        for (const auto& actor : impl_->actors) {
            auto body = impl_->body(actor, scene, physics);
            if (!body)
                return body.error();
            auto velocity = physics.velocity(scene, actor.body);
            if (!velocity)
                return velocity.error();
            MovementActorCut copied{actor.connection,
                                    actor.session,
                                    actor.stream,
                                    actor.entity,
                                    actor.body,
                                    body.value(),
                                    velocity.value(),
                                    actor.accepted,
                                    actor.applied,
                                    actor.domain,
                                    {}};
            copied.pending.reserve(actor.count);
            for (std::size_t index = 0; index < actor.count; ++index)
                copied.pending.push_back(
                    *actor.queue[(actor.head + index) % movement_pending_inputs]);
            cut.actors.push_back(std::move(copied));
        }
        detail::CutEncoder encoded;
        encoded.value(cut);
        cut.bytes = std::move(encoded.bytes);
        return cut;
    } catch (...) {
        return rejected(operation, "Cannot allocate bounded movement cut",
                        ErrorCode::execution_failed);
    }
}

} // namespace shark::server
