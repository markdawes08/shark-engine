#include "shark/server/region_executor.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <thread>

namespace shark::server {
namespace {
struct Failure {
    Error error;
};
[[noreturn]] void fail(const char* text) {
    throw Failure{{ErrorCode::conflict, text, {}}};
}
template <class T> T need(Result<T> value) {
    if (!value)
        throw Failure{std::move(value).error()};
    return std::move(value).value();
}
void need(Result<void> value) {
    if (!value)
        throw Failure{std::move(value).error()};
}
template <class T, class F> Result<T> boundary(const char* operation, F&& run) {
    try {
        if constexpr (std::is_void_v<T>) {
            run();
            return {};
        } else
            return run();
    } catch (Failure& error) {
        error.error.context.operation = operation;
        return std::move(error.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed, "Region executor operation failed", {operation}};
    }
}
WorldEntityId identity(const EntityReference& reference) {
    const auto* id = std::get_if<WorldEntityId>(&reference.identity);
    if (!id)
        fail("Region executor requires world entity identities");
    return *id;
}
bool npc_intent(const std::optional<math::Vec3>& value) {
    return !value || (std::isfinite(value->x) && std::isfinite(value->y) &&
                      std::isfinite(value->z) && value->y == 0 &&
                      value->x * value->x + value->z * value->z <= movement_speed * movement_speed);
}
} // namespace

struct RegionExecutor::Impl {
    struct Npc {
        EntityReference entity;
        PhysicsBodyHandle body;
        RigidBody settings;
        std::optional<math::Vec3> pending;
    };
    const std::thread::id owner{std::this_thread::get_id()};
    RegionExecutorPhase phase{RegionExecutorPhase::staged};
    WorldLocalFrame frame;
    RegionInstanceId instance;
    std::vector<RegionDefinition> regions;
    std::vector<MovementDomain> domains;
    std::shared_ptr<const std::vector<RegionImageCell>> cells;
    std::vector<RegionImageAsset> assets;
    std::shared_ptr<const PreparedAssets> prepared;
    std::unique_ptr<Scene> scene;
    std::unique_ptr<PhysicsWorld> physics;
    std::unique_ptr<WorldCellResidency> residency;
    std::unique_ptr<WorldTraversal> traversal;
    std::shared_ptr<ExecutionFence> fence;
    ExecutionPermit permit;
    std::unique_ptr<CharacterMovement> movement;
    std::unique_ptr<CharacterCombat> combat;
    std::vector<GameplayTransferBinding> bindings;
    std::vector<Npc> npcs;
    std::vector<RegionPeer> peers;
    std::vector<storage::RegionWriter> writers;
    std::uint64_t watermark{0};
    MonotonicTime last_time{};
    Impl(WorldLocalFrame f, RegionInstanceId i) : frame(f), instance(i) {}
    ~Impl() {
        combat.reset();
        movement.reset();
        if (traversal) {
            if (!traversal->close()) {
                if (physics)
                    static_cast<void>(physics->close());
                if (!traversal->close())
                    std::terminate();
            }
            traversal.reset();
        }
        if (physics) {
            static_cast<void>(physics->close());
            physics.reset();
        }
        if (residency) {
            static_cast<void>(residency->close());
            residency.reset();
        }
        if (scene)
            static_cast<void>(scene->close());
    }
    void check(bool executing = false) const {
        if (owner != std::this_thread::get_id())
            fail("Region executor requires its creating thread");
        if (phase == RegionExecutorPhase::faulted)
            fail("Region executor has uncertain native effects");
        need(fence->check(permit, !executing));
        if (executing && phase != RegionExecutorPhase::active)
            fail("Region executor is not active");
        need(traversal->check_context(*scene, *physics));
    }
    GameplayTransferBinding& map(const EntityReference& source) {
        const auto found = std::find_if(bindings.begin(), bindings.end(),
                                        [&](const auto& m) { return m.source == source; });
        if (found == bindings.end())
            fail("Region executor source entity is not mapped");
        return *found;
    }
    void clock(MonotonicTime now, bool executing) {
        check(executing);
        if (now < MonotonicTime::zero() || now < last_time)
            fail("Executor clock cannot regress");
        last_time = now;
    }
    MovementDomain corridor(RegionId id) const {
        const auto region =
            std::find_if(regions.begin(), regions.end(), [&](const auto& r) { return r.id == id; });
        if (region == regions.end() || !std::isfinite(region->interest_overlap) ||
            region->interest_overlap < 0)
            fail("Executor writer region or overlap is invalid");
        return {std::max(-physics_local_position_limit, region->min_x - region->interest_overlap),
                std::min(physics_local_position_limit, region->max_x + region->interest_overlap),
                std::max(-physics_local_position_limit, region->min_z - region->interest_overlap),
                std::min(physics_local_position_limit, region->max_z + region->interest_overlap)};
    }
    PhysicsBodyState npc_body(const Npc& npc) const {
        need(scene->resolve(npc.entity));
        const auto body = need(physics->body_state(*scene, npc.body));
        if (body.entity != npc.entity || !body.settings || *body.settings != npc.settings ||
            body.motion != BodyMotion::dynamic_body ||
            body.geometry != PhysicsBodyGeometry::primitive)
            fail("Region NPC native identity or settings changed");
        return body;
    }
    static double radius(const PhysicsBodyState& body) {
        const auto value = body.settings && body.settings->shape == ColliderShape::sphere
                               ? body.collider_half_extents.x
                               : glm::length(body.collider_half_extents);
        if (!std::isfinite(value) || value <= 0 || value > physics_maximum_dimension)
            fail("Region NPC footprint exceeds traversal bounds");
        return value;
    }
    void add_npc(const EntityReference& entity, PhysicsBodyHandle body,
                 std::optional<math::Vec3> pending) {
        if (!npc_intent(pending) || npcs.size() >= region_world_maximum_npcs ||
            std::any_of(npcs.begin(), npcs.end(),
                        [&](const auto& n) { return n.entity == entity || n.body == body; }))
            fail("Executor NPC intent or native identity is invalid or duplicated");
        const auto native = need(physics->body_state(*scene, body));
        if (!native.settings)
            fail("Executor NPC requires a primitive");
        Npc candidate{entity, body, *native.settings, pending};
        static_cast<void>(npc_body(candidate));
        const auto p = native.pose.translation;
        const auto global = need(translate_world_position(frame.origin, {p.x, p.y, p.z}));
        if (!need(movement_coverage(domains, p, p)) ||
            !need(traversal->allows_motion(global, global, radius(native))))
            fail("Executor NPC footprint is outside ready regional coverage");
        npcs.push_back(std::move(candidate));
    }
    static std::unique_ptr<Impl> build(const RegionProcessImage& image, RegionInstanceId instance,
                                       bool restore) {
        if (image.frame.generation == 0 || image.regions.size() < 2 || image.regions.size() > 8 ||
            image.entities.size() > 4096 || image.cells.size() > 8 || image.npcs.size() > 16 ||
            image.peers.size() > 32 || image.writers.size() > 32)
            fail("Region executor image exceeds its bounded profile");
        need(validate_world_position(image.frame.origin));
        for (std::size_t i = 0; i < image.npcs.size(); ++i) {
            const auto& npc = image.npcs[i];
            if (!npc_intent(npc.pending))
                fail("Executor image has an invalid pending NPC intent");
            const auto id = identity(npc.source);
            if (std::any_of(image.peers.begin(), image.peers.end(),
                            [&](const auto& peer) { return peer.entity == id; }))
                fail("Executor NPC cannot reuse a player identity");
            for (std::size_t j = 0; j < i; ++j)
                if (identity(image.npcs[j].source) == id || image.npcs[j].body == npc.body)
                    fail("Executor image duplicates an NPC identity or body");
            if (restore && std::none_of(image.physics.bodies.begin(), image.physics.bodies.end(),
                                        [&](const auto& body) {
                                            return body.source == npc.body &&
                                                   body.native.entity == npc.source && !body.mesh &&
                                                   body.native.motion == BodyMotion::dynamic_body;
                                        }))
                fail("Executor NPC does not identify its exact captured dynamic primitive");
        }
        auto result = std::make_unique<Impl>(image.frame, instance);
        result->regions = image.regions;
        for (const auto& region : image.regions)
            result->domains.push_back({region.min_x, region.max_x, region.min_z, region.max_z});
        need(movement_coverage(result->domains, {}, {}));
        result->assets = image.assets;
        std::vector<PreparedAssetInput> inputs;
        for (const auto& asset : result->assets)
            inputs.push_back({asset.asset, asset.payload});
        result->prepared = need(PreparedAssets::create(inputs));
        result->cells = std::make_shared<const std::vector<RegionImageCell>>(image.cells);
        result->scene = need(Scene::create(RegionSceneScope{
            image.frame.origin.world, {image.physics.scope.authority.region, instance, 1}}));
        result->physics = need(PhysicsWorld::create(image.physics.config));
        const auto mesh_proxy = [&](WorldEntityId id) {
            if (std::any_of(image.cells.begin(), image.cells.end(),
                            [&](const auto& c) { return c.proxy == id; }))
                return true;
            return restore && std::any_of(image.physics.bodies.begin(), image.physics.bodies.end(),
                                          [&](const auto& b) {
                                              return b.mesh && identity(b.native.entity) == id;
                                          });
        };
        std::vector<bool> created(image.entities.size(), false);
        for (std::size_t pass = 0; pass <= image.entities.size(); ++pass) {
            bool progress = false, remaining = false;
            for (std::size_t i = 0; i < image.entities.size(); ++i) {
                if (created[i])
                    continue;
                const auto& saved = image.entities[i];
                const auto id = identity(saved.source);
                if (mesh_proxy(id)) {
                    created[i] = true;
                    continue;
                }
                std::optional<EntityHandle> parent;
                if (saved.parent) {
                    const auto found = result->scene->find(*saved.parent);
                    if (!found) {
                        remaining = true;
                        continue;
                    }
                    parent = found.value();
                }
                EntitySeed seed{id};
                seed.name = saved.name;
                seed.transform = saved.transform;
                seed.parent = parent;
                seed.camera = saved.camera;
                seed.mesh_renderer = saved.mesh_renderer;
                seed.rigid_body = saved.rigid_body;
                if (seed.mesh_renderer)
                    need(result->prepared->resolve(*seed.mesh_renderer));
                const auto entity = need(result->scene->create_entity(std::move(seed)));
                result->bindings.push_back(
                    {saved.source, need(result->scene->reference(entity)), {}, {}});
                created[i] = true;
                progress = true;
            }
            if (!remaining)
                break;
            if (!progress)
                fail("Region executor image hierarchy is unresolved");
        }
        if (restore) {
            for (const auto& body : image.physics.bodies)
                if (!body.mesh) {
                    auto& mapped = result->map(body.native.entity);
                    mapped.source_body = body.source;
                    mapped.destination_body = need(
                        result->physics->create_body(*result->scene, mapped.destination.handle));
                }
        } else {
            for (auto& mapped : result->bindings) {
                const auto state = need(result->scene->entity_state(mapped.destination.handle));
                if (state.rigid_body)
                    mapped.destination_body = need(
                        result->physics->create_body(*result->scene, mapped.destination.handle));
            }
        }
        need(result->physics->begin_simulation(*result->scene));
        const auto cells = result->cells;
        result->residency = need(
            WorldCellResidency::create([cells](const WorldCellReadRequest& request,
                                               std::stop_token) -> Result<std::vector<std::byte>> {
                for (const auto& cell : *cells)
                    if (cell.key == request.key &&
                        cell.content_revision == request.content_revision)
                        return cell.payload;
                return Error{ErrorCode::conflict,
                             "Requested cell is absent from immutable executor image",
                             {}};
            }));
        result->traversal = need(WorldTraversal::create(*result->residency, *result->physics,
                                                        *result->scene, image.frame));
        const auto mesh_settings = [&](WorldEntityId proxy) {
            PhysicsStaticMeshSettings settings;
            if (restore)
                for (const auto& body : image.physics.bodies)
                    if (body.mesh && identity(body.native.entity) == proxy)
                        settings = {body.settings.friction, body.settings.restitution,
                                    body.settings.collision_layer, body.settings.collision_mask};
            return settings;
        };
        for (const auto& cell : *cells) {
            const auto job =
                need(result->residency->submit(cell.key, cell.content_revision, result->prepared));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            for (;;) {
                const auto jobs = need(result->residency->jobs());
                const auto found = std::find_if(jobs.begin(), jobs.end(),
                                                [&](const auto& j) { return j.id == job; });
                if (found == jobs.end())
                    fail("Executor cell job lost its retained identity");
                if (found->state == WorldCellJobState::ready)
                    break;
                if (found->state == WorldCellJobState::failed ||
                    found->state == WorldCellJobState::cancelled ||
                    std::chrono::steady_clock::now() >= deadline)
                    fail("Executor cell preparation failed or exceeded startup bound");
                std::this_thread::yield();
            }
            need(result->residency->drain());
            const auto loaded = need(result->residency->take(job));
            if (!loaded.resident_generation)
                fail("Executor cell was not published");
            need(result->traversal->activate(cell.key, *loaded.resident_generation, cell.proxy,
                                             mesh_settings(cell.proxy)));
        }
        if (restore)
            for (const auto& body : image.physics.bodies)
                if (body.mesh) {
                    auto handle = result->scene->find(identity(body.native.entity));
                    if (!handle) {
                        const auto original = std::find_if(
                            image.entities.begin(), image.entities.end(),
                            [&](const auto& e) { return e.source == body.native.entity; });
                        if (original == image.entities.end())
                            fail("Executor mesh proxy lacks its Scene record");
                        const auto mesh = need(result->physics->create_static_mesh_body(
                            *result->scene, identity(body.native.entity), original->name,
                            *body.mesh, mesh_settings(identity(body.native.entity))));
                        handle = result->scene->find(identity(body.native.entity));
                        if (!handle)
                            fail("Executor mesh proxy was not created");
                        result->bindings.push_back({body.native.entity,
                                                    need(result->scene->reference(handle.value())),
                                                    body.source, mesh});
                    } else
                        result->bindings.push_back(
                            {body.native.entity, need(result->scene->reference(handle.value())),
                             body.source,
                             need(result->physics->find_body(
                                 *result->scene, need(result->scene->reference(handle.value()))))});
                }
        if (restore) {
            std::vector<PhysicsTransferBinding> body_bindings;
            for (const auto& map : result->bindings)
                if (map.source_body && map.destination_body)
                    body_bindings.push_back({*map.source_body, *map.destination_body});
            need(result->physics->restore_transfer(*result->scene, image.physics, body_bindings));
        }
        result->fence = need(ExecutionFence::create());
        result->permit = need(result->fence->permit());
        result->peers = image.peers;
        result->writers = image.writers;
        result->watermark = image.route_watermark;
        return result;
    }
};

RegionExecutor::RegionExecutor(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
RegionExecutor::~RegionExecutor() = default;
RegionExecutorPhase RegionExecutor::phase() const noexcept {
    return impl_->phase;
}
Result<std::unique_ptr<RegionExecutor>> RegionExecutor::bootstrap(const RegionProcessImage& image,
                                                                  RegionInstanceId instance,
                                                                  GameplayAccess& access,
                                                                  MonotonicTime now) {
    return boundary<std::unique_ptr<RegionExecutor>>("server.executor.bootstrap", [&] {
        if (now < MonotonicTime::zero() || image.physics.completed_tick != 0 ||
            !image.movement.actors.empty() || !image.combat.actors.empty())
            fail("Executor bootstrap requires a pristine zero-tick seed");
        auto impl = Impl::build(image, instance, false);
        impl->last_time = now;
        impl->movement = need(
            CharacterMovement::create(*impl->scene, *impl->physics, image.frame.origin,
                                      impl->traversal->instance(), impl->domains, impl->fence));
        impl->combat = need(
            CharacterCombat::create(*impl->scene, *impl->physics,
                                    need(CombatRules::restore_portable(image.rules)), impl->fence));
        for (const auto& peer : image.peers) {
            const auto binding = need(
                impl->combat->bind_host(access, peer.connection, *impl->movement, *impl->scene,
                                        *impl->physics, need(impl->scene->find(peer.entity)), now));
            const auto writer =
                std::find_if(image.writers.begin(), image.writers.end(), [&](const auto& w) {
                    return w.character == peer.session.character && w.entity == peer.entity;
                });
            if (writer == image.writers.end())
                fail("Executor bootstrap player lacks its durable writer");
            need(impl->movement->set_actor_domain(binding.movement.stream,
                                                  impl->corridor(writer->region)));
        }
        for (const auto& npc : image.npcs) {
            const auto entity =
                need(impl->scene->reference(need(impl->scene->find(identity(npc.source)))));
            const auto body = need(impl->physics->find_body(*impl->scene, entity));
            impl->add_npc(entity, body, npc.pending);
        }
        need(impl->fence->freeze(impl->permit));
        return std::unique_ptr<RegionExecutor>{new RegionExecutor{std::move(impl)}};
    });
}
Result<std::unique_ptr<RegionExecutor>> RegionExecutor::stage(const RegionProcessImage& image,
                                                              MonotonicTime now) {
    return stage(image, image.executor_instance, now);
}
Result<std::unique_ptr<RegionExecutor>> RegionExecutor::stage(const RegionProcessImage& image,
                                                              RegionInstanceId instance,
                                                              MonotonicTime now) {
    return boundary<std::unique_ptr<RegionExecutor>>("server.executor.stage", [&] {
        auto impl = Impl::build(image, instance, true);
        impl->last_time = now;
        need(impl->fence->freeze(impl->permit));
        impl->movement = need(CharacterMovement::stage_transfer(
            *impl->scene, *impl->physics, image.movement, impl->bindings, now,
            impl->traversal->instance(), impl->fence));
        impl->combat = need(CharacterCombat::stage_transfer(
            *impl->scene, *impl->physics, image.combat, impl->bindings, now,
            need(CombatRules::restore_portable(image.rules)), impl->fence));
        for (const auto& npc : image.npcs) {
            const auto& map = impl->map(npc.source);
            if (map.source_body != npc.body || !map.destination_body)
                fail("Executor NPC lacks its exact native map");
            impl->add_npc(map.destination, *map.destination_body, npc.pending);
        }
        return std::unique_ptr<RegionExecutor>{new RegionExecutor{std::move(impl)}};
    });
}
Result<void> RegionExecutor::set_context(std::span<const RegionPeer> peers,
                                         std::span<const storage::RegionWriter> writers,
                                         std::uint64_t watermark) {
    return boundary<void>("server.executor.context", [&] {
        impl_->check();
        if (peers.size() > 32 || writers.size() > 32 || watermark < impl_->watermark)
            fail("Executor context exceeds its bound or regresses routing");
        std::vector<RegionPeer> candidate_peers(peers.begin(), peers.end());
        std::vector<storage::RegionWriter> candidate_writers(writers.begin(), writers.end());
        for (std::size_t i = 0; i < peers.size(); ++i) {
            if (peers[i].connection == 0)
                fail("Executor peer connection is invalid");
            need(impl_->scene->find(peers[i].entity));
            for (std::size_t j = 0; j < i; ++j)
                if (peers[j].connection == peers[i].connection ||
                    peers[j].entity == peers[i].entity)
                    fail("Executor peer identity is duplicated");
        }
        for (std::size_t i = 0; i < writers.size(); ++i) {
            if (writers[i].epoch == 0)
                fail("Executor writer epoch is invalid");
            need(impl_->scene->find(writers[i].entity));
            static_cast<void>(impl_->corridor(writers[i].region));
            for (std::size_t j = 0; j < i; ++j)
                if (writers[j].entity == writers[i].entity ||
                    writers[j].character == writers[i].character ||
                    writers[j].store != writers[i].store)
                    fail("Executor writer context is inconsistent");
        }
        if (impl_->phase == RegionExecutorPhase::active && candidate_writers != impl_->writers)
            fail("Executor cannot change writer context during active gameplay");
        impl_->peers.swap(candidate_peers);
        impl_->writers.swap(candidate_writers);
        impl_->watermark = watermark;
    });
}
Result<void> RegionExecutor::activate(std::span<const storage::RegionWriter> writers) {
    if (auto set = set_context(impl_->peers, writers, impl_->watermark); !set)
        return set;
    return activate();
}
Result<void> RegionExecutor::activate() {
    return boundary<void>("server.executor.activate", [&] {
        impl_->check();
        if (impl_->phase == RegionExecutorPhase::active)
            return;
        const auto combat = need(impl_->combat->capture_cut(*impl_->scene, *impl_->physics));
        const auto movement = need(impl_->movement->capture_cut(*impl_->scene, *impl_->physics));
        if (impl_->writers.size() != combat.actors.size())
            fail("Executor activation lacks complete writer authority");
        for (const auto& actor : combat.actors) {
            const auto writer =
                std::find_if(impl_->writers.begin(), impl_->writers.end(), [&](const auto& w) {
                    return w.character == actor.session.character &&
                           w.entity == identity(actor.entity);
                });
            if (writer == impl_->writers.end() || writer->instance != impl_->instance ||
                writer->epoch == 0)
                fail("Executor activation writer does not belong to this incarnation");
            static_cast<void>(impl_->corridor(writer->region));
        }
        need(impl_->fence->resume(impl_->permit));
        try {
            for (const auto& actor : movement.actors) {
                const auto writer =
                    std::find_if(impl_->writers.begin(), impl_->writers.end(), [&](const auto& w) {
                        return w.character == actor.session.character &&
                               w.entity == identity(actor.entity);
                    });
                if (writer == impl_->writers.end())
                    fail("Executor movement lacks its writer");
                need(impl_->movement->set_actor_domain(actor.stream,
                                                       impl_->corridor(writer->region)));
            }
        } catch (...) {
            static_cast<void>(impl_->fence->freeze(impl_->permit));
            impl_->phase = RegionExecutorPhase::faulted;
            throw;
        }
        impl_->phase = RegionExecutorPhase::active;
    });
}
Result<void> RegionExecutor::freeze() {
    return boundary<void>("server.executor.freeze", [&] {
        impl_->check();
        if (impl_->phase == RegionExecutorPhase::active)
            need(impl_->fence->freeze(impl_->permit));
        impl_->phase = RegionExecutorPhase::frozen;
    });
}
Result<void> RegionExecutor::submit_movement(GameplayAccess& access, std::uint64_t connection,
                                             const net::MovementInput& input, MonotonicTime now) {
    return boundary<void>("server.executor.movement", [&] {
        impl_->clock(now, true);
        need(impl_->movement->submit_host(access, connection, *impl_->scene, *impl_->physics, input,
                                          now));
    });
}
Result<void> RegionExecutor::submit_combat(GameplayAccess& access, std::uint64_t connection,
                                           const net::CombatAction& action, MonotonicTime now) {
    return boundary<void>("server.executor.combat", [&] {
        impl_->clock(now, true);
        need(impl_->combat->submit_host(access, connection, *impl_->scene, *impl_->physics, action,
                                        now));
    });
}
Result<void> RegionExecutor::submit_npc_velocity(WorldEntityId id, math::Vec3 velocity) {
    return boundary<void>("server.executor.npc", [&] {
        impl_->check(true);
        const auto found = std::find_if(impl_->npcs.begin(), impl_->npcs.end(),
                                        [&](const auto& n) { return identity(n.entity) == id; });
        if (found == impl_->npcs.end() || found->pending || !std::isfinite(velocity.x) ||
            !std::isfinite(velocity.y) || !std::isfinite(velocity.z) || velocity.y != 0 ||
            glm::dot(velocity, velocity) > movement_speed * movement_speed)
            fail("Executor NPC intent is unknown, repeated or outside horizontal speed bounds");
        static_cast<void>(impl_->npc_body(*found));
        found->pending = velocity;
    });
}
Result<RegionTick> RegionExecutor::step(GameplayAccess& access,
                                        std::span<const std::uint64_t> connections,
                                        MonotonicTime now) {
    return boundary<RegionTick>("server.executor.step", [&] {
        impl_->clock(now, true);
        if (connections.size() > 32)
            fail("Executor live connection inventory exceeds its bound");
        for (std::size_t i = 0; i < connections.size(); ++i)
            if (connections[i] != 0 &&
                std::find(connections.begin(), connections.begin() + static_cast<std::ptrdiff_t>(i),
                          connections[i]) != connections.begin() + static_cast<std::ptrdiff_t>(i))
                fail("Executor live connection inventory is duplicated");
        RegionTick report{};
        report.gameplay.movement.completed_tick = need(impl_->movement->stats()).completed_ticks;
        struct Plan {
            PhysicsBodyVelocity velocity;
            math::Vec3 from;
            double radius;
        };
        std::array<Plan, 16> plans{};
        for (std::size_t i = 0; i < impl_->npcs.size(); ++i) {
            const auto& npc = impl_->npcs[i];
            const auto native = impl_->npc_body(npc);
            auto& plan = plans[i];
            plan.from = native.pose.translation;
            plan.radius = Impl::radius(native);
            plan.velocity = need(impl_->physics->velocity(*impl_->scene, npc.body));
            const auto input = npc.pending.value_or(math::Vec3{});
            plan.velocity.linear.x = input.x;
            plan.velocity.linear.z = input.z;
            plan.velocity.linear.y =
                std::clamp(plan.velocity.linear.y, -movement_maximum_vertical_speed,
                           movement_maximum_vertical_speed);
            const auto to =
                plan.from + plan.velocity.linear / static_cast<double>(movement_tick_hz);
            const auto start = need(translate_world_position(
                impl_->frame.origin, {plan.from.x, plan.from.y, plan.from.z}));
            const auto end =
                need(translate_world_position(impl_->frame.origin, {to.x, to.y, to.z}));
            if (!need(movement_coverage(impl_->domains, plan.from, to)) ||
                !need(impl_->traversal->allows_motion(start, end, plan.radius))) {
                plan.velocity.linear.x = 0;
                plan.velocity.linear.z = 0;
            }
        }
        impl_->phase = RegionExecutorPhase::faulted;
        for (std::size_t i = 0; i < impl_->npcs.size(); ++i) {
            auto written =
                impl_->physics->set_velocity(*impl_->scene, impl_->npcs[i].body, plans[i].velocity);
            if (!written) {
                report.error = std::move(written).error();
                return report;
            }
            ++report.npc_velocity_writes;
        }
        auto tick =
            impl_->combat->step_host(access, *impl_->movement, *impl_->scene, *impl_->physics,
                                     connections, now, nullptr, impl_->traversal.get());
        if (!tick) {
            report.error = std::move(tick).error();
            const auto movement = need(impl_->movement->stats());
            report.gameplay.movement_completed =
                movement.completed_ticks > report.gameplay.movement.completed_tick;
            report.gameplay.movement.completed_tick = movement.completed_ticks;
            if (report.npc_velocity_writes == 0 && !need(impl_->combat->stats()).faulted &&
                !movement.faulted)
                impl_->phase = RegionExecutorPhase::active;
            return report;
        }
        report.gameplay = std::move(tick).value();
        if (report.gameplay.error) {
            report.error = report.gameplay.error;
            return report;
        }
        try {
            for (std::size_t i = 0; i < impl_->npcs.size(); ++i) {
                const auto native = impl_->npc_body(impl_->npcs[i]);
                const auto from = need(translate_world_position(
                    impl_->frame.origin, {plans[i].from.x, plans[i].from.y, plans[i].from.z}));
                const auto& p = native.pose.translation;
                const auto to =
                    need(translate_world_position(impl_->frame.origin, {p.x, p.y, p.z}));
                if (!need(movement_coverage(impl_->domains, plans[i].from, p)) ||
                    !need(impl_->traversal->allows_motion(from, to, plans[i].radius))) {
                    report.error = Error{
                        ErrorCode::conflict, "NPC left committed ready regional coverage", {}};
                    return report;
                }
                impl_->npcs[i].pending.reset();
            }
            for (const auto& rebound : report.gameplay.rebindings) {
                const auto peer =
                    std::find_if(impl_->peers.begin(), impl_->peers.end(), [&](const auto& p) {
                        return p.session.session == rebound.binding.movement.session;
                    });
                if (peer == impl_->peers.end())
                    fail("Respawn binding lacks its retained gateway peer");
                const auto writer =
                    std::find_if(impl_->writers.begin(), impl_->writers.end(), [&](const auto& w) {
                        return w.character == peer->session.character && w.entity == peer->entity;
                    });
                if (writer == impl_->writers.end())
                    fail("Respawn binding lacks its current writer");
                need(impl_->movement->set_actor_domain(rebound.binding.movement.stream,
                                                       impl_->corridor(writer->region)));
            }
        } catch (Failure& failure) {
            report.error = std::move(failure.error);
            return report;
        } catch (...) {
            report.error = Error{
                ErrorCode::execution_failed, "Executor publication failed after native work", {}};
            return report;
        }
        impl_->phase = RegionExecutorPhase::active;
        return report;
    });
}
Result<net::CombatState> RegionExecutor::view(GameplayAccess& access, std::uint64_t connection,
                                              std::span<const net::CombatTargetId> visible,
                                              MonotonicTime now) const {
    return boundary<net::CombatState>("server.executor.view", [&] {
        impl_->clock(now, false);
        return need(impl_->combat->view_host(access, connection, *impl_->scene, *impl_->physics,
                                             visible, now));
    });
}
Result<RegionProcessImage> RegionExecutor::capture() const {
    return capture(impl_->peers, impl_->writers, impl_->watermark);
}
Result<RegionProcessImage> RegionExecutor::capture(std::span<const RegionPeer> peers,
                                                   std::span<const storage::RegionWriter> writers,
                                                   std::uint64_t watermark) const {
    return boundary<RegionProcessImage>("server.executor.capture", [&] {
        impl_->check();
        RegionProcessImage image{impl_->frame,
                                 impl_->regions,
                                 {},
                                 need(impl_->physics->capture_transfer(*impl_->scene)),
                                 need(impl_->movement->capture_cut(*impl_->scene, *impl_->physics)),
                                 need(impl_->combat->capture_cut(*impl_->scene, *impl_->physics)),
                                 need(impl_->combat->capture_portable_rules()),
                                 *impl_->cells,
                                 impl_->assets,
                                 {},
                                 {peers.begin(), peers.end()},
                                 {writers.begin(), writers.end()},
                                 watermark,
                                 impl_->instance};
        SceneQueryOptions options;
        options.page_size = 256;
        options.scan_budget = 4096;
        auto page = need(impl_->scene->query_entities(options));
        for (;;) {
            for (const auto& row : page.entities()) {
                const auto state = need(impl_->scene->entity_state(row.reference.handle));
                if (state.script)
                    fail("Entity ScriptRuntime cannot enter a portable executor image");
                std::optional<WorldEntityId> parent;
                if (state.parent)
                    parent = identity(need(impl_->scene->reference(*state.parent)));
                image.entities.push_back({state.reference, state.name, state.local_transform,
                                          parent, state.camera, state.mesh_renderer,
                                          state.rigid_body});
            }
            if (!page.next_cursor())
                break;
            page = need(impl_->scene->continue_query(*page.next_cursor()));
        }
        for (const auto& npc : impl_->npcs) {
            static_cast<void>(impl_->npc_body(npc));
            image.npcs.push_back({npc.entity, npc.body, npc.pending});
        }
        return image;
    });
}
} // namespace shark::server
