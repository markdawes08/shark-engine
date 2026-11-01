#include <shark/physics.hpp>
#include <shark/scene.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
    std::string text = "18d00000-0000-4000-8000-000000000001";
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
RigidBody sensor() {
    RigidBody body;
    body.is_trigger = true;
    body.half_extents = {1, 1, 1};
    return body;
}
RigidBody mover() {
    RigidBody body;
    body.motion = BodyMotion::kinematic_body;
    return body;
}
} // namespace

TEST_CASE("Bilateral collision masks affect real response and triggers detect without response",
          "[core-physics]") {
    for (int scenario = 0; scenario < 4; ++scenario) {
        CAPTURE(scenario);
        Fixture f;
        RigidBody floor;
        floor.half_extents = {20, 0.5, 20};
        floor.collision_layer = 2;
        floor.collision_mask = scenario == 1 ? 0 : 1U << 3U;
        floor.is_trigger = scenario == 3;
        const auto ground = f.add(2, floor, {0, -0.5, 0});
        RigidBody ball;
        ball.motion = BodyMotion::dynamic_body;
        ball.shape = ColliderShape::sphere;
        ball.collision_layer = 3;
        ball.collision_mask = scenario == 2 ? 0 : 1U << 2U;
        const auto falling = f.add(3, ball, {0, 4, 0});
        take(f.world->begin_simulation(*f.scene));
        for (int tick = 0; tick < 120; ++tick)
            f.step();
        const auto y = take(f.scene->entity_state(falling.first)).local_transform.translation.y;
        const auto events = take(f.world->take_events());
        if (scenario == 0) {
            CHECK(y == Catch::Approx(0.5).margin(0.04));
            REQUIRE_FALSE(events.empty());
            CHECK(events.front().phase == PhysicsContactPhase::enter);
            CHECK_FALSE(events.front().first.is_trigger);
        } else {
            CHECK(y < -10);
            if (scenario == 3) {
                REQUIRE(events.size() == 2);
                CHECK(events[0].phase == PhysicsContactPhase::enter);
                CHECK(events[1].phase == PhysicsContactPhase::exit);
                CHECK(events[0].first.is_trigger);
                CHECK(events[0].first.body == ground.second);
                CHECK(events[1].sequence == events[0].sequence + 1);
            } else
                CHECK(events.empty());
        }
    }
}

TEST_CASE("Trigger transitions are fixed-tick owned observations with bounded partial take",
          "[core-physics]") {
    Fixture f({.collision_substeps = 3, .gravity = {0, 0, 0}});
    f.add(2, sensor());
    const auto moving = f.add(3, mover(), {-2, 0, 0});
    take(f.world->begin_simulation(*f.scene));
    Transform target;
    target.translation.x = -0.5;
    take(f.world->set_kinematic_target(*f.scene, moving.second, target));
    f.step();
    for (int tick = 0; tick < 120; ++tick)
        f.step(); // Stable overlap outlasts the native sleep interval without false exits.
    CHECK(take(f.world->stats()).pending_events == 1);
    bool foreign_rejected = false;
    std::thread foreign([&] { foreign_rejected = !f.world->take_events(); });
    foreign.join();
    CHECK(foreign_rejected);
    CHECK_FALSE(f.world->take_events(0));
    const auto entered = take(f.world->take_events(1));
    REQUIRE(entered.size() == 1);
    CHECK(entered[0].completed_tick == 1);
    CHECK(entered[0].cause == PhysicsContactCause::simulation);
    CHECK(entered[0].second.entity.handle == moving.first);
    target.translation.x = 2;
    take(f.world->set_kinematic_target(*f.scene, moving.second, target));
    f.step();
    // Discrete contacts precede the final integration. The completed native
    // cache still contains the last substep contact; the next fixed tick retires it.
    CHECK(take(f.world->take_events()).empty());
    f.step();
    const auto exited = take(f.world->take_events());
    REQUIRE(exited.size() == 1);
    CHECK(exited[0].phase == PhysicsContactPhase::exit);
    CHECK(exited[0].completed_tick == 123);
    CHECK(exited[0].sequence == entered[0].sequence + 1);
    CHECK(take(f.world->take_events()).empty());
}

TEST_CASE("Active removal preserves queued participants across entity generation reuse and close",
          "[core-physics]") {
    Fixture f({.gravity = {0, 0, 0}});
    const auto trigger = f.add(2, sensor());
    f.add(3, mover());
    take(f.world->begin_simulation(*f.scene));
    f.step();
    const auto revision = take(f.scene->stats()).revision;
    take(f.world->remove_body(*f.scene, trigger.second));
    CHECK(take(f.scene->stats()).revision == revision + 1);
    CHECK_FALSE(take(f.scene->entity_state(trigger.first)).rigid_body);
    CHECK_FALSE(f.world->body_state(*f.scene, trigger.second));
    CHECK(take(f.world->stats()).pending_events == 2);
    const auto entered = take(f.world->take_events(1));
    REQUIRE(entered.size() == 1);
    take(f.world->end_simulation(*f.scene));
    take(f.scene->destroy_entity(trigger.first));
    const auto replacement = f.add(2, sensor());
    CHECK(replacement.first != trigger.first);
    CHECK(replacement.second.slot == trigger.second.slot);
    CHECK(replacement.second.generation != trigger.second.generation);
    take(f.world->begin_simulation(*f.scene));
    f.step();
    const auto removed = take(f.world->take_events(1));
    REQUIRE(removed.size() == 1);
    CHECK(removed[0].cause == PhysicsContactCause::body_removed);
    CHECK(removed[0].phase == PhysicsContactPhase::exit);
    CHECK(removed[0].first.body == trigger.second);
    CHECK(removed[0].first.entity == entered[0].first.entity);
    CHECK_FALSE(f.scene->resolve(removed[0].first.entity));
    CHECK(take(f.world->stats()).pending_events == 1);
    f.scene.reset();
    take(f.world->close());
    CHECK(take(f.world->stats()).discarded_events == 1);
    CHECK(take(f.world->stats()).pending_events == 0);
    CHECK(removed[0].first.entity.handle == trigger.first); // Owned after teardown.
}

