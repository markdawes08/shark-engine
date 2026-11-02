#include <shark/physics.hpp>
#include <shark/scene.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
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
template <typename Id> Id id(unsigned value) {
    std::string text = "20b00000-0000-4000-8000-000000000001";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 15U];
        value >>= 4U;
    }
    return Id{take(StableId::parse(text))};
}
struct Fixture {
    std::unique_ptr<Scene> scene = take(Scene::create(AuthoredSceneScope{id<SceneId>(1)}));
    std::unique_ptr<PhysicsWorld> world;
    explicit Fixture(PhysicsWorldConfig config = {}) : world(take(PhysicsWorld::create(config))) {}
    std::pair<EntityHandle, PhysicsBodyHandle> add(unsigned number, RigidBody body,
                                                   math::Vec3 position = {0, 0, 0}) {
        EntitySeed seed{id<AuthoredEntityId>(number)};
        seed.rigid_body = body;
        seed.transform.translation = position;
        const auto entity = take(scene->create_entity(seed));
        return {entity, take(world->create_body(*scene, entity))};
    }
    void step() {
        take(world->step(*scene));
    }
};
RigidBody mover() {
    RigidBody body;
    body.motion = BodyMotion::kinematic_body;
    return body;
}
} // namespace

TEST_CASE("Physics queries return ordered checked hits with bilateral filtering and bounded output",
          "[core-physics]") {
    Fixture f({.gravity = {0, 0, 0}});
    RigidBody box;
    box.collision_layer = 2;
    box.collision_mask = 1U << 3U;
    // Deliberately allocate in reverse identity order to exercise tie sorting.
    const auto later = f.add(5, box, {2, 0, 0});
    const auto first = f.add(2, box, {2, 0, 0});
    RigidBody trigger;
    trigger.shape = ColliderShape::sphere;
    trigger.is_trigger = true;
    trigger.collision_layer = 4;
    trigger.collision_mask = 1U << 3U;
    const auto sphere = f.add(3, trigger, {5, 0, 0});
    auto blocked = box;
    blocked.collision_mask = 0; // Query mask alone is insufficient for bilateral admission.
    f.add(4, blocked, {7, 0, 0});
    take(f.world->begin_simulation(*f.scene));

    PhysicsQueryFilter filter;
    filter.layer = 3;
    filter.mask = (1U << 2U) | (1U << 4U);
    const auto state = take(f.scene->stats());
    const auto before = take(f.world->stats());
    const auto ray = take(f.world->raycast(*f.scene, {0, 0, 0}, {10, 0, 0}, filter));
    REQUIRE(ray.hits.size() == 3);
    CHECK(ray.world == first.second.world);
    CHECK(ray.revision == SceneRevision{state.instance, state.revision});
    CHECK(ray.completed_ticks == 0);
    CHECK(ray.hits[0].body == first.second);
    CHECK(ray.hits[1].body == later.second);
    CHECK(ray.hits[2].body == sphere.second);
    CHECK(ray.hits[0].entity.handle == first.first);
    CHECK_FALSE(ray.hits[0].is_trigger);
    CHECK(ray.hits[2].is_trigger);
    CHECK(ray.hits[0].fraction == Catch::Approx(0.15).margin(1e-5));
    CHECK(ray.hits[2].fraction == Catch::Approx(0.45).margin(1e-5));
    CHECK(ray.hits[0].position.x == Catch::Approx(1.5).margin(1e-5));
    REQUIRE(ray.hits[0].normal);
    CHECK(ray.hits[0].normal->x == Catch::Approx(-1).margin(1e-5));

    const auto inside = take(f.world->raycast(*f.scene, {2, 0, 0}, {2.1, 0, 0}, filter));
    REQUIRE(inside.hits.size() == 2);
    CHECK(inside.hits[0].fraction == 0);
    CHECK_FALSE(inside.hits[0].normal);
    filter.include_triggers = false;
    CHECK(take(f.world->raycast(*f.scene, {0, 0, 0}, {10, 0, 0}, filter)).hits.size() == 2);
    filter.include_triggers = true;
    filter.ignore = first.second;
    const auto ignored = take(f.world->raycast(*f.scene, {0, 0, 0}, {10, 0, 0}, filter));
    REQUIRE(ignored.hits.size() == 2);
    CHECK(ignored.hits[0].body == later.second);
    filter.ignore.reset();
    filter.mask = 1U << 4U;
    const auto triggers = take(f.world->raycast(*f.scene, {0, 0, 0}, {10, 0, 0}, filter));
    REQUIRE(triggers.hits.size() == 1);
    CHECK(triggers.hits[0].body == sphere.second);
    filter.mask = (1U << 2U) | (1U << 4U);
    CHECK_FALSE(f.world->raycast(*f.scene, {0, 0, 0}, {10, 0, 0}, filter, 2));
    CHECK_FALSE(f.world->overlap_sphere(*f.scene, {2, 0, 0}, 1, filter, 1));
    CHECK_FALSE(f.world->overlap_box(*f.scene, {2, 0, 0}, {1, 1, 1}, filter, 1));
    CHECK(take(f.scene->stats()).revision == state.revision);
    CHECK(take(f.world->stats()).completed_ticks == before.completed_ticks);
    CHECK(take(f.world->stats()).pending_events == before.pending_events);
    CHECK_FALSE(take(f.world->stats()).faulted);
}

