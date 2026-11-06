#include <shark/tools/prefab_instance.hpp>
#include <shark/tools/prepared_content.hpp>
#include <shark/tools/project_files.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace shark;
using namespace shark::tools;
namespace fs = std::filesystem;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.operation);
    }
    REQUIRE(result);
    return std::move(result).value();
}
template <typename T> T id(unsigned number) {
    std::string value = "21e10000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        value[35 - index] = digits[number & 15U];
        number >>= 4U;
    }
    return T{take(StableId::parse(value))};
}
std::string read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream);
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}
struct Scratch {
    fs::path path;
    bool complete{false};
    Scratch() {
        const fs::path parent{SHARK_PREFAB_ACCEPTANCE_SCRATCH};
        fs::create_directories(parent);
        path = parent / take(generate_stable_id()).to_string();
        REQUIRE(fs::create_directory(path));
    }
    ~Scratch() {
        if (!complete)
            return;
        std::error_code ignored;
        for (const auto* name : {"Source.sharkprefab.sharkasset", "Source.sharkprefab",
                                 "Project.sharkproject", "Scene.sharkscene", ".shark-write.lock"})
            fs::remove(path / name, ignored);
        fs::remove(path, ignored);
    }
};
const Scene& scene(const AuthoredScene& owner) {
    return take(owner.scene()).get();
}
SceneRevision revision(const AuthoredScene& owner) {
    return take(owner.status()).revision;
}
AuthoredEntityId mapped(const InstantiatePrefabCommand& command, AuthoredEntityId source) {
    const auto found = std::find_if(command.remap.begin(), command.remap.end(),
                                    [&](const auto& value) { return value.source == source; });
    REQUIRE(found != command.remap.end());
    return found->instance;
}
SceneEntityState state(const AuthoredScene& owner, AuthoredEntityId entity) {
    const auto& current = scene(owner);
    return take(current.entity_state(take(current.find(entity))));
}
} // namespace

TEST_CASE("Runtime prefab preparation rejects unsafe composed transforms before a host sees them",
          "[prefab-acceptance]") {
    SourceEntity parent{id<AuthoredEntityId>(10), {}};
    parent.transform.scale = {1e200, 1, 1};
    SourceEntity child{id<AuthoredEntityId>(11), parent.id};
    child.transform.translation.x = 1e200;
    const PrefabDocument source{id<ProjectId>(1), id<AssetId>(20), parent.id, {child, parent}};
    REQUIRE(validate_prefab_document(source));
    REQUIRE(transform_matrix(parent.transform));
    REQUIRE(transform_matrix(child.transform));
    const auto rejected = prepare_runtime_prefab(source);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().context.operation == "content.prefab.prepare");
    REQUIRE(rejected.error().context.asset_id == source.asset.value().to_string());
    REQUIRE(rejected.error().context.entity_id == child.id.value().to_string());
    REQUIRE(source.entities[0].transform.translation.x == 1e200);
}

