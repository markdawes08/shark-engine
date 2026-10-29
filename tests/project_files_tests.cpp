#include <shark/tools/project_files.hpp>

#include "authored_scene_test_access.hpp"
#include "project_files_native.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

namespace {
using namespace shark;
using namespace shark::tools;
namespace fs = std::filesystem;

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

template <typename Id> Id id(unsigned value) {
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index != 8; ++index) {
        text[35 - index] = digits[value & 15U];
        value >>= 4U;
    }
    return Id{take(StableId::parse(text))};
}

ProjectPath path(std::string_view text) {
    return take(ProjectPath::parse(text));
}

ProjectDocument project() {
    return {id<ProjectId>(1),
            id<WorldId>(2),
            {"Shared project"},
            id<SceneId>(3),
            {{id<SceneId>(3), path("scenes/start.sharkscene")},
             {id<SceneId>(4), path("scenes/other.sharkscene")}},
            "{}"};
}

SceneDocument scene(std::string name = "First") {
    return {id<ProjectId>(1),
            id<SceneId>(3),
            WorldPosition{id<WorldId>(2), {100, -200, 300}, {0.25, 0.5, 0.75}},
            {{id<AuthoredEntityId>(10),
              std::nullopt,
              {std::move(name)},
              {},
              R"({"source-note":"retained"})"}},
            "{}"};
}

void write_bytes(const fs::path& target, std::string_view bytes) {
    std::ofstream output{target, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
        throw std::runtime_error{"fixture write failed"};
    }
}

std::string read_bytes(const fs::path& target) {
    std::ifstream input{target, std::ios::binary};
    if (!input) {
        throw std::runtime_error{"fixture read failed"};
    }
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

class Fixture final {
  public:
    Fixture() {
        const auto unique = take(generate_stable_id()).to_string();
        tree = fs::temp_directory_path() / ("shark-project-files-" + unique);
        REQUIRE(fs::create_directory(tree));
        root = tree / fs::path{u8"Shared 世界 project"};
        REQUIRE(fs::create_directory(root));
        REQUIRE(fs::create_directory(root / "scenes"));
    }
    ~Fixture() {
        std::error_code error;
        fs::remove_all(tree, error);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    fs::path tree;
    fs::path root;
    fs::path scene_path() const {
        return root / "scenes/start.sharkscene";
    }
    std::unique_ptr<ProjectFiles> open() const {
        return take(ProjectFiles::open(root));
    }
    void seed(const SceneDocument& value = scene()) const {
        write_bytes(root / "project.sharkproject", take(encode_project_document(project())));
        write_bytes(scene_path(), take(encode_scene_document(value)));
    }
};

class Control final {
  public:
    explicit Control(detail::FileTestControl value)
        : previous_(detail::set_file_test_control(value)) {}
    ~Control() {
        static_cast<void>(detail::set_file_test_control(previous_));
    }
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;

  private:
    detail::FileTestControl previous_;
};

std::size_t regular_files(const fs::path& directory) {
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator{directory}) {
        if (entry.is_regular_file()) {
            ++count;
        }
    }
    return count;
}

const Scene& live(const AuthoredScene& owner) {
    const auto access = take(owner.scene());
    return access.get();
}
Scene& fixture_scene(AuthoredScene& owner) {
    return take(detail::AuthoredSceneTestAccess::scene(owner)).get();
}
EntityHandle first(AuthoredScene& owner) {
    return take(live(owner).find(id<AuthoredEntityId>(10)));
}
AuthoredSceneStatus status(AuthoredScene& owner) {
    return take(owner.status());
}

Result<AuthoringCommandEffect> execute(AuthoredScene& owner, const AuthoringCommand& command) {
    auto reply = take(owner.execute(command, status(owner).revision));
    CHECK(reply.observed_revision == status(owner).revision);
    return std::move(reply.outcome);
}

static_assert(!std::is_copy_constructible_v<ProjectFiles>);
static_assert(!std::is_move_constructible_v<ProjectFiles>);
static_assert(!std::is_default_constructible_v<FileRevision>);

} // namespace

TEST_CASE("project files read validated owned sources with exact content revisions",
          "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    auto loaded_project = take(files->read_project(path("project.sharkproject")));
    CHECK(loaded_project.source.document == project());
    CHECK(loaded_project.source.source_schema_version == 1);
    CHECK(loaded_project.revision.valid());
    CHECK(loaded_project.revision.byte_count() ==
          read_bytes(fixture.root / "project.sharkproject").size());
    auto loaded = take(files->read_scene(project(), id<SceneId>(3)));
    CHECK(loaded.source.document == scene());
    CHECK(loaded.source.source_schema_version == 1);
    CHECK(loaded.revision == take(files->read_scene(project(), id<SceneId>(3))).revision);
    const auto old = loaded.revision;
    write_bytes(fixture.scene_path(), take(encode_scene_document(scene("Other"))));
    CHECK(loaded.source.document.entities.front().name.value == "First");
    CHECK_FALSE(old == take(files->read_scene(project(), id<SceneId>(3))).revision);
}

