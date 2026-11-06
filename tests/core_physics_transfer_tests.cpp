#include <shark/physics.hpp>
#include <shark/scene.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

namespace {
using namespace shark;
template <class T> T take_transfer(Result<T> value) {
    if (!value)
        INFO(value.error().context.operation << ": " << value.error().message);
    REQUIRE(value);
    return std::move(value).value();
}
void take_transfer(Result<void> value) {
    if (!value)
        INFO(value.error().context.operation << ": " << value.error().message);
    REQUIRE(value);
}
template <class T> T transfer_id(unsigned value) {
    std::string text = "a05f0000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 15U];
        value >>= 4U;
    }
    return T{take_transfer(StableId::parse(text))};
}
PhysicsWorldConfig transfer_config() {
    PhysicsWorldConfig result;
    result.tick_hz = 20;
    result.collision_substeps = 3;
    result.max_bodies = 16;
    result.max_body_pairs = 128;
    result.max_contact_constraints = 128;
    result.gravity = {};
    return result;
}
struct NativeTransferFixture {
    std::unique_ptr<Scene> scene;
    std::unique_ptr<PhysicsWorld> physics;
    explicit NativeTransferFixture(unsigned incarnation = 3)
        : scene(take_transfer(Scene::create(RegionSceneScope{
              transfer_id<WorldId>(1),
              {transfer_id<RegionId>(2), transfer_id<RegionInstanceId>(incarnation), 1}}))),
          physics(take_transfer(PhysicsWorld::create(transfer_config()))) {}
    PhysicsBodyHandle add(unsigned identity, RigidBody settings, Transform pose = {}) {
        EntitySeed seed{transfer_id<WorldEntityId>(identity)};
        seed.rigid_body = settings;
        seed.transform = pose;
        const auto entity = take_transfer(scene->create_entity(seed));
        return take_transfer(physics->create_body(*scene, entity));
    }
    void step() {
        take_transfer(physics->step(*scene));
    }
};
struct RestoredNative {
    NativeTransferFixture target{4};
    std::vector<PhysicsTransferBinding> bindings;
    explicit RestoredNative(const PhysicsTransferState& state) {
        // Deliberately reverse primitive slot order: restore must relocate
        // handles/contact pairs rather than relying on identical allocations.
        for (auto entry = state.bodies.rbegin(); entry != state.bodies.rend(); ++entry) {
            if (entry->mesh)
                continue;
            EntitySeed seed{std::get<WorldEntityId>(entry->native.entity.identity)};
            seed.rigid_body = entry->settings;
            seed.transform = entry->source_pose;
            const auto entity = take_transfer(target.scene->create_entity(seed));
            bindings.push_back(
                {entry->source, take_transfer(target.physics->create_body(*target.scene, entity))});
        }
        take_transfer(target.physics->begin_simulation(*target.scene));
        for (const auto& body : state.bodies) {
            if (!body.mesh)
                continue;
            const PhysicsStaticMeshSettings settings{
                body.settings.friction, body.settings.restitution, body.settings.collision_layer,
                body.settings.collision_mask};
            bindings.push_back(
                {body.source,
                 take_transfer(target.physics->create_static_mesh_body(
                     *target.scene, std::get<WorldEntityId>(body.native.entity.identity),
                     Name{"Mesh"}, *body.mesh, settings))});
        }
    }
    PhysicsBodyHandle mapped(PhysicsBodyHandle source) const {
        const auto entry = std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) {
            return binding.source == source;
        });
        REQUIRE(entry != bindings.end());
        return entry->destination;
    }
    void restore(const PhysicsTransferState& state) {
        take_transfer(target.physics->restore_transfer(*target.scene, state, bindings));
    }
};
} // namespace

