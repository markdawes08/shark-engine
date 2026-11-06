// Engine-authored collision fixtures; project MIT license.
#include <shark/physics.hpp>
#include <shark/scene.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <string>
#include <thread>

namespace {
using namespace shark;
template <typename T> T take(Result<T> result) {
    if (!result)
        INFO(result.error().message);
    REQUIRE(result);
    return std::move(result).value();
}
template <typename Id> Id id(unsigned value) {
    std::string text{"65c00000-0000-4000-8000-000000000000"};
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 15U];
        value >>= 4U;
    }
    return Id{take(StableId::parse(text))};
}
std::unique_ptr<Scene> region() {
    return take(Scene::create(
        RegionSceneScope{id<WorldId>(1), {id<RegionId>(2), id<RegionInstanceId>(3), 1}}));
}
PhysicsWorldConfig config() {
    PhysicsWorldConfig result;
    result.tick_hz = 20;
    result.collision_substeps = 3;
    result.max_bodies = 16;
    result.max_body_pairs = 128;
    result.max_contact_constraints = 128;
    return result;
}
PhysicsStaticMesh ground() {
    return {{{-4, 0, -4}, {-4, 0, 4}, {4, 0, 4}, {4, 0, -4}}, {{0, 1, 2}, {0, 2, 3}}};
}
EntityHandle actor(Scene& scene, unsigned number, ColliderShape shape, double x) {
    EntitySeed seed{id<WorldEntityId>(number)};
    seed.transform.translation = {x, 2, 0};
    seed.rigid_body = RigidBody{};
    seed.rigid_body->motion = BodyMotion::dynamic_body;
    seed.rigid_body->shape = shape;
    seed.rigid_body->restitution = 0;
    return take(scene.create_entity(seed));
}
} // namespace

TEST_CASE("Native static mesh geometry supports sphere and box contacts without source components",
          "[static-mesh-physics]") {
    const auto scene = region();
    const auto sphere = actor(*scene, 11, ColliderShape::sphere, -1);
    const auto box = actor(*scene, 12, ColliderShape::box, 1);
    const auto physics = take(PhysicsWorld::create(config()));
    take(physics->create_body(*scene, sphere));
    take(physics->create_body(*scene, box));
    take(physics->begin_simulation(*scene));
    const auto mesh = take(
        physics->create_static_mesh_body(*scene, id<WorldEntityId>(4), Name{"Terrain"}, ground()));
    const auto state = take(physics->body_state(*scene, mesh));
    CHECK(state.geometry == PhysicsBodyGeometry::static_mesh);
    CHECK_FALSE(state.settings);
    CHECK(state.triangle_count == 2);
    CHECK(state.motion == BodyMotion::static_body);
    CHECK(state.inverse_mass == 0);
    CHECK_FALSE(state.is_trigger);
    const auto proxy = take(scene->entity_state(state.entity.handle));
    CHECK_FALSE(proxy.rigid_body);
    CHECK_FALSE(proxy.script);
    CHECK_FALSE(proxy.mesh_renderer);
    CHECK(proxy.local_transform == Transform{});
    CHECK_FALSE(physics->set_static_pose(*scene, mesh, Transform{}));
    CHECK_FALSE(physics->set_velocity(*scene, mesh, {}));
    bool sphere_contact = false, box_contact = false;
    for (unsigned tick = 0; tick < 60; ++tick) {
        take(physics->step(*scene));
        for (const auto& event : take(physics->take_events(*scene))) {
            if (event.phase != PhysicsContactPhase::enter)
                continue;
            if (event.first.body != mesh && event.second.body != mesh)
                continue;
            const auto& peer = event.first.body == mesh ? event.second : event.first;
            sphere_contact = sphere_contact || peer.entity.handle == sphere;
            box_contact = box_contact || peer.entity.handle == box;
        }
    }
    CHECK(sphere_contact);
    CHECK(box_contact);
    // Existing primitive ground integration uses this same native slop envelope.
    CHECK(take(scene->entity_state(sphere)).local_transform.translation.y ==
          Catch::Approx(0.5).margin(0.04));
    CHECK(take(scene->entity_state(box)).local_transform.translation.y ==
          Catch::Approx(0.5).margin(0.04));
    const auto ray = take(physics->raycast(*scene, {3, 2, 0}, {3, -1, 0}));
    REQUIRE(ray.hits.size() == 1);
    CHECK(ray.hits[0].body == mesh);
    CHECK(ray.hits[0].entity == state.entity);
    CHECK(ray.hits[0].fraction == Catch::Approx(2.0 / 3.0).margin(1e-6));
    REQUIRE(ray.hits[0].normal);
    CHECK(ray.hits[0].normal->y == Catch::Approx(1).margin(1e-6));
    const auto stats = take(physics->stats());
    CHECK(stats.temporary_high_water_bytes <= stats.temporary_required_bytes);
    take(physics->remove_body(*scene, mesh));
    const auto exits = take(physics->take_events(*scene));
    REQUIRE(exits.size() == 2);
    for (const auto& event : exits) {
        CHECK(event.phase == PhysicsContactPhase::exit);
        CHECK(event.cause == PhysicsContactCause::body_removed);
        CHECK((event.first.body == mesh || event.second.body == mesh));
    }
    CHECK_FALSE(scene->resolve(state.entity));
    CHECK_FALSE(physics->body_state(*scene, mesh));
    const auto recreated = take(physics->create_static_mesh_body(
        *scene, id<WorldEntityId>(4), Name{"New terrain incarnation"}, ground()));
    CHECK(recreated != mesh);
    CHECK(take(physics->body_state(*scene, recreated)).entity != state.entity);
    take(physics->remove_body(*scene, recreated));
    take(physics->end_simulation(*scene));
}