TEST_CASE("Physics overlaps use exact collider geometry rather than broadphase boxes",
          "[core-physics]") {
    Fixture f({.gravity = {0, 0, 0}});
    const auto box = f.add(2, RigidBody{}, {-0.5, 0, 0});
    RigidBody ball;
    ball.shape = ColliderShape::sphere;
    const auto sphere = f.add(3, ball, {1.45, 1.45, 0});
    take(f.world->begin_simulation(*f.scene));
    const auto first_sphere = take(f.world->overlap_sphere(*f.scene, {0, 0, 0}, 1));
    const auto first_box = take(f.world->overlap_box(*f.scene, {0, 0, 0}, {1, 1, 1}));
    // The sphere's AABB overlaps both query AABBs, but its corner is too far
    // from either primitive. These expected distances are independent of Jolt.
    REQUIRE(first_sphere.hits.size() == 1);
    REQUIRE(first_box.hits.size() == 1);
    CHECK(first_sphere.hits[0].body == box.second);
    CHECK(first_box.hits[0].body == box.second);
    Transform reposition;
    reposition.translation = {1.05, 1.05, 0};
    take(f.world->set_static_pose(*f.scene, sphere.second, reposition));
    const auto moved_sphere = take(f.world->overlap_sphere(*f.scene, {0, 0, 0}, 1));
    const auto moved_box = take(f.world->overlap_box(*f.scene, {0, 0, 0}, {1, 1, 1}));
    REQUIRE(moved_sphere.hits.size() == 2);
    REQUIRE(moved_box.hits.size() == 2);
    CHECK(moved_sphere.hits[1].body == sphere.second);
    CHECK(moved_box.hits[1].body == sphere.second);
    CHECK(moved_box.revision != first_box.revision);
    CHECK(first_box.hits.size() == 1); // Owned observations do not silently update.
}

TEST_CASE("Physics queries honor rotated boxes and capsule geometry", "[core-physics]") {
    Fixture f({.gravity = {0, 0, 0}});
    RigidBody long_box;
    long_box.half_extents = {2, 0.25, 0.25};
    const auto box = f.add(2, long_box);
    RigidBody capsule;
    capsule.shape = ColliderShape::capsule;
    capsule.radius = 0.5;
    capsule.half_height = 1;
    const auto cap = f.add(3, capsule, {4, 0, 0});
    take(f.world->begin_simulation(*f.scene));
    Transform rotated;
    const auto half_sqrt = std::sqrt(0.5);
    rotated.rotation = math::Quat{half_sqrt, 0, 0, half_sqrt};
    take(f.world->set_static_pose(*f.scene, box.second, rotated));

    // Rotating the two-meter local X half-axis onto world Y puts the left
    // surface at x=-0.25 even at y=1; the unrotated shape misses this ray.
    const auto box_ray = take(f.world->raycast(*f.scene, {-3, 1, 0}, {3, 1, 0}));
    REQUIRE(box_ray.hits.size() == 1);
    CHECK(box_ray.hits[0].body == box.second);
    CHECK(box_ray.hits[0].fraction == Catch::Approx(11.0 / 24.0).margin(1e-5));
    REQUIRE(box_ray.hits[0].normal);
    CHECK(box_ray.hits[0].normal->x == Catch::Approx(-1).margin(1e-5));
    const auto box_overlap = take(f.world->overlap_sphere(*f.scene, {0, 1.5, 0}, 0.1));
    REQUIRE(box_overlap.hits.size() == 1);
    CHECK(box_overlap.hits[0].body == box.second);

    // A vertical capsule with radius 0.5 centered at x=4 is first hit at
    // x=3.5 and extends to y=1.5 including its rounded cap.
    const auto capsule_ray = take(f.world->raycast(*f.scene, {1, 0, 0}, {7, 0, 0}));
    REQUIRE(capsule_ray.hits.size() == 1);
    CHECK(capsule_ray.hits[0].body == cap.second);
    CHECK(capsule_ray.hits[0].fraction == Catch::Approx(5.0 / 12.0).margin(1e-5));
    const auto cap_overlap = take(f.world->overlap_sphere(*f.scene, {4, 1.4, 0}, 0.1));
    REQUIRE(cap_overlap.hits.size() == 1);
    CHECK(cap_overlap.hits[0].body == cap.second);
}