TEST_CASE("Event backpressure rejects before movement and drains without losing the queued target",
          "[core-physics]") {
    Fixture f({.gravity = {0, 0, 0}, .max_contact_pairs = 1, .max_queued_events = 2});
    f.add(2, sensor());
    const auto moving = f.add(3, mover());
    take(f.world->begin_simulation(*f.scene));
    f.step();
    Transform target;
    target.translation.x = 2;
    take(f.world->set_kinematic_target(*f.scene, moving.second, target));
    const auto before = take(f.scene->stats());
    const auto blocked = f.world->step(*f.scene);
    REQUIRE_FALSE(blocked);
    CHECK(blocked.error().context.operation == "physics.events.capacity");
    CHECK_FALSE(take(f.world->stats()).faulted);
    CHECK(take(f.world->stats()).completed_ticks == 1);
    CHECK(take(f.scene->stats()).revision == before.revision);
    CHECK(take(f.scene->entity_state(moving.first)).local_transform.translation.x == 0);
    REQUIRE(take(f.world->take_events()).size() == 1);
    f.step();
    CHECK(take(f.scene->entity_state(moving.first)).local_transform.translation.x ==
          Catch::Approx(2).margin(1e-5));
    CHECK(take(f.world->take_events()).empty());
    f.step(); // Next collision evaluation observes the completed movement.
    const auto exited = take(f.world->take_events());
    REQUIRE(exited.size() == 1);
    CHECK(exited[0].phase == PhysicsContactPhase::exit);
}

TEST_CASE("Contact collector overflow faults without publishing a partial tick or events",
          "[core-physics]") {
    CHECK_FALSE(PhysicsWorld::create({.max_contact_pairs = 0}));
    CHECK_FALSE(PhysicsWorld::create({.max_contact_pairs = 2, .max_queued_events = 3}));
    Fixture f({.gravity = {0, 0, 0}, .max_contact_pairs = 1, .max_queued_events = 2});
    f.add(2, sensor());
    f.add(3, sensor(), {0.25, 0, 0});
    f.add(4, mover());
    take(f.world->begin_simulation(*f.scene));
    const auto before = take(f.scene->stats()).revision;
    CHECK_FALSE(f.world->step(*f.scene));
    CHECK(take(f.world->stats()).faulted);
    CHECK(take(f.world->stats()).completed_ticks == 0);
    CHECK(take(f.world->stats()).pending_events == 0);
    CHECK(take(f.scene->stats()).revision == before);
    CHECK(take(f.world->take_events()).empty());
    take(f.world->close());
}

TEST_CASE("Many actors sharing one trigger publish canonical batches and bulk removal exits",
          "[core-physics]") {
    Fixture f({.gravity = {0, 0, 0}, .max_contact_pairs = 16, .max_queued_events = 32});
    auto actor = mover();
    actor.collision_layer = 1;
    actor.collision_mask = 1U << 2U;
    for (unsigned index = 0; index < 16; ++index)
        f.add(index + 2, actor, {static_cast<double>(index) * 0.5 - 3.75, 0, 0});
    auto area = sensor();
    area.half_extents = {10, 10, 10};
    area.collision_layer = 2;
    area.collision_mask = 1U << 1U;
    const auto trigger = f.add(18, area);
    take(f.world->begin_simulation(*f.scene));
    f.step();
    const auto entered = take(f.world->take_events());
    REQUIRE(entered.size() == 16);
    for (std::size_t index = 0; index < entered.size(); ++index) {
        CHECK(entered[index].sequence == index + 1);
        CHECK(entered[index].first.body.slot == index);
        CHECK(entered[index].second.body == trigger.second);
        CHECK(entered[index].second.is_trigger);
        CHECK(entered[index].phase == PhysicsContactPhase::enter);
    }
    f.step();
    CHECK(take(f.world->take_events()).empty());
    take(f.world->remove_body(*f.scene, trigger.second));
    const auto exited = take(f.world->take_events());
    REQUIRE(exited.size() == 16);
    for (std::size_t index = 0; index < exited.size(); ++index) {
        CHECK(exited[index].sequence == index + 17);
        CHECK(exited[index].first.body == entered[index].first.body);
        CHECK(exited[index].second.body == trigger.second);
        CHECK(exited[index].phase == PhysicsContactPhase::exit);
        CHECK(exited[index].cause == PhysicsContactCause::body_removed);
    }
    CHECK(take(f.world->stats()).active_contact_pairs == 0);
    f.step(); // Late native removals cannot revive or duplicate the old participants.
    CHECK(take(f.world->take_events()).empty());
}
