#include "../editor/project_session.hpp"

#include <shark/tools/project_files.hpp>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace {
using namespace shark;
using namespace shark::editor;
using namespace shark::tools;
using Json = nlohmann::json;
namespace fs = std::filesystem;

constexpr auto project_id = "16000000-0000-4000-8000-000000000001";
constexpr auto world_id = "16000000-0000-4000-8000-000000000002";
constexpr auto scene_id = "16000000-0000-4000-8000-000000000003";
constexpr auto entity_id = "16000000-0000-4000-8000-000000000004";

template <typename T> T take(Result<T> result) {
    REQUIRE(result);
    return std::move(result).value();
}

template <typename T> void rejected(const Result<T>& result, ErrorCode code) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().message.empty());
    CHECK_FALSE(result.error().context.operation.empty());
}

Json result(AuthoringProtocolReply reply) {
    REQUIRE_FALSE(reply.failed);
    const auto document = Json::parse(reply.json);
    REQUIRE_FALSE(document.contains("error"));
    return document.at("result");
}

Json call(ProjectSession& owner, std::string_view method, Json parameters = Json::object()) {
    return result(take(owner.request(method, parameters.dump())));
}

Json terminal_call(AuthoringProtocol& owner, std::string_view method,
                   Json parameters = Json::object()) {
    return Json::parse(take(owner.handle(Json{{"jsonrpc", "2.0"},
                                              {"protocol_version", 1},
                                              {"id", "terminal-fixture"},
                                              {"method", method},
                                              {"params", std::move(parameters)}}
                                             .dump()))
                           .json);
}

void rename(ProjectSession& owner, const std::string& name) {
    const auto state = call(owner, "scene.mutation_status");
    const auto changed =
        call(owner, "scene.mutate",
             {{"mutation_id", state.at("next")},
              {"expected_revision", state.at("revision")},
              {"operation",
               {{"type", "command"},
                {"command", {{"type", "set_name"}, {"entity_id", entity_id}, {"name", name}}}}}});
    REQUIRE(changed.at("outcome").at("ok").get<bool>());
    REQUIRE(changed.at("outcome").at("value").at("result").at("ok").get<bool>());
}

