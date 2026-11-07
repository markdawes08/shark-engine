#include <shark/server/region_image.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string>

namespace {
using namespace shark;
using namespace shark::server;
template <class T> T image_id(unsigned value) {
    std::string text = "a0500000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 15U];
        value >>= 4U;
    }
    return T{StableId::parse(text).value()};
}
RegionProcessImage empty_image() {
    const auto world = image_id<WorldId>(1);
    const auto first = image_id<RegionId>(2);
    const auto second = image_id<RegionId>(3);
    const RegionSceneScope scope{world, {first, image_id<RegionInstanceId>(4), 1}};
    const WorldPosition origin{world, {world_cell_max, 0, world_cell_min}, {1000, 0, 24}};
    const SceneRevision revision{11, 7};
    PhysicsWorldConfig config;
    config.tick_hz = 20;
    config.collision_substeps = 3;
    CombatRulesPortableState rules{
        {"scripts/image.lua", "function on_combat(k,t,a,b,d,s) return true,1,10 end"}, {}, {}, 9};
    rules.state[0] = std::numeric_limits<std::int64_t>::min();
    rules.state[7] = std::numeric_limits<std::int64_t>::max();
    return {{origin, 8},
            {{first, -10, 0, -10, 10, 110}, {second, 0, 10, -10, 10, 110}},
            {},
            {5, revision, scope, config, 17, 23, 2, {}, {}},
            {revision,
             scope,
             5,
             origin,
             12,
             {},
             17,
             MonotonicTime{40},
             ExecutionPermit{20, 3},
             {},
             {std::byte{0xee}}},
            {revision,
             scope,
             5,
             17,
             MonotonicTime{40},
             81,
             true,
             {},
             {9, false},
             ExecutionPermit{20, 3},
             {},
             {std::byte{0xdd}}},
            rules,
            {},
            {},
            {},
            {},
            {},
            123,
            image_id<RegionInstanceId>(6)};
}
} // namespace

TEST_CASE("Region process image has canonical bounded bytes and owns explicit portable state",
          "[server-region-image]") {
    auto original = empty_image();
    const auto encoded = encode_region_process_image(original);
    REQUIRE(encoded);
    const std::array<std::byte, 8> prefix{std::byte{0x53}, std::byte{0x48}, std::byte{0x52},
                                          std::byte{0x49}, std::byte{0},    std::byte{0},
                                          std::byte{0},    std::byte{2}};
    REQUIRE(encoded.value().size() > prefix.size());
    CHECK(std::equal(prefix.begin(), prefix.end(), encoded.value().begin()));
    const auto decoded = decode_region_process_image(encoded.value());
    REQUIRE(decoded);
    CHECK(decoded.value().frame == original.frame);
    CHECK(decoded.value().physics.completed_tick == 17);
    CHECK(decoded.value().physics.next_contact_sequence == 23);
    CHECK(decoded.value().physics.discarded_events == 2);
    CHECK(decoded.value().rules.source.text == original.rules.source.text);
    CHECK(decoded.value().rules.state == original.rules.state);
    CHECK(decoded.value().rules.evaluations == 9);
    CHECK(decoded.value().route_watermark == 123);
    CHECK(decoded.value().executor_instance == original.executor_instance);
    CHECK_FALSE(decoded.value().movement.bytes.empty());
    CHECK_FALSE(decoded.value().combat.bytes.empty());
    CHECK(encode_region_process_image(decoded.value()).value() == encoded.value());
    original.movement.bytes.clear();
    original.combat.bytes.assign(100, std::byte{0xab});
    CHECK(encode_region_process_image(original).value() == encoded.value());
    auto legacy = encoded.value();
    legacy.resize(legacy.size() - 40); // Limits and two empty v2 extension counts.
    legacy[7] = std::byte{1};
    const auto old = decode_region_process_image(legacy);
    REQUIRE(old);
    CHECK(old.value().frame == original.frame);
    CHECK(old.value().residency_limits.maximum_resident_cells == 8);
    CHECK(encode_region_process_image(old.value()).value() == encoded.value());
}

