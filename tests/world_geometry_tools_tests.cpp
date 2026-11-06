// Original Shark-authored geometry preparation fixtures; project MIT license.
#include "shark/tools/prepared_content.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>
#include <thread>

namespace {
using namespace shark;
using namespace shark::tools;
template <typename T> T take(Result<T> value) {
    if (!value)
        INFO(value.error().message);
    REQUIRE(value);
    return std::move(value).value();
}
template <typename Id> Id id(char digit) {
    std::string value{"64000000-0000-4000-8000-000000000001"};
    value.back() = digit;
    return Id{take(StableId::parse(value))};
}
WorldCellKey key() {
    return {id<WorldId>('2'), {1'000'000'000, 0, 1'000'000'000}};
}
SceneDocument document() {
    SourceEntity root{id<AuthoredEntityId>('4'), {}};
    root.name.value = "Parent";
    root.transform.translation = {10, 1, 20};
    root.transform.scale = {2, 1, 4};
    SourceEntity ground{id<AuthoredEntityId>('5'), root.id};
    ground.name.value = "Hidden ground";
    ground.transform.translation = {3, 2, 5};
    ground.transform.scale = {2, 1, 2};
    ground.mesh_renderer = MeshRenderer{builtin_plane_asset_id(), 0, false};
    SourceEntity decorative{id<AuthoredEntityId>('6'), {}};
    decorative.transform.translation = {30, 1, 30};
    decorative.mesh_renderer = MeshRenderer{builtin_cube_asset_id(), 0, true};
    return {id<ProjectId>('1'),
            id<SceneId>('3'),
            WorldPosition{key().world, key().cell, {0, 0, 0}},
            {root, ground, decorative},
            "{\"private_geometry_metadata\":true}"};
}
std::unique_ptr<AuthoredScene> authored(SceneDocument source = document()) {
    ProjectDocument project{id<ProjectId>('1'),
                            key().world,
                            Name{"Geometry fixture"},
                            source.scene,
                            {{source.scene, take(ProjectPath::parse("cells/source.sharkscene"))}},
                            "{}"};
    return take(AuthoredScene::create(project, source));
}
std::vector<std::byte> prepare(const AuthoredScene& owner,
                               std::shared_ptr<const PreparedAssets> assets) {
    const std::array selected{id<AuthoredEntityId>('5')};
    return take(prepare_world_cell_geometry(owner, take(owner.status()).revision, key(), 12,
                                            std::move(assets), selected, selected));
}
} // namespace

TEST_CASE(
    "world geometry preparation bakes full hierarchy and hidden selections without source mutation",
    "[world-geometry-tools]") {
    const auto owner = authored();
    const auto changed =
        take(owner->execute(SetNameCommand{id<AuthoredEntityId>('5'), Name{"Edited ground"}},
                            take(owner->status()).revision));
    REQUIRE(changed.outcome);
    const auto before = take(owner->capture());
    const auto history = take(owner->history());
    const auto assets = take(PreparedAssets::create());
    const auto bytes = prepare(*owner, assets);
    const auto result = take(decode_world_cell(bytes));
    REQUIRE(result.geometry);
    CHECK(result.content_revision == 12);
    CHECK(result.key == key());
    CHECK(result.geometry->vertices ==
          std::vector<math::Vec3>{{14, 3, 44}, {18, 3, 44}, {18, 3, 36}, {14, 3, 36}});
    CHECK(result.geometry->collision_triangles ==
          std::vector<std::array<std::uint32_t, 3>>{{0, 1, 2}, {0, 2, 3}});
    CHECK(result.geometry->navigation_triangles == result.geometry->collision_triangles);
    CHECK(take(LoadedWorldCell::load(bytes, assets))->geometry() == result.geometry);
    CHECK(prepare(*owner, assets) == bytes);
    const auto after = take(owner->capture());
    CHECK(after.scene == before.scene);
    CHECK(after.revision == before.revision);
    CHECK(after.dirty);
    const auto after_history = take(owner->history());
    CHECK(after_history.undo_entries == history.undo_entries);
    REQUIRE(after_history.next_undo);
    REQUIRE(history.next_undo);
    CHECK(after_history.next_undo->ticket == history.next_undo->ticket);
}

