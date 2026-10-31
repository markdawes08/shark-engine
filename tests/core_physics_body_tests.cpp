#include <shark/physics.hpp>
#include <shark/scene.hpp>
#include <shark/schema.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <thread>

namespace {
using namespace shark;
template <typename T> T take(Result<T> result) {
    if (!result)
        INFO(result.error().message);
    REQUIRE(result);
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        INFO(result.error().message);
    REQUIRE(result);
}
template <typename Id> Id id(unsigned digit) {
    auto value = std::string{"30000000-0000-4000-8000-000000000000"};
    value.back() = static_cast<char>('0' + digit);
    return Id{take(StableId::parse(value))};
}
std::unique_ptr<Scene> scene() {
    return take(Scene::create(AuthoredSceneScope{id<SceneId>(1)}));
}
EntitySeed seed(unsigned digit = 2) {
    EntitySeed value{id<AuthoredEntityId>(digit)};
    value.rigid_body.emplace();
    return value;
}
} // namespace

TEST_CASE("RigidBody validates all settings, root scale and bounded local poses",
          "[core-physics]") {
    RigidBody body;
    REQUIRE(validate_rigid_body(body));
    CHECK(take(component_schema(ComponentSchemaId::rigid_body)).properties.size() == 8);
    CHECK(validate_component(ComponentSchemaId::rigid_body, 1, body));
    for (auto field : {&RigidBody::radius, &RigidBody::half_height, &RigidBody::mass,
                       &RigidBody::friction, &RigidBody::restitution}) {
        auto invalid = body;
        invalid.*field = std::numeric_limits<double>::quiet_NaN();
        CHECK_FALSE(validate_rigid_body(invalid));
    }
    body.shape = static_cast<ColliderShape>(99);
    CHECK_FALSE(validate_rigid_body(body));
    body = {};
    body.motion = static_cast<BodyMotion>(99);
    CHECK_FALSE(validate_rigid_body(body));
    Transform pose;
    for (auto scale : {math::Vec3{-1}, math::Vec3{1, 2, 1}, math::Vec3{0.001}}) {
        pose.scale = scale;
        CHECK_FALSE(validate_rigid_body_pose({}, pose));
    }
    pose = {};
    pose.translation.x = physics_local_position_limit + 1;
    CHECK_FALSE(validate_rigid_body_pose({}, pose));
    pose.translation.x = physics_local_position_limit;
    CHECK(validate_rigid_body_pose({}, pose));
    RigidBody capsule;
    capsule.shape = ColliderShape::capsule;
    capsule.half_height = 256;
    CHECK_FALSE(validate_rigid_body_pose(capsule, {}));
}

TEST_CASE("RigidBody Scene mutations preserve hierarchy, authority and deferred atomicity",
          "[core-physics]") {
    auto owner = scene();
    const auto root = take(owner->create_entity(seed()));
    const auto original = take(owner->stats()).revision;
    take(owner->set_rigid_body(root, RigidBody{}));
    CHECK(take(owner->stats()).revision == original);
    auto visual = seed(3);
    visual.rigid_body.reset();
    const auto other = take(owner->create_entity(visual));
    CHECK_FALSE(owner->reparent(root, other, ReparentMode::keep_local));
    take(owner->reparent(other, root, ReparentMode::keep_local));
    CHECK_FALSE(owner->set_rigid_body(other, RigidBody{}));
    auto bad = Transform{};
    bad.scale = {-1, -1, -1};
    CHECK_FALSE(owner->set_transform(root, bad));
    const auto before = take(owner->stats()).revision;
    take(owner->defer_set_transform(root, bad));
    auto changed = RigidBody{};
    changed.shape = ColliderShape::sphere;
    take(owner->defer_set_rigid_body(root, changed));
    const auto report = take(owner->apply_deferred());
    REQUIRE(report.outcomes.size() == 2);
    CHECK_FALSE(report.outcomes[0].result);
    CHECK(report.outcomes[1].result);
    CHECK(report.revision_after == before + 1);
    auto page = take(owner->inspect_entity(root, {false, false, false, false, false, true}));
    CHECK(page.entities().front().rigid_body == changed);
    CHECK_FALSE(page.entities().front().transform);
    take(owner->set_rigid_body(root, std::nullopt));
    take(owner->set_transform(root, bad)); // Render-only signed scales still work.
    auto replica = take(Scene::create(ReplicaSceneScope{id<WorldId>(4), id<ReplicaSessionId>(5)}));
    EntitySeed replica_seed{id<WorldEntityId>(6),
                            AuthorityStamp{id<RegionId>(7), id<RegionInstanceId>(8), 1}};
    replica_seed.rigid_body.emplace();
    CHECK_FALSE(replica->create_entity(replica_seed));
}