TEST_CASE("project files create only absent destinations and require existing parents",
          "[project-files]") {
    Fixture fixture;
    auto files = fixture.open();
    auto committed =
        take(files->write_project(path("project.sharkproject"), project(), std::nullopt));
    CHECK(committed.revision.valid());
    CHECK(take(files->read_project(path("project.sharkproject"))).revision == committed.revision);
    rejected(files->write_project(path("project.sharkproject"), project(), std::nullopt),
             ErrorCode::conflict);
    const auto saved = take(files->write_scene(project(), scene(), std::nullopt));
    CHECK(take(files->read_scene(project(), id<SceneId>(3))).revision == saved.revision);
    rejected(files->write_scene(project(), scene("Other"), std::nullopt), ErrorCode::conflict);
    const auto missing =
        files->write_project(path("missing/project.sharkproject"), project(), std::nullopt);
    REQUIRE_FALSE(missing);
    CHECK_FALSE(fs::exists(fixture.root / "missing"));
    CHECK(read_bytes(fixture.scene_path()) == take(encode_scene_document(scene())));
}

TEST_CASE("content revisions reject same size timestamp edits and accept exact byte return",
          "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    const auto old = take(files->read_scene(project(), id<SceneId>(3))).revision;
    const auto bytes = read_bytes(fixture.scene_path());
    const auto timestamp = fs::last_write_time(fixture.scene_path());
    const auto external = take(encode_scene_document(scene("Other")));
    REQUIRE(external.size() == bytes.size());
    write_bytes(fixture.scene_path(), external);
    fs::last_write_time(fixture.scene_path(), timestamp);
    rejected(files->write_scene(project(), scene("Saved"), old), ErrorCode::conflict);
    CHECK(read_bytes(fixture.scene_path()) == external);
    write_bytes(fixture.scene_path(), bytes);
    CHECK(take(files->read_scene(project(), id<SceneId>(3))).revision == old);
    CHECK(files->write_scene(project(), scene("Saved"), old));
}

TEST_CASE("content revisions bind canonical root and relative path across owner reopen",
          "[project-files]") {
    Fixture first_fixture;
    Fixture second_fixture;
    first_fixture.seed();
    second_fixture.seed();
    auto first_files = first_fixture.open();
    auto second_files = second_fixture.open();
    const auto revision = take(first_files->read_scene(project(), id<SceneId>(3))).revision;
    rejected(second_files->write_scene(project(), scene("Other"), revision), ErrorCode::conflict);
    auto other_scene = scene();
    other_scene.scene = id<SceneId>(4);
    rejected(first_files->write_scene(project(), other_scene, revision), ErrorCode::conflict);
    rejected(first_files->write_project(path("project.sharkproject"), project(), revision),
             ErrorCode::conflict);
    first_files.reset();
    first_files = first_fixture.open();
    CHECK(first_files->write_scene(project(), scene("Other"), revision));
}

TEST_CASE("expected existing files cannot be silently recreated after deletion",
          "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    const auto revision = take(files->read_scene(project(), id<SceneId>(3))).revision;
    REQUIRE(fs::remove(fixture.scene_path()));
    rejected(files->write_scene(project(), scene(), revision), ErrorCode::conflict);
    CHECK_FALSE(fs::exists(fixture.scene_path()));
    CHECK(files->write_scene(project(), scene(), std::nullopt));
}

TEST_CASE("moved revisions are invalid and cannot authorize a write", "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    auto revision = take(files->read_scene(project(), id<SceneId>(3))).revision;
    const auto retained = std::move(revision);
    CHECK_FALSE(revision.valid());
    CHECK(revision.byte_count() == 0);
    CHECK_FALSE(revision == retained);
    rejected(files->write_scene(project(), scene("Other"), revision), ErrorCode::conflict);
    CHECK(files->write_scene(project(), scene("Other"), retained));
}

TEST_CASE("project read rejects missing malformed oversized and nonregular sources",
          "[project-files]") {
    Fixture fixture;
    auto files = fixture.open();
    REQUIRE_FALSE(files->read_project(path("missing.sharkproject")));
    write_bytes(fixture.root / "project.sharkproject", "{bad json");
    REQUIRE_FALSE(files->read_project(path("project.sharkproject")));
    write_bytes(fixture.root / "project.sharkproject", std::string(source_maximum_bytes + 1, ' '));
    REQUIRE_FALSE(files->read_project(path("project.sharkproject")));
    REQUIRE_FALSE(files->read_project(path("scenes")));
    CHECK(fs::is_directory(fixture.root / "scenes"));
}

TEST_CASE("source files validate catalog project world and composed scene references",
          "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    auto candidate = scene();
    candidate.project = id<ProjectId>(99);
    REQUIRE_FALSE(files->write_scene(project(), candidate, std::nullopt));
    write_bytes(fixture.scene_path(), take(encode_scene_document(candidate)));
    REQUIRE_FALSE(files->read_scene(project(), id<SceneId>(3)));
    candidate = scene();
    candidate.world_origin->world = id<WorldId>(99);
    write_bytes(fixture.scene_path(), take(encode_scene_document(candidate)));
    REQUIRE_FALSE(files->read_scene(project(), id<SceneId>(3)));
    REQUIRE_FALSE(files->read_scene(project(), id<SceneId>(99)));
    candidate = scene();
    candidate.scene = id<SceneId>(4);
    write_bytes(fixture.scene_path(), take(encode_scene_document(candidate)));
    REQUIRE_FALSE(files->read_scene(project(), id<SceneId>(3)));
}