std::string read(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void write(const fs::path& path, std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    REQUIRE(output);
}

class Fixture final {
  public:
    Fixture() {
        tree = fs::temp_directory_path() /
               ("shark-editor-session-" + take(generate_stable_id()).to_string());
        REQUIRE(fs::create_directory(tree));
        root = tree / fs::path{u8"Project café 世界"};
        REQUIRE(fs::create_directory(root));
        REQUIRE(fs::create_directory(root / "scenes"));
        auto files = take(ProjectFiles::open(root));
        ProjectDocument project{ProjectId{take(StableId::parse(project_id))},
                                WorldId{take(StableId::parse(world_id))},
                                {"Editor fixture"},
                                SceneId{take(StableId::parse(scene_id))},
                                {{SceneId{take(StableId::parse(scene_id))},
                                  take(ProjectPath::parse("scenes/Main.sharkscene"))}}};
        SceneDocument scene{
            project.project,
            project.startup_scene,
            {},
            {{AuthoredEntityId{take(StableId::parse(entity_id))}, {}, {"Original"}, {}, "{}"}}};
        static_cast<void>(take(files->write_scene(project, scene, std::nullopt)));
        static_cast<void>(take(files->write_project(
            take(ProjectPath::parse("Project.sharkproject")), project, std::nullopt)));
    }
    ~Fixture() {
        std::error_code ignored;
        fs::remove_all(tree, ignored);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    fs::path tree;
    fs::path root;
    fs::path scene_path() const {
        return root / "scenes/Main.sharkscene";
    }
};
} // namespace

TEST_CASE("Editor project session retains shared dirty history and releases one writer",
          "[editor-project-session]") {
    Fixture fixture;
    auto editor = take(ProjectSession::create(fixture.root, "Project.sharkproject"));
    REQUIRE(editor->state().open);
    CHECK_FALSE(editor->state().dirty);
    CHECK(editor->state().name == "Editor fixture");
    CHECK(editor->state().project_id == project_id);
    const auto original = read(fixture.scene_path());

    auto terminal = take(AuthoringProtocol::create(fixture.root));
    const auto busy = terminal_call(*terminal, "project.open", {{"path", "Project.sharkproject"}});
    REQUIRE(busy.contains("error"));
    CHECK(busy.at("error").at("data").at("code") == "conflict");

    rename(*editor, "Kept unsaved");
    const auto dirty = editor->state();
    const auto history = call(*editor, "scene.history");
    const auto retries = call(*editor, "scene.mutation_status");
    REQUIRE(dirty.dirty);
    CHECK(history.at("undo_entries") == 1);
    CHECK(read(fixture.scene_path()) == original);
    rejected(editor->open("missing.sharkproject"), ErrorCode::conflict);
    CHECK(editor->state() == dirty);
    const auto cancelled = take(editor->close(CloseChoice::cancel, dirty.revision));
    CHECK(cancelled.cancelled);
    CHECK_FALSE(cancelled.closed);
    rejected(editor->close(CloseChoice::reject, dirty.revision), ErrorCode::conflict);
    auto stale = dirty.revision;
    ++stale.revision;
    rejected(editor->close(CloseChoice::discard, stale), ErrorCode::conflict);
    CHECK(editor->state() == dirty);
    CHECK(call(*editor, "scene.history") == history);
    CHECK(call(*editor, "scene.mutation_status") == retries);

    const auto saved = take(editor->save(dirty.revision));
    CHECK(saved.published);
    CHECK(saved.current_scene_clean);
    CHECK(saved.bytes == fs::file_size(fixture.scene_path()));
    CHECK_FALSE(saved.durability.empty());
    CHECK_FALSE(editor->state().dirty);
    CHECK(call(*editor, "scene.history") == history);
    CHECK(call(*editor, "scene.mutation_status") == retries);
    CHECK(read(fixture.scene_path()) != original);
    REQUIRE(take(editor->close(CloseChoice::reject, editor->state().revision)).closed);
    CHECK_FALSE(editor->state().open);
    const auto opened =
        terminal_call(*terminal, "project.open", {{"path", "Project.sharkproject"}});
    REQUIRE(opened.contains("result"));
    CHECK_FALSE(opened.at("result").at("dirty").get<bool>());
    const auto inspected =
        terminal_call(*terminal, "scene.inspect", {{"entity_id", entity_id}, {"fields", {"name"}}});
    CHECK(inspected.at("result").at("entities").at(0).at("name") == "Kept unsaved");
}

TEST_CASE("Editor save conflict preserves dirty owner and exact external bytes",
          "[editor-project-session]") {
    Fixture fixture;
    auto editor = take(ProjectSession::create(fixture.root, "Project.sharkproject"));
    const auto original = read(fixture.scene_path());
    rename(*editor, "Local edit");
    const auto dirty = editor->state();
    const auto history = call(*editor, "scene.history");
    write(fixture.scene_path(), original + "\n");
    rejected(editor->save(dirty.revision), ErrorCode::conflict);
    rejected(editor->close(CloseChoice::save, dirty.revision), ErrorCode::conflict);
    CHECK(editor->state() == dirty);
    CHECK(call(*editor, "scene.history") == history);
    CHECK(read(fixture.scene_path()) == original + "\n");

    const auto discarded = take(editor->close(CloseChoice::discard, dirty.revision));
    CHECK(discarded.closed);
    CHECK_FALSE(discarded.saved);
    REQUIRE(editor->open("Project.sharkproject"));
    CHECK_FALSE(editor->state().dirty);
    CHECK(call(*editor, "scene.history").at("undo_entries") == 0);
    CHECK(call(*editor, "scene.inspect", {{"entity_id", entity_id}, {"fields", {"name"}}})
              .at("entities")
              .at(0)
              .at("name") == "Original");
}

TEST_CASE("Editor explicit save-close hands off while failed opening retains no lock",
          "[editor-project-session]") {
    Fixture fixture;
    auto editor = take(ProjectSession::create(fixture.root));
    rejected(editor->open("../outside.sharkproject"), ErrorCode::invalid_path);
    CHECK_FALSE(editor->state().open);
    REQUIRE_FALSE(editor->open("missing.sharkproject"));
    CHECK_FALSE(editor->state().open);
    REQUIRE(editor->open("Project.sharkproject"));
    rename(*editor, "Saved on close");
    const auto closed = take(editor->close(CloseChoice::save, editor->state().revision));
    REQUIRE(closed.closed);
    REQUIRE(closed.saved);
    CHECK(closed.saved->published);
    CHECK_FALSE(closed.error);
    auto next = take(ProjectSession::create(fixture.root, "Project.sharkproject"));
    CHECK_FALSE(next->state().dirty);
    CHECK(call(*next, "scene.inspect", {{"entity_id", entity_id}, {"fields", {"name"}}})
              .at("entities")
              .at(0)
              .at("name") == "Saved on close");
    CHECK(call(*next, "scene.history").at("undo_entries") == 0);
}

TEST_CASE("Editor session preserves strict protocol rejection and owner-thread access",
          "[editor-project-session]") {
    Fixture fixture;
    auto editor = take(ProjectSession::create(fixture.root, "Project.sharkproject"));
    const auto before = editor->state();
    auto duplicate =
        take(editor->request("project.close", R"({"unsaved":"reject","unsaved":"discard"})"));
    CHECK(duplicate.failed);
    CHECK(Json::parse(duplicate.json).at("error").at("data").at("code") == "invalid_configuration");
    CHECK(editor->state() == before);
    std::optional<Result<void>> foreign;
    std::thread worker([&] { foreign.emplace(editor->refresh()); });
    worker.join();
    REQUIRE(foreign);
    rejected(*foreign, ErrorCode::conflict);
    CHECK(editor->state() == before);

    const auto status = call(*editor, "scene.mutation_status");
    const Json rejected_mutation{
        {"mutation_id", status.at("next")},
        {"expected_revision", status.at("revision")},
        {"operation",
         {{"type", "command"},
          {"command", {{"type", "set_name"}, {"entity_id", world_id}, {"name", "Missing"}}}}}};
    const auto reply = take(editor->request("scene.mutate", rejected_mutation.dump()));
    REQUIRE(reply.failed);
    const auto document = Json::parse(reply.json);
    REQUIRE_FALSE(document.contains("error"));
    const auto& retained = document.at("result").at("outcome");
    REQUIRE(retained.at("ok").get<bool>());
    CHECK_FALSE(retained.at("value").at("result").at("ok").get<bool>());
    CHECK(editor->state() == before);
    CHECK(call(*editor, "scene.mutation_status").at("retained_entries") == 1);
}

TEST_CASE("Editor viewport resource inventory uses the same owner and rejects invalid input",
          "[editor-project-session]") {
    Fixture fixture;
    auto editor = take(ProjectSession::create(fixture.root, "Project.sharkproject"));
    rename(*editor, "Unsaved inventory observation");
    const auto before = editor->state();
    const auto history = call(*editor, "scene.history");
    REQUIRE(editor->refresh_asset_inventory({}));
    const std::array<std::string_view, 1> escaping{"../outside.sharkasset"};
    REQUIRE_FALSE(editor->refresh_asset_inventory(escaping));
    const std::string oversized(16385, 'x');
    const std::array<std::string_view, 1> excessive{oversized};
    rejected(editor->refresh_asset_inventory(excessive), ErrorCode::invalid_argument);
    CHECK(editor->state() == before);
    CHECK(call(*editor, "scene.history") == history);
    auto competing = take(ProjectSession::create(fixture.root));
    rejected(competing->open("Project.sharkproject"), ErrorCode::conflict);
}
