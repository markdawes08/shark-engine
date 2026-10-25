#include <shark/tools/project_files.hpp>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace shark;
using namespace shark::tools;
namespace fs = std::filesystem;

template <typename T> T take(Result<T> result) {
    REQUIRE(result);
    return std::move(result).value();
}

template <typename T>
void rejected(const Result<T>& result, ErrorCode code, std::string_view operation) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().message.empty());
    CHECK(result.error().context.operation == operation);
}

template <typename Id> Id id(std::string_view text) {
    return Id{take(StableId::parse(text))};
}

AuthoredEntityId child_id() {
    return id<AuthoredEntityId>("00000000-0000-4000-8000-000000000011");
}

AuthoredEntityId parent_id() {
    return id<AuthoredEntityId>("00000000-0000-4000-8000-000000000010");
}

ProjectPath project_path() {
    return take(ProjectPath::parse("project.sharkproject"));
}

std::string read_bytes(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        throw std::runtime_error{"integration fixture read failed"};
    }
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void write_bytes(const fs::path& path, std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        throw std::runtime_error{"integration fixture write failed"};
    }
}

std::string fixture_bytes(std::string_view name) {
    return read_bytes(fs::path{SHARK_SOURCE_FIXTURE_DIRECTORY} / name);
}

std::string replace_once(std::string source, std::string_view before, std::string_view after) {
    const auto position = source.find(before);
    REQUIRE(position != std::string::npos);
    source.replace(position, before.size(), after);
    return source;
}

class Fixture final {
  public:
    Fixture()
        : parent(fs::current_path()),
          tree(parent / ("shark-persistence-" + take(generate_stable_id()).to_string())),
          root(tree / fs::path{u8"Shared \u4e16\u754c project"}),
          legacy_project(fixture_bytes("project-v0.sharkproject")),
          legacy_scene(fixture_bytes("scene-v0.sharkscene")),
          project(take(decode_project_document(fixture_bytes("project-v1.sharkproject"))).document),
          scene(take(decode_scene_document(fixture_bytes("scene-v1.sharkscene"))).document) {
        REQUIRE(tree.is_absolute());
        REQUIRE(tree.parent_path() == parent);
        REQUIRE(fs::create_directory(tree));
        REQUIRE(fs::create_directory(root));
        REQUIRE(fs::create_directory(root / "scenes"));
        REQUIRE(project.scenes.size() == 2);
        REQUIRE(scene.entities.size() == 2);
        REQUIRE(project.scenes.front().scene == scene.scene);
        REQUIRE(scene.entities.front().id == child_id());
        REQUIRE(scene.entities.back().id == parent_id());
    }