TEST_CASE("invalid source candidates cannot alter an existing valid file", "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    const auto loaded = take(files->read_scene(project(), id<SceneId>(3)));
    const auto bytes = read_bytes(fixture.scene_path());
    auto invalid = scene();
    invalid.entities.front().name.value.clear();
    invalid.entities.front().transform.scale.x = 0;
    REQUIRE_FALSE(files->write_scene(project(), invalid, loaded.revision));
    CHECK(read_bytes(fixture.scene_path()) == bytes);
    CHECK(take(files->read_scene(project(), id<SceneId>(3))).revision == loaded.revision);
    CHECK(regular_files(fixture.root / "scenes") == 1);
}

TEST_CASE("portable file paths reject traversal aliases and catalog case collisions",
          "[project-files]") {
    for (const auto* reference :
         {"../escape", "/absolute", "C:/absolute", "bad\\path", "a/../b", "NUL", "name:stream"}) {
        REQUIRE_FALSE(ProjectPath::parse(reference));
    }
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    auto collision = project();
    collision.scenes.back().path = path("SCENES/START.SHARKSCENE");
    REQUIRE_FALSE(files->write_project(path("collision.sharkproject"), collision, std::nullopt));
    CHECK_FALSE(fs::exists(fixture.root / "collision.sharkproject"));
    auto prefix = project();
    prefix.scenes.back().path = path("scenes/start.sharkscene/child.sharkscene");
    REQUIRE_FALSE(files->write_project(path("prefix.sharkproject"), prefix, std::nullopt));
    CHECK_FALSE(fs::exists(fixture.root / "prefix.sharkproject"));
    rejected(files->read_project(path("PROJECT.SHARKPROJECT")), ErrorCode::invalid_path);
}

TEST_CASE("same root permits one cooperating owner and releases ownership on destruction",
          "[project-files]") {
    Fixture fixture;
    auto files = fixture.open();
    rejected(ProjectFiles::open(fixture.root), ErrorCode::conflict);
    files.reset();
    CHECK(ProjectFiles::open(fixture.root));
    REQUIRE_FALSE(ProjectFiles::open(fixture.root / "missing"));
    write_bytes(fixture.root / "file", "fixture");
    REQUIRE_FALSE(ProjectFiles::open(fixture.root / "file"));
}

TEST_CASE("catalog rejects project scene overlap and absent case variant directory aliases",
          "[project-files]") {
    Fixture fixture;
    auto files = fixture.open();
    rejected(files->write_project(path("scenes/start.sharkscene"), project(), std::nullopt),
             ErrorCode::conflict);
    auto nested = project();
    nested.scenes.front().path = path("new/scene.sharkscene");
    nested.scenes.back().path = path("new/other.sharkscene");
    rejected(files->write_project(path("new"), nested, std::nullopt), ErrorCode::conflict);
    nested.scenes.back().path = path("NEW/other.sharkscene");
    rejected(files->write_project(path("project.sharkproject"), nested, std::nullopt),
             ErrorCode::conflict);
    CHECK_FALSE(fs::exists(fixture.root / "new"));
    CHECK_FALSE(fs::exists(fixture.root / "project.sharkproject"));
}

TEST_CASE("reserved service paths and overdeep references reject without creating directories",
          "[project-files]") {
    Fixture fixture;
    auto files = fixture.open();
    for (const auto* reference : {".shark-write.lock", ".SHARK-WRITE.LOCK", ".shark-stage-owned",
                                  "scenes/.SHARK-STAGE-foreign", ".shark-stage-dir/scene"}) {
        rejected(files->write_project(path(reference), project(), std::nullopt),
                 ErrorCode::invalid_path);
    }
    std::string deep;
    for (unsigned index = 0; index < 64; ++index) {
        deep += "a/";
    }
    deep += "project.sharkproject";
    rejected(files->write_project(path(deep), project(), std::nullopt), ErrorCode::invalid_path);
    CHECK_FALSE(fs::exists(fixture.root / "a"));
    CHECK_FALSE(fs::exists(fixture.root / ".shark-stage-dir"));
}

TEST_CASE("project files reject internal and escaping symlink reparse aliases", "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    REQUIRE(fs::create_directory(fixture.tree / "outside"));
    write_bytes(fixture.tree / "outside/project.sharkproject",
                take(encode_project_document(project())));
    fs::create_directory_symlink(fixture.tree / "outside", fixture.root / "escape");
    fs::create_directory_symlink(fixture.root / "scenes", fixture.root / "alias");
    fs::create_symlink(fixture.scene_path(), fixture.root / "scene-alias");
    fs::create_symlink(fixture.tree / "outside/project.sharkproject", fixture.root / "file-escape");
    for (const auto* reference :
         {"escape/project.sharkproject", "escape/new.sharkproject", "file-escape"}) {
        const auto result = files->write_project(path(reference), project(), std::nullopt);
        REQUIRE_FALSE(result);
        CHECK((result.error().code == ErrorCode::path_outside_root ||
               result.error().code == ErrorCode::invalid_path));
    }
    for (const auto* reference : {"alias/new.sharkproject", "scene-alias"}) {
        rejected(files->write_project(path(reference), project(), std::nullopt),
                 ErrorCode::invalid_path);
    }
    CHECK_FALSE(fs::exists(fixture.tree / "outside/new.sharkproject"));
    CHECK_FALSE(fs::exists(fixture.root / "scenes/new.sharkproject"));
    CHECK(read_bytes(fixture.scene_path()) == take(encode_scene_document(scene())));
    fs::create_symlink(fixture.tree / "missing", fixture.root / "dangling");
    REQUIRE_FALSE(files->read_project(path("dangling")));
}