TEST_CASE("Region process image rejects unknown versions truncation trailing bytes and huge counts",
          "[server-region-image]") {
    const auto encoded = encode_region_process_image(empty_image());
    REQUIRE(encoded);
    auto changed = encoded.value();
    changed[7] = std::byte{3};
    CHECK_FALSE(decode_region_process_image(changed));
    for (const auto length :
         {std::size_t{0}, std::size_t{7}, std::size_t{80}, encoded.value().size() - 1})
        CHECK_FALSE(decode_region_process_image(std::span{encoded.value()}.first(length)));
    changed = encoded.value();
    changed.push_back(std::byte{0});
    CHECK_FALSE(decode_region_process_image(changed));
    changed = encoded.value();
    // Eight-byte header, explicit 64-byte WorldPosition and 8-byte generation.
    // The next count must reject before allocating any region records.
    std::fill(changed.begin() + 80, changed.begin() + 88, std::byte{0xff});
    CHECK_FALSE(decode_region_process_image(changed));
    auto unsupported = empty_image();
    unsupported.combat.script_instance = 44;
    CHECK_FALSE(encode_region_process_image(unsupported));
}

TEST_CASE("Region process image enforces aggregate byte capacity before publishing bytes",
          "[server-region-image]") {
    auto image = empty_image();
    for (unsigned index = 0; index < 9; ++index)
        image.assets.push_back({image_id<AssetId>(100 + index),
                                std::vector<std::byte>(prepared_mesh_maximum_payload_bytes)});
    CHECK_FALSE(encode_region_process_image(image));
    image.assets.clear();
    const std::vector<std::byte> oversized(region_process_image_maximum_bytes + 1);
    CHECK_FALSE(decode_region_process_image(oversized));
}

TEST_CASE("Region process image rejects malformed NPC intent and duplicate source bindings",
          "[server-region-image]") {
    auto image = empty_image();
    const EntityReference source{{image.physics.revision.scene_instance, 0, 1},
                                 image_id<WorldEntityId>(10),
                                 image.physics.scope,
                                 image.physics.scope.authority};
    image.npcs.push_back({source, {image.physics.world, 0, 1}, math::Vec3{1, 0, 0}});
    const auto encoded = encode_region_process_image(image);
    REQUIRE(encoded);
    REQUIRE(decode_region_process_image(encoded.value()));
    for (const auto invalid : {math::Vec3{1, 1, 0}, math::Vec3{movement_speed + 1, 0, 0},
                               math::Vec3{std::numeric_limits<double>::quiet_NaN(), 0, 0}}) {
        auto changed = image;
        changed.npcs.front().pending = invalid;
        CHECK_FALSE(encode_region_process_image(changed));
    }
    auto duplicate = image;
    duplicate.npcs.push_back(duplicate.npcs.front());
    CHECK_FALSE(encode_region_process_image(duplicate));
    duplicate.npcs.back().source.handle.slot = 1;
    duplicate.npcs.back().source.identity = image_id<WorldEntityId>(11);
    CHECK_FALSE(encode_region_process_image(duplicate)); // Same native body.
    auto changed = encoded.value();
    changed.resize(changed.size() - 41); // Empty limits/counts plus absent pursuit.
    changed[7] = std::byte{1};
    REQUIRE(decode_region_process_image(changed));
    // One NPC's final pending vector precedes empty peer/writer counts (16),
    // route watermark (8) and executor UUID (16). Change Y to an exact +1.
    const std::array<std::byte, 8> one{std::byte{0x3f}, std::byte{0xf0}, std::byte{0},
                                       std::byte{0},    std::byte{0},    std::byte{0},
                                       std::byte{0},    std::byte{0}};
    std::copy(one.begin(), one.end(), changed.end() - 56);
    CHECK_FALSE(decode_region_process_image(changed));
}

