// Original Shark-authored in-memory fixtures; project MIT license.
#include "shark/tools/prepared_content.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <string>
#include <thread>

namespace {
using namespace shark;
using namespace shark::tools;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.operation);
    }
    REQUIRE(result);
    return std::move(result).value();
}

template <typename T> T id(char digit) {
    std::string value = "65000000-0000-4000-8000-000000000001";
    value.back() = digit;
    return T{take(StableId::parse(value))};
}

WorldCellKey key() {
    return {id<WorldId>('2'), {1'000'000'000, -7, 2}};
}

WorldPosition anchor() {
    return {key().world, key().cell, {0.0, 0.0, 0.0}};
}

SceneDocument document() {
    Transform child;
    child.translation = {1.0, 2.0, 3.0};
    return {id<ProjectId>('1'),
            id<SceneId>('3'),
            anchor(),
            {{id<AuthoredEntityId>('4'),
              {},
              Name{"Cell ground"},
              {},
              "{\"private_root_metadata\":true}",
              {},
              MeshRenderer{builtin_plane_asset_id(), 0, true},
              RigidBody{}},
             {id<AuthoredEntityId>('5'),
              id<AuthoredEntityId>('4'),
              Name{"Cell decoration"},
              child,
              "{\"private_child_metadata\":true}",
              {},
              MeshRenderer{builtin_cube_asset_id(), 0, false}}},
            "{\"private_cell_metadata\":true}"};
}

std::unique_ptr<AuthoredScene> authored(SceneDocument source = document()) {
    const ProjectDocument project{
        id<ProjectId>('1'),
        id<WorldId>('2'),
        Name{"Cell fixture"},
        source.scene,
        {{source.scene, take(ProjectPath::parse("cells/private-cell.sharkscene"))}},
        "{\"private_project_metadata\":true}"};
    return take(AuthoredScene::create(project, source));
}

bool contains(std::span<const std::byte> payload, std::string_view text) {
    const auto value = std::as_bytes(std::span{text.data(), text.size()});
    return std::search(payload.begin(), payload.end(), value.begin(), value.end()) != payload.end();
}
} // namespace

TEST_CASE("world-cell preparation captures one authored revision without changing source state",
          "[world-cell-tools]") {
    const auto owner = authored();
    const auto initial = take(owner->status());
    const auto changed = take(owner->execute(
        SetNameCommand{id<AuthoredEntityId>('5'), Name{"Edited decoration"}}, initial.revision));
    REQUIRE(changed.outcome);
    const auto before = take(owner->capture());
    const auto history = take(owner->history());
    const auto payload = take(prepare_world_cell(*owner, before.revision, key(), 17));
    const auto cell = take(decode_world_cell(payload));
    CHECK(cell.key == key());
    CHECK(cell.content_revision == 17);
    CHECK(cell.scene.scene == before.scene.scene);
    CHECK(cell.scene.world_origin == anchor());
    REQUIRE(cell.scene.entities.size() == 2);
    CHECK(cell.scene.entities[0].id == id<AuthoredEntityId>('4'));
    CHECK(cell.scene.entities[0].rigid_body == RigidBody{});
    CHECK(cell.scene.entities[0].mesh_renderer == before.scene.entities[0].mesh_renderer);
    CHECK(cell.scene.entities[1].id == id<AuthoredEntityId>('5'));
    CHECK(cell.scene.entities[1].parent == id<AuthoredEntityId>('4'));
    CHECK(cell.scene.entities[1].transform.translation == math::Vec3{1.0, 2.0, 3.0});
    CHECK(cell.scene.entities[1].name == Name{"Edited decoration"});
    CHECK_FALSE(cell.scene.entities[1].mesh_renderer->visible);
    CHECK_FALSE(contains(payload, "private_root_metadata"));
    CHECK_FALSE(contains(payload, "private_child_metadata"));
    CHECK_FALSE(contains(payload, "private_cell_metadata"));
    CHECK_FALSE(contains(payload, "private_project_metadata"));
    CHECK_FALSE(contains(payload, "private-cell.sharkscene"));
    CHECK(take(prepare_world_cell(*owner, before.revision, key(), 17)) == payload);

    const auto after = take(owner->capture());
    CHECK(after.revision == before.revision);
    CHECK(after.scene == before.scene);
    CHECK(after.dirty);
    const auto after_history = take(owner->history());
    CHECK(after_history.undo_entries == history.undo_entries);
    CHECK(after_history.redo_entries == history.redo_entries);
    REQUIRE(after_history.next_undo);
    REQUIRE(history.next_undo);
    CHECK(after_history.next_undo->ticket == history.next_undo->ticket);
}

TEST_CASE("world-cell preparation rejects relabeling source anchors and zero content revisions",
          "[world-cell-tools]") {
    const auto owner = authored();
    const auto before = take(owner->capture());
    auto wrong_world = key();
    wrong_world.world = id<WorldId>('8');
    auto wrong_cell = key();
    ++wrong_cell.cell.x;
    auto invalid_cell = key();
    invalid_cell.cell.y = world_cell_max + 1;
    for (const auto& candidate : std::array{wrong_world, wrong_cell, invalid_cell}) {
        const auto rejected = prepare_world_cell(*owner, before.revision, candidate, 1);
        REQUIRE_FALSE(rejected);
        CHECK(rejected.error().context.operation == "content.cell.prepare");
    }
    CHECK_FALSE(prepare_world_cell(*owner, before.revision, key(), 0));

    auto source = document();
    source.world_origin.reset();
    const auto missing_anchor = authored(source);
    CHECK_FALSE(
        prepare_world_cell(*missing_anchor, take(missing_anchor->status()).revision, key(), 1));
    source.world_origin = anchor();
    source.world_origin->offset[0] = 0.5;
    const auto offset_anchor = authored(source);
    CHECK_FALSE(
        prepare_world_cell(*offset_anchor, take(offset_anchor->status()).revision, key(), 1));
    const auto after = take(owner->capture());
    CHECK(after.revision == before.revision);
    CHECK(after.scene == before.scene);
    CHECK_FALSE(after.dirty);
}

TEST_CASE("world-cell preparation preserves capture revision and thread fences",
          "[world-cell-tools]") {
    const auto owner = authored();
    const auto initial = take(owner->status());
    const auto changed = take(owner->execute(
        SetNameCommand{id<AuthoredEntityId>('5'), Name{"New name"}}, initial.revision));
    REQUIRE(changed.outcome);
    const auto stale = prepare_world_cell(*owner, initial.revision, key(), 1);
    REQUIRE_FALSE(stale);
    CHECK(stale.error().code == ErrorCode::conflict);
    CHECK(stale.error().context.operation == "content.cell.prepare");
    const auto current = take(owner->status()).revision;
    bool rejected = false;
    std::string operation;
    std::thread worker([&] {
        const auto result = prepare_world_cell(*owner, current, key(), 1);
        rejected = !result;
        if (!result)
            operation = result.error().context.operation;
    });
    worker.join();
    CHECK(rejected);
    CHECK(operation == "content.cell.prepare");
    CHECK(prepare_world_cell(*owner, current, key(), 1));
}

TEST_CASE("world-cell preparation enforces the static-content profile through Core validation",
          "[world-cell-tools]") {
    for (const auto motion : {BodyMotion::dynamic_body, BodyMotion::kinematic_body}) {
        auto source = document();
        source.entities[0].rigid_body->motion = motion;
        const auto owner = authored(source);
        CHECK_FALSE(prepare_world_cell(*owner, take(owner->status()).revision, key(), 1));
    }
    auto source = document();
    source.entities[0].rigid_body->is_trigger = true;
    const auto trigger = authored(source);
    CHECK_FALSE(prepare_world_cell(*trigger, take(trigger->status()).revision, key(), 1));
    source = document();
    source.entities[1].camera = Camera{};
    const auto camera = authored(source);
    CHECK_FALSE(prepare_world_cell(*camera, take(camera->status()).revision, key(), 1));
    for (const bool enabled : {false, true}) {
        source = document();
        source.entities[1].script = Script{"scripts/cell.lua", enabled};
        const auto script = authored(source);
        CHECK_FALSE(prepare_world_cell(*script, take(script->status()).revision, key(), 1));
    }
}

TEST_CASE("adjacent authored cells share a bounded local frame at a distant world seam",
          "[world-cell-tools]") {
    const auto first_key = key();
    auto second_key = first_key;
    ++second_key.cell.x;
    const auto first_owner = authored();
    auto second_source = document();
    second_source.scene = id<SceneId>('7');
    second_source.world_origin = WorldPosition{second_key.world, second_key.cell, {0.0, 0.0, 0.0}};
    const auto second_owner = authored(second_source);
    const auto first_bytes =
        take(prepare_world_cell(*first_owner, take(first_owner->status()).revision, first_key, 17));
    const auto second_bytes = take(
        prepare_world_cell(*second_owner, take(second_owner->status()).revision, second_key, 18));
    const auto assets = take(PreparedAssets::create());
    const auto first = take(LoadedWorldCell::load(first_bytes, assets));
    const auto second = take(LoadedWorldCell::load(second_bytes, assets));
    CHECK(first->key() == first_key);
    CHECK(second->key() == second_key);
    CHECK(first->content_revision() == 17);
    CHECK(second->content_revision() == 18);
    CHECK(first->assets() == assets);
    CHECK(second->assets() == assets);

    const auto first_handle = take(first->scene().find(id<AuthoredEntityId>('5')));
    const auto second_handle = take(second->scene().find(id<AuthoredEntityId>('5')));
    const auto first_state = take(first->scene().entity_state(first_handle));
    const auto second_state = take(second->scene().entity_state(second_handle));
    CHECK(first_handle.scene_instance != second_handle.scene_instance);
    CHECK(first_handle.scene_instance != take(first_owner->status()).revision.scene_instance);
    CHECK(second_handle.scene_instance != take(second_owner->status()).revision.scene_instance);
    CHECK(first_state.reference.identity == EntityIdentity{id<AuthoredEntityId>('5')});
    CHECK(second_state.reference.identity == EntityIdentity{id<AuthoredEntityId>('5')});
    CHECK(first_state.reference.scope == SceneScope{AuthoredSceneScope{id<SceneId>('3')}});
    CHECK(second_state.reference.scope == SceneScope{AuthoredSceneScope{id<SceneId>('7')}});
    CHECK_FALSE(first->scene().resolve(second_state.reference));
    CHECK_FALSE(second->scene().resolve(first_state.reference));
    const auto global = [](const LoadedWorldCell& loaded, const SceneEntityState& state) {
        return take(world_position_from_local(
            loaded.origin(),
            {state.world_matrix[3][0], state.world_matrix[3][1], state.world_matrix[3][2]},
            world_local_coordinate_limit));
    };
    const auto first_global = global(*first, first_state);
    const auto second_global = global(*second, second_state);
    CHECK(first_global == WorldPosition{first_key.world, first_key.cell, {1.0, 2.0, 3.0}});
    CHECK(second_global == WorldPosition{second_key.world, second_key.cell, {1.0, 2.0, 3.0}});

    WorldLocalFrame frame{WorldPosition{first_key.world, first_key.cell, {1023.5, 0.0, 0.0}}, 1};
    const auto first_packed = take(pack_world_position(first_global, frame));
    const auto second_packed = take(pack_world_position(second_global, frame));
    CHECK(first_packed.meters == std::array<float, 3>{-1022.5F, 2.0F, 3.0F});
    CHECK(second_packed.meters == std::array<float, 3>{1.5F, 2.0F, 3.0F});
    CHECK(take(unpack_world_position(first_packed, frame)) == first_global);
    CHECK(take(unpack_world_position(second_packed, frame)) == second_global);

    frame.origin = take(translate_world_position(frame.origin, {1.0, 0.0, 0.0}));
    ++frame.generation;
    CHECK_FALSE(unpack_world_position(first_packed, frame));
    CHECK_FALSE(unpack_world_position(second_packed, frame));
    const auto first_shifted = take(pack_world_position(first_global, frame));
    const auto second_shifted = take(pack_world_position(second_global, frame));
    CHECK(first_shifted.meters == std::array<float, 3>{-1023.5F, 2.0F, 3.0F});
    CHECK(second_shifted.meters == std::array<float, 3>{0.5F, 2.0F, 3.0F});
    CHECK(take(unpack_world_position(first_shifted, frame)) == first_global);
    CHECK(take(unpack_world_position(second_shifted, frame)) == second_global);
}