TEST_CASE("Native rigid-body modes and scaled box sphere capsule geometry are real",
          "[core-physics]") {
    auto owner = scene();
    auto config = PhysicsWorldConfig{};
    config.max_bodies = 2;
    auto world = take(PhysicsWorld::create(config));
    auto value = seed();
    value.transform.translation = {3, 4, -5};
    value.transform.rotation = math::Quat{0.9, 0.1, 0.2, 0.3};
    value.transform.scale = {2, 2, 2};
    auto handle = take(owner->create_entity(value));
    for (auto motion :
         {BodyMotion::static_body, BodyMotion::dynamic_body, BodyMotion::kinematic_body}) {
        for (auto shape : {ColliderShape::box, ColliderShape::sphere, ColliderShape::capsule}) {
            RigidBody body{motion, shape, {0.5, 0.75, 1}, 0.25, 0.75, 2, 0.3, 0.2};
            take(owner->set_rigid_body(handle, body));
            auto native = take(world->create_body(*owner, handle));
            const auto state = take(world->body_state(*owner, native));
            CHECK(state.motion == motion);
            CHECK(state.settings == body);
            CHECK(state.pose.translation == math::Vec3{3, 4, -5});
            const auto authored_rotation =
                take(owner->entity_state(handle)).local_transform.rotation;
            CHECK(state.pose.rotation.x == Catch::Approx(authored_rotation.x));
            CHECK(state.pose.rotation.y == Catch::Approx(authored_rotation.y));
            CHECK(state.pose.rotation.z == Catch::Approx(authored_rotation.z));
            CHECK(state.pose.rotation.w == Catch::Approx(authored_rotation.w));
            const auto expected = shape == ColliderShape::box      ? math::Vec3{1, 1.5, 2}
                                  : shape == ColliderShape::sphere ? math::Vec3{0.5}
                                                                   : math::Vec3{0.5, 2, 0.5};
            for (int axis = 0; axis < 3; ++axis)
                CHECK(state.collider_half_extents[axis] == Catch::Approx(expected[axis]));
            CHECK(state.friction == Catch::Approx(0.3));
            CHECK(state.restitution == Catch::Approx(0.2));
            if (motion == BodyMotion::dynamic_body)
                CHECK(state.inverse_mass == Catch::Approx(0.5));
            CHECK_FALSE(world->create_body(*owner, handle));
            CHECK_FALSE(world->step()); // 18c owns nonempty simulation admission.
            CHECK(take(world->stats()).completed_ticks == 0);
            take(world->remove_body(native));
            CHECK_FALSE(world->body_state(*owner, native));
            CHECK_FALSE(world->remove_body(native));
        }
    }
    take(world->step());
    CHECK(take(world->stats()).bodies == 0);
}

TEST_CASE("Physics body tokens fence world and entity incarnations and survive source destruction "
          "for cleanup",
          "[core-physics]") {
    auto owner = scene();
    auto world = take(PhysicsWorld::create({.max_bodies = 1}));
    auto other_world = take(PhysicsWorld::create());
    const auto handle = take(owner->create_entity(seed()));
    const auto body = take(world->create_body(*owner, handle));
    CHECK_FALSE(other_world->remove_body(body));
    const auto second = take(owner->create_entity(seed(3)));
    CHECK_FALSE(world->create_body(*owner, second));
    bool rejected = false;
    std::thread foreign([&] { rejected = !world->remove_body(body); });
    foreign.join();
    CHECK(rejected);
    auto moved = Transform{};
    moved.translation.x = 1;
    take(owner->set_transform(handle, moved));
    CHECK_FALSE(world->body_state(*owner, body));
    take(owner->destroy_entity(handle));
    auto recreated = take(owner->create_entity(seed()));
    CHECK(recreated != handle);
    CHECK_FALSE(world->body_state(*owner, body));
    take(world->remove_body(body));
    const auto replacement = take(world->create_body(*owner, recreated));
    CHECK(replacement.slot == body.slot);
    CHECK(replacement.generation != body.generation);
    CHECK_FALSE(world->remove_body(body));
    owner.reset();
    take(world->remove_body(replacement)); // No dangling Scene borrow.
    auto final_scene = scene();
    const auto final_entity = take(final_scene->create_entity(seed()));
    const auto final_body = take(world->create_body(*final_scene, final_entity));
    take(world->close());
    CHECK(take(world->stats()).bodies == 0);
    CHECK_FALSE(world->body_state(*final_scene, final_body));
    CHECK_FALSE(world->remove_body(replacement));
}