TEST_CASE("hard linked destinations reject rather than mutate or split another alias",
          "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    const auto revision = take(files->read_scene(project(), id<SceneId>(3))).revision;
    fs::create_hard_link(fixture.scene_path(), fixture.tree / "outside-link");
    rejected(files->read_scene(project(), id<SceneId>(3)), ErrorCode::invalid_path);
    rejected(files->write_scene(project(), scene("Other"), revision), ErrorCode::invalid_path);
    CHECK(read_bytes(fixture.tree / "outside-link") == take(encode_scene_document(scene())));
    CHECK(fs::equivalent(fixture.tree / "outside-link", fixture.scene_path()));
}

TEST_CASE("path inspection has a fixed bound on directory entries", "[project-files]") {
    Fixture fixture;
    auto files = fixture.open();
    REQUIRE(fs::create_directory(fixture.root / "crowded"));
    for (unsigned index = 0; index != 8193; ++index) {
        write_bytes(fixture.root / "crowded" / ("entry-" + std::to_string(index)), "");
    }
    REQUIRE_FALSE(
        files->write_project(path("crowded/project.sharkproject"), project(), std::nullopt));
    CHECK_FALSE(fs::exists(fixture.root / "crowded/project.sharkproject"));
}

TEST_CASE("scene loading rejects composition failures without changing source bytes",
          "[project-files]") {
    Fixture fixture;
    auto invalid = scene();
    invalid.entities.front().transform.scale = {1e200, 1e200, 1e200};
    auto child = invalid.entities.front();
    child.id = id<AuthoredEntityId>(11);
    child.parent = invalid.entities.front().id;
    invalid.entities.push_back(child);
    fixture.seed(invalid);
    auto files = fixture.open();
    const auto bytes = read_bytes(fixture.scene_path());
    REQUIRE(files->read_scene(project(), id<SceneId>(3)));
    REQUIRE_FALSE(files->load_scene(project(), id<SceneId>(3)));
    const auto revision = take(files->read_scene(project(), id<SceneId>(3))).revision;
    REQUIRE_FALSE(files->write_scene(project(), invalid, revision));
    CHECK(read_bytes(fixture.scene_path()) == bytes);
}

TEST_CASE("native read and prepublication faults preserve old bytes and remove own staging",
          "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    const auto revision = take(files->read_scene(project(), id<SceneId>(3))).revision;
    const auto old = read_bytes(fixture.scene_path());
    for (const auto fault :
         {detail::FileFault::read, detail::FileFault::stage_create, detail::FileFault::stage_write,
          detail::FileFault::stage_flush, detail::FileFault::publish}) {
        INFO(static_cast<int>(fault));
        {
            Control control{{fault, nullptr, nullptr}};
            rejected(files->write_scene(project(), scene("Other"), revision),
                     ErrorCode::io_failure);
        }
        CHECK(read_bytes(fixture.scene_path()) == old);
        CHECK(take(files->read_scene(project(), id<SceneId>(3))).revision == revision);
        CHECK(regular_files(fixture.root / "scenes") == 1);
    }
    {
        Control control{{detail::FileFault::read, nullptr, nullptr}};
        rejected(files->read_scene(project(), id<SceneId>(3)), ErrorCode::io_failure);
    }
}

TEST_CASE("postpublication directory flush failure returns committed bytes and retry revision",
          "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    const auto revision = take(files->read_scene(project(), id<SceneId>(3))).revision;
    std::optional<FileCommit> committed;
    {
        Control control{{detail::FileFault::directory_flush, nullptr, nullptr}};
        committed = take(files->write_scene(project(), scene("Other"), revision));
    }
    CHECK(committed->durability == FileDurability::directory_flush_failed);
    CHECK(read_bytes(fixture.scene_path()) == take(encode_scene_document(scene("Other"))));
    CHECK(take(files->read_scene(project(), id<SceneId>(3))).revision == committed->revision);
    rejected(files->write_scene(project(), scene("Retry"), revision), ErrorCode::conflict);
    CHECK(files->write_scene(project(), scene("Retry"), committed->revision));
}

