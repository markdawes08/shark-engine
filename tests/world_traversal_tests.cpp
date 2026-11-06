// Original bounded terrain/streaming fixtures; project MIT license.
#include "shark/scene.hpp"
#include "shark/world_traversal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

namespace {
using namespace shark;
template <class T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.operation);
    }
    REQUIRE(result);
    return std::move(result).value();
}
template <class Id> Id id(char digit) {
    std::string text{"85000000-0000-4000-8000-000000000001"};
    text.back() = digit;
    return Id{take(StableId::parse(text))};
}
WorldCellKey key(bool second = false) {
    return {id<WorldId>('1'), {1'000'000'000 + (second ? 1 : 0), 0, 0}};
}
WorldPosition origin() {
    return {key().world, key().cell, {1020, 0, 100}};
}
WorldPosition surface(bool second, double x) {
    return {key().world, key(second).cell, {x, 0, 104}};
}
std::vector<std::byte> payload(bool second, std::uint64_t revision, bool geometry = true) {
    const double x = second ? 0 : 1020;
    WorldCellGeometry ground{{{x, 0, 100}, {x, 0, 108}, {x + 4, 0, 100}, {x + 4, 0, 108}},
                             {{0, 1, 2}, {2, 1, 3}},
                             {{0, 1, 2}, {2, 1, 3}}};
    PreparedWorldCell cell{
        key(second),
        revision,
        {id<SceneId>('2'), WorldPosition{key().world, key(second).cell, {0, 0, 0}}, {}}};
    if (geometry)
        cell.geometry = std::move(ground);
    return take(encode_world_cell(cell));
}
bool wait_ready(WorldCellResidency& owner, WorldCellJobId job) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto jobs = take(owner.jobs());
        const auto found =
            std::find_if(jobs.begin(), jobs.end(), [&](const auto& j) { return j.id == job; });
        REQUIRE(found != jobs.end());
        if (found->state == WorldCellJobState::ready)
            return true;
        std::this_thread::yield();
    }
    return false;
}
std::unique_ptr<WorldCellResidency> residency(bool geometry = true) {
    auto a = payload(false, 1, geometry), a2 = payload(false, 2, geometry),
         b = payload(true, 1, geometry);
    const auto first_key = key();
    return take(
        WorldCellResidency::create([a = std::move(a), a2 = std::move(a2), b = std::move(b),
                                    first_key](const WorldCellReadRequest& request,
                                               std::stop_token) -> Result<std::vector<std::byte>> {
            if (request.key == first_key)
                return request.content_revision == 1 ? a : a2;
            return b;
        }));
}
std::uint64_t load(WorldCellResidency& owner, bool second = false) {
    const auto job = take(owner.submit(key(second), 1, take(PreparedAssets::create())));
    REQUIRE(wait_ready(owner, job));
    CHECK(take(owner.drain()) == 1);
    const auto receipt = take(owner.take(job));
    REQUIRE(receipt.state == WorldCellJobState::published);
    REQUIRE(receipt.resident_generation);
    return *receipt.resident_generation;
}
struct Fixture {
    std::unique_ptr<Scene> scene;
    std::unique_ptr<PhysicsWorld> physics;
    std::unique_ptr<WorldCellResidency> content;
    std::unique_ptr<WorldTraversal> traversal;
    EntityHandle actor{};
    PhysicsBodyHandle body{};
    explicit Fixture(bool geometry = true) {
        scene = take(Scene::create(
            RegionSceneScope{key().world, {id<RegionId>('3'), id<RegionInstanceId>('4'), 1}}));
        EntitySeed seed{WorldEntityId{id<WorldEntityId>('5')}};
        seed.name = Name{"Pursuing NPC"};
        seed.transform.translation = {1, 1, 4};
        RigidBody settings;
        settings.motion = BodyMotion::dynamic_body;
        settings.shape = ColliderShape::sphere;
        settings.radius = .25;
        settings.friction = 0;
        seed.rigid_body = settings;
        actor = take(scene->create_entity(seed));
        PhysicsWorldConfig config;
        config.tick_hz = 20;
        config.collision_substeps = 3;
        config.max_bodies = 16;
        config.max_body_pairs = 64;
        config.max_contact_constraints = 64;
        config.temporary_bytes = 4 * 1024 * 1024;
        physics = take(PhysicsWorld::create(config));
        body = take(physics->create_body(*scene, actor));
        take(physics->begin_simulation(*scene));
        content = residency(geometry);
        traversal = take(WorldTraversal::create(*content, *physics, *scene, {origin(), 1}));
    }
    ~Fixture() {
        if (!traversal->close()) {
            static_cast<void>(physics->close());
            if (!traversal->close())
                std::terminate();
        }
    }
};
} // namespace