TEST_CASE("Region image v2 carries cell source residency and pursuit without local path cursors",
          "[server-region-image]") {
    auto image = empty_image();
    image.residency_limits = {2, 8 * 1024 * 1024, 16 * 1024 * 1024};
    const RegionCellFileSource file{std::filesystem::current_path().lexically_normal() /
                                        "region-image-cell.sharkcell",
                                    {},
                                    128};
    image.cells.push_back({{image.frame.origin.world, image.frame.origin.cell},
                           3,
                           image_id<WorldEntityId>(12),
                           {},
                           RegionCellState::unloaded,
                           file,
                           {0.3, 0.25, 2, 4}});
    const EntityReference source{{image.physics.revision.scene_instance, 0, 1},
                                 image_id<WorldEntityId>(10),
                                 image.physics.scope,
                                 image.physics.scope.authority};
    image.npcs.push_back(
        {source,
         {image.physics.world, 0, 1},
         {},
         RegionNpcPursuit{7, image_id<WorldEntityId>(20), image_id<net::CombatTargetId>(21), 2, 1,
                          9, 2.5, RegionPursuitStatus::awaiting_readiness}});
    const auto anchor = translate_world_position(image.frame.origin, {.5, 0, 0});
    REQUIRE(anchor);
    image.npcs.front().pursuit->steering_anchor = anchor.value();
    const auto encoded = encode_region_process_image(image);
    REQUIRE(encoded);
    const auto decoded = decode_region_process_image(encoded.value());
    REQUIRE(decoded);
    CHECK(decoded.value().cells.front().state == RegionCellState::unloaded);
    CHECK(decoded.value().cells.front().source == image.cells.front().source);
    CHECK(decoded.value().cells.front().settings.friction == 0.3);
    CHECK(decoded.value().cells.front().settings.collision_mask == 4);
    CHECK(decoded.value().npcs.front().pursuit == image.npcs.front().pursuit);
    CHECK(decoded.value().npcs.front().pursuit->steering_anchor == anchor.value());
    CHECK(decoded.value().residency_limits.maximum_resident_cells == 2);
    CHECK(encode_region_process_image(decoded.value()).value() == encoded.value());
    auto frame = image.frame;
    --frame.origin.cell.x;
    ++frame.generation;
    const auto shifted = reframe_region_process_image(image, frame);
    REQUIRE(shifted);
    CHECK(shifted.value().npcs.front().pursuit->steering_anchor == anchor.value());
    auto malformed = image;
    malformed.cells.front().source->path = "relative.sharkcell";
    CHECK_FALSE(encode_region_process_image(malformed));
    malformed = image;
    malformed.npcs.front().pursuit->request = 0;
    CHECK_FALSE(encode_region_process_image(malformed));
    malformed = image;
    malformed.npcs.front().pursuit->speed = movement_speed + 1;
    CHECK_FALSE(encode_region_process_image(malformed));
    malformed = image;
    malformed.npcs.front().pursuit->stop_distance = .5;
    CHECK_FALSE(encode_region_process_image(malformed));
    malformed = image;
    malformed.npcs.front().pursuit->steering_anchor->world = image_id<WorldId>(99);
    CHECK_FALSE(encode_region_process_image(malformed));
    malformed = image;
    malformed.residency_limits.maximum_resident_cells = 0;
    CHECK_FALSE(encode_region_process_image(malformed));
}