TEST_CASE(
    "authored saves preserve dirty snapshots through faults and acknowledge committed durability",
    "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    auto loaded = take(files->load_scene(project(), id<SceneId>(3)));
    auto& owner = *loaded.owner;
    const auto handle = first(owner);
    REQUIRE(execute(owner, SetNameCommand{id<AuthoredEntityId>(10), {"Unsaved authoring work"}}));
    const auto before = take(owner.capture());
    REQUIRE(before.dirty);
    const auto old_bytes = read_bytes(fixture.scene_path());
    for (const auto fault :
         {detail::FileFault::read, detail::FileFault::stage_create, detail::FileFault::stage_write,
          detail::FileFault::stage_flush, detail::FileFault::publish}) {
        INFO(static_cast<int>(fault));
        {
            Control control{{fault, nullptr, nullptr}};
            rejected(files->save_scene(owner, before.revision, loaded.file_revision),
                     ErrorCode::io_failure);
        }
        const auto retained = take(owner.capture());
        CHECK(retained.project == before.project);
        CHECK(retained.scene == before.scene);
        CHECK(retained.revision == before.revision);
        CHECK(retained.dirty);
        CHECK(first(owner) == handle);
        CHECK(status(owner).pending_mutations == 0);
        CHECK(read_bytes(fixture.scene_path()) == old_bytes);
        CHECK(take(files->read_scene(project(), id<SceneId>(3))).revision == loaded.file_revision);
        CHECK(regular_files(fixture.root / "scenes") == 1);
    }
    std::optional<SavedAuthoredScene> saved;
    {
        Control control{{detail::FileFault::directory_flush, nullptr, nullptr}};
        saved = take(files->save_scene(owner, before.revision, loaded.file_revision));
    }
    CHECK(saved->file.durability == FileDurability::directory_flush_failed);
    CHECK(saved->saved_revision == before.revision);
    CHECK(saved->current_scene_clean);
    CHECK_FALSE(status(owner).dirty);
    CHECK(status(owner).revision == before.revision);
    CHECK(read_bytes(fixture.scene_path()) == take(encode_scene_document(before.scene)));
    CHECK(take(files->read_scene(project(), id<SceneId>(3))).revision == saved->file.revision);
    rejected(files->save_scene(owner, before.revision, loaded.file_revision), ErrorCode::conflict);
    CHECK_FALSE(status(owner).dirty);
    files.reset();
    files = fixture.open();
    const auto reopened = take(files->load_scene(project(), id<SceneId>(3)));
    const auto recovered = take(reopened.owner->capture());
    CHECK(recovered.project == before.project);
    CHECK(recovered.scene == before.scene);
    CHECK_FALSE(recovered.dirty);
    CHECK(recovered.revision.scene_instance != before.revision.scene_instance);
}

TEST_CASE("final comparison detects an external edit after staging before publication",
          "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    const auto revision = take(files->read_scene(project(), id<SceneId>(3))).revision;
    struct Edit {
        fs::path target;
        std::string bytes;
        bool done{};
        bool failed{};
    };
    Edit edit{fixture.scene_path(), take(encode_scene_document(scene("Third")))};
    const auto hook = [](detail::FilePhase phase, void* context) noexcept {
        auto& value = *static_cast<Edit*>(context);
        if (phase == detail::FilePhase::before_publish) {
            try {
                write_bytes(value.target, value.bytes);
                value.done = true;
            } catch (...) {
                value.failed = true;
            }
        }
    };
    {
        Control control{{detail::FileFault::none, hook, &edit}};
        rejected(files->write_scene(project(), scene("Saved"), revision), ErrorCode::conflict);
    }
    REQUIRE(edit.done);
    CHECK_FALSE(edit.failed);
    CHECK(read_bytes(fixture.scene_path()) == edit.bytes);
    CHECK(regular_files(fixture.root / "scenes") == 1);
}

TEST_CASE("publication prevents or detects relocation of its held ancestor directory",
          "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    const auto revision = take(files->read_scene(project(), id<SceneId>(3))).revision;
    const auto old = read_bytes(fixture.scene_path());
    const auto ancestor = fixture.root / "scenes";
    const auto relocated = fixture.tree / "relocated";
    // Both move targets stay within this fixture's explicitly owned tree.
    REQUIRE(fs::canonical(ancestor).parent_path() == fs::canonical(fixture.root));
    REQUIRE(fs::canonical(relocated.parent_path()) == fs::canonical(fixture.tree));
    REQUIRE_FALSE(fs::exists(relocated));
    write_bytes(ancestor / ".shark-stage-foreign", "unowned stage");
    struct Relocation {
        fs::path source;
        fs::path destination;
        std::error_code error;
        bool called{};
        bool threw{};
    };
    Relocation relocation{ancestor, relocated, {}};
    const auto hook = [](detail::FilePhase phase, void* context) noexcept {
        if (phase != detail::FilePhase::before_publish) {
            return;
        }
        auto& value = *static_cast<Relocation*>(context);
        value.called = true;
        try {
            fs::rename(value.source, value.destination, value.error);
        } catch (...) {
            value.threw = true;
        }
    };
    const auto result = [&] {
        Control control{{detail::FileFault::none, hook, &relocation}};
        return files->write_scene(project(), scene("Saved"), revision);
    }();
    REQUIRE(relocation.called);
    REQUIRE_FALSE(relocation.threw);
#if defined(_WIN32)
    // The held parent denies delete sharing, so Windows refuses the rename.
    REQUIRE(relocation.error);
    REQUIRE(result);
    CHECK_FALSE(fs::exists(relocated));
    CHECK(read_bytes(fixture.scene_path()) == take(encode_scene_document(scene("Saved"))));
    CHECK(read_bytes(ancestor / ".shark-stage-foreign") == "unowned stage");
    CHECK(regular_files(ancestor) == 2);
#else
    // POSIX permits directory movement despite the open descriptor; the final
    // topology check rejects and cleanup follows only the owned stage handle.
    REQUIRE_FALSE(relocation.error);
    rejected(result, ErrorCode::conflict);
    CHECK_FALSE(fs::exists(ancestor));
    CHECK(read_bytes(relocated / "start.sharkscene") == old);
    CHECK(read_bytes(relocated / ".shark-stage-foreign") == "unowned stage");
    CHECK(regular_files(relocated) == 2);
#endif
}