    ~Fixture() {
        // Only this unique, absolute child of the recorded working directory is owned.
        if (tree.is_absolute() && tree.parent_path() == parent &&
            tree.filename().string().starts_with("shark-persistence-")) {
            std::error_code error;
            fs::remove_all(tree, error);
        }
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    fs::path scene_path() const {
        return take(take(ProjectRoot::open(root)).resolve(project.scenes.front().path));
    }

    fs::path catalog_path() const {
        return root / "project.sharkproject";
    }

    void seed() const {
        write_bytes(catalog_path(), legacy_project);
        write_bytes(scene_path(), legacy_scene);
    }

    std::unique_ptr<ProjectFiles> open() const {
        return take(ProjectFiles::open(root));
    }

    const fs::path parent;
    const fs::path tree;
    const fs::path root;
    const std::string legacy_project;
    const std::string legacy_scene;
    const ProjectDocument project;
    const SceneDocument scene;
};

const Scene& live(const AuthoredScene& owner) {
    return take(owner.scene()).get();
}

Result<AuthoringCommandEffect> execute(AuthoredScene& owner, const AuthoringCommand& command) {
    auto reply = take(owner.execute(command, take(owner.status()).revision));
    CHECK(reply.observed_revision == take(owner.status()).revision);
    return std::move(reply.outcome);
}

EntityHandle find(AuthoredScene& owner, AuthoredEntityId entity) {
    return take(live(owner).find(entity));
}

void same_capture(const CapturedScene& actual, const CapturedScene& expected) {
    CHECK(actual.project == expected.project);
    CHECK(actual.scene == expected.scene);
    CHECK(actual.revision == expected.revision);
    CHECK(actual.dirty == expected.dirty);
}

void unchanged_owner(AuthoredScene& owner, const CapturedScene& before, EntityHandle child) {
    same_capture(take(owner.capture()), before);
    CHECK(find(owner, child_id()) == child);
    const auto current = take(owner.status());
    CHECK(current.revision == before.revision);
    CHECK(current.dirty);
    CHECK(current.pending_mutations == 0);
    CHECK(take(live(owner).entity_state(child)).name.value == "Unsaved local child");
}

void check_file_flush(FileDurability durability) {
#if defined(_WIN32)
    CHECK(durability == FileDurability::file_flushed);
#else
    CHECK(durability == FileDurability::file_and_directory_flushed);
#endif
}

struct InvalidSource {
    std::string name;
    std::string bytes;
    ErrorCode code;
    bool requires_activation{false};
};

std::vector<InvalidSource> invalid_sources() {
    const auto bytes = fixture_bytes("scene-v1.sharkscene");
    auto source = take(decode_scene_document(bytes)).document;
    auto wrong_project = source;
    wrong_project.project = id<ProjectId>("00000000-0000-4000-8000-000000000090");
    auto wrong_world = source;
    wrong_world.world_origin->world = id<WorldId>("00000000-0000-4000-8000-000000000091");
    auto wrong_scene = source;
    wrong_scene.scene = id<SceneId>("00000000-0000-4000-8000-000000000092");
    auto overflow = source;
    overflow.entities.front().transform.scale = {1e200, 1e200, 1e200};
    overflow.entities.back().transform.scale = {1e200, 1e200, 1e200};
    REQUIRE(validate_scene_document(overflow));
    return {
        {"truncated JSON", bytes.substr(0, bytes.size() / 2), ErrorCode::invalid_configuration},
        {"future schema", replace_once(bytes, "\"schema_version\": 1", "\"schema_version\": 2"),
         ErrorCode::invalid_configuration},
        {"duplicate key",
         replace_once(bytes, "\"schema_version\": 1",
                      "\"schema_version\": 1, \"schema_version\": 1"),
         ErrorCode::invalid_configuration},
        {"duplicate entity",
         replace_once(bytes, "\"entity_id\": \"00000000-0000-4000-8000-000000000011\"",
                      "\"entity_id\": \"00000000-0000-4000-8000-000000000010\""),
         ErrorCode::invalid_configuration},
        {"missing parent",
         replace_once(bytes, "\"parent\": \"00000000-0000-4000-8000-000000000010\"",
                      "\"parent\": \"00000000-0000-4000-8000-000000000099\""),
         ErrorCode::invalid_configuration},
        {"foreign project", take(encode_scene_document(wrong_project)),
         ErrorCode::invalid_configuration},
        {"foreign world", take(encode_scene_document(wrong_world)),
         ErrorCode::invalid_configuration},
        {"undeclared scene", take(encode_scene_document(wrong_scene)),
         ErrorCode::invalid_configuration},
        {"composed transform overflow", take(encode_scene_document(overflow)),
         ErrorCode::invalid_argument, true},
    };
}

} // namespace