TEST_CASE("streamed collision and navigation gate a native NPC crossing at a distant seam",
          "[world-traversal]") {
    Fixture f;
    const auto a = load(*f.content), b = load(*f.content, true);
    const auto start = surface(false, 1021), goal = surface(true, 3);
    CHECK_FALSE(f.traversal->path(start, goal));
    const auto first = take(f.traversal->activate(key(), a, id<WorldEntityId>('6')));
    CHECK_FALSE(f.traversal->path(start, goal));
    const auto before = take(world_position_from_local(origin(), {3.75, .25, 4}, 8192));
    const auto after = take(world_position_from_local(origin(), {4.1, .25, 4}, 8192));
    CHECK_FALSE(take(f.traversal->allows_motion(before, after, .25)));
    const auto second = take(f.traversal->activate(key(true), b, id<WorldEntityId>('7')));
    CHECK(take(f.traversal->allows_motion(before, after, .25)));
    CHECK(take(f.content->usage()).pins == 2);
    CHECK_FALSE(f.content->evict(key(), a));
    const auto old_content = take(f.content->resident(key()));
    const auto update = take(f.content->submit(key(), 2, take(PreparedAssets::create())));
    REQUIRE(wait_ready(*f.content, update));
    CHECK(take(f.content->drain()) == 1);
    CHECK(take(f.content->take(update)).state == WorldCellJobState::failed);
    CHECK(take(f.content->resident(key())) == old_content);

    const auto hits = take(f.physics->raycast(*f.scene, {5, 2, 4}, {5, -2, 4}));
    REQUIRE(hits.hits.size() == 1);
    CHECK(hits.hits.front().body == second.collision);
    for (int tick = 0; tick < 20; ++tick)
        take(f.physics->step(*f.scene));
    const auto route = take(f.traversal->path(start, goal));
    auto cursor = take(f.traversal->begin(route));
    for (int tick = 0; tick < 80; ++tick) {
        const auto candidate = take(f.traversal->advance(route, cursor, .1));
        const auto target = take(world_position_relative_to(candidate.position, origin(), 8192));
        const auto state = take(f.physics->body_state(*f.scene, f.body));
        auto velocity = take(f.physics->velocity(*f.scene, f.body));
        velocity.linear.x = (target[0] - state.pose.translation.x) * 20;
        velocity.linear.z = (target[2] - state.pose.translation.z) * 20;
        take(f.physics->set_velocity(*f.scene, f.body, velocity));
        take(f.physics->step(*f.scene));
        const auto committed = take(f.physics->body_state(*f.scene, f.body));
        CHECK(std::abs(committed.pose.translation.x - target[0]) < .02);
        CHECK(std::abs(committed.pose.translation.z - target[2]) < .02);
        CHECK(committed.pose.translation.y > .2);
        CHECK(committed.pose.translation.y < .35);
        // This fixture adopts a steering cursor only after checking native motion.
        cursor = candidate;
    }
    const auto final = take(f.physics->body_state(*f.scene, f.body));
    CHECK(std::abs(final.pose.translation.x - 7) < .02);
    CHECK(std::abs(final.pose.translation.z - 4) < .02);
    const auto events = take(f.physics->take_events(*f.scene));
    CHECK(std::any_of(events.begin(), events.end(), [&](const auto& event) {
        return event.first.body == first.collision || event.second.body == first.collision;
    }));
    CHECK(std::any_of(events.begin(), events.end(), [&](const auto& event) {
        return event.first.body == second.collision || event.second.body == second.collision;
    }));
    take(f.traversal->deactivate(key(true), b));
    CHECK_FALSE(f.physics->body_state(*f.scene, second.collision));
    CHECK_FALSE(f.scene->find(id<WorldEntityId>('7')));
    CHECK_FALSE(f.traversal->begin(route));
    CHECK_FALSE(f.traversal->advance(route, cursor, .1));
    CHECK_FALSE(f.traversal->path(start, goal));
    take(f.traversal->close());
    CHECK(take(f.content->usage()).pins == 0);
    take(f.content->evict(key(), a));
    take(f.content->evict(key(true), b));
}

