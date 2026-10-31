#include <shark/physics.hpp>
#include <shark/scene.hpp>

#include <iostream>
#include <utility>

int main() {
    auto created = shark::PhysicsWorld::create();
    if (!created) {
        std::cerr << created.error().message << '\n';
        return 1;
    }
    auto world = std::move(created).value();
    for (int tick = 0; tick < 3; ++tick) {
        const auto stepped = world->step();
        if (!stepped) {
            std::cerr << stepped.error().message << '\n';
            return 1;
        }
    }
    const auto stats = world->stats();
    if (!stats || stats.value().completed_ticks != 3 || stats.value().bodies != 0 ||
        stats.value().closed || stats.value().faulted)
        return 1;
    const auto scene_id = shark::StableId::parse("50000000-0000-4000-8000-000000000001");
    const auto entity_id = shark::StableId::parse("50000000-0000-4000-8000-000000000002");
    if (!scene_id || !entity_id)
        return 1;
    auto scene = shark::Scene::create(shark::AuthoredSceneScope{shark::SceneId{scene_id.value()}});
    if (!scene)
        return 1;
    shark::EntitySeed seed{shark::AuthoredEntityId{entity_id.value()}};
    seed.rigid_body = shark::RigidBody{};
    seed.rigid_body->motion = shark::BodyMotion::dynamic_body;
    const auto entity = scene.value()->create_entity(seed);
    if (!entity)
        return 1;
    const auto body = world->create_body(*scene.value(), entity.value());
    if (!body || !world->body_state(*scene.value(), body.value()))
        return 1;
    if (!world->begin_simulation(*scene.value()))
        return 1;
    auto schedule = shark::OfflineScheduler::create(shark::MonotonicTime{0}, 3);
    if (!schedule)
        return 1;
    auto pass =
        world->poll_offline(*scene.value(), *schedule.value(), shark::MonotonicTime{16'666'667});
    if (!pass || pass.value().executed != 1)
        return 1;
    auto rendered = world->render_poses(*scene.value(), pass.value().interpolation_alpha());
    if (!rendered || rendered.value().poses.size() != 1 || !world->end_simulation(*scene.value()))
        return 1;
    if (!world->remove_body(body.value()))
        return 1;
    const auto closed = world->close();
    if (!closed) {
        std::cerr << closed.error().message << '\n';
        return 1;
    }
    return 0;
}