TEST_CASE("external edits and dirty reload preserve both authors until explicit resolution",
          "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    auto loaded = take(files->load_scene(project(), id<SceneId>(3)));
    auto& owner = *loaded.owner;
    const auto old_handle = first(owner);
    REQUIRE(execute(owner, SetNameCommand{id<AuthoredEntityId>(10), {"Unsaved human"}}));
    const auto dirty = take(owner.capture());
    const auto external = take(encode_scene_document(scene("External agent")));
    write_bytes(fixture.scene_path(), external);
    rejected(files->save_scene(owner, dirty.revision, loaded.file_revision), ErrorCode::conflict);
    rejected(files->reload_scene(owner, dirty.revision), ErrorCode::conflict);
    CHECK(take(owner.capture()).scene == dirty.scene);
    CHECK(status(owner).revision == dirty.revision);
    CHECK(status(owner).dirty);
    CHECK(read_bytes(fixture.scene_path()) == external);
    const auto reloaded =
        take(files->reload_scene(owner, dirty.revision, UnsavedScenePolicy::discard));
    CHECK_FALSE(reloaded.revision == dirty.revision);
    CHECK_FALSE(status(owner).dirty);
    CHECK(take(owner.capture()).scene.entities.front().name.value == "External agent");
    rejected(live(owner).entity_state(old_handle), ErrorCode::invalid_id);
    CHECK(take(files->read_scene(project(), id<SceneId>(3))).revision == reloaded.file_revision);
    CHECK(dirty.scene.entities.front().name.value == "Unsaved human");
}

TEST_CASE("create only publication rechecks a destination created after staging",
          "[project-files]") {
    Fixture fixture;
    auto files = fixture.open();
    struct Creation {
        fs::path target;
        std::string bytes;
        bool done{};
    };
    Creation external{fixture.scene_path(), take(encode_scene_document(scene("External")))};
    const auto hook = [](detail::FilePhase phase, void* context) noexcept {
        if (phase != detail::FilePhase::before_publish) {
            return;
        }
        auto& value = *static_cast<Creation*>(context);
        try {
            write_bytes(value.target, value.bytes);
            value.done = true;
        } catch (...) {
            value.done = false;
        }
    };
    {
        Control control{{detail::FileFault::none, hook, &external}};
        rejected(files->write_scene(project(), scene("Saved"), std::nullopt), ErrorCode::conflict);
    }
    REQUIRE(external.done);
    CHECK(read_bytes(fixture.scene_path()) == external.bytes);
    CHECK(regular_files(fixture.root / "scenes") == 1);
}

TEST_CASE("failed reload preserves active revision metadata pending work and dirty state",
          "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    auto loaded = take(files->load_scene(project(), id<SceneId>(3)));
    auto& owner = *loaded.owner;
    REQUIRE(execute(owner, SetNameCommand{id<AuthoredEntityId>(10), {"Unsaved"}}));
    const auto before = take(owner.capture());
    write_bytes(fixture.scene_path(), "{broken");
    REQUIRE_FALSE(files->reload_scene(owner, before.revision, UnsavedScenePolicy::discard));
    CHECK(take(owner.capture()).scene == before.scene);
    CHECK(status(owner).revision == before.revision);
    CHECK(status(owner).dirty);
    fixture.seed();
    REQUIRE(fixture_scene(owner).defer_set_name(first(owner), {"Queued"}));
    rejected(files->reload_scene(owner, before.revision, UnsavedScenePolicy::discard),
             ErrorCode::conflict);
    rejected(files->save_scene(owner, before.revision, loaded.file_revision), ErrorCode::conflict);
    CHECK(status(owner).pending_mutations == 1);
    CHECK(take(fixture_scene(owner).apply_deferred()).outcomes.size() == 1);
    CHECK(take(owner.capture()).scene.entities.front().name.value == "Queued");
}

TEST_CASE("successful save acknowledges only captured committed scene revision",
          "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    auto loaded = take(files->load_scene(project(), id<SceneId>(3)));
    auto& owner = *loaded.owner;
    REQUIRE(execute(owner, SetNameCommand{id<AuthoredEntityId>(10), {"Saved human"}}));
    const auto before = status(owner);
    REQUIRE(before.dirty);
    const auto saved = take(files->save_scene(owner, before.revision, loaded.file_revision));
    CHECK(saved.saved_revision == before.revision);
    CHECK(saved.current_scene_clean);
    CHECK_FALSE(status(owner).dirty);
    CHECK(status(owner).revision == before.revision);
    CHECK(take(files->read_scene(project(), id<SceneId>(3)))
              .source.document.entities.front()
              .name.value == "Saved human");
    REQUIRE(execute(owner, SetNameCommand{id<AuthoredEntityId>(10), {"New unsaved"}}));
    rejected(files->save_scene(owner, before.revision, saved.file.revision), ErrorCode::conflict);
    CHECK(status(owner).dirty);
}

TEST_CASE("save commit reports later owner mutation without falsely clearing dirty state",
          "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    auto loaded = take(files->load_scene(project(), id<SceneId>(3)));
    auto& owner = *loaded.owner;
    REQUIRE(execute(owner, SetNameCommand{id<AuthoredEntityId>(10), {"Captured"}}));
    const auto expected = status(owner).revision;
    struct Mutation {
        AuthoredScene* owner;
        AuthoredEntityId entity;
        bool success{};
    };
    Mutation mutation{&owner, id<AuthoredEntityId>(10)};
    const auto hook = [](detail::FilePhase phase, void* context) noexcept {
        auto& value = *static_cast<Mutation*>(context);
        if (phase == detail::FilePhase::published) {
            try {
                const auto current = value.owner->status();
                if (current) {
                    const auto reply = value.owner->execute(
                        SetNameCommand{value.entity, {"Later edit"}}, current.value().revision);
                    value.success = reply && static_cast<bool>(reply.value().outcome);
                }
            } catch (...) {
                value.success = false;
            }
        }
    };
    std::optional<SavedAuthoredScene> saved;
    {
        Control control{{detail::FileFault::none, hook, &mutation}};
        saved = take(files->save_scene(owner, expected, loaded.file_revision));
    }
    REQUIRE(mutation.success);
    CHECK(saved->saved_revision == expected);
    CHECK_FALSE(saved->current_scene_clean);
    CHECK(status(owner).dirty);
    CHECK(take(owner.capture()).scene.entities.front().name.value == "Later edit");
    CHECK(take(files->read_scene(project(), id<SceneId>(3)))
              .source.document.entities.front()
              .name.value == "Captured");
}

