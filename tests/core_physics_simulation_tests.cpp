#include <shark/physics.hpp>
#include <shark/scene.hpp>
#include <shark/scene_render.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <limits>
#include <numbers>
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
    std::string text = "18c00000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 15U];
        value >>= 4U;
    }
    return Id{take(StableId::parse(text))};
}
std::unique_ptr<Scene> scene() {
    return take(Scene::create(AuthoredSceneScope{id<SceneId>(1)}));
}
SceneRevision revision(const Scene& owner) {
    const auto stats = take(owner.stats());
    return {stats.instance, stats.revision};
}
EntityHandle body(Scene& owner, unsigned value, BodyMotion motion, math::Vec3 position) {
    EntitySeed seed{id<AuthoredEntityId>(value)};
    seed.rigid_body = RigidBody{};
    seed.rigid_body->motion = motion;
    seed.transform.translation = position;
    return take(owner.create_entity(seed));
}
Transform pose(const Scene& owner, EntityHandle handle) {
    return take(owner.entity_state(handle)).local_transform;
}
MonotonicTime time_at(std::uint64_t frame, std::uint64_t rate) {
    return MonotonicTime{static_cast<std::int64_t>((frame * 1'000'000'000ULL + rate - 1) / rate)};
}
} // namespace

TEST_CASE(
    "Simulation admission and weak Scene writer leases reject incomplete and conflicting work",
    "[core-physics]") {
    auto owner = scene();
    auto first = body(*owner, 2, BodyMotion::dynamic_body, {0, 10, 0});
    auto second = body(*owner, 3, BodyMotion::static_body, {20, 0, 0});
    auto small = take(PhysicsWorld::create({.temporary_bytes = physics_minimum_temporary_bytes}));
    take(small->create_body(*owner, first));
    CHECK(take(small->stats()).temporary_required_bytes > physics_minimum_temporary_bytes);
    const auto before = revision(*owner);
    CHECK_FALSE(small->begin_simulation(*owner));
    CHECK(revision(*owner) == before);
    CHECK_FALSE(take(small->stats()).faulted);
    auto world = take(PhysicsWorld::create());
    const auto token = take(world->create_body(*owner, first));
    CHECK_FALSE(world->begin_simulation(*owner));
    take(world->create_body(*owner, second));
    take(world->begin_simulation(*owner));
    CHECK_FALSE(owner->set_transform(first, {}));
    CHECK_FALSE(owner->set_name(first, Name{"Competing"}));
    CHECK_FALSE(owner->defer_destroy(first));
    CHECK_FALSE(owner->defer_set_transform(first, {}));
    CHECK_FALSE(owner->apply_deferred());
    CHECK_FALSE(world->remove_body(token));
    CHECK_FALSE(world->create_body(*owner, first));
    CHECK_FALSE(world->step());
    CHECK_FALSE(small->begin_simulation(*owner));
    bool rejected = false;
    std::thread foreign([&] { rejected = !world->step(*owner); });
    foreign.join();
    CHECK(rejected);
    CHECK(revision(*owner) == before);
    take(world->step(*owner));
    take(world->end_simulation(*owner));
    take(owner->set_name(first, Name{"Editable"}));
    take(world->begin_simulation(*owner));
    take(world->close());
    take(owner->set_transform(first, {})); // Expired weak lease restores ordinary writes.
    CHECK_FALSE(take(world->stats()).simulating);
    auto final = take(PhysicsWorld::create());
    take(final->create_body(*owner, first));
    take(final->create_body(*owner, second));
    take(final->begin_simulation(*owner));
    owner.reset();
    take(final->close()); // No Scene pointer retained.
}

TEST_CASE("Fixed boundaries give physics dynamic poses and explicit static and kinematic authority",
          "[core-physics]") {
    auto owner = scene();
    const auto dynamic = body(*owner, 2, BodyMotion::dynamic_body, {0, 10, 0});
    const auto fixed = body(*owner, 3, BodyMotion::static_body, {10, 0, 0});
    const auto kinematic = body(*owner, 4, BodyMotion::kinematic_body, {20, 4, 0});
    EntitySeed child_seed{id<AuthoredEntityId>(5)};
    child_seed.parent = dynamic;
    child_seed.transform.translation.y = 2;
    const auto child = take(owner->create_entity(child_seed));
    auto world = take(PhysicsWorld::create({.collision_substeps = 2}));
    const auto d = take(world->create_body(*owner, dynamic));
    const auto s = take(world->create_body(*owner, fixed));
    const auto k = take(world->create_body(*owner, kinematic));
    take(world->begin_simulation(*owner));
    auto static_pose = pose(*owner, fixed);
    static_pose.translation.y = 5;
    take(world->set_static_pose(*owner, s, static_pose));
    auto target = pose(*owner, kinematic);
    target.translation.x += 1;
    target.rotation = glm::angleAxis(0.3, math::Vec3{0, 1, 0});
    take(world->set_kinematic_target(*owner, k, target));
    const auto initial = pose(*owner, dynamic);
    const auto before = revision(*owner);
    take(world->step(*owner));
    CHECK(revision(*owner).revision == before.revision + 1);
    CHECK(pose(*owner, dynamic).translation.y < initial.translation.y);
    CHECK(pose(*owner, fixed) == static_pose);
    CHECK(pose(*owner, kinematic).translation.x == Catch::Approx(21).margin(1e-5));
    CHECK(std::abs(glm::dot(pose(*owner, kinematic).rotation, target.rotation)) ==
          Catch::Approx(1).margin(1e-5));
    const auto child_state = take(owner->entity_state(child));
    CHECK(child_state.world_matrix[3].y == Catch::Approx(pose(*owner, dynamic).translation.y + 2));
    const auto observed = take(world->body_state(*owner, d));
    const auto interpolated = take(world->render_poses(*owner, 0.5));
    CHECK(interpolated.poses[0].local.translation.y ==
          Catch::Approx((initial.translation.y + observed.pose.translation.y) / 2));
    CHECK(interpolated.poses[1].local == static_pose);
    const auto committed = revision(*owner);
    CHECK_FALSE(world->render_poses(*owner, -0.1));
    CHECK(revision(*owner) == committed);
    auto next = pose(*owner, kinematic);
    next.translation.x += 0.25;
    take(world->set_kinematic_target(*owner, k, next));
    next.translation.y = std::numeric_limits<double>::quiet_NaN();
    CHECK_FALSE(world->set_kinematic_target(*owner, k, next));
    take(world->step(*owner));
    CHECK(pose(*owner, kinematic).translation.x == Catch::Approx(21).margin(1e-5));
    auto reset = initial;
    reset.translation.y = 12;
    CHECK_FALSE(world->set_static_pose(*owner, d, reset));
    CHECK_FALSE(world->teleport(*owner, k, reset));
    reset.scale = {2, 2, 2};
    CHECK_FALSE(world->teleport(*owner, d, reset));
    reset.scale = {1, 1, 1};
    take(world->teleport(*owner, d, reset));
    CHECK(take(world->render_poses(*owner, 0)).poses[0].local == reset);
    take(world->step(*owner));
    // Teleport clears old falling velocity. Two collision substeps are distinct
    // from one 60 Hz tick: semi-implicit displacement = g*(dt/2)^2*(1+2).
    CHECK(pose(*owner, dynamic).translation.y ==
          Catch::Approx(12 - 9.81 * 3 / (120 * 120)).margin(1e-5));
    const auto stats = take(world->stats());
    CHECK(stats.temporary_high_water_bytes > 0);
    CHECK(stats.temporary_high_water_bytes <= stats.temporary_required_bytes);
    CHECK(stats.temporary_required_bytes <= stats.config.temporary_bytes);
}

TEST_CASE(
    "Existing offline schedules preserve fixed physics across render rates and paused stepping",
    "[core-physics]") {
    std::optional<double> reference;
    for (const std::uint64_t fps : {30U, 60U, 144U, 240U}) {
        auto owner = scene();
        const auto entity = body(*owner, 2, BodyMotion::dynamic_body, {0, 10, 0});
        auto world = take(PhysicsWorld::create());
        take(world->create_body(*owner, entity));
        take(world->begin_simulation(*owner));
        auto schedule = take(OfflineScheduler::create(MonotonicTime{0}));
        for (std::uint64_t frame = 1; frame <= 2 * fps; ++frame) {
            const auto pass = take(world->poll_offline(*owner, *schedule, time_at(frame, fps)));
            CHECK(pass.discarded == 0);
            const auto unchanged = revision(*owner);
            take(world->render_poses(*owner, pass.interpolation_alpha()));
            CHECK(revision(*owner) == unchanged);
        }
        CHECK(take(world->stats()).completed_ticks == 120);
        const auto y = pose(*owner, entity).translation.y;
        CHECK(y == Catch::Approx(10 - 9.81 * 120 * 121 / (2 * 60 * 60)).margin(1e-3));
        if (reference)
            CHECK(y == Catch::Approx(*reference).margin(1e-5));
        reference = y;
        const auto before_pause = revision(*owner);
        auto paused = take(world->poll_offline(*owner, *schedule, time_at(3, 1), {.paused = true}));
        CHECK(paused.executed == 0);
        CHECK(revision(*owner) == before_pause);
        auto stepped = take(world->poll_offline(*owner, *schedule, time_at(4, 1),
                                                {.paused = true, .single_step = true}));
        CHECK(stepped.executed == 1);
        CHECK(stepped.interpolation_alpha() == 0);
        CHECK(take(world->stats()).completed_ticks == 121);
    }
}

TEST_CASE("Authoritative scheduler debt keeps 20 Hz ticks distinct from three collision substeps",
          "[core-physics]") {
    auto owner = take(Scene::create(
        RegionSceneScope{id<WorldId>(1), {id<RegionId>(2), id<RegionInstanceId>(3), 1}}));
    EntitySeed seed{id<WorldEntityId>(4)};
    seed.rigid_body = RigidBody{};
    seed.rigid_body->motion = BodyMotion::dynamic_body;
    seed.transform.translation.y = 10;
    const auto entity = take(owner->create_entity(seed));
    auto world = take(PhysicsWorld::create({.tick_hz = 20, .collision_substeps = 3}));
    take(world->create_body(*owner, entity));
    take(world->begin_simulation(*owner));
    auto wrong = take(ServerScheduler::create({.tick_rate_hz = 60}, MonotonicTime{0}));
    CHECK_FALSE(world->poll_server(*owner, *wrong, time_at(1, 1)));
    CHECK(wrong->backlog().pending == 0);
    auto offline = take(OfflineScheduler::create(MonotonicTime{0}));
    CHECK_FALSE(world->poll_offline(*owner, *offline, time_at(1, 1)));
    auto schedule = take(ServerScheduler::create({}, MonotonicTime{0}));
    const auto first = take(world->poll_server(*owner, *schedule, time_at(1, 1)));
    CHECK(first.executed == 8);
    CHECK(first.pending == 12);
    CHECK(first.discarded == 0);
    take(world->poll_server(*owner, *schedule, time_at(1, 1)));
    take(world->poll_server(*owner, *schedule, time_at(1, 1)));
    CHECK(take(world->stats()).completed_ticks == 20);
    CHECK(pose(*owner, entity).translation.y ==
          Catch::Approx(10 - 9.81 * 60 * 61 / (2 * 60 * 60)).margin(1e-3));
    CHECK(schedule->next_tick() == 20);
    auto mismatched = take(ServerScheduler::create({}, time_at(1, 1)));
    CHECK_FALSE(world->poll_server(*owner, *mismatched, time_at(2, 1)));
    CHECK(mismatched->backlog().pending == 0);
}

TEST_CASE(
    "Native contacts settle on static geometry and failed updates retain last committed Scene",
    "[core-physics]") {
    SECTION("Exact arena admission boundary with 64 active bodies and 16 substeps") {
        auto owner = scene();
        PhysicsWorldConfig config;
        config.max_bodies = 64;
        config.max_body_pairs = 128;
        config.max_contact_constraints = 128;
        config.collision_substeps = 16;
        auto probe = take(PhysicsWorld::create(config));
        const auto required = take(probe->stats()).temporary_required_bytes;
        take(probe->close());
        config.temporary_bytes = required - 16;
        auto short_arena = take(PhysicsWorld::create(config));
        config.temporary_bytes = required;
        auto world = take(PhysicsWorld::create(config));
        for (unsigned index = 0; index < 64; ++index) {
            const auto entity = body(*owner, index + 2, BodyMotion::dynamic_body,
                                     {static_cast<double>(index) * 2, 10, 0});
            take(short_arena->create_body(*owner, entity));
            take(world->create_body(*owner, entity));
        }
        CHECK_FALSE(short_arena->begin_simulation(*owner));
        CHECK_FALSE(take(short_arena->stats()).faulted);
        take(world->begin_simulation(*owner));
        take(world->step(*owner));
        CHECK(take(world->stats()).bodies == 64);
        CHECK(take(world->stats()).temporary_high_water_bytes <= required);
    }
    SECTION("Real contacts and bounded temporary work") {
        auto owner = scene();
        const auto floor = body(*owner, 2, BodyMotion::static_body, {0, -0.5, 0});
        auto ground = RigidBody{};
        ground.half_extents = {20, 0.5, 20};
        take(owner->set_rigid_body(floor, ground));
        const auto falling = body(*owner, 3, BodyMotion::dynamic_body, {0, 5, 0});
        auto ball = RigidBody{};
        ball.motion = BodyMotion::dynamic_body;
        ball.shape = ColliderShape::sphere;
        take(owner->set_rigid_body(falling, ball));
        auto world = take(PhysicsWorld::create());
        take(world->create_body(*owner, floor));
        take(world->create_body(*owner, falling));
        take(world->begin_simulation(*owner));
        for (int tick = 0; tick < 180; ++tick)
            take(world->step(*owner));
        CHECK(pose(*owner, falling).translation.y == Catch::Approx(0.5).margin(0.04));
        CHECK(pose(*owner, floor).translation.y == -0.5);
        CHECK(take(world->stats()).temporary_high_water_bytes <=
              take(world->stats()).temporary_required_bytes);
    }
    SECTION("Post-native output bounds fault without a partial Scene publication") {
        auto owner = scene();
        const auto entity =
            body(*owner, 2, BodyMotion::dynamic_body, {0, physics_local_position_limit, 0});
        auto world = take(PhysicsWorld::create({.gravity = {0, 100, 0}}));
        take(world->create_body(*owner, entity));
        take(world->begin_simulation(*owner));
        const auto before = revision(*owner);
        CHECK_FALSE(world->step(*owner));
        CHECK(take(world->stats()).faulted);
        CHECK(take(world->stats()).completed_ticks == 0);
        CHECK(revision(*owner) == before);
        CHECK(pose(*owner, entity).translation.y == physics_local_position_limit);
        CHECK_FALSE(world->step(*owner));
        CHECK_FALSE(world->end_simulation(*owner));
        take(world->close());
        take(owner->set_name(entity, Name{"Reconcile explicitly"}));
    }
}

TEST_CASE("Interpolated roots recompose render subtrees without writing Scene and retain revision "
          "provenance",
          "[core-physics]") {
    auto owner = scene();
    EntitySeed camera_seed{id<AuthoredEntityId>(2)};
    camera_seed.camera = Camera{};
    const auto camera = take(owner->create_entity(camera_seed));
    const auto root = body(*owner, 3, BodyMotion::kinematic_body, {0, 0, -5});
    auto initial_pose = pose(*owner, root);
    initial_pose.rotation = glm::angleAxis(170 * std::numbers::pi / 180, math::Vec3{0, 1, 0});
    take(owner->set_transform(root, initial_pose));
    const auto other = body(*owner, 4, BodyMotion::kinematic_body, {20, 0, -5});
    EntitySeed visual{id<AuthoredEntityId>(5)};
    visual.parent = root;
    visual.transform.translation.y = 1;
    visual.transform.scale = {1, 2, -1};
    visual.mesh_renderer = MeshRenderer{builtin_cube_asset_id()};
    const auto child = take(owner->create_entity(visual));
    auto world = take(PhysicsWorld::create({.gravity = {0, 0, 0}}));
    const auto a = take(world->create_body(*owner, root));
    const auto b = take(world->create_body(*owner, other));
    take(world->begin_simulation(*owner));
    auto target = pose(*owner, root);
    target.translation.x = 1;
    target.rotation = glm::angleAxis(190 * std::numbers::pi / 180, math::Vec3{0, 1, 0});
    take(world->set_kinematic_target(*owner, a, target));
    target = pose(*owner, other);
    target.translation.x = 19;
    take(world->set_kinematic_target(*owner, b, target));
    take(world->step(*owner));
    const auto current = revision(*owner);
    RenderExtractionOptions options{camera, current};
    options.frustum_culling = false;
    options.physics_poses = take(world->render_poses(*owner, 0.5));
    CHECK(std::abs(glm::dot(options.physics_poses->poses[0].local.rotation,
                            glm::angleAxis(std::numbers::pi, math::Vec3{0, 1, 0}))) ==
          Catch::Approx(1).margin(1e-6));
    const auto assets = take(PreparedAssets::create());
    const auto rendered = take(extract_render_scene(*owner, *assets, options));
    REQUIRE(rendered.records().size() == 1);
    CHECK(rendered.records()[0].entity.handle == child);
    CHECK(rendered.records()[0].mirrored);
    CHECK(rendered.physics_tick() == 1);
    const auto expected = take(transform_matrix(options.physics_poses->poses[0].local)) *
                          take(transform_matrix(visual.transform));
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            CHECK(rendered.records()[0].world_matrix[col][row] ==
                  Catch::Approx(expected[col][row]).margin(1e-9));
    CHECK(revision(*owner) == current);
    auto malformed = options;
    malformed.physics_poses->poses.push_back(malformed.physics_poses->poses.front());
    CHECK_FALSE(extract_render_scene(*owner, *assets, malformed));
    malformed = options;
    malformed.transform_preview = RenderTransformOverride{root, {}};
    CHECK_FALSE(extract_render_scene(*owner, *assets, malformed));
    take(world->end_simulation(*owner));
    take(owner->set_name(root, Name{"Later"}));
    options.expected_revision = revision(*owner);
    CHECK_FALSE(extract_render_scene(*owner, *assets, options));
}