TEST_CASE("Physics queries validate ownership, finite bounds and current body generations",
          "[core-physics]") {
    Fixture f({.gravity = {0, 0, 0}});
    const auto moving = f.add(2, mover(), {4, 0, 0});
    auto other = take(Scene::create(AuthoredSceneScope{id<SceneId>(9)}));
    CHECK_FALSE(f.world->raycast(*f.scene, {0, 0, 0}, {10, 0, 0})); // No lease yet.
    take(f.world->begin_simulation(*f.scene));
    const auto initial = take(f.world->raycast(*f.scene, {0, 0, 0}, {10, 0, 0}));
    REQUIRE(initial.hits.size() == 1);
    CHECK(initial.hits[0].fraction == Catch::Approx(0.35).margin(1e-5));
    Transform target;
    target.translation = {6, 0, 0};
    take(f.world->set_kinematic_target(*f.scene, moving.second, target));
    CHECK(take(f.world->raycast(*f.scene, {0, 0, 0}, {10, 0, 0})).hits[0].fraction ==
          Catch::Approx(0.35).margin(1e-5)); // Queued target has not stepped.
    f.step();
    const auto moved = take(f.world->raycast(*f.scene, {0, 0, 0}, {10, 0, 0}));
    REQUIRE(moved.hits.size() == 1);
    CHECK(moved.hits[0].fraction == Catch::Approx(0.55).margin(1e-5));
    CHECK(moved.completed_ticks == 1);
    CHECK(initial.completed_ticks == 0);

    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto inf = std::numeric_limits<double>::infinity();
    const auto maximum = std::numeric_limits<double>::max();
    CHECK_FALSE(f.world->raycast(*f.scene, {0, 0, 0}, {0, 0, 0}));
    CHECK_FALSE(f.world->raycast(*f.scene, {nan, 0, 0}, {1, 0, 0}));
    CHECK_FALSE(f.world->raycast(*f.scene, {0, 0, 0}, {inf, 0, 0}));
    CHECK_FALSE(f.world->raycast(*f.scene, {0, 0, 0}, {maximum, 0, 0}));
    CHECK_FALSE(f.world->raycast(*f.scene, {0, 0, 0}, {8193, 0, 0}));
    CHECK_FALSE(f.world->raycast(*f.scene, {8192, 0, 0}, {8191.99999, 0, 0}));
    CHECK_FALSE(f.world->overlap_sphere(*f.scene, {0, 0, 0}, 0));
    CHECK_FALSE(f.world->overlap_sphere(*f.scene, {8192, 0, 0}, 1));
    CHECK_FALSE(f.world->overlap_sphere(*f.scene, {0, 0, 0}, inf));
    CHECK_FALSE(f.world->overlap_sphere(*f.scene, {maximum, 0, 0}, 1));
    CHECK_FALSE(f.world->overlap_sphere(*f.scene, {0, 0, 0}, maximum));
    CHECK_FALSE(f.world->overlap_box(*f.scene, {0, 0, 0}, {1, -1, 1}));
    CHECK_FALSE(f.world->overlap_box(*f.scene, {0, 0, 0}, {1, maximum, 1}));
    CHECK_FALSE(f.world->overlap_box(*f.scene, {8192, 0, 0}, {1, 1, 1}));
    PhysicsQueryFilter invalid;
    invalid.layer = 16;
    CHECK_FALSE(f.world->raycast(*f.scene, {0, 0, 0}, {1, 0, 0}, invalid));
    invalid.layer = 0;
    invalid.mask = 65536;
    CHECK_FALSE(f.world->overlap_sphere(*f.scene, {0, 0, 0}, 1, invalid));
    invalid.mask = 0xffff;
    CHECK_FALSE(f.world->raycast(*f.scene, {0, 0, 0}, {1, 0, 0}, invalid, 0));
    CHECK_FALSE(f.world->raycast(*f.scene, {0, 0, 0}, {1, 0, 0}, invalid, 257));
    CHECK_FALSE(f.world->overlap_box(*other, {0, 0, 0}, {1, 1, 1}));
    bool foreign_rejected = false;
    std::thread foreign(
        [&] { foreign_rejected = !f.world->raycast(*f.scene, {0, 0, 0}, {10, 0, 0}); });
    foreign.join();
    CHECK(foreign_rejected);
    CHECK_FALSE(take(f.world->stats()).faulted);

    take(f.world->remove_body(*f.scene, moving.second));
    invalid.ignore = moving.second;
    CHECK_FALSE(f.world->raycast(*f.scene, {0, 0, 0}, {10, 0, 0}, invalid));
    CHECK(take(f.world->raycast(*f.scene, {0, 0, 0}, {10, 0, 0})).hits.empty());
    CHECK(initial.hits[0].body == moving.second);
    take(f.world->end_simulation(*f.scene));
    take(f.scene->destroy_entity(moving.first));
    const auto replacement = f.add(2, mover(), {4, 0, 0});
    CHECK(replacement.first != moving.first);
    CHECK(replacement.second.generation != moving.second.generation);
    CHECK_FALSE(f.scene->resolve(initial.hits[0].entity));
    take(f.world->begin_simulation(*f.scene));
    CHECK_FALSE(f.world->overlap_sphere(*f.scene, {4, 0, 0}, 1, invalid));
    take(f.world->close());
    CHECK_FALSE(f.world->raycast(*f.scene, {0, 0, 0}, {10, 0, 0}));
}