TEST_CASE("legacy disk project migrates only on explicit saves and reopens edited source",
          "[persistence-integration]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    const auto catalog = take(files->read_project(project_path()));
    CHECK(catalog.source.source_schema_version == 0);
    CHECK(catalog.source.document == fixture.project);
    auto loaded = take(files->load_scene(catalog.source.document, fixture.project.startup_scene));
    CHECK(loaded.source_schema_version == 0);
    const auto original = take(loaded.owner->capture());
    CHECK(original.scene == fixture.scene);
    CHECK(original.project == fixture.project);
    CHECK_FALSE(original.dirty);
    CHECK(read_bytes(fixture.catalog_path()) == fixture.legacy_project);
    CHECK(read_bytes(fixture.scene_path()) == fixture.legacy_scene);
    CHECK_FALSE(fs::exists(fixture.root / "scenes/interior.sharkscene"));

    const auto child = find(*loaded.owner, child_id());
    const auto parent = find(*loaded.owner, parent_id());
    CHECK(take(live(*loaded.owner).entity_state(child)).parent == parent);
    CHECK_FALSE(take(live(*loaded.owner).reference(child)).authority);
    auto expected = fixture.scene;
    expected.entities.front().name.value = "Edited coastal marker";
    expected.entities.back().transform.translation = {10.0, 20.0, 30.0};
    expected.entities.back().transform.scale = {2.0, 2.0, 2.0};
    REQUIRE(execute(*loaded.owner, SetNameCommand{child_id(), expected.entities.front().name}));
    REQUIRE(execute(*loaded.owner,
                    SetTransformCommand{parent_id(), expected.entities.back().transform}));
    const SourceEntity added{id<AuthoredEntityId>("00000000-0000-4000-8000-000000000020"),
                             parent_id(),
                             {"New authored marker"},
                             {},
                             "{}"};
    REQUIRE(execute(*loaded.owner,
                    CreateEntityCommand{added.id, added.name, added.transform, parent_id()}));
    expected.entities.push_back(added);
    const auto edited = take(loaded.owner->capture());
    CHECK(edited.scene == expected);
    CHECK(edited.dirty);
    const auto saved =
        take(files->save_scene(*loaded.owner, edited.revision, loaded.file_revision));
    CHECK(saved.saved_revision == edited.revision);
    CHECK(saved.current_scene_clean);
    check_file_flush(saved.file.durability);
    CHECK_FALSE(take(loaded.owner->status()).dirty);
    CHECK(read_bytes(fixture.scene_path()) == take(encode_scene_document(expected)));
    CHECK(read_bytes(fixture.catalog_path()) == fixture.legacy_project);
    CHECK(take(files->read_project(project_path())).source.source_schema_version == 0);

    const auto migrated_project =
        take(files->write_project(project_path(), fixture.project, catalog.revision));
    check_file_flush(migrated_project.durability);
    const auto saved_project_bytes = read_bytes(fixture.catalog_path());
    const auto saved_scene_bytes = read_bytes(fixture.scene_path());
    CHECK(saved_project_bytes == take(encode_project_document(fixture.project)));
    loaded.owner.reset();
    files.reset();

    files = fixture.open();
    const auto reopened_catalog = take(files->read_project(project_path()));
    CHECK(reopened_catalog.source.source_schema_version == 1);
    CHECK(reopened_catalog.revision == migrated_project.revision);
    CHECK(reopened_catalog.source.document == fixture.project);
    auto reopened = take(files->load_scene(reopened_catalog.source.document,
                                           reopened_catalog.source.document.startup_scene));
    CHECK(reopened.source_schema_version == 1);
    CHECK(reopened.file_revision == saved.file.revision);
    const auto roundtrip = take(reopened.owner->capture());
    CHECK(roundtrip.scene == expected);
    CHECK(roundtrip.project == fixture.project);
    CHECK_FALSE(roundtrip.dirty);
    CHECK(roundtrip.revision.scene_instance != edited.revision.scene_instance);
    CHECK_FALSE(live(*reopened.owner).entity_state(child));
    const auto child_state =
        take(live(*reopened.owner).entity_state(find(*reopened.owner, child_id())));
    CHECK(child_state.world_matrix[3][0] == 12.5);
    CHECK(child_state.world_matrix[3][1] == 15.0);
    CHECK(child_state.world_matrix[3][2] == 30.0);
    CHECK(child_state.depth == 2);
    CHECK_FALSE(child_state.reference.authority);
    CHECK(read_bytes(fixture.catalog_path()) == saved_project_bytes);
    CHECK(read_bytes(fixture.scene_path()) == saved_scene_bytes);
}

TEST_CASE("invalid external sources preserve dirty incarnation metadata and exact disk bytes",
          "[persistence-integration]") {
    for (const auto& invalid : invalid_sources()) {
        DYNAMIC_SECTION(invalid.name) {
            Fixture fixture;
            fixture.seed();
            auto files = fixture.open();
            const auto catalog = take(files->read_project(project_path()));
            auto loaded =
                take(files->load_scene(catalog.source.document, fixture.project.startup_scene));
            const auto child = find(*loaded.owner, child_id());
            REQUIRE(execute(*loaded.owner, SetNameCommand{child_id(), {"Unsaved local child"}}));
            const auto unsaved = take(loaded.owner->capture());
            write_bytes(fixture.scene_path(), invalid.bytes);
            for (const auto policy : {UnsavedScenePolicy::reject, UnsavedScenePolicy::discard}) {
                // Valid local values reach activation's dirty guard before composition checks.
                const auto code =
                    invalid.requires_activation && policy == UnsavedScenePolicy::reject
                        ? ErrorCode::conflict
                        : invalid.code;
                rejected(files->reload_scene(*loaded.owner, unsaved.revision, policy), code,
                         "source.file.reload_scene");
                unchanged_owner(*loaded.owner, unsaved, child);
                CHECK(read_bytes(fixture.scene_path()) == invalid.bytes);
                CHECK(read_bytes(fixture.catalog_path()) == fixture.legacy_project);
            }
            rejected(files->save_scene(*loaded.owner, unsaved.revision, loaded.file_revision),
                     ErrorCode::conflict, "source.file.save_scene");
            unchanged_owner(*loaded.owner, unsaved, child);
            CHECK(read_bytes(fixture.scene_path()) == invalid.bytes);

            // Repairing the external file still requires an explicit dirty-state decision.
            write_bytes(fixture.scene_path(), fixture.legacy_scene);
            rejected(files->reload_scene(*loaded.owner, unsaved.revision), ErrorCode::conflict,
                     "source.file.reload_scene");
            unchanged_owner(*loaded.owner, unsaved, child);
            const auto restored = take(
                files->reload_scene(*loaded.owner, unsaved.revision, UnsavedScenePolicy::discard));
            CHECK(restored.source_schema_version == 0);
            CHECK(restored.revision.scene_instance != unsaved.revision.scene_instance);
            CHECK(take(loaded.owner->capture()).scene == fixture.scene);
            CHECK_FALSE(take(loaded.owner->status()).dirty);
            CHECK_FALSE(live(*loaded.owner).entity_state(child));
            CHECK(read_bytes(fixture.scene_path()) == fixture.legacy_scene);
        }
    }
}