TEST_CASE("Static mesh queries keep bilateral filters and owned body identity",
          "[static-mesh-physics]") {
    const auto scene = region();
    const auto physics = take(PhysicsWorld::create(config()));
    take(physics->begin_simulation(*scene));
    const auto mesh = take(physics->create_static_mesh_body(
        *scene, id<WorldEntityId>(4), Name{"Filtered terrain"}, ground(), {0.3, 0.1, 4, 1U << 2U}));
    const auto state = take(physics->body_state(*scene, mesh));
    CHECK(state.collision_layer == 4);
    CHECK(state.collision_mask == (1U << 2U));
    CHECK(state.friction == Catch::Approx(0.3));
    CHECK(state.restitution == Catch::Approx(0.1));
    PhysicsQueryFilter filter{2, 1U << 4U, false};
    CHECK(take(physics->raycast(*scene, {0, 2, 0}, {0, -1, 0}, filter)).hits.size() == 1);
    CHECK(take(physics->overlap_sphere(*scene, {0, 0.1, 0}, 0.5, filter)).hits.size() == 1);
    filter.layer = 1;
    CHECK(take(physics->raycast(*scene, {0, 2, 0}, {0, -1, 0}, filter)).hits.empty());
    filter.layer = 2;
    filter.ignore = mesh;
    CHECK(take(physics->raycast(*scene, {0, 2, 0}, {0, -1, 0}, filter)).hits.empty());
    take(physics->remove_body(*scene, mesh));
    CHECK_FALSE(physics->raycast(*scene, {0, 2, 0}, {0, -1, 0}, filter));
    take(physics->end_simulation(*scene));
}