TEST_CASE("Saved prefab instances remain independent through source edits and Scene reload",
          "[prefab-acceptance]") {
    Scratch scratch;
    const auto scene_id = id<SceneId>(3);
    const auto source_path = take(ProjectPath::parse("Source.sharkprefab"));
    const ProjectDocument project{id<ProjectId>(1),
                                  id<WorldId>(2),
                                  {"Connected prefabs"},
                                  scene_id,
                                  {{scene_id, take(ProjectPath::parse("Scene.sharkscene"))}}};
    const SceneDocument initial{project.project, scene_id, {}, {}};
    SourceEntity root{id<AuthoredEntityId>(10), {}, {"Beacon"}};
    root.rigid_body = RigidBody{};
    root.rigid_body->motion = BodyMotion::dynamic_body;
    root.rigid_body->mass = 3;
    root.script = Script{"scripts/beacon.lua", false};
    root.extras_json = "{\"private\":\"authoring\"}";
    SourceEntity child{id<AuthoredEntityId>(11), root.id, {"Lens"}};
    child.transform.translation = {1, 2, 0};
    child.mesh_renderer = MeshRenderer{builtin_cube_asset_id()};
    child.extras_json = "{\"lens\":true}";
    SourceEntity leaf{id<AuthoredEntityId>(12), child.id, {"View"}};
    leaf.transform.translation.z = 1;
    leaf.camera = Camera{};
    const PrefabDocument source{project.project,
                                id<AssetId>(20),
                                root.id,
                                {leaf, child, root},
                                "{\"note\":\"source only\"}"};
    auto files = take(ProjectFiles::open(scratch.path));
    static_cast<void>(
        take(files->write_project(take(ProjectPath::parse("Project.sharkproject")), project, {})));
    const auto baseline = take(files->write_scene(project, initial, {}));
    static_cast<void>(take(files->write_asset_metadata(
        take(asset_sidecar_path(source_path)),
        {project.project, source.asset, "prefab", "Shark fixture", "MIT", "shark.prefab"}, {})));
    const auto source_commit = take(files->write_prefab(source_path, source, {}));
    const auto source_bytes = read(scratch.path / source_path.text());
    const auto observed = take(files->read_prefab(source_path));
    const auto runtime_value = take(prepare_runtime_prefab(observed.source.document));
    REQUIRE(runtime_value.asset == source.asset);
    REQUIRE(runtime_value.root == root.id);
    REQUIRE(runtime_value.entities.size() == 3);
    REQUIRE(runtime_value.entities[0].id == leaf.id);
    REQUIRE(runtime_value.entities[1].parent == root.id);
    REQUIRE(runtime_value.entities[1].mesh_renderer == child.mesh_renderer);
    REQUIRE(runtime_value.entities[2].rigid_body == root.rigid_body);
    REQUIRE(runtime_value.entities[2].script == root.script);
    auto invalid = source;
    invalid.entities[0].parent.reset();
    const auto rejected = prepare_runtime_prefab(invalid);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().context.operation == "content.prefab.prepare");
    REQUIRE(rejected.error().context.asset_id == source.asset.value().to_string());

    auto owner = take(AuthoredScene::create(project, initial));
    std::vector<std::array<AuthoredEntityId, 3>> instances;
    std::set<AuthoredEntityId> identities{root.id, child.id, leaf.id};
    std::optional<AuthoringMutationRequest> original_request;
    std::shared_ptr<const AuthoringMutationOutcome> original_receipt;
    for (unsigned index = 0; index < 32; ++index) {
        Transform pose;
        pose.translation.x = static_cast<double>(index) * 4;
        const auto command = take(prepare_prefab_instance(
            observed.source.document,
            {{}, {{root.id, Name{"Instance " + std::to_string(index)}, pose}}}));
        const auto status = take(owner->mutation_status());
        REQUIRE(status.next);
        const AuthoringMutationRequest request{*status.next, status.revision,
                                               AuthoringCommand{command}};
        const auto outcome = take(owner->mutate(request));
        const auto receipt = take(outcome.outcome);
        REQUIRE(take(receipt->result).changed);
        if (index == 0) {
            original_request = request;
            original_receipt = receipt;
        }
        instances.push_back({command.id, mapped(command, child.id), mapped(command, leaf.id)});
        for (const auto identity : instances.back())
            REQUIRE(identities.insert(identity).second);
    }
    REQUIRE(identities.size() == 99);
    REQUIRE(take(owner->capture()).scene.entities.size() == 96);
    for (std::size_t index = 0; index < instances.size(); ++index) {
        const auto& ids = instances[index];
        const auto instance_root = state(*owner, ids[0]);
        const auto instance_child = state(*owner, ids[1]);
        const auto instance_leaf = state(*owner, ids[2]);
        REQUIRE(instance_root.world_matrix[3][0] == static_cast<double>(index) * 4);
        REQUIRE(instance_child.world_matrix[3][0] == static_cast<double>(index) * 4 + 1);
        REQUIRE(instance_leaf.world_matrix[3][1] == 2);
        REQUIRE(instance_leaf.world_matrix[3][2] == 1);
        REQUIRE(instance_child.parent == instance_root.reference.handle);
        REQUIRE(instance_leaf.parent == instance_child.reference.handle);
        REQUIRE(instance_child.mesh_renderer == child.mesh_renderer);
    }
    REQUIRE(take(take(owner->execute(SetNameCommand{instances[7][1], Name{"Edited lens"}},
                                     revision(*owner)))
                     .outcome)
                .changed);
    REQUIRE(state(*owner, instances[6][1]).name == Name{"Lens"});
    REQUIRE(state(*owner, instances[8][1]).name == Name{"Lens"});
    REQUIRE(read(scratch.path / source_path.text()) == source_bytes);
    const auto old_entity = state(*owner, instances[7][1]).reference;
    const auto captured = take(owner->capture());
    const auto saved = take(files->save_scene(*owner, captured.revision, baseline.revision));
    REQUIRE(saved.current_scene_clean);
    REQUIRE(take(owner->history()).undo_entries == 32);
    const auto reloaded = take(files->reload_scene(*owner, revision(*owner)));
    REQUIRE(reloaded.revision.scene_instance != captured.revision.scene_instance);
    REQUIRE_FALSE(scene(*owner).resolve(old_entity));
    REQUIRE(take(owner->capture()).scene == captured.scene);
    REQUIRE(take(owner->history()).undo_entries == 0);
    const auto replay = take(owner->mutate(*original_request));
    REQUIRE(replay.state == AuthoringMutationState::replayed);
    REQUIRE(take(replay.outcome) == original_receipt);
    REQUIRE(take(owner->capture()).scene == captured.scene);

    auto updated = source;
    updated.entities[1].name = Name{"Future template lens"};
    static_cast<void>(take(files->write_prefab(source_path, updated, source_commit.revision)));
    REQUIRE(state(*owner, instances[7][1]).name == Name{"Edited lens"});
    REQUIRE(state(*owner, instances[8][1]).name == Name{"Lens"});
    REQUIRE(runtime_value.entities[1].name == Name{"Lens"});
    REQUIRE(take(prepare_runtime_prefab(take(files->read_prefab(source_path)).source.document))
                .entities[1]
                .name == Name{"Future template lens"});
    owner.reset();
    files.reset();
    files = take(ProjectFiles::open(scratch.path));
    auto reopened = take(files->load_scene(project, scene_id));
    REQUIRE(take(reopened.owner->capture()).scene == captured.scene);
    REQUIRE(state(*reopened.owner, instances[7][1]).name == Name{"Edited lens"});
    REQUIRE(take(reopened.owner->mutation_status()).scope != original_request->id.scope);
    REQUIRE_FALSE(take(reopened.owner->mutation_outcome(original_request->id)).outcome);
    reopened.owner.reset();
    files.reset();
    scratch.complete = true;
}