TEST_CASE("external edits can be reconciled explicitly without losing either authors source",
          "[persistence-integration]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    const auto catalog = take(files->read_project(project_path()));
    auto loaded = take(files->load_scene(catalog.source.document, fixture.project.startup_scene));
    const auto child = find(*loaded.owner, child_id());
    REQUIRE(execute(*loaded.owner, SetNameCommand{child_id(), {"Unsaved local child"}}));
    const auto local = take(loaded.owner->capture());
    auto external = fixture.scene;
    external.entities.back().name.value = "External parent edit";
    external.extras_json = R"({"external_note":"preserved","layer":"coast"})";
    const auto external_bytes = take(encode_scene_document(external));
    write_bytes(fixture.scene_path(), external_bytes);
    auto external_project = fixture.project;
    external_project.name.value = "External project edit";
    const auto external_project_bytes = take(encode_project_document(external_project));
    write_bytes(fixture.catalog_path(), external_project_bytes);

    rejected(files->save_scene(*loaded.owner, local.revision, loaded.file_revision),
             ErrorCode::conflict, "source.file.save_scene");
    rejected(files->write_project(project_path(), fixture.project, catalog.revision),
             ErrorCode::conflict, "source.file.write_project");
    rejected(files->reload_scene(*loaded.owner, local.revision), ErrorCode::conflict,
             "source.file.reload_scene");
    unchanged_owner(*loaded.owner, local, child);
    CHECK(read_bytes(fixture.scene_path()) == external_bytes);
    CHECK(read_bytes(fixture.catalog_path()) == external_project_bytes);

    // Both snapshots are owned values. No implicit merge or second mutable scene is needed.
    const auto observed_external =
        take(files->read_scene(catalog.source.document, fixture.project.startup_scene));
    CHECK(observed_external.source.document == external);
    const auto reloaded =
        take(files->reload_scene(*loaded.owner, local.revision, UnsavedScenePolicy::discard));
    CHECK(reloaded.file_revision == observed_external.revision);
    CHECK(reloaded.source_schema_version == 1);
    CHECK_FALSE(live(*loaded.owner).entity_state(child));
    CHECK_FALSE(take(loaded.owner->status()).dirty);
    CHECK(take(loaded.owner->capture()).scene == external);
    CHECK(take(loaded.owner->capture()).project == fixture.project);
    CHECK(local.scene.entities.front().name.value == "Unsaved local child");
    auto merged = external;
    merged.entities.front().name = local.scene.entities.front().name;
    REQUIRE(execute(*loaded.owner, SetNameCommand{child_id(), merged.entities.front().name}));
    const auto merged_capture = take(loaded.owner->capture());
    CHECK(merged_capture.scene == merged);
    rejected(files->save_scene(*loaded.owner, local.revision, reloaded.file_revision),
             ErrorCode::conflict, "source.file.save_scene");
    CHECK(read_bytes(fixture.scene_path()) == external_bytes);
    const auto saved =
        take(files->save_scene(*loaded.owner, merged_capture.revision, reloaded.file_revision));
    CHECK(saved.current_scene_clean);
    check_file_flush(saved.file.durability);
    CHECK(read_bytes(fixture.scene_path()) == take(encode_scene_document(merged)));
    CHECK(read_bytes(fixture.catalog_path()) == external_project_bytes);
    CHECK(local.scene.entities.back().name == fixture.scene.entities.back().name);
    CHECK(observed_external.source.document == external);
    loaded.owner.reset();
    files.reset();

    // The explicit project reread adopts its external edit at the next owner construction.
    files = fixture.open();
    const auto fresh_catalog = take(files->read_project(project_path()));
    CHECK(fresh_catalog.source.document == external_project);
    auto fresh = take(files->load_scene(fresh_catalog.source.document,
                                        fresh_catalog.source.document.startup_scene));
    const auto final = take(fresh.owner->capture());
    CHECK(final.scene == merged);
    CHECK(final.project == external_project);
    CHECK_FALSE(final.dirty);
    CHECK(fresh.file_revision == saved.file.revision);
    CHECK(read_bytes(fixture.catalog_path()) == external_project_bytes);
}