TEST_CASE("Static mesh admission rejects malformed geometry without proxy publication",
          "[static-mesh-physics]") {
    const auto scene = region();
    const auto physics = take(PhysicsWorld::create(config()));
    CHECK_FALSE(
        physics->create_static_mesh_body(*scene, id<WorldEntityId>(4), Name{"Terrain"}, ground()));
    take(physics->begin_simulation(*scene));
    const auto before = take(scene->stats());
    const auto reject = [&](const PhysicsStaticMesh& mesh,
                            PhysicsStaticMeshSettings material = {}) {
        CHECK_FALSE(physics->create_static_mesh_body(*scene, id<WorldEntityId>(4), Name{"Terrain"},
                                                     mesh, material));
        CHECK(take(scene->stats()).revision == before.revision);
        CHECK(take(scene->stats()).live_entities == 0);
        CHECK(take(physics->stats()).bodies == 0);
    };
    auto bad = ground();
    bad.vertices.clear();
    reject(bad);
    bad = ground();
    bad.triangles.clear();
    reject(bad);
    bad = ground();
    bad.triangles[0][0] = 4;
    reject(bad);
    bad = ground();
    bad.triangles[0][1] = bad.triangles[0][0];
    reject(bad);
    bad = ground();
    bad.triangles.push_back({2, 1, 0});
    reject(bad);
    bad = ground();
    bad.vertices[0].x = std::numeric_limits<double>::quiet_NaN();
    reject(bad);
    bad = ground();
    bad.vertices[0].x = physics_local_position_limit + 1;
    reject(bad);
    bad = {{{4096, 0, 0}, {4096 + 1e-7, 0, 0}, {4096, 0, 1}}, {{0, 1, 2}}};
    reject(bad); // Distinct doubles collapse before native float triangle creation.
    bad = ground();
    bad.vertices.resize(physics_static_mesh_maximum_vertices + 1);
    reject(bad);
    bad = ground();
    bad.triangles.resize(physics_static_mesh_maximum_triangles + 1);
    reject(bad);
    reject(ground(), {-1, 0, 0, 0xffff});
    reject(ground(), {0.5, 0, 16, 0xffff});
    reject(ground(), {0.5, 0, 0, 0x10000});
    bool rejected_thread = false;
    const auto triangle = ground();
    std::thread other([&] {
        const auto result = physics->create_static_mesh_body(*scene, id<WorldEntityId>(4),
                                                             Name{"Terrain"}, triangle);
        rejected_thread = !result && result.error().code == ErrorCode::conflict;
    });
    other.join();
    CHECK(rejected_thread);
    take(physics->end_simulation(*scene));
    const auto authored = take(Scene::create(AuthoredSceneScope{id<SceneId>(7)}));
    take(physics->begin_simulation(*authored));
    CHECK_FALSE(physics->create_static_mesh_body(*authored, id<WorldEntityId>(4), Name{"Terrain"},
                                                 triangle));
    take(physics->end_simulation(*authored));
}

TEST_CASE("Static mesh world triangle budgets and native teardown stay bounded",
          "[static-mesh-physics]") {
    PhysicsStaticMesh grid;
    for (unsigned z = 0; z <= 32; ++z)
        for (unsigned x = 0; x <= 32; ++x)
            grid.vertices.push_back({static_cast<double>(x), 0, static_cast<double>(z)});
    for (std::uint32_t z = 0; z < 32; ++z)
        for (std::uint32_t x = 0; x < 32; ++x) {
            const auto a = z * 33 + x;
            grid.triangles.push_back({a, a + 33, a + 34});
            grid.triangles.push_back({a, a + 34, a + 1});
        }
    auto scene = region();
    const auto physics = take(PhysicsWorld::create(config()));
    take(physics->begin_simulation(*scene));
    std::vector<PhysicsBodyHandle> bodies;
    for (unsigned index = 0; index < 8; ++index)
        bodies.push_back(take(physics->create_static_mesh_body(
            *scene, id<WorldEntityId>(20 + index), Name{"Grid"}, grid)));
    const auto before = take(scene->stats());
    CHECK_FALSE(physics->create_static_mesh_body(*scene, id<WorldEntityId>(40), Name{"Over budget"},
                                                 ground()));
    CHECK(take(scene->stats()).revision == before.revision);
    CHECK(take(physics->stats()).bodies == 8);
    take(physics->remove_body(*scene, bodies.back()));
    const auto accepted = take(physics->create_static_mesh_body(*scene, id<WorldEntityId>(40),
                                                                Name{"Budget released"}, ground()));
    CHECK(take(physics->body_state(*scene, accepted)).triangle_count == 2);
    // Destroying the Scene before native owners remains safe: physics retains
    // owned checked references and never borrows the Scene for native close.
    scene.reset();
    take(physics->close());
    CHECK(take(physics->stats()).closed);
    CHECK(take(physics->stats()).bodies == 0);
}