TEST_CASE("missing geometry and failed activation retain pins and readiness atomically",
          "[world-traversal]") {
    SECTION("readiness cannot be borrowed from another Scene or native world") {
        Fixture f;
        const auto foreign_scene = take(Scene::create(
            RegionSceneScope{key().world, {id<RegionId>('3'), id<RegionInstanceId>('8'), 1}}));
        const auto foreign_physics = take(PhysicsWorld::create());
        CHECK(f.traversal->check_context(*f.scene, *f.physics));
        CHECK_FALSE(f.traversal->check_context(*foreign_scene, *f.physics));
        CHECK_FALSE(f.traversal->check_context(*f.scene, *foreign_physics));
        CHECK_FALSE(f.traversal->check_context(*foreign_scene, *foreign_physics));
        take(f.traversal->close());
        CHECK_FALSE(f.traversal->check_context(*f.scene, *f.physics));
    }
    SECTION("legacy content remains a valid resident but cannot authorize traversal") {
        Fixture f(false);
        const auto a = load(*f.content);
        CHECK_FALSE(f.traversal->activate(key(), a, id<WorldEntityId>('6')));
        CHECK(take(f.content->usage()).pins == 0);
        CHECK(take(f.traversal->cells()).empty());
        CHECK(take(f.physics->stats()).bodies == 1);
    }
    SECTION("duplicate proxy identity cannot retire an existing tile or leak its new pin") {
        Fixture f;
        const auto a = load(*f.content), b = load(*f.content, true);
        const auto first = take(f.traversal->activate(key(), a, id<WorldEntityId>('6')));
        CHECK_FALSE(f.traversal->activate(key(true), b, id<WorldEntityId>('6')));
        CHECK(take(f.content->usage()).pins == 1);
        CHECK(take(f.traversal->cells()).size() == 1);
        CHECK(f.physics->body_state(*f.scene, first.collision));
        CHECK_FALSE(f.traversal->deactivate(key(), a + 1));
    }
}

TEST_CASE("residency pins are exact bounded receipts and survive admission close",
          "[world-traversal]") {
    const auto owner = residency();
    const auto generation = load(*owner);
    std::vector<WorldCellPin> pins;
    for (std::size_t i = 0; i < world_cell_maximum_pins; ++i)
        pins.push_back(take(owner->pin(key(), generation)));
    CHECK_FALSE(owner->pin(key(), generation));
    CHECK_FALSE(owner->pin(key(), generation + 1));
    auto foreign = pins.front();
    foreign.scope += 100;
    CHECK_FALSE(owner->unpin(foreign));
    take(owner->close());
    CHECK(take(owner->usage()).pins == world_cell_maximum_pins);
    for (const auto& pin : pins)
        take(owner->unpin(pin));
    CHECK_FALSE(owner->unpin(pins.front()));
    CHECK(take(owner->usage()).pins == 0);
    take(owner->evict(key(), generation));
}