TEST_CASE("Legacy cell materials recover exact authored settings before immutable comparison",
          "[server-region-image]") {
    auto image = empty_image();
    const auto proxy = image_id<WorldEntityId>(12);
    const EntityReference reference{{image.physics.revision.scene_instance, 0, 1},
                                    proxy,
                                    image.physics.scope,
                                    image.physics.scope.authority};
    RigidBody settings;
    settings.friction = .3;
    settings.restitution = .2;
    settings.collision_layer = 2;
    settings.collision_mask = 4;
    const PhysicsStaticMesh mesh{{{-1, 0, -1}, {-1, 0, 1}, {1, 0, -1}}, {{0, 1, 2}}};
    const PhysicsBodyState native{reference,
                                  {},
                                  {},
                                  {1, 0, 1},
                                  BodyMotion::static_body,
                                  0,
                                  static_cast<double>(.3F),
                                  static_cast<double>(.2F),
                                  PhysicsBodyGeometry::static_mesh,
                                  2,
                                  4,
                                  1,
                                  false};
    image.physics.bodies.push_back(
        {{image.physics.world, 0, 1}, native, settings, {}, {}, {}, {}, {}, mesh});
    image.entities.push_back({reference, Name{"Terrain"}, {}, {}, {}, {}, {}});
    const WorldCellKey key{image.frame.origin.world, image.frame.origin.cell};
    const WorldCellGeometry geometry{
        {{999, 0, 23}, {999, 0, 25}, {1001, 0, 23}}, {{0, 1, 2}}, {{0, 1, 2}}};
    const auto payload = encode_world_cell(
        {key,
         1,
         {image_id<SceneId>(40), WorldPosition{key.world, key.cell, {0, 0, 0}}, {}},
         geometry});
    REQUIRE(payload);
    image.cells.push_back(
        {key, 1, proxy, payload.value(), RegionCellState::active, {}, {.3, .2, 2, 4}});
    const auto encoded = encode_region_process_image(image);
    REQUIRE(encoded);
    auto legacy = encoded.value();
    // Limits (24), cell count (8), state/source/material (29), NPC count (8).
    legacy.resize(legacy.size() - 69);
    legacy[7] = std::byte{1};
    const auto decoded = decode_region_process_image(legacy);
    REQUIRE(decoded);
    CHECK(decoded.value().cells.front().settings.friction == settings.friction);
    CHECK(decoded.value().cells.front().settings.restitution == settings.restitution);
    CHECK(decoded.value().cells.front().settings.collision_layer == 2);
    CHECK(decoded.value().cells.front().settings.collision_mask == 4);
    CHECK(decoded.value().cells.front().settings.friction != native.friction);
    CHECK(encode_region_process_image(decoded.value()).value() == encoded.value());
}