TEST_CASE("project file methods reject foreign threads without touching files or scenes",
          "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    auto loaded = take(files->load_scene(project(), id<SceneId>(3)));
    const auto expected = status(*loaded.owner).revision;
    const auto old = read_bytes(fixture.scene_path());
    const auto project_path = path("project.sharkproject");
    const auto project_document = project();
    const auto scene_document = scene();
    const auto scene_id = id<SceneId>(3);
    std::array<bool, 9> results{};
    const auto script_path = path("scripts/main.lua");
    std::thread foreign{[&] {
        const auto check = [](const auto& result) {
            return !result && result.error().code == ErrorCode::conflict;
        };
        results[0] = check(files->read_project(project_path));
        results[1] = check(files->read_scene(project_document, scene_id));
        results[2] = check(files->write_project(project_path, project_document, std::nullopt));
        results[3] =
            check(files->write_scene(project_document, scene_document, loaded.file_revision));
        results[4] = check(files->load_scene(project_document, scene_id));
        results[5] = check(files->reload_scene(*loaded.owner, expected));
        results[6] = check(files->save_scene(*loaded.owner, expected, loaded.file_revision));
        results[7] = check(files->read_text(script_path));
        results[8] = check(files->write_text(script_path, "return 1\n", std::nullopt));
    }};
    foreign.join();
    for (const auto result : results) {
        CHECK(result);
    }
    CHECK(read_bytes(fixture.scene_path()) == old);
    CHECK(status(*loaded.owner).revision == expected);
}

TEST_CASE("script text preserves UTF-8 bytes and rejects conflicting content revisions",
          "[project-files][project-text]") {
    Fixture fixture;
    REQUIRE(fs::create_directory(fixture.root / "scripts"));
    auto files = fixture.open();
    const auto script = path("scripts/main.lua");
    const auto second = path("scripts/other.lua");
    const std::string original = "-- \xe9\x9b\xaa\r\nreturn 1\n";
    const auto first = take(files->write_text(script, original, std::nullopt));
    const auto read = take(files->read_text(script));
    CHECK(read.text == original);
    CHECK(read.revision == first.revision);
    CHECK(read.revision.byte_count() == original.size());
    rejected(files->write_text(script, "return 2\n", std::nullopt), ErrorCode::conflict);
    rejected(files->write_text(second, "return 2\n", read.revision), ErrorCode::conflict);
    CHECK_FALSE(fs::exists(fixture.root / "scripts/other.lua"));

    const auto timestamp = fs::last_write_time(fixture.root / "scripts/main.lua");
    auto external = original;
    external[external.size() - 2] = '3';
    write_bytes(fixture.root / "scripts/main.lua", external);
    fs::last_write_time(fixture.root / "scripts/main.lua", timestamp);
    rejected(files->write_text(script, "return 2\n", read.revision), ErrorCode::conflict);
    CHECK(read_bytes(fixture.root / "scripts/main.lua") == external);
    CHECK(read.text == original);

    write_bytes(fixture.root / "scripts/main.lua", original);
    const auto changed = take(files->write_text(script, "return 2\n", read.revision));
    CHECK(take(files->read_text(script)).revision == changed.revision);
    files.reset();
    files = fixture.open();
    const auto empty = take(files->write_text(script, "", changed.revision));
    CHECK(empty.revision.byte_count() == 0);
    CHECK(take(files->read_text(script)).text.empty());
}

TEST_CASE("script text enforces bounded UTF-8 and a distinct path domain",
          "[project-files][project-text]") {
    Fixture fixture;
    REQUIRE(fs::create_directory(fixture.root / "scripts"));
    auto files = fixture.open();
    const auto script = path("scripts/main.lua");
    for (const auto rejected_path :
         {"main.lua", "Scripts/main.lua", "scripts/main.LUA", "scripts/main.sharkscene",
          "scripts/.shark-stage-main.lua", "scripts/.shark-write.lock/main.lua"}) {
        INFO(rejected_path);
        rejected(files->write_text(path(rejected_path), "return 1\n", std::nullopt),
                 ErrorCode::invalid_path);
        rejected(files->read_text(path(rejected_path)), ErrorCode::invalid_path);
    }
    for (const auto& invalid : {std::string{"a\0b", 3}, std::string{"\xc0\xaf", 2},
                                std::string(project_text_maximum_bytes + 1, 'x')}) {
        rejected(files->write_text(script, invalid, std::nullopt), ErrorCode::invalid_argument);
        CHECK_FALSE(fs::exists(fixture.root / "scripts/main.lua"));
    }
    const std::string maximum(project_text_maximum_bytes, 'x');
    const auto saved = take(files->write_text(script, maximum, std::nullopt));
    CHECK(take(files->read_text(script)).text == maximum);
    write_bytes(fixture.root / "scripts/main.lua", std::string{"a\0b", 3});
    rejected(files->read_text(script), ErrorCode::invalid_argument);
    write_bytes(fixture.root / "scripts/main.lua", std::string{"\xc0\xaf", 2});
    rejected(files->read_text(script), ErrorCode::invalid_argument);
    write_bytes(fixture.root / "scripts/main.lua",
                std::string(project_text_maximum_bytes + 1, 'x'));
    REQUIRE_FALSE(files->read_text(script));
    REQUIRE_FALSE(files->write_text(path("scripts/missing/main.lua"), "", std::nullopt));
    CHECK_FALSE(fs::exists(fixture.root / "scripts/missing"));
    CHECK(saved.revision.byte_count() == project_text_maximum_bytes);
}