TEST_CASE("world geometry preparation keeps reflection parity and rejects unsnapped output",
          "[world-geometry-tools]") {
    const auto assets = take(PreparedAssets::create());
    auto source = document();
    source.entities[1].transform.scale.x = -2;
    const auto mirrored = authored(source);
    const auto geometry = *take(decode_world_cell(prepare(*mirrored, assets))).geometry;
    CHECK(geometry.vertices.front() == math::Vec3{18, 3, 44});
    CHECK(geometry.collision_triangles.front() == std::array<std::uint32_t, 3>{0, 2, 1});
    CHECK(validate_world_cell_geometry(geometry));

    source = document();
    source.entities[1].transform.scale.x = 0.1;
    const auto unsnapped = authored(source);
    const auto before = take(unsnapped->capture());
    const std::array selected{id<AuthoredEntityId>('5')};
    const auto rejected = prepare_world_cell_geometry(*unsnapped, before.revision, key(), 1, assets,
                                                      selected, selected);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().context.operation == "content.cell.geometry.prepare");
    CHECK(take(unsnapped->capture()).scene == before.scene);
}

TEST_CASE(
    "world geometry preparation requires exact revisions assets and explicit collision selections",
    "[world-geometry-tools]") {
    const auto owner = authored();
    const auto assets = take(PreparedAssets::create());
    const auto revision = take(owner->status()).revision;
    const std::array selected{id<AuthoredEntityId>('5')};
    const std::array decorative{id<AuthoredEntityId>('6')};
    const std::array root{id<AuthoredEntityId>('4')};
    const std::array missing{id<AuthoredEntityId>('9')};
    const std::array duplicate{id<AuthoredEntityId>('5'), id<AuthoredEntityId>('5')};
    CHECK_FALSE(
        prepare_world_cell_geometry(*owner, revision, key(), 1, nullptr, selected, selected));
    CHECK_FALSE(prepare_world_cell_geometry(*owner, revision, key(), 1, assets, {}, {}));
    CHECK_FALSE(
        prepare_world_cell_geometry(*owner, revision, key(), 1, assets, duplicate, selected));
    CHECK_FALSE(
        prepare_world_cell_geometry(*owner, revision, key(), 1, assets, selected, decorative));
    CHECK_FALSE(prepare_world_cell_geometry(*owner, revision, key(), 1, assets, root, {}));
    CHECK_FALSE(prepare_world_cell_geometry(*owner, revision, key(), 1, assets, missing, {}));
    CHECK_FALSE(
        prepare_world_cell_geometry(*owner, revision, key(), 1, assets, decorative, decorative));
    const auto collision_only = take(decode_world_cell(
        take(prepare_world_cell_geometry(*owner, revision, key(), 1, assets, decorative, {}))));
    REQUIRE(collision_only.geometry);
    CHECK(collision_only.geometry->collision_triangles.size() == 12);
    CHECK(collision_only.geometry->navigation_triangles.empty());
    const auto changed = take(
        owner->execute(SetNameCommand{id<AuthoredEntityId>('5'), Name{"New ground"}}, revision));
    REQUIRE(changed.outcome);
    CHECK_FALSE(
        prepare_world_cell_geometry(*owner, revision, key(), 1, assets, selected, selected));
    bool wrong_thread{};
    const auto current = take(owner->status()).revision;
    std::thread other([&] {
        const auto rejected =
            prepare_world_cell_geometry(*owner, current, key(), 1, assets, selected, selected);
        wrong_thread = !rejected && rejected.error().code == ErrorCode::conflict;
    });
    other.join();
    CHECK(wrong_thread);

    auto source = document();
    source.entities[1].mesh_renderer->asset = id<AssetId>('9');
    const auto absent = authored(source);
    CHECK_FALSE(prepare_world_cell_geometry(*absent, take(absent->status()).revision, key(), 1,
                                            assets, selected, selected));
}