TEST_CASE("Lossless server reframing moves complete local state while preserving global identity",
          "[server-region-image]") {
    auto image = empty_image();
    image.frame.origin.cell = {world_cell_max - 2, 0, -99};
    image.frame.origin.offset = {};
    image.movement.origin = image.frame.origin;
    image.rules.source.text =
        "function on_combat(k,t,a,b,d,s) s[1]=s[1]+1 return true,10+s[1],10 end";
    image.rules.state[0] = 1;
    image.rules.evaluations = 1;
    image.combat.rules.evaluations = 1;
    image.regions[0].min_x = 1000;
    image.regions[0].max_x = 1024;
    image.regions[1].min_x = 1024;
    image.regions[1].max_x = 1048;
    const auto reference = [&](unsigned id, std::uint32_t slot) {
        return EntityReference{{image.physics.revision.scene_instance, slot, 1},
                               image_id<WorldEntityId>(id),
                               image.physics.scope,
                               image.physics.scope.authority};
    };
    const auto actor_ref = reference(10, 0);
    const auto child_ref = reference(11, 1);
    const auto mesh_ref = reference(12, 2);
    const auto platform_ref = reference(13, 3);
    const auto target_ref = reference(30, 9);
    const PhysicsBodyHandle actor_body{image.physics.world, 0, 1};
    const PhysicsBodyHandle mesh_body{image.physics.world, 1, 1};
    const PhysicsBodyHandle platform_body{image.physics.world, 2, 1};
    RigidBody settings;
    settings.motion = BodyMotion::dynamic_body;
    settings.shape = ColliderShape::sphere;
    Transform pose;
    pose.translation = {1023, 2, 0};
    auto previous = pose;
    previous.translation.x = 1022.5;
    const PhysicsBodyVelocity velocity{{1, 0, 0}, {0, 1, 0}};
    const PhysicsBodyState native{actor_ref,
                                  settings,
                                  pose,
                                  {.5, .5, .5},
                                  BodyMotion::dynamic_body,
                                  1,
                                  .5,
                                  0,
                                  PhysicsBodyGeometry::primitive,
                                  0,
                                  0xffff,
                                  0,
                                  false};
    image.physics.bodies.push_back(
        {actor_body, native, settings, pose, previous, velocity, {2, 0, 0}, {}, {}});
    RigidBody fixed;
    const PhysicsBodyState mesh_native{mesh_ref,
                                       {},
                                       {},
                                       {2, 0, 2},
                                       BodyMotion::static_body,
                                       0,
                                       .5,
                                       0,
                                       PhysicsBodyGeometry::static_mesh,
                                       0,
                                       0xffff,
                                       1,
                                       false};
    const PhysicsStaticMesh geometry{{{1022, 0, -2}, {1022, 0, 2}, {1026, 0, -2}}, {{0, 1, 2}}};
    image.physics.bodies.push_back({mesh_body, mesh_native, fixed, {}, {}, {}, {}, {}, geometry});
    RigidBody kinematic;
    kinematic.motion = BodyMotion::kinematic_body;
    auto platform_pose = pose;
    platform_pose.translation.x = 1026;
    auto target_pose = platform_pose;
    target_pose.translation.x = 1026.5;
    auto platform_native = native;
    platform_native.entity = platform_ref;
    platform_native.settings = kinematic;
    platform_native.pose = platform_pose;
    platform_native.motion = BodyMotion::kinematic_body;
    platform_native.inverse_mass = 0;
    image.physics.bodies.push_back({platform_body,
                                    platform_native,
                                    kinematic,
                                    platform_pose,
                                    platform_pose,
                                    {},
                                    {},
                                    target_pose,
                                    {}});
    Transform child;
    child.translation = {.125, 1, 0};
    image.entities.push_back({actor_ref, Name{"Actor"}, pose, {}, {}, {}, settings});
    image.entities.push_back(
        {child_ref, Name{"Child"}, child, image_id<WorldEntityId>(10), {}, {}, {}});
    image.entities.push_back({mesh_ref, Name{"Terrain"}, {}, {}, {}, {}, {}});
    image.entities.push_back(
        {platform_ref, Name{"Platform"}, platform_pose, {}, {}, {}, kinematic});
    auto dead_pose = pose;
    dead_pose.translation.x = 1025;
    image.entities.push_back({target_ref, Name{"Dead target"}, dead_pose, {}, {}, {}, {}});
    const net::SessionSnapshot session{image_id<net::SessionId>(20),
                                       image_id<net::AccountId>(21),
                                       image_id<net::CharacterId>(22),
                                       1,
                                       7,
                                       true,
                                       true};
    const auto stream = image_id<net::MovementStreamId>(23);
    const net::SessionEnvelope envelope{session.session, 1, 8, session.character};
    const MovementDomain domain{1000, 1048, -10, 10};
    const net::MovementInput pending{envelope, stream, 2, 1000, 0};
    image.movement.domains = {domain};
    image.movement.actors.push_back(
        {1, session, stream, actor_ref, actor_body, native, velocity, 2, 1, domain, {pending}});
    const auto combat_stream = image_id<net::CombatStreamId>(24);
    const auto life = image_id<net::CombatTargetId>(25);
    const auto target_life = image_id<net::CombatTargetId>(26);
    const net::MovementState correction{session.session, 1, stream, 17, 1, pose.translation,
                                        velocity.linear};
    const ProjectileCut flight{
        {envelope, combat_stream, 1, target_life, CombatActionKind::projectile},
        {true, 11, 10},
        life,
        actor_body,
        {image.physics.world, 9, 1},
        target_ref,
        {1023.5, 2, 0},
        {1, 0, 0},
        18,
        81};
    CombatActorCut actor{
        1,        session, combat_stream, life,  actor_ref, actor_body, native, velocity,
        settings, pose,    correction,    100,   27,        0,          1,      81,
        {},       {},      false,         false, {},        flight};
    image.combat.actors.push_back(actor);
    actor.connection = 2;
    actor.session.session = image_id<net::SessionId>(31);
    actor.session.account = image_id<net::AccountId>(32);
    actor.session.character = image_id<net::CharacterId>(33);
    actor.stream = image_id<net::CombatStreamId>(34);
    actor.entity = target_ref;
    actor.target = target_life;
    actor.body.reset();
    actor.native.reset();
    actor.velocity.reset();
    actor.health = 0;
    actor.respawn_tick = 77;
    actor.spawn = dead_pose;
    actor.movement.position = dead_pose.translation;
    actor.movement.session = actor.session.session;
    actor.movement.stream = image_id<net::MovementStreamId>(35);
    actor.accepted = 0;
    actor.pending_order = 0;
    actor.projectile.reset();
    image.combat.actors.push_back(actor);
    const auto original = encode_region_process_image(image);
    REQUIRE(original);
    auto target = image.frame;
    ++target.origin.cell.x;
    ++target.generation;
    const auto changed = reframe_region_process_image(image, target);
    REQUIRE(changed);
    CHECK(changed.value().frame == target);
    CHECK(changed.value().regions.front().min_x == -24);
    CHECK(changed.value().entities[0].transform.translation.x == -1);
    CHECK(changed.value().entities[1].transform == child);
    CHECK(changed.value().entities[2].transform == Transform{});
    CHECK(changed.value().physics.bodies[0].native.pose.translation.x == -1);
    CHECK(changed.value().physics.bodies[0].previous_pose.translation.x == -1.5);
    CHECK(changed.value().physics.bodies[0].pending_force == math::Vec3{2, 0, 0});
    CHECK(changed.value().physics.bodies[1].native.pose == Transform{});
    CHECK(changed.value().physics.bodies[1].mesh->vertices[0].x == -2);
    CHECK(changed.value().physics.bodies[2].target->translation.x == 2.5);
    CHECK(changed.value().movement.actors[0].pending[0] == pending);
    CHECK(changed.value().movement.actors[0].native.pose.translation.x == -1);
    CHECK(changed.value().combat.actors[0].movement.position.x == -1);
    CHECK(changed.value().combat.actors[0].projectile->position.x == -.5);
    CHECK(changed.value().combat.actors[0].projectile->direction == flight.direction);
    CHECK(changed.value().combat.actors[0].projectile->decision == flight.decision);
    CHECK(changed.value().combat.actors[0].projectile->request == flight.request);
    CHECK(changed.value().combat.actors[1].spawn.translation.x == 1);
    CHECK(changed.value().combat.actors[1].respawn_tick == 77);
    CHECK(changed.value().rules.state == image.rules.state);
    CHECK(changed.value().route_watermark == image.route_watermark);
    CHECK(encode_region_process_image(image).value() == original.value());
    const auto returned =
        reframe_region_process_image(changed.value(), {image.frame.origin, target.generation + 1});
    REQUIRE(returned);
    CHECK(returned.value().physics.bodies[0].native.pose == native.pose);
    CHECK(*returned.value().physics.bodies[1].mesh == geometry);
    auto invalid = target;
    invalid.origin.offset[0] = .5;
    CHECK_FALSE(reframe_region_process_image(image, invalid));
    invalid = target;
    invalid.generation = image.frame.generation;
    CHECK_FALSE(reframe_region_process_image(image, invalid));
    auto lossy = image;
    lossy.physics.bodies[0].native.pose.translation.x = static_cast<double>(0.1F);
    CHECK_FALSE(reframe_region_process_image(lossy, target));
    lossy = image;
    lossy.physics.bodies[2].target->translation.x = 1026.1;
    const auto shifted_target = lossy.physics.bodies[2].target->translation.x - 1024;
    REQUIRE(shifted_target + 1024 == lossy.physics.bodies[2].target->translation.x);
    REQUIRE(static_cast<double>(static_cast<float>(shifted_target)) !=
            static_cast<double>(static_cast<float>(1026.1)) - 1024);
    CHECK_FALSE(reframe_region_process_image(lossy, target));
}