TEST_CASE("script text reports prepublication rollback and committed durability limits",
          "[project-files][project-text]") {
    Fixture fixture;
    REQUIRE(fs::create_directory(fixture.root / "scripts"));
    auto files = fixture.open();
    const auto script = path("scripts/main.lua");
    const auto original = take(files->write_text(script, "return 1\n", std::nullopt));
    {
        Control control{{detail::FileFault::publish, nullptr, nullptr}};
        rejected(files->write_text(script, "return 2\n", original.revision), ErrorCode::io_failure);
    }
    CHECK(read_bytes(fixture.root / "scripts/main.lua") == "return 1\n");
    CHECK(take(files->read_text(script)).revision == original.revision);
    CHECK(regular_files(fixture.root / "scripts") == 1);
    std::optional<FileCommit> committed;
    {
        Control control{{detail::FileFault::directory_flush, nullptr, nullptr}};
        committed = take(files->write_text(script, "return 2\n", original.revision));
    }
    CHECK(committed->durability == FileDurability::directory_flush_failed);
    CHECK(take(files->read_text(script)).text == "return 2\n");
    CHECK(take(files->read_text(script)).revision == committed->revision);
    rejected(files->write_text(script, "return 3\n", original.revision), ErrorCode::conflict);
}

TEST_CASE("capture and save during a read visit are permitted while reload cannot replace it",
          "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    auto loaded = take(files->load_scene(project(), id<SceneId>(3)));
    auto& owner = *loaded.owner;
    REQUIRE(execute(owner, SetNameCommand{id<AuthoredEntityId>(10), {"Visited"}}));
    const auto expected = status(owner).revision;
    bool saved = false;
    REQUIRE(fixture_scene(owner).visit_entities([&](const EntityReference&) -> Result<void> {
        auto result = files->save_scene(owner, expected, loaded.file_revision);
        if (!result) {
            return result.error();
        }
        saved = result.value().current_scene_clean;
        const auto reload = files->reload_scene(owner, expected);
        if (reload || reload.error().code != ErrorCode::conflict) {
            return Error{ErrorCode::execution_failed, "reload unexpectedly replaced visited owner"};
        }
        return {};
    }));
    CHECK(saved);
    CHECK_FALSE(status(owner).dirty);
    CHECK(status(owner).revision == expected);
}

TEST_CASE("postcapture pending mutation preserves committed outcome without clean acknowledgement",
          "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    auto loaded = take(files->load_scene(project(), id<SceneId>(3)));
    auto& owner = *loaded.owner;
    REQUIRE(execute(owner, SetNameCommand{id<AuthoredEntityId>(10), {"Captured"}}));
    const auto expected = status(owner).revision;
    struct Pending {
        Scene* owner;
        EntityHandle entity;
        bool queued{};
    };
    Pending pending{&fixture_scene(owner), first(owner)};
    const auto hook = [](detail::FilePhase reached, void* context) noexcept {
        if (reached != detail::FilePhase::published) {
            return;
        }
        auto& value = *static_cast<Pending*>(context);
        try {
            value.queued = static_cast<bool>(value.owner->defer_set_name(value.entity, {"Queued"}));
        } catch (...) {
            value.queued = false;
        }
    };
    std::optional<SavedAuthoredScene> saved;
    {
        Control control{{detail::FileFault::none, hook, &pending}};
        saved = take(files->save_scene(owner, expected, loaded.file_revision));
    }
    REQUIRE(pending.queued);
    CHECK_FALSE(saved->current_scene_clean);
    CHECK(status(owner).pending_mutations == 1);
    CHECK(status(owner).dirty);
    CHECK(take(files->read_scene(project(), id<SceneId>(3)))
              .source.document.entities.front()
              .name.value == "Captured");
    REQUIRE(fixture_scene(owner).apply_deferred());
    CHECK(take(owner.capture()).scene.entities.front().name.value == "Queued");
}

TEST_CASE("closed authored owner cannot reload or save its former file", "[project-files]") {
    Fixture fixture;
    fixture.seed();
    auto files = fixture.open();
    auto loaded = take(files->load_scene(project(), id<SceneId>(3)));
    const auto expected = status(*loaded.owner).revision;
    const auto old = read_bytes(fixture.scene_path());
    REQUIRE(fixture_scene(*loaded.owner).close());
    rejected(files->save_scene(*loaded.owner, expected, loaded.file_revision), ErrorCode::conflict);
    rejected(files->reload_scene(*loaded.owner, expected), ErrorCode::conflict);
    CHECK(read_bytes(fixture.scene_path()) == old);
}