TEST_CASE("Native transfer preserves staged force target exact poses and original mesh values",
          "[core-physics][core-physics-transfer]") {
    NativeTransferFixture source;
    RigidBody dynamic;
    dynamic.motion = BodyMotion::dynamic_body;
    dynamic.shape = ColliderShape::sphere;
    dynamic.friction = 0;
    Transform first;
    first.translation = {0, 2, 0};
    const auto ball = source.add(10, dynamic, first);
    RigidBody kinematic;
    kinematic.motion = BodyMotion::kinematic_body;
    Transform second;
    second.translation = {4, 2, 0};
    const auto platform = source.add(11, kinematic, second);
    take_transfer(source.physics->begin_simulation(*source.scene));
    const PhysicsStaticMesh mesh{{{-4, -2, -4}, {-4, -2, 4}, {4, -2, 4}, {4, -2, -4}},
                                 {{0, 1, 2}, {0, 2, 3}}};
    const PhysicsStaticMeshSettings settings{0.75, 0.25, 2, 0xffff};
    take_transfer(source.physics->create_static_mesh_body(
        *source.scene, transfer_id<WorldEntityId>(12), Name{"Ground"}, mesh, settings));
    source.step();
    take_transfer(source.physics->take_events(*source.scene));
    take_transfer(source.physics->set_velocity(*source.scene, ball, {{1, 2, 0}, {0, 0, 3}}));
    take_transfer(source.physics->apply_force(*source.scene, ball, {20, 0, 0}));
    second.translation.x += 0.25;
    take_transfer(source.physics->set_kinematic_target(*source.scene, platform, second));
    const auto snapshot = take_transfer(source.physics->capture_transfer(*source.scene));
    REQUIRE(snapshot.bodies.size() == 3);
    CHECK(snapshot.completed_tick == 1);
    CHECK(snapshot.bodies.front().pending_force == math::Vec3{20, 0, 0});
    REQUIRE(snapshot.bodies.back().mesh);
    CHECK(*snapshot.bodies.back().mesh == mesh);
    CHECK(snapshot.bodies.back().settings.friction == 0.75);
    RestoredNative destination{snapshot};
    destination.restore(snapshot);
    CHECK(take_transfer(destination.target.physics->stats()).completed_ticks == 1);
    const auto restored =
        take_transfer(destination.target.physics->capture_transfer(*destination.target.scene));
    const auto copied =
        std::find_if(restored.bodies.begin(), restored.bodies.end(),
                     [&](const auto& row) { return row.source == destination.mapped(ball); });
    REQUIRE(copied != restored.bodies.end());
    CHECK(copied->source_pose == snapshot.bodies.front().source_pose);
    CHECK(copied->previous_pose == snapshot.bodies.front().previous_pose);
    CHECK(copied->native.pose == snapshot.bodies.front().native.pose);
    CHECK(copied->velocity == snapshot.bodies.front().velocity);
    CHECK(copied->pending_force == math::Vec3{20, 0, 0});
    source.step();
    destination.target.step();
    const auto a = take_transfer(source.physics->velocity(*source.scene, ball));
    const auto b = take_transfer(
        destination.target.physics->velocity(*destination.target.scene, destination.mapped(ball)));
    CHECK(a.linear.x == Catch::Approx(2).margin(1e-5));
    CHECK(b.linear.x == Catch::Approx(2).margin(1e-5));
    CHECK(b.linear.y == Catch::Approx(2).margin(1e-5));
    CHECK(b.angular.z == Catch::Approx(3).margin(1e-5));
    CHECK(take_transfer(destination.target.physics->body_state(*destination.target.scene,
                                                               destination.mapped(platform)))
              .pose.translation.x == Catch::Approx(4.25).margin(1e-5));
    const auto after =
        take_transfer(destination.target.physics->capture_transfer(*destination.target.scene));
    for (const auto& body : after.bodies) {
        CHECK(body.pending_force == math::Vec3{});
        CHECK_FALSE(body.target);
    }
    CHECK_FALSE(destination.target.physics->restore_transfer(*destination.target.scene, snapshot,
                                                             destination.bindings));
}

TEST_CASE("Native transfer remaps active contacts without repeated enters and keeps event sequence",
          "[core-physics][core-physics-transfer]") {
    NativeTransferFixture source;
    RigidBody sensor;
    sensor.is_trigger = true;
    sensor.half_extents = {1, 1, 1};
    source.add(10, sensor);
    RigidBody moving;
    moving.motion = BodyMotion::kinematic_body;
    const auto mover = source.add(11, moving);
    take_transfer(source.physics->begin_simulation(*source.scene));
    source.step();
    CHECK_FALSE(source.physics->capture_transfer(*source.scene));
    const auto entered = take_transfer(source.physics->take_events(*source.scene));
    REQUIRE(entered.size() == 1);
    const auto snapshot = take_transfer(source.physics->capture_transfer(*source.scene));
    REQUIRE(snapshot.active_pairs.size() == 1);
    RestoredNative destination{snapshot};
    destination.restore(snapshot);
    destination.target.step();
    CHECK(
        take_transfer(destination.target.physics->take_events(*destination.target.scene)).empty());
    Transform target;
    target.translation.x = 2;
    take_transfer(destination.target.physics->set_kinematic_target(
        *destination.target.scene, destination.mapped(mover), target));
    destination.target.step();
    CHECK(
        take_transfer(destination.target.physics->take_events(*destination.target.scene)).empty());
    destination.target.step();
    const auto exited =
        take_transfer(destination.target.physics->take_events(*destination.target.scene));
    REQUIRE(exited.size() == 1);
    CHECK(exited.front().phase == PhysicsContactPhase::exit);
    CHECK(exited.front().sequence == entered.front().sequence + 1);
    CHECK(exited.front().completed_tick == 4);
    CHECK(exited.front().first.body.world ==
          take_transfer(destination.target.physics->capture_transfer(*destination.target.scene))
              .world);
    CHECK(exited.front().first.entity.handle.scene_instance ==
          take_transfer(destination.target.scene->stats()).instance);
}

TEST_CASE("Native transfer rejects malformed mappings and raw poses before any native write",
          "[core-physics][core-physics-transfer]") {
    NativeTransferFixture source;
    RigidBody body;
    body.motion = BodyMotion::dynamic_body;
    source.add(10, body);
    source.add(11, body, Transform{{4, 0, 0}});
    take_transfer(source.physics->begin_simulation(*source.scene));
    const auto snapshot = take_transfer(source.physics->capture_transfer(*source.scene));
    RestoredNative destination{snapshot};
    const auto reject = [&](const PhysicsTransferState& value,
                            std::span<const PhysicsTransferBinding> bindings) {
        CHECK_FALSE(destination.target.physics->restore_transfer(*destination.target.scene, value,
                                                                 bindings));
        const auto stats = take_transfer(destination.target.physics->stats());
        CHECK_FALSE(stats.faulted);
        CHECK(stats.completed_ticks == 0);
    };
    reject(snapshot, std::span{destination.bindings}.first(1));
    auto repeated = destination.bindings;
    repeated[1].destination = repeated[0].destination;
    reject(snapshot, repeated);
    auto malformed = snapshot;
    malformed.bodies.front().native.pose.rotation.w = 100;
    reject(malformed, destination.bindings);
    malformed = snapshot;
    malformed.bodies.front().settings.mass = 2;
    reject(malformed, destination.bindings);
    destination.restore(snapshot);
}
