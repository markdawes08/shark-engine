#include <shark/tools/authored_scene.hpp>
#include <shark/tools/project_files.hpp>

#include "authored_scene_test_access.hpp"
#include "authoring_allocation_fault.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
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

template <typename Id> Id id(std::uint32_t value) {
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 0xfU];
        value >>= 4U;
    }
    return Id{take(StableId::parse(text))};
}

ProjectDocument project() {
    return {id<ProjectId>(1),
            id<WorldId>(2),
            {"Shared command project"},
            id<SceneId>(3),
            {{id<SceneId>(3), take(ProjectPath::parse("scenes/start.sharkscene"))}},
            R"({"project-note":"keep"})"};
}

SourceEntity entity(std::uint32_t value, std::optional<std::uint32_t> parent = std::nullopt) {
    return {id<AuthoredEntityId>(value),
            parent ? std::optional{id<AuthoredEntityId>(*parent)} : std::nullopt,
            {"Entity " + std::to_string(value)},
            {},
            "{}"};
}

SceneDocument source() {
    auto child = entity(30, 10);
    child.transform.translation = {1.0, 2.0, 3.0};
    child.extras_json = R"({"source-note":"child"})";
    auto parent = entity(10);
    parent.transform.translation = {10.0, 20.0, 30.0};
    parent.transform.scale = {2.0, 2.0, 2.0};
    parent.extras_json = R"({"source-note":"parent"})";
    // Source order is intentionally unrelated to stable ID and hierarchy order.
    return {id<ProjectId>(1),
            id<SceneId>(3),
            WorldPosition{id<WorldId>(2), {-19, 200, 3000}, {0.25, 512.0, 1023.5}},
            {child, entity(20, 30), parent, entity(40)},
            R"({"scene-note":[7,true]})"};
}

std::unique_ptr<AuthoredScene> owner(const SceneDocument& document = source(),
                                     SceneLimits limits = {}, AuthoringHistoryLimits history = {}) {
    return take(AuthoredScene::create(project(), document, limits, history));
}

const Scene& live(const AuthoredScene& value) {
    return take(value.scene()).get();
}

Scene& fixture_scene(AuthoredScene& value) {
    return take(detail::AuthoredSceneTestAccess::scene(value)).get();
}

EntityHandle find(const AuthoredScene& value, std::uint32_t identity) {
    return take(live(value).find(id<AuthoredEntityId>(identity)));
}

SceneEntityState state(const AuthoredScene& value, std::uint32_t identity) {
    return take(live(value).entity_state(find(value, identity)));
}

AuthoredSceneStatus status(const AuthoredScene& value) {
    return take(value.status());
}

CapturedScene capture(const AuthoredScene& value) {
    return take(value.capture());
}

template <typename T> void rejected(const Result<T>& result, ErrorCode code) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().message.empty());
    CHECK_FALSE(result.error().context.operation.empty());
}

void same_capture(const CapturedScene& actual, const CapturedScene& expected) {
    CHECK(actual.revision == expected.revision);
    CHECK(actual.project == expected.project);
    CHECK(actual.scene == expected.scene);
    CHECK(actual.dirty == expected.dirty);
}

AuthoringCommandReply changed(AuthoredScene& value, const AuthoringCommand& command,
                              std::vector<AuthoredEntityId> affected) {
    const auto before = status(value).revision;
    auto reply = take(value.execute(command, before));
    REQUIRE(reply.outcome);
    CHECK(reply.outcome.value().changed);
    CHECK(reply.outcome.value().affected_entities == affected);
    CHECK(reply.observed_revision.scene_instance == before.scene_instance);
    CHECK(reply.observed_revision.revision == before.revision + 1);
    CHECK(reply.observed_revision == status(value).revision);
    CHECK(status(value).dirty);
    return reply;
}

void command_rejected(AuthoredScene& value, const AuthoringCommand& command, SceneRevision expected,
                      ErrorCode code) {
    const auto before = capture(value);
    const auto reply = take(value.execute(command, expected));
    rejected(reply.outcome, code);
    CHECK(reply.outcome.error().context.operation == "authoring.execute");
    CHECK(reply.observed_revision == before.revision);
    same_capture(capture(value), before);
}

void command_rejected(AuthoredScene& value, const AuthoringCommand& command, ErrorCode code) {
    command_rejected(value, command, status(value).revision, code);
}

const SourceEntity& captured_entity(const CapturedScene& value, std::uint32_t identity) {
    const auto wanted = id<AuthoredEntityId>(identity);
    for (const auto& current : value.scene.entities) {
        if (current.id == wanted) {
            return current;
        }
    }
    FAIL("expected authored entity is absent from capture");
}

class FileFixture final {
  public:
    FileFixture() {
        root = fs::current_path() /
               ("shark-authoring-commands-" + take(generate_stable_id()).to_string());
        REQUIRE(fs::create_directory(root));
        REQUIRE(fs::create_directory(root / "scenes"));
    }
    ~FileFixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }
    FileFixture(const FileFixture&) = delete;
    FileFixture& operator=(const FileFixture&) = delete;
    fs::path root;
};

static_assert(std::is_same_v<decltype(std::declval<AuthoredScene&>().scene()),
                             Result<std::reference_wrapper<const Scene>>>);
static_assert(std::is_same_v<decltype(std::declval<const AuthoredScene&>().scene()),
                             Result<std::reference_wrapper<const Scene>>>);
static_assert(std::variant_size_v<AuthoringCommand> == 5);

} // namespace

TEST_CASE("authoring five typed edits preserve the active instance and advance once",
          "[authoring-commands]") {
    auto value = owner();
    const auto original = find(*value, 30);
    const auto reference = take(live(*value).reference(original));
    changed(
        *value,
        CreateEntityCommand{id<AuthoredEntityId>(50), {"Created"}, {}, id<AuthoredEntityId>(40)},
        {id<AuthoredEntityId>(40), id<AuthoredEntityId>(50)});
    const auto created = find(*value, 50);
    CHECK(state(*value, 50).parent == find(*value, 40));
    changed(*value, SetNameCommand{id<AuthoredEntityId>(50), {"Renamed"}},
            {id<AuthoredEntityId>(50)});
    Transform moved;
    moved.translation = {4.0, 5.0, 6.0};
    changed(*value, SetTransformCommand{id<AuthoredEntityId>(50), moved},
            {id<AuthoredEntityId>(50)});
    changed(*value, ReparentEntityCommand{id<AuthoredEntityId>(50), id<AuthoredEntityId>(10)},
            {id<AuthoredEntityId>(10), id<AuthoredEntityId>(40), id<AuthoredEntityId>(50)});
    CHECK(state(*value, 50).local_transform == moved);
    CHECK(state(*value, 50).world_matrix[3][0] == 18.0);
    CHECK(find(*value, 50) == created);
    changed(*value, DeleteEntityCommand{id<AuthoredEntityId>(50)},
            {id<AuthoredEntityId>(10), id<AuthoredEntityId>(50)});
    rejected(live(*value).entity_state(created), ErrorCode::invalid_id);
    CHECK(find(*value, 30) == original);
    CHECK(take(live(*value).resolve(reference)) == original);
    CHECK(take(live(*value).stats()).live_entities == 4);
}

TEST_CASE("authoring canonical noops retain revision clean state and query cursors",
          "[authoring-commands]") {
    auto value = owner();
    const auto before = capture(*value);
    auto canonical = state(*value, 30).local_transform;
    canonical.rotation = {-8.0, -0.0, -0.0, -0.0};
    SceneQueryOptions options;
    options.page_size = 1;
    const auto page = take(value->query_entities(options));
    REQUIRE(page.next_cursor());
    const std::array<AuthoringCommand, 4> commands{
        SetNameCommand{id<AuthoredEntityId>(30), state(*value, 30).name},
        SetTransformCommand{id<AuthoredEntityId>(30), canonical},
        ReparentEntityCommand{id<AuthoredEntityId>(30), id<AuthoredEntityId>(10),
                              ReparentMode::keep_local},
        ReparentEntityCommand{id<AuthoredEntityId>(30), id<AuthoredEntityId>(10),
                              ReparentMode::keep_world}};
    for (const auto& command : commands) {
        const auto reply = take(value->execute(command, before.revision));
        REQUIRE(reply.outcome);
        CHECK_FALSE(reply.outcome.value().changed);
        CHECK(reply.outcome.value().affected_entities.empty());
        CHECK(reply.observed_revision == before.revision);
        same_capture(capture(*value), before);
        REQUIRE(value->continue_query(*page.next_cursor()));
    }
}

TEST_CASE("authoring stale foreign and future revisions reject every edit including noops",
          "[authoring-commands]") {
    auto value = owner();
    const auto stale = status(*value).revision;
    changed(*value, SetNameCommand{id<AuthoredEntityId>(40), {"New observation"}},
            {id<AuthoredEntityId>(40)});
    auto foreign = owner();
    const auto current = status(*value).revision;
    const std::array expected{SceneRevision{}, stale, status(*foreign).revision,
                              SceneRevision{current.scene_instance, current.revision + 1}};
    const std::array<AuthoringCommand, 5> commands{
        CreateEntityCommand{id<AuthoredEntityId>(50)},
        DeleteEntityCommand{id<AuthoredEntityId>(40)},
        SetNameCommand{id<AuthoredEntityId>(40), {"New observation"}},
        SetTransformCommand{id<AuthoredEntityId>(30), state(*value, 30).local_transform},
        ReparentEntityCommand{id<AuthoredEntityId>(30), id<AuthoredEntityId>(10)}};
    for (const auto revision : expected) {
        for (const auto& command : commands) {
            command_rejected(*value, command, revision, ErrorCode::conflict);
        }
    }
}

TEST_CASE("authoring missing identities and parents fail without creating partial entities",
          "[authoring-commands]") {
    auto value = owner();
    const auto missing = id<AuthoredEntityId>(999);
    const std::array<AuthoringCommand, 6> commands{
        DeleteEntityCommand{missing},
        SetNameCommand{missing, {"Unknown"}},
        SetTransformCommand{missing, {}},
        ReparentEntityCommand{missing, id<AuthoredEntityId>(10)},
        ReparentEntityCommand{id<AuthoredEntityId>(30), missing},
        CreateEntityCommand{id<AuthoredEntityId>(50), {}, {}, missing}};
    for (const auto& command : commands) {
        command_rejected(*value, command, ErrorCode::invalid_id);
    }
    rejected(live(*value).find(id<AuthoredEntityId>(50)), ErrorCode::invalid_id);
    rejected(value->inspect_entity(missing), ErrorCode::invalid_id);
}

TEST_CASE("authoring duplicate creation preserves the original incarnation and metadata",
          "[authoring-commands]") {
    auto value = owner();
    const auto original = find(*value, 30);
    command_rejected(*value,
                     CreateEntityCommand{
                         id<AuthoredEntityId>(30), {"Replacement"}, {}, id<AuthoredEntityId>(40)},
                     ErrorCode::conflict);
    CHECK(find(*value, 30) == original);
    CHECK(captured_entity(capture(*value), 30).extras_json == R"({"source-note":"child"})");
}

TEST_CASE("authoring components use the shared validators before publishing mutations",
          "[authoring-commands]") {
    auto value = owner();
    for (const auto& text : std::array<std::string, 5>{"", std::string(257, 'x'), "line\nfeed",
                                                       std::string{"\xc0\xaf"}, "delete\x7f"}) {
        command_rejected(*value, SetNameCommand{id<AuthoredEntityId>(30), {text}},
                         ErrorCode::invalid_argument);
        command_rejected(*value, CreateEntityCommand{id<AuthoredEntityId>(50), {text}},
                         ErrorCode::invalid_argument);
    }
    for (int scenario = 0; scenario != 4; ++scenario) {
        CAPTURE(scenario);
        Transform invalid;
        if (scenario == 0) {
            invalid.scale.x = 0.0;
        } else if (scenario == 1) {
            invalid.translation.y = std::numeric_limits<double>::infinity();
        } else if (scenario == 2) {
            invalid.rotation = {0.0, 0.0, 0.0, 0.0};
        } else {
            invalid.scale.z = std::numeric_limits<double>::quiet_NaN();
        }
        command_rejected(*value, SetTransformCommand{id<AuthoredEntityId>(30), invalid},
                         ErrorCode::invalid_argument);
        command_rejected(*value, CreateEntityCommand{id<AuthoredEntityId>(50), {}, invalid},
                         ErrorCode::invalid_argument);
    }
}

TEST_CASE("authoring composed transform overflow rolls back create set and reparent",
          "[authoring-commands]") {
    auto document = source();
    document.entities = {entity(10), entity(20, 10), entity(30)};
    document.entities[0].transform.scale = {1e200, 1e200, 1e200};
    document.entities[2].transform.scale = {1e200, 1e200, 1e200};
    auto value = owner(document);
    Transform oversized;
    oversized.scale = {1e200, 1e200, 1e200};
    command_rejected(
        *value,
        CreateEntityCommand{id<AuthoredEntityId>(40), {}, oversized, id<AuthoredEntityId>(10)},
        ErrorCode::invalid_argument);
    command_rejected(*value, SetTransformCommand{id<AuthoredEntityId>(20), oversized},
                     ErrorCode::invalid_argument);
    command_rejected(*value,
                     ReparentEntityCommand{id<AuthoredEntityId>(30), id<AuthoredEntityId>(10)},
                     ErrorCode::invalid_argument);
    CHECK(state(*value, 20).local_transform == Transform{});
    CHECK_FALSE(state(*value, 30).parent);
    CHECK(state(*value, 10).children == std::vector{find(*value, 20)});
}

TEST_CASE("authoring keep-world reparent preserves subtree poses and checked handles",
          "[authoring-commands]") {
    auto value = owner();
    const auto child = state(*value, 30);
    const auto descendant = state(*value, 20);
    changed(*value,
            ReparentEntityCommand{id<AuthoredEntityId>(30), std::nullopt, ReparentMode::keep_world},
            {id<AuthoredEntityId>(10), id<AuthoredEntityId>(20), id<AuthoredEntityId>(30)});
    CHECK(state(*value, 30).reference.handle == child.reference.handle);
    CHECK(state(*value, 20).reference.handle == descendant.reference.handle);
    CHECK(state(*value, 30).world_matrix == child.world_matrix);
    CHECK(state(*value, 20).world_matrix == descendant.world_matrix);
    CHECK(state(*value, 30).local_transform.translation == math::Vec3(12.0, 24.0, 36.0));
    CHECK(state(*value, 30).local_transform.scale == math::Vec3(2.0, 2.0, 2.0));
    CHECK(state(*value, 20).depth == 2);
}

TEST_CASE("authoring nonrepresentable keep-world reparent retains the whole hierarchy",
          "[authoring-commands]") {
    auto document = source();
    document.entities[2].transform.scale = {2.0, 3.0, 4.0};
    constexpr double half_angle = 0.39269908169872415481;
    document.entities[0].transform.rotation = {std::cos(half_angle), 0.0, 0.0,
                                               std::sin(half_angle)};
    auto value = owner(document);
    const auto before_child = state(*value, 30);
    const auto before_descendant = state(*value, 20);
    command_rejected(
        *value,
        ReparentEntityCommand{id<AuthoredEntityId>(30), std::nullopt, ReparentMode::keep_world},
        ErrorCode::invalid_argument);
    CHECK(state(*value, 30).world_matrix == before_child.world_matrix);
    CHECK(state(*value, 20).world_matrix == before_descendant.world_matrix);
    CHECK(state(*value, 30).parent == before_child.parent);
}

TEST_CASE("authoring cycles invalid modes and lower hierarchy limits reject unchanged",
          "[authoring-commands]") {
    auto value = owner(source(), SceneLimits{5, 4, 3});
    command_rejected(*value,
                     ReparentEntityCommand{id<AuthoredEntityId>(10), id<AuthoredEntityId>(10)},
                     ErrorCode::conflict);
    command_rejected(*value,
                     ReparentEntityCommand{id<AuthoredEntityId>(10), id<AuthoredEntityId>(20)},
                     ErrorCode::conflict);
    command_rejected(*value,
                     ReparentEntityCommand{id<AuthoredEntityId>(40), id<AuthoredEntityId>(20)},
                     ErrorCode::conflict);
    command_rejected(
        *value, CreateEntityCommand{id<AuthoredEntityId>(50), {}, {}, id<AuthoredEntityId>(20)},
        ErrorCode::conflict);
    command_rejected(*value,
                     ReparentEntityCommand{id<AuthoredEntityId>(30), id<AuthoredEntityId>(40),
                                           static_cast<ReparentMode>(99)},
                     ErrorCode::invalid_argument);
    changed(*value, CreateEntityCommand{id<AuthoredEntityId>(50)}, {id<AuthoredEntityId>(50)});
    command_rejected(*value, CreateEntityCommand{id<AuthoredEntityId>(60)}, ErrorCode::conflict);
}

TEST_CASE("authoring refresh identities include descendants and changed links in stable order",
          "[authoring-commands]") {
    auto value = owner();
    changed(*value, SetNameCommand{id<AuthoredEntityId>(10), {"Only the named entity"}},
            {id<AuthoredEntityId>(10)});
    auto transform = state(*value, 10).local_transform;
    transform.translation.x = 99.0;
    changed(*value, SetTransformCommand{id<AuthoredEntityId>(10), transform},
            {id<AuthoredEntityId>(10), id<AuthoredEntityId>(20), id<AuthoredEntityId>(30)});
    changed(*value, ReparentEntityCommand{id<AuthoredEntityId>(30), id<AuthoredEntityId>(40)},
            {id<AuthoredEntityId>(10), id<AuthoredEntityId>(20), id<AuthoredEntityId>(30),
             id<AuthoredEntityId>(40)});
    CHECK(state(*value, 10).children.empty());
    CHECK(state(*value, 40).children == std::vector{find(*value, 30)});
}

TEST_CASE("authoring maximum subtree refresh is complete bounded and sorted",
          "[authoring-commands]") {
    auto document = source();
    document.entities.clear();
    document.entities.reserve(source_maximum_entities);
    std::vector<AuthoredEntityId> expected;
    expected.reserve(source_maximum_entities);
    // Descending source order and one broad parent expose truncation, omission,
    // duplicate-parent IDs and accidental source/slot ordering in the receipt.
    for (std::size_t index = 0; index < source_maximum_entities; ++index) {
        const auto value = static_cast<std::uint32_t>(source_maximum_entities - index);
        document.entities.push_back(entity(value, value == 1 ? std::nullopt : std::optional{1U}));
        expected.push_back(id<AuthoredEntityId>(static_cast<std::uint32_t>(index + 1)));
    }
    auto value = owner(document);
    Transform moved;
    moved.translation = {4.0, 5.0, 6.0};
    const auto reply =
        changed(*value, SetTransformCommand{id<AuthoredEntityId>(1), moved}, expected);
    CHECK(reply.outcome.value().affected_entities.size() == source_maximum_entities);
    CHECK(state(*value, 4096).world_matrix[3][0] == 4.0);
    CHECK(state(*value, 4096).world_matrix[3][1] == 5.0);
    CHECK(state(*value, 4096).world_matrix[3][2] == 6.0);
    command_rejected(
        *value, CreateEntityCommand{id<AuthoredEntityId>(4097), {}, {}, id<AuthoredEntityId>(1)},
        ErrorCode::conflict);
    CHECK(take(live(*value).stats()).live_entities == source_maximum_entities);
}

TEST_CASE("authoring subtree delete reports removed IDs plus its surviving parent",
          "[authoring-commands]") {
    auto value = owner();
    const auto old_child = find(*value, 30);
    const auto old_descendant = find(*value, 20);
    const auto root = find(*value, 10);
    changed(*value, DeleteEntityCommand{id<AuthoredEntityId>(30)},
            {id<AuthoredEntityId>(10), id<AuthoredEntityId>(20), id<AuthoredEntityId>(30)});
    rejected(live(*value).entity_state(old_child), ErrorCode::invalid_id);
    rejected(live(*value).entity_state(old_descendant), ErrorCode::invalid_id);
    CHECK(find(*value, 10) == root);
    CHECK(state(*value, 10).children.empty());
    const auto saved = capture(*value);
    REQUIRE(saved.scene.entities.size() == 2);
    CHECK(saved.scene.entities[0].id == id<AuthoredEntityId>(10));
    CHECK(saved.scene.entities[1].id == id<AuthoredEntityId>(40));
    changed(*value, DeleteEntityCommand{id<AuthoredEntityId>(10)}, {id<AuthoredEntityId>(10)});
    changed(*value, DeleteEntityCommand{id<AuthoredEntityId>(40)}, {id<AuthoredEntityId>(40)});
    CHECK(capture(*value).scene.entities.empty());
}

TEST_CASE("authoring recreate never inherits deleted incarnation metadata or references",
          "[authoring-commands]") {
    auto value = owner();
    const auto old = find(*value, 30);
    const auto reference = take(live(*value).reference(old));
    changed(*value, DeleteEntityCommand{id<AuthoredEntityId>(30)},
            {id<AuthoredEntityId>(10), id<AuthoredEntityId>(20), id<AuthoredEntityId>(30)});
    changed(*value, CreateEntityCommand{id<AuthoredEntityId>(30), {"Recreated"}},
            {id<AuthoredEntityId>(30)});
    CHECK(find(*value, 30) != old);
    rejected(live(*value).resolve(reference), ErrorCode::invalid_id);
    const auto saved = capture(*value);
    CHECK(captured_entity(saved, 30).extras_json == "{}");
    CHECK(captured_entity(saved, 10).extras_json == R"({"source-note":"parent"})");
    CHECK(saved.project == project());
    CHECK(saved.scene.extras_json == source().extras_json);
    CHECK(saved.scene.world_origin == source().world_origin);
}

TEST_CASE("authoring edits retain independent command payload and reply values",
          "[authoring-commands]") {
    auto value = owner();
    SetNameCommand command{id<AuthoredEntityId>(30), {"Owned result"}};
    auto reply = changed(*value, command, {id<AuthoredEntityId>(30)});
    command.name.value = "Caller edited input";
    reply.outcome.value().affected_entities.clear();
    CHECK(state(*value, 30).name.value == "Owned result");
    CHECK(captured_entity(capture(*value), 30).extras_json == R"({"source-note":"child"})");
    changed(*value, SetNameCommand{id<AuthoredEntityId>(30), {"Entity 30"}},
            {id<AuthoredEntityId>(30)});
    CHECK(capture(*value).scene == source());
    CHECK(status(*value).dirty);
}

TEST_CASE("authoring save acknowledges the command revision without replacing the scene",
          "[authoring-commands]") {
    FileFixture fixture;
    auto files = take(ProjectFiles::open(fixture.root));
    const auto initial_file = take(files->write_scene(project(), source(), std::nullopt));
    auto loaded = take(files->load_scene(project(), id<SceneId>(3)));
    auto& value = *loaded.owner;
    const auto handle = find(value, 30);
    changed(value, SetNameCommand{id<AuthoredEntityId>(30), {"Saved command"}},
            {id<AuthoredEntityId>(30)});
    const auto revision = status(value).revision;
    const auto saved = take(files->save_scene(value, revision, initial_file.revision));
    CHECK(saved.current_scene_clean);
    CHECK(saved.saved_revision == revision);
    CHECK(status(value).revision == revision);
    CHECK_FALSE(status(value).dirty);
    CHECK(find(value, 30) == handle);
    const auto persisted = take(files->read_scene(project(), id<SceneId>(3)));
    CHECK(persisted.source.document == capture(value).scene);
    const auto no_op =
        take(value.execute(SetNameCommand{id<AuthoredEntityId>(30), {"Saved command"}}, revision));
    REQUIRE(no_op.outcome);
    CHECK_FALSE(no_op.outcome.value().changed);
    CHECK_FALSE(status(value).dirty);
    changed(value, SetNameCommand{id<AuthoredEntityId>(30), {"Later unsaved command"}},
            {id<AuthoredEntityId>(30)});
    rejected(files->save_scene(value, revision, saved.file.revision), ErrorCode::conflict);
    CHECK(status(value).dirty);
    CHECK(take(files->read_scene(project(), id<SceneId>(3))).source.document ==
          persisted.source.document);
}

TEST_CASE("authoring bounded query continuation preserves filters and independent values",
          "[authoring-commands]") {
    auto value = owner();
    SceneQueryOptions options;
    options.name_prefix = "Entity 2";
    options.page_size = 1;
    options.scan_budget = 1;
    options.fields = {true, false, false};
    const auto before = capture(*value);
    auto page = take(value->query_entities(options));
    CHECK(page.entities().empty());
    CHECK(page.scanned_slots() == 1);
    REQUIRE(page.next_cursor());
    const auto cursor = *page.next_cursor();
    options.name_prefix = "Never use changed options";
    std::size_t pages = 1;
    while (page.next_cursor()) {
        page = take(value->continue_query(*page.next_cursor()));
        CHECK(page.revision() == before.revision);
        CHECK(page.scanned_slots() <= 1);
        CHECK(page.entities().size() <= 1);
        ++pages;
    }
    CHECK(pages == 4);
    REQUIRE(page.entities().size() == 1);
    CHECK(page.entities().front().reference.identity == EntityIdentity{id<AuthoredEntityId>(20)});
    REQUIRE(page.entities().front().name);
    CHECK(page.entities().front().name->value == "Entity 20");
    CHECK_FALSE(page.entities().front().transform);
    CHECK_FALSE(page.entities().front().hierarchy);
    const auto replay = take(value->continue_query(cursor));
    CHECK(replay.entities().empty());
    same_capture(capture(*value), before);
    changed(*value, SetNameCommand{id<AuthoredEntityId>(20), {"Later name"}},
            {id<AuthoredEntityId>(20)});
    CHECK(page.entities().front().name->value == "Entity 20");
    value.reset();
    CHECK(page.revision() == before.revision);
    CHECK(page.entities().front().name->value == "Entity 20");
}

TEST_CASE("authoring queries share revision fences after command mutation and activation",
          "[authoring-commands]") {
    auto value = owner();
    SceneQueryOptions options;
    options.page_size = 1;
    const auto page = take(value->query_entities(options));
    REQUIRE(page.next_cursor());
    const auto old = page.revision();
    auto other = owner();
    rejected(other->continue_query(*page.next_cursor()), ErrorCode::conflict);
    command_rejected(*value, DeleteEntityCommand{id<AuthoredEntityId>(999)}, ErrorCode::invalid_id);
    REQUIRE(value->continue_query(*page.next_cursor()));
    changed(*value, SetNameCommand{id<AuthoredEntityId>(30), {"Changed"}},
            {id<AuthoredEntityId>(30)});
    rejected(value->continue_query(*page.next_cursor()), ErrorCode::conflict);
    options.expected_revision = old;
    rejected(value->query_entities(options), ErrorCode::conflict);
    rejected(value->inspect_entity(id<AuthoredEntityId>(30), {}, old), ErrorCode::conflict);
    const auto fresh = take(value->query_entities());
    const auto before_reload = capture(*value);
    REQUIRE(
        value->activate(before_reload.scene, before_reload.revision, UnsavedScenePolicy::discard));
    CHECK(status(*value).revision.scene_instance != fresh.revision().scene_instance);
    command_rejected(*value, SetNameCommand{id<AuthoredEntityId>(30), {"Changed"}},
                     before_reload.revision, ErrorCode::conflict);
    CHECK_FALSE(status(*value).dirty);
}

TEST_CASE("authoring semantic queries select hierarchy and enforce existing field and work bounds",
          "[authoring-commands]") {
    auto value = owner();
    const auto before = capture(*value);
    SceneQueryOptions options;
    options.selection = SceneQuerySelection::children;
    options.parent = find(*value, 10);
    options.fields = {false, false, true};
    options.expected_revision = before.revision;
    const auto children = take(value->query_entities(options));
    REQUIRE(children.entities().size() == 1);
    CHECK(children.entities().front().reference.identity ==
          EntityIdentity{id<AuthoredEntityId>(30)});
    CHECK_FALSE(children.entities().front().name);
    CHECK_FALSE(children.entities().front().transform);
    REQUIRE(children.entities().front().hierarchy);
    CHECK(children.entities().front().hierarchy->child_count == 1);
    const auto inspected = take(
        value->inspect_entity(id<AuthoredEntityId>(30), {true, false, false}, before.revision));
    REQUIRE(inspected.entities().size() == 1);
    CHECK(inspected.entities().front().name->value == "Entity 30");
    CHECK_FALSE(inspected.entities().front().hierarchy);
    CHECK_FALSE(inspected.next_cursor());
    for (int scenario = 0; scenario != 6; ++scenario) {
        CAPTURE(scenario);
        SceneQueryOptions invalid;
        if (scenario == 0) {
            invalid.page_size = 0;
        } else if (scenario == 1) {
            invalid.page_size = scene_query_maximum_page_size + 1;
        } else if (scenario == 2) {
            invalid.scan_budget = 0;
        } else if (scenario == 3) {
            invalid.scan_budget = scene_query_maximum_scan_budget + 1;
        } else if (scenario == 4) {
            invalid.selection = SceneQuerySelection::children;
        } else {
            invalid.name_prefix = "bad\nfilter";
        }
        rejected(value->query_entities(invalid), ErrorCode::invalid_argument);
    }
    same_capture(capture(*value), before);
}

TEST_CASE("authoring human and agent equivalent requests share one unsaved revision stream",
          "[authoring-commands]") {
    auto value = owner();
    const auto human_observation = take(value->inspect_entity(id<AuthoredEntityId>(30)));
    const auto agent_observation = take(value->inspect_entity(id<AuthoredEntityId>(30)));
    CHECK(human_observation.revision() == agent_observation.revision());
    const auto human = take(value->execute(SetNameCommand{id<AuthoredEntityId>(30), {"Human edit"}},
                                           human_observation.revision()));
    REQUIRE(human.outcome);
    command_rejected(*value, SetNameCommand{id<AuthoredEntityId>(30), {"Stale agent edit"}},
                     agent_observation.revision(), ErrorCode::conflict);
    const auto refreshed = take(value->inspect_entity(id<AuthoredEntityId>(30)));
    CHECK(refreshed.entities().front().name->value == "Human edit");
    const auto agent = take(value->execute(
        CreateEntityCommand{
            id<AuthoredEntityId>(50), {"Agent child"}, {}, id<AuthoredEntityId>(30)},
        refreshed.revision()));
    REQUIRE(agent.outcome);
    CHECK(agent.observed_revision.revision == human.observed_revision.revision + 1);
    CHECK(agent.observed_revision.scene_instance == human.observed_revision.scene_instance);
    command_rejected(*value, DeleteEntityCommand{id<AuthoredEntityId>(50)}, human.observed_revision,
                     ErrorCode::conflict);
    const auto authored = capture(*value);
    CHECK(authored.dirty);
    CHECK(captured_entity(authored, 30).name.value == "Human edit");
    CHECK(captured_entity(authored, 50).parent == id<AuthoredEntityId>(30));
    CHECK(captured_entity(authored, 30).extras_json == R"({"source-note":"child"})");
    const auto decoded = take(decode_scene_document(take(encode_scene_document(authored.scene))));
    const auto reopened = owner(decoded.document);
    CHECK(capture(*reopened).scene == authored.scene);
    CHECK_FALSE(status(*reopened).dirty);
}

TEST_CASE("authoring wrong thread rejects before exposing a revision or accessing live state",
          "[authoring-commands]") {
    auto value = owner();
    const auto before = capture(*value);
    SceneQueryOptions options;
    options.page_size = 1;
    const auto page = take(value->query_entities(options));
    REQUIRE(page.next_cursor());
    const auto cursor = *page.next_cursor();
    const auto target = id<AuthoredEntityId>(30);
    std::array<bool, 5> denied{};
    std::thread worker([&] {
        const auto edit = value->execute(SetNameCommand{target, {"Wrong thread"}}, before.revision);
        denied[0] = !edit && edit.error().code == ErrorCode::conflict;
        const auto query = value->query_entities();
        denied[1] = !query && query.error().code == ErrorCode::conflict;
        const auto continuation = value->continue_query(cursor);
        denied[2] = !continuation && continuation.error().code == ErrorCode::conflict;
        const auto inspected = value->inspect_entity(target);
        denied[3] = !inspected && inspected.error().code == ErrorCode::conflict;
        const auto accessed = value->scene();
        denied[4] = !accessed && accessed.error().code == ErrorCode::conflict;
    });
    worker.join();
    for (const bool result : denied) {
        CHECK(result);
    }
    same_capture(capture(*value), before);
}

TEST_CASE("authoring pending writes reject decisions without applying the queue",
          "[authoring-commands]") {
    auto value = owner();
    const auto before = capture(*value);
    const auto ticket = take(fixture_scene(*value).defer_set_name(find(*value, 30), {"Pending"}));
    const std::array<AuthoringCommand, 2> commands{
        SetNameCommand{id<AuthoredEntityId>(30), {"Immediate"}},
        SetNameCommand{id<AuthoredEntityId>(30), {"Entity 30"}}};
    for (const auto& command : commands) {
        const auto reply = take(value->execute(command, before.revision));
        rejected(reply.outcome, ErrorCode::conflict);
        CHECK(reply.observed_revision == before.revision);
        CHECK(status(*value).pending_mutations == 1);
        CHECK_FALSE(status(*value).dirty);
        CHECK(state(*value, 30).name.value == "Entity 30");
        const auto inspected = take(value->inspect_entity(id<AuthoredEntityId>(30)));
        CHECK(inspected.revision() == before.revision);
        CHECK(inspected.entities().front().name->value == "Entity 30");
    }
    const auto applied = take(fixture_scene(*value).apply_deferred());
    REQUIRE(applied.outcomes.size() == 1);
    CHECK(applied.outcomes.front().ticket == ticket);
    REQUIRE(applied.outcomes.front().result);
    CHECK(state(*value, 30).name.value == "Pending");
    command_rejected(*value, SetNameCommand{id<AuthoredEntityId>(30), {"Immediate"}},
                     before.revision, ErrorCode::conflict);
}

TEST_CASE("authoring active visits permit inspection and reject even noop edits",
          "[authoring-commands]") {
    auto value = owner();
    const auto before = capture(*value);
    std::size_t visited = 0;
    REQUIRE(fixture_scene(*value).visit_entities([&](const EntityReference&) -> Result<void> {
        ++visited;
        const auto reply = take(value->execute(
            SetNameCommand{id<AuthoredEntityId>(30), {"Entity 30"}}, before.revision));
        rejected(reply.outcome, ErrorCode::conflict);
        CHECK(reply.observed_revision == before.revision);
        const auto page = take(value->query_entities());
        CHECK(page.revision() == before.revision);
        CHECK(page.entities().size() == 4);
        same_capture(capture(*value), before);
        return {};
    }));
    CHECK(visited == 4);
    same_capture(capture(*value), before);
}

TEST_CASE("authoring closed owners reject commands and queries while owned pages survive",
          "[authoring-commands]") {
    auto value = owner();
    SceneQueryOptions options;
    options.page_size = 1;
    const auto page = take(value->query_entities(options));
    REQUIRE(page.next_cursor());
    const auto before = page.revision();
    REQUIRE(fixture_scene(*value).close());
    rejected(value->execute(DeleteEntityCommand{id<AuthoredEntityId>(30)}, before),
             ErrorCode::conflict);
    rejected(value->query_entities(), ErrorCode::conflict);
    rejected(value->continue_query(*page.next_cursor()), ErrorCode::conflict);
    rejected(value->inspect_entity(id<AuthoredEntityId>(30)), ErrorCode::conflict);
    CHECK(page.entities().front().name->value == "Entity 10");
    CHECK(page.revision() == before);
}

namespace {
AuthoringBatch batch(std::initializer_list<AuthoringCommand> commands) {
    return {id<AuthoringBatchId>(900), commands};
}
void batch_rejected(AuthoredScene& value, const AuthoringBatch& commands, SceneRevision expected,
                    ErrorCode code) {
    const auto before = capture(value);
    for (bool preview : {true, false}) {
        auto reply =
            take(preview ? value.preview(commands, expected) : value.execute(commands, expected));
        CHECK(reply.id == commands.id);
        CHECK(reply.observed_revision == before.revision);
        rejected(reply.outcome, code);
        CHECK(reply.outcome.error().context.request_id == commands.id.value().to_string());
        same_capture(capture(value), before);
    }
}
} // namespace

TEST_CASE("batch preview and commit share sequential validation with one publication",
          "[authoring-batches]") {
    auto value = owner();
    const auto before = capture(*value);
    const Scene* borrowed = &live(*value);
    const auto survivor = find(*value, 30);
    const auto commands =
        batch({CreateEntityCommand{id<AuthoredEntityId>(50), {"new"}},
               SetNameCommand{id<AuthoredEntityId>(50), {"renamed"}},
               ReparentEntityCommand{id<AuthoredEntityId>(50), id<AuthoredEntityId>(40)},
               DeleteEntityCommand{id<AuthoredEntityId>(20)}});
    auto preview = take(value->preview(commands, before.revision));
    REQUIRE(preview.outcome);
    CHECK(preview.outcome.value().changed);
    CHECK(preview.observed_revision == before.revision);
    same_capture(capture(*value), before);
    auto committed = take(value->execute(commands, before.revision));
    REQUIRE(committed.outcome);
    CHECK(committed.outcome.value().affected_entities == preview.outcome.value().affected_entities);
    CHECK(committed.outcome.value().affected_entities ==
          std::vector<AuthoredEntityId>{id<AuthoredEntityId>(20), id<AuthoredEntityId>(30),
                                        id<AuthoredEntityId>(40), id<AuthoredEntityId>(50)});
    CHECK(committed.observed_revision ==
          SceneRevision{before.revision.scene_instance, before.revision.revision + 1});
    CHECK(&live(*value) == borrowed);
    CHECK(find(*value, 30) == survivor);
    CHECK(state(*value, 50).name.value == "renamed");
    CHECK(state(*value, 50).parent == find(*value, 40));
    CHECK(status(*value).dirty);
}

TEST_CASE("batch late invalid requests discard every earlier edit", "[authoring-batches]") {
    auto value = owner();
    const std::array<AuthoringCommand, 5> invalid{
        CreateEntityCommand{id<AuthoredEntityId>(10)},
        DeleteEntityCommand{id<AuthoredEntityId>(99)},
        SetNameCommand{id<AuthoredEntityId>(30), {std::string(1000, 'x')}},
        SetTransformCommand{id<AuthoredEntityId>(30), Transform{{}, {}, {0.0, 1.0, 1.0}}},
        ReparentEntityCommand{id<AuthoredEntityId>(10), id<AuthoredEntityId>(20)}};
    for (const auto& last : invalid) {
        auto commands = batch({SetNameCommand{id<AuthoredEntityId>(40), {"candidate only"}}, last});
        auto before = capture(*value);
        for (bool preview : {true, false}) {
            auto reply = take(preview ? value->preview(commands, before.revision)
                                      : value->execute(commands, before.revision));
            REQUIRE_FALSE(reply.outcome);
            CHECK(reply.outcome.error().context.path == "/commands/1");
            CHECK_FALSE(reply.outcome.error().context.entity_id.empty());
            same_capture(capture(*value), before);
        }
    }
}

TEST_CASE("batch commit revalidates after preview and rejects stale empty batches",
          "[authoring-batches]") {
    auto value = owner();
    const auto expected = status(*value).revision;
    auto commands = batch({SetNameCommand{id<AuthoredEntityId>(40), {"previewed"}}});
    REQUIRE(take(value->preview(commands, expected)).outcome);
    changed(*value, SetNameCommand{id<AuthoredEntityId>(30), {"intervening"}},
            {id<AuthoredEntityId>(30)});
    batch_rejected(*value, commands, expected, ErrorCode::conflict);
    batch_rejected(*value, batch({}), expected, ErrorCode::conflict);
    auto foreign = owner();
    batch_rejected(*value, commands, status(*foreign).revision, ErrorCode::conflict);
}

TEST_CASE("batch noops and empty batches preserve clean baseline and cursors",
          "[authoring-batches]") {
    auto value = owner();
    const auto before = capture(*value);
    SceneQueryOptions options;
    options.page_size = 1;
    const auto page = take(value->query_entities(options));
    REQUIRE(page.next_cursor());
    for (const auto& commands :
         {batch({}),
          batch({SetNameCommand{id<AuthoredEntityId>(40), state(*value, 40).name},
                 SetTransformCommand{id<AuthoredEntityId>(30), state(*value, 30).local_transform},
                 ReparentEntityCommand{id<AuthoredEntityId>(30), id<AuthoredEntityId>(10)}})}) {
        for (bool preview : {true, false}) {
            const auto reply = take(preview ? value->preview(commands, before.revision)
                                            : value->execute(commands, before.revision));
            REQUIRE(reply.outcome);
            CHECK_FALSE(reply.outcome.value().changed);
            CHECK(reply.outcome.value().affected_entities.empty());
            same_capture(capture(*value), before);
            REQUIRE(value->continue_query(*page.next_cursor()));
        }
    }
}

TEST_CASE("batch changing then restoring values is one real edit", "[authoring-batches]") {
    auto value = owner();
    const auto before = capture(*value);
    auto reply = take(
        value->execute(batch({SetNameCommand{id<AuthoredEntityId>(40), {"temporary"}},
                              SetNameCommand{id<AuthoredEntityId>(40), state(*value, 40).name}}),
                       before.revision));
    REQUIRE(reply.outcome);
    CHECK(reply.outcome.value().changed);
    CHECK(capture(*value).scene == before.scene);
    CHECK(status(*value).dirty);
    CHECK(reply.observed_revision.revision == before.revision.revision + 1);
}

TEST_CASE("batch delete recreate invalidates only removed incarnations and their extras",
          "[authoring-batches]") {
    auto value = owner();
    const auto before = capture(*value);
    const auto removed = find(*value, 30);
    const auto survivor = find(*value, 10);
    auto reply = take(value->execute(
        batch({DeleteEntityCommand{id<AuthoredEntityId>(30)},
               CreateEntityCommand{
                   id<AuthoredEntityId>(30), {"replacement"}, {}, id<AuthoredEntityId>(10)}}),
        before.revision));
    REQUIRE(reply.outcome);
    rejected(live(*value).reference(removed), ErrorCode::invalid_id);
    CHECK(find(*value, 30) != removed);
    CHECK(find(*value, 10) == survivor);
    const auto after = capture(*value);
    CHECK(captured_entity(after, 30).extras_json == "{}");
    CHECK(captured_entity(after, 10).extras_json == captured_entity(before, 10).extras_json);
    CHECK(after.scene.extras_json == before.scene.extras_json);
    CHECK(after.scene.world_origin == before.scene.world_origin);
}

TEST_CASE("batch failed recreate preserves handles generations and metadata",
          "[authoring-batches]") {
    auto value = owner();
    const auto original = find(*value, 30);
    auto commands =
        batch({DeleteEntityCommand{id<AuthoredEntityId>(30)},
               CreateEntityCommand{id<AuthoredEntityId>(30)},
               ReparentEntityCommand{id<AuthoredEntityId>(10), id<AuthoredEntityId>(99)}});
    batch_rejected(*value, commands, status(*value).revision, ErrorCode::invalid_id);
    CHECK(find(*value, 30) == original);
}

TEST_CASE("batch bounds admit 256 commands and reject 257 before evaluation",
          "[authoring-batches]") {
    auto value = owner();
    auto commands = batch({});
    for (std::size_t i = 0; i < authoring_maximum_batch_commands; ++i)
        commands.commands.emplace_back(SetNameCommand{id<AuthoredEntityId>(40), {"bounded"}});
    auto expected = status(*value).revision;
    REQUIRE(take(value->preview(commands, expected)).outcome);
    auto reply = take(value->execute(commands, expected));
    REQUIRE(reply.outcome);
    CHECK(reply.observed_revision.revision == expected.revision + 1);
    commands.commands.emplace_back(DeleteEntityCommand{id<AuthoredEntityId>(99)});
    batch_rejected(*value, commands, status(*value).revision, ErrorCode::invalid_argument);
}

TEST_CASE("batch intermediate entity capacity and hierarchy must be valid", "[authoring-batches]") {
    auto value = owner(source(), {4, 16, 3});
    batch_rejected(*value,
                   batch({CreateEntityCommand{id<AuthoredEntityId>(50)},
                          DeleteEntityCommand{id<AuthoredEntityId>(40)}}),
                   status(*value).revision, ErrorCode::conflict);
    auto accepted = batch({DeleteEntityCommand{id<AuthoredEntityId>(40)},
                           CreateEntityCommand{id<AuthoredEntityId>(50)}});
    REQUIRE(take(value->execute(accepted, status(*value).revision)).outcome);
    batch_rejected(
        *value,
        batch({CreateEntityCommand{id<AuthoredEntityId>(60), {}, {}, id<AuthoredEntityId>(20)}}),
        status(*value).revision, ErrorCode::conflict);
}

TEST_CASE("batch pending visits wrong thread and closed owner reject", "[authoring-batches]") {
    auto value = owner();
    const auto expected = status(*value).revision;
    const auto commands = batch({SetNameCommand{id<AuthoredEntityId>(40), {"blocked"}}});
    auto& raw = fixture_scene(*value);
    REQUIRE(raw.defer_set_name(find(*value, 40), {"queued"}));
    for (bool preview : {true, false}) {
        auto reply =
            take(preview ? value->preview(commands, expected) : value->execute(commands, expected));
        rejected(reply.outcome, ErrorCode::conflict);
    }
    REQUIRE(raw.apply_deferred());
    const auto current = status(*value).revision;
    REQUIRE(raw.visit_entities([&](const EntityReference&) -> Result<void> {
        rejected(take(value->preview(commands, current)).outcome, ErrorCode::conflict);
        rejected(take(value->execute(commands, current)).outcome, ErrorCode::conflict);
        return {};
    }));
    std::optional<Result<AuthoringBatchReply>> wrong;
    std::thread worker([&] { wrong = value->preview(commands, current); });
    worker.join();
    REQUIRE(wrong);
    rejected(*wrong, ErrorCode::conflict);
    REQUIRE(raw.close());
    rejected(value->preview(commands, current), ErrorCode::conflict);
    rejected(value->execute(commands, current), ErrorCode::conflict);
}

TEST_CASE("batch correlation identity does not retain outcomes or permit stale retries",
          "[authoring-batches]") {
    auto value = owner();
    auto commands = batch({SetNameCommand{id<AuthoredEntityId>(40), {"one"}}});
    const auto first = status(*value).revision;
    REQUIRE(take(value->execute(commands, first)).outcome);
    batch_rejected(*value, commands, first, ErrorCode::conflict);
    commands.commands = {SetNameCommand{id<AuthoredEntityId>(40), {"two"}}};
    auto reply = take(value->execute(commands, status(*value).revision));
    REQUIRE(reply.outcome);
    CHECK(reply.id == commands.id);
    CHECK(state(*value, 40).name.value == "two");
}

TEST_CASE("batch saved baseline preview and file conflicts preserve shared unsaved work",
          "[authoring-batches]") {
    FileFixture fixture;
    auto files = take(ProjectFiles::open(fixture.root));
    auto value = owner();
    auto saved = take(files->save_scene(*value, status(*value).revision, std::nullopt));
    CHECK(saved.current_scene_clean);
    const auto before = capture(*value);
    const auto commands = batch({SetNameCommand{id<AuthoredEntityId>(40), {"human"}},
                                 SetNameCommand{id<AuthoredEntityId>(30), {"agent"}}});
    REQUIRE(take(value->preview(commands, before.revision)).outcome);
    same_capture(capture(*value), before);
    REQUIRE(take(value->execute(commands, before.revision)).outcome);
    const auto dirty = capture(*value);
    REQUIRE(dirty.dirty);
    auto conflict = files->save_scene(*value, before.revision, saved.file.revision);
    REQUIRE_FALSE(conflict);
    same_capture(capture(*value), dirty);
    auto final = take(files->save_scene(*value, dirty.revision, saved.file.revision));
    CHECK(final.current_scene_clean);
    CHECK_FALSE(status(*value).dirty);
    CHECK(take(files->read_scene(project(), id<SceneId>(3))).source.document == dirty.scene);
}

TEST_CASE("batch preview preserves cursors and does not consume scene incarnations",
          "[authoring-batches]") {
    auto value = owner();
    const auto expected = status(*value).revision;
    SceneQueryOptions options;
    options.page_size = 1;
    auto page = take(value->query_entities(options));
    REQUIRE(page.next_cursor());
    const auto commands = batch({DeleteEntityCommand{id<AuthoredEntityId>(30)},
                                 CreateEntityCommand{id<AuthoredEntityId>(50)}});
    for (int attempt = 0; attempt < 3; ++attempt) {
        REQUIRE(take(value->preview(commands, expected)).outcome);
        REQUIRE(value->continue_query(*page.next_cursor()));
    }
    auto next = owner();
    CHECK(status(*next).revision.scene_instance == expected.scene_instance + 1);
    REQUIRE(take(value->execute(commands, expected)).outcome);
    rejected(value->continue_query(*page.next_cursor()), ErrorCode::conflict);
}

TEST_CASE("batch composed transforms and keep world use the staged hierarchy",
          "[authoring-batches]") {
    auto value = owner();
    const auto expected = status(*value).revision;
    auto transform = state(*value, 40).local_transform;
    transform.translation = {7.0, 8.0, 9.0};
    const auto world = state(*value, 30).world_matrix;
    const auto commands =
        batch({SetTransformCommand{id<AuthoredEntityId>(40), transform},
               ReparentEntityCommand{id<AuthoredEntityId>(30), id<AuthoredEntityId>(40),
                                     ReparentMode::keep_world}});
    auto result = take(value->execute(commands, expected));
    REQUIRE(result.outcome);
    CHECK(transform_matrices_equivalent(state(*value, 30).world_matrix, world));
    CHECK(state(*value, 30).parent == find(*value, 40));
    CHECK(result.outcome.value().affected_entities ==
          std::vector<AuthoredEntityId>{id<AuthoredEntityId>(10), id<AuthoredEntityId>(20),
                                        id<AuthoredEntityId>(30), id<AuthoredEntityId>(40)});
}

TEST_CASE("batch worker values reenter the owner with the original revision fence",
          "[authoring-batches]") {
    auto value = owner();
    const auto expected = status(*value).revision;
    std::optional<AuthoringBatch> completed;
    std::thread worker([&] {
        completed = batch({SetNameCommand{id<AuthoredEntityId>(40), {"worker candidate"}}});
    });
    worker.join();
    REQUIRE(completed);
    changed(*value, SetNameCommand{id<AuthoredEntityId>(30), {"human intervened"}},
            {id<AuthoredEntityId>(30)});
    batch_rejected(*value, *completed, expected, ErrorCode::conflict);
    REQUIRE(take(value->execute(*completed, status(*value).revision)).outcome);
}

TEST_CASE("batch full scene candidates retain all surviving handles within fixed source bounds",
          "[authoring-batches]") {
    auto document = source();
    document.entities.clear();
    for (std::uint32_t i = 1; i <= source_maximum_entities; ++i)
        document.entities.push_back(entity(i));
    auto value = owner(document);
    const auto expected = status(*value).revision;
    const auto survivor = find(*value, 4000);
    auto commands = batch({DeleteEntityCommand{id<AuthoredEntityId>(1)},
                           CreateEntityCommand{id<AuthoredEntityId>(5000)}});
    REQUIRE(take(value->preview(commands, expected)).outcome);
    REQUIRE(take(value->execute(commands, expected)).outcome);
    CHECK(take(live(*value).stats()).live_entities == source_maximum_entities);
    CHECK(find(*value, 4000) == survivor);
    batch_rejected(*value, batch({CreateEntityCommand{id<AuthoredEntityId>(5001)}}),
                   status(*value).revision, ErrorCode::conflict);
}

TEST_CASE("batch allocation failures before publication preserve the complete live owner",
          "[authoring-batches]") {
    for (bool preview : {true, false}) {
        bool exhausted = false;
        std::size_t observed_faults = 0;
        for (std::size_t index = 0; index < 512; ++index) {
            auto value = owner();
            const auto before = capture(*value);
            const auto original = find(*value, 30);
            const auto commands =
                batch({SetNameCommand{id<AuthoredEntityId>(40), {"allocate staged name"}},
                       DeleteEntityCommand{id<AuthoredEntityId>(30)},
                       CreateEntityCommand{id<AuthoredEntityId>(50),
                                           {"allocate staged entity"},
                                           {},
                                           id<AuthoredEntityId>(10)}});
#ifdef _WIN32
            REQUIRE(shark::tools::detail::AuthoredSceneTestAccess::fail_batch_after(*value, index));
#else
            shark::test::allocation_fault::arm(index);
#endif
            auto reply = preview ? value->preview(commands, before.revision)
                                 : value->execute(commands, before.revision);
#ifdef _WIN32
            const bool fired = !reply || !reply.value().outcome;
#else
            const bool fired = shark::test::allocation_fault::disarm();
#endif
            if (fired) {
                ++observed_faults;
                CHECK_FALSE((reply && reply.value().outcome));
                same_capture(capture(*value), before);
                CHECK(find(*value, 30) == original);
            } else {
                REQUIRE(reply);
                REQUIRE(reply.value().outcome);
                CHECK(reply.value().outcome.value().changed);
                if (preview)
                    same_capture(capture(*value), before);
                else
                    CHECK(status(*value).revision.revision == before.revision.revision + 1);
                exhausted = true;
                break;
            }
        }
        CHECK(exhausted);
        CHECK(observed_faults >= 5);
    }
}

namespace {
AuthoringHistoryStatus history(const AuthoredScene& value) {
    return take(value.history(status(value).revision));
}
void same_history(const AuthoringHistoryStatus& actual, const AuthoringHistoryStatus& expected) {
    CHECK(actual.revision == expected.revision);
    CHECK(actual.synchronized == expected.synchronized);
    CHECK(actual.undo_entries == expected.undo_entries);
    CHECK(actual.redo_entries == expected.redo_entries);
    CHECK(actual.retained_bytes == expected.retained_bytes);
    CHECK(actual.next_undo.has_value() == expected.next_undo.has_value());
    CHECK(actual.next_redo.has_value() == expected.next_redo.has_value());
    if (actual.next_undo && expected.next_undo) {
        CHECK(actual.next_undo->ticket == expected.next_undo->ticket);
        CHECK(actual.next_undo->command_count == expected.next_undo->command_count);
    }
    if (actual.next_redo && expected.next_redo) {
        CHECK(actual.next_redo->ticket == expected.next_redo->ticket);
        CHECK(actual.next_redo->command_count == expected.next_redo->command_count);
    }
}
AuthoringHistoryReply move_history(AuthoredScene& value, bool forward = false) {
    const auto before = history(value);
    const auto next = forward ? before.next_redo : before.next_undo;
    REQUIRE(next);
    auto reply = take(forward ? value.redo(next->ticket, before.revision)
                              : value.undo(next->ticket, before.revision));
    REQUIRE(reply.outcome);
    CHECK(reply.ticket == next->ticket);
    CHECK(reply.outcome.value().changed);
    CHECK(reply.observed_revision == status(value).revision);
    CHECK(reply.observed_revision.scene_instance == before.revision.scene_instance);
    CHECK(reply.observed_revision.revision == before.revision.revision + 1);
    CHECK(status(value).dirty);
    return reply;
}
void history_rejected(AuthoredScene& value, AuthoringHistoryTicket ticket, SceneRevision expected,
                      bool forward = false) {
    const auto before = capture(value);
    const auto old = history(value);
    auto reply = take(forward ? value.redo(ticket, expected) : value.undo(ticket, expected));
    rejected(reply.outcome, ErrorCode::conflict);
    CHECK(reply.observed_revision == before.revision);
    same_capture(capture(value), before);
    same_history(history(value), old);
}
void edit_name(AuthoredScene& value, const char* name) {
    REQUIRE(take(value.execute(SetNameCommand{id<AuthoredEntityId>(40), {name}},
                               status(value).revision))
                .outcome);
}
} // namespace

TEST_CASE("history single edits and atomic batches share ordered groups across callers",
          "[authoring-history]") {
    auto value = owner();
    const auto initial = capture(*value);
    CHECK(history(*value).synchronized);
    CHECK(history(*value).retained_bytes == 0);
    edit_name(*value, "human");
    const auto human = capture(*value);
    const auto first = history(*value);
    REQUIRE(first.next_undo);
    CHECK(first.next_undo->command_count == 1);
    Transform transformed;
    transformed.translation = {3.0, 4.0, 5.0};
    REQUIRE(take(value->execute(batch({SetTransformCommand{id<AuthoredEntityId>(40), transformed},
                                       ReparentEntityCommand{id<AuthoredEntityId>(30),
                                                             id<AuthoredEntityId>(40)}}),
                                status(*value).revision))
                .outcome);
    const auto agent = capture(*value);
    CHECK(history(*value).undo_entries == 2);
    REQUIRE(history(*value).next_undo);
    CHECK(history(*value).next_undo->command_count == 2);
    const auto handle = find(*value, 40);
    move_history(*value);
    CHECK(capture(*value).scene == human.scene);
    CHECK(find(*value, 40) == handle);
    move_history(*value);
    CHECK(capture(*value).scene == initial.scene);
    CHECK(history(*value).undo_entries == 0);
    CHECK(history(*value).redo_entries == 2);
    move_history(*value, true);
    CHECK(capture(*value).scene == human.scene);
    move_history(*value, true);
    CHECK(capture(*value).scene == agent.scene);
    CHECK(state(*value, 30).world_matrix[3][0] == 4.0);
    CHECK(history(*value).next_undo->command_count == 2);
}

TEST_CASE("history subtree deletion restores source ordering hierarchy extras and fresh handles",
          "[authoring-history]") {
    auto value = owner();
    const auto initial = capture(*value);
    const auto parent = find(*value, 10);
    const auto child = find(*value, 30);
    const auto other = find(*value, 40);
    const auto world = state(*value, 20).world_matrix;
    REQUIRE(take(value->execute(DeleteEntityCommand{id<AuthoredEntityId>(10)}, initial.revision))
                .outcome);
    const auto removed = capture(*value);
    const auto effect = move_history(*value).outcome.value();
    CHECK(effect.affected_entities ==
          std::vector<AuthoredEntityId>{id<AuthoredEntityId>(10), id<AuthoredEntityId>(20),
                                        id<AuthoredEntityId>(30), id<AuthoredEntityId>(40)});
    CHECK(capture(*value).scene == initial.scene);
    CHECK(find(*value, 10) != parent);
    CHECK(find(*value, 30) != child);
    CHECK(find(*value, 40) == other);
    rejected(live(*value).reference(parent), ErrorCode::invalid_id);
    rejected(live(*value).reference(child), ErrorCode::invalid_id);
    CHECK(transform_matrices_equivalent(state(*value, 20).world_matrix, world));
    const auto restored = find(*value, 10);
    move_history(*value, true);
    CHECK(capture(*value).scene == removed.scene);
    rejected(live(*value).reference(restored), ErrorCode::invalid_id);
    move_history(*value);
    CHECK(find(*value, 10) != restored);
    CHECK(capture(*value).scene == initial.scene);
}

TEST_CASE("history replacement groups never resurrect or reuse the opposite incarnation",
          "[authoring-history]") {
    auto value = owner();
    const auto initial = capture(*value);
    const auto original = find(*value, 30);
    const auto survivor = find(*value, 10);
    REQUIRE(take(value->execute(batch({DeleteEntityCommand{id<AuthoredEntityId>(30)},
                                       CreateEntityCommand{id<AuthoredEntityId>(30),
                                                           {"new incarnation"},
                                                           {},
                                                           id<AuthoredEntityId>(40)}}),
                                initial.revision))
                .outcome);
    const auto replacement = find(*value, 30);
    const auto replaced = capture(*value);
    CHECK(captured_entity(replaced, 30).extras_json == "{}");
    move_history(*value);
    const auto restored = find(*value, 30);
    CHECK(restored != original);
    CHECK(restored != replacement);
    CHECK(capture(*value).scene == initial.scene);
    rejected(live(*value).reference(replacement), ErrorCode::invalid_id);
    move_history(*value, true);
    CHECK(find(*value, 30) != restored);
    CHECK(find(*value, 30) != replacement);
    rejected(live(*value).reference(restored), ErrorCode::invalid_id);
    CHECK(capture(*value).scene == replaced.scene);
    CHECK(find(*value, 10) == survivor);
}

TEST_CASE("history net source neutral transaction still moves its revision once",
          "[authoring-history]") {
    auto value = owner();
    const auto before = capture(*value);
    REQUIRE(take(value->execute(batch({SetNameCommand{id<AuthoredEntityId>(40), {"temporary"}},
                                       SetNameCommand{id<AuthoredEntityId>(40), {"Entity 40"}}}),
                                before.revision))
                .outcome);
    CHECK(capture(*value).scene == before.scene);
    CHECK(history(*value).undo_entries == 1);
    move_history(*value);
    CHECK(capture(*value).scene == before.scene);
    move_history(*value, true);
    CHECK(capture(*value).scene == before.scene);
}

TEST_CASE("history previews noops and failures preserve redo and ticket allocation",
          "[authoring-history]") {
    auto value = owner();
    edit_name(*value, "one");
    move_history(*value);
    const auto before = capture(*value);
    const auto old = history(*value);
    REQUIRE(take(value->preview(batch({SetNameCommand{id<AuthoredEntityId>(40), {"preview"}}}),
                                before.revision))
                .outcome);
    REQUIRE(take(value->execute(batch({}), before.revision)).outcome);
    REQUIRE(take(value->execute(SetNameCommand{id<AuthoredEntityId>(40), {"Entity 40"}},
                                before.revision))
                .outcome);
    command_rejected(*value, DeleteEntityCommand{id<AuthoredEntityId>(99)}, ErrorCode::invalid_id);
    same_capture(capture(*value), before);
    same_history(history(*value), old);
    edit_name(*value, "branch");
    const auto next = history(*value);
    CHECK(next.redo_entries == 0);
    CHECK(next.undo_entries == 1);
    CHECK(next.next_undo->ticket.value == old.next_redo->ticket.value + 1);
    history_rejected(*value, old.next_redo->ticket, next.revision, true);
}

TEST_CASE("history stale foreign future and wrong ticket requests preserve all state",
          "[authoring-history]") {
    auto value = owner();
    const auto stale = status(*value).revision;
    edit_name(*value, "one");
    const auto observed = history(*value);
    auto other = owner();
    for (const auto expected :
         {SceneRevision{}, stale, status(*other).revision,
          SceneRevision{observed.revision.scene_instance, observed.revision.revision + 1}}) {
        history_rejected(*value, observed.next_undo->ticket, expected);
        rejected(value->history(expected), ErrorCode::conflict);
        rejected(value->clear_history(expected), ErrorCode::conflict);
    }
    history_rejected(*value, {0}, observed.revision);
    history_rejected(*value, {observed.next_undo->ticket.value + 1}, observed.revision);
    history_rejected(*value, observed.next_undo->ticket, observed.revision, true);
    move_history(*value);
    history_rejected(*value, observed.next_undo->ticket, history(*value).revision);
    history_rejected(*value, observed.next_undo->ticket, observed.revision, true);
}

TEST_CASE("history out of band Core mutations require explicit reconciliation",
          "[authoring-history]") {
    auto value = owner();
    edit_name(*value, "authoring");
    const auto ticket = history(*value).next_undo->ticket;
    REQUIRE(fixture_scene(*value).set_name(find(*value, 30), {"intervening Core value"}));
    const auto before = capture(*value);
    CHECK_FALSE(history(*value).synchronized);
    history_rejected(*value, ticket, before.revision);
    command_rejected(*value, SetNameCommand{id<AuthoredEntityId>(40), {"blocked"}},
                     ErrorCode::conflict);
    rejected(take(value->preview(batch({}), before.revision)).outcome, ErrorCode::conflict);
    const auto cleared = take(value->clear_history(before.revision));
    CHECK(cleared.synchronized);
    CHECK(cleared.retained_bytes == 0);
    CHECK(cleared.undo_entries == 0);
    same_capture(capture(*value), before);
    edit_name(*value, "reconciled");
    CHECK(history(*value).next_undo->ticket.value == ticket.value + 1);
    move_history(*value);
    CHECK(capture(*value).scene == before.scene);
}

TEST_CASE("history saves retain groups and undo redo conservatively mark dirty",
          "[authoring-history]") {
    FileFixture fixture;
    auto files = take(ProjectFiles::open(fixture.root));
    auto value = owner();
    const auto initial = capture(*value);
    auto saved = take(files->save_scene(*value, initial.revision, std::nullopt));
    edit_name(*value, "saved edit");
    const auto old = history(*value);
    rejected(files->save_scene(*value, initial.revision, saved.file.revision), ErrorCode::conflict);
    same_history(history(*value), old);
    saved = take(files->save_scene(*value, old.revision, saved.file.revision));
    CHECK(saved.current_scene_clean);
    same_history(history(*value), old);
    move_history(*value);
    CHECK(capture(*value).scene == initial.scene);
    move_history(*value, true);
    CHECK(state(*value, 40).name.value == "saved edit");
    CHECK(status(*value).dirty);
}

TEST_CASE("history successful reload clears groups while rejected reload preserves them",
          "[authoring-history]") {
    FileFixture fixture;
    auto files = take(ProjectFiles::open(fixture.root));
    auto value = owner();
    REQUIRE(files->save_scene(*value, status(*value).revision, std::nullopt));
    edit_name(*value, "unsaved");
    const auto before = capture(*value);
    const auto old = history(*value);
    rejected(files->reload_scene(*value, before.revision), ErrorCode::conflict);
    same_capture(capture(*value), before);
    same_history(history(*value), old);
    REQUIRE(files->reload_scene(*value, before.revision, UnsavedScenePolicy::discard));
    CHECK(history(*value).undo_entries == 0);
    CHECK(history(*value).redo_entries == 0);
    CHECK(history(*value).retained_bytes == 0);
    CHECK(status(*value).revision.scene_instance != before.revision.scene_instance);
    CHECK_FALSE(status(*value).dirty);
    history_rejected(*value, old.next_undo->ticket, status(*value).revision);
    edit_name(*value, "after reload");
    CHECK(history(*value).next_undo->ticket.value > old.next_undo->ticket.value);
}

TEST_CASE("history failed activation retains history and successful activation resets it",
          "[authoring-history]") {
    auto value = owner();
    edit_name(*value, "old");
    move_history(*value);
    const auto old = history(*value);
    auto invalid = source();
    invalid.entities.front().parent = id<AuthoredEntityId>(99);
    REQUIRE_FALSE(value->activate(invalid, old.revision, UnsavedScenePolicy::discard));
    same_history(history(*value), old);
    REQUIRE(value->activate(source(), old.revision, UnsavedScenePolicy::discard));
    CHECK(history(*value).retained_bytes == 0);
    CHECK_FALSE(history(*value).next_redo);
}

TEST_CASE("history entry bounds evict the oldest group without reusing tickets",
          "[authoring-history]") {
    auto value = owner(source(), {}, {2, 1024 * 1024});
    edit_name(*value, "one");
    const auto first = history(*value).next_undo->ticket;
    edit_name(*value, "two");
    edit_name(*value, "three");
    CHECK(history(*value).undo_entries == 2);
    CHECK(history(*value).next_undo->ticket.value == first.value + 2);
    move_history(*value);
    CHECK(state(*value, 40).name.value == "two");
    move_history(*value);
    CHECK(state(*value, 40).name.value == "one");
    history_rejected(*value, first, status(*value).revision);
    const auto before = capture(*value);
    const auto cleared = take(value->clear_history(before.revision));
    CHECK(cleared.retained_bytes == 0);
    same_capture(capture(*value), before);
    edit_name(*value, "four");
    CHECK(history(*value).next_undo->ticket.value == first.value + 3);
}

TEST_CASE("history byte pressure evicts deterministically and oversized edits reject atomically",
          "[authoring-history]") {
    auto probe = owner();
    edit_name(*probe, "one");
    const auto charge = history(*probe).retained_bytes;
    REQUIRE(charge > 1);
    auto bounded = owner(source(), {}, {32, charge});
    edit_name(*bounded, "one");
    edit_name(*bounded, "two");
    CHECK(history(*bounded).undo_entries == 1);
    CHECK(history(*bounded).retained_bytes <= charge);
    move_history(*bounded);
    CHECK(state(*bounded, 40).name.value == "one");
    auto tiny = owner(source(), {}, {32, 1});
    command_rejected(*tiny, SetNameCommand{id<AuthoredEntityId>(40), {"cannot retain"}},
                     ErrorCode::conflict);
    command_rejected(*tiny, DeleteEntityCommand{id<AuthoredEntityId>(10)}, ErrorCode::conflict);
    CHECK(history(*tiny).retained_bytes == 0);
    CHECK(history(*tiny).undo_entries == 0);
}

TEST_CASE("history invalid limits reject owner creation without changing source",
          "[authoring-history]") {
    const auto document = source();
    for (const auto limits : {AuthoringHistoryLimits{0, 1},
                              AuthoringHistoryLimits{authoring_maximum_history_entries + 1, 1},
                              AuthoringHistoryLimits{1, 0},
                              AuthoringHistoryLimits{1, authoring_maximum_history_bytes + 1}}) {
        rejected(AuthoredScene::create(project(), document, {}, limits),
                 ErrorCode::invalid_configuration);
    }
    CHECK(document == source());
}

TEST_CASE("history pending work visits wrong threads and closed owners reject safely",
          "[authoring-history]") {
    auto value = owner();
    edit_name(*value, "one");
    const auto old = history(*value);
    const auto ticket = old.next_undo->ticket;
    auto& raw = fixture_scene(*value);
    REQUIRE(raw.defer_set_name(find(*value, 30), {"queued"}));
    rejected(take(value->undo(ticket, old.revision)).outcome, ErrorCode::conflict);
    rejected(value->clear_history(old.revision), ErrorCode::conflict);
    same_history(history(*value), old);
    REQUIRE(raw.apply_deferred());
    REQUIRE(value->clear_history(status(*value).revision));
    edit_name(*value, "two");
    const auto current = history(*value);
    REQUIRE(raw.visit_entities([&](const EntityReference&) -> Result<void> {
        rejected(take(value->undo(current.next_undo->ticket, current.revision)).outcome,
                 ErrorCode::conflict);
        rejected(value->clear_history(current.revision), ErrorCode::conflict);
        return {};
    }));
    std::array<bool, 4> denied{};
    std::thread worker([&] {
        denied[0] = !value->undo(current.next_undo->ticket, current.revision);
        denied[1] = !value->redo(current.next_undo->ticket, current.revision);
        denied[2] = !value->history();
        denied[3] = !value->clear_history(current.revision);
    });
    worker.join();
    for (const auto result : denied)
        CHECK(result);
    same_history(history(*value), current);
    REQUIRE(raw.close());
    REQUIRE_FALSE(value->undo(ticket, current.revision));
    REQUIRE_FALSE(value->redo(ticket, current.revision));
    REQUIRE_FALSE(value->history());
    REQUIRE_FALSE(value->clear_history(current.revision));
}

TEST_CASE("history moves invalidate query cursors while clear preserves them",
          "[authoring-history]") {
    auto value = owner();
    edit_name(*value, "one");
    SceneQueryOptions options;
    options.page_size = 1;
    auto page = take(value->query_entities(options));
    REQUIRE(page.next_cursor());
    move_history(*value);
    rejected(value->continue_query(*page.next_cursor()), ErrorCode::conflict);
    page = take(value->query_entities(options));
    REQUIRE(page.next_cursor());
    REQUIRE(value->clear_history(status(*value).revision));
    REQUIRE(value->continue_query(*page.next_cursor()));
}

TEST_CASE("history maximum source scene replays within the fixed entity bound",
          "[authoring-history]") {
    auto document = source();
    document.entities.clear();
    for (std::uint32_t index = 1; index <= source_maximum_entities; ++index)
        document.entities.push_back(entity(index));
    auto value = owner(document);
    const auto survivor = find(*value, 4000);
    REQUIRE(take(value->execute(batch({DeleteEntityCommand{id<AuthoredEntityId>(1)},
                                       CreateEntityCommand{id<AuthoredEntityId>(5000)}}),
                                status(*value).revision))
                .outcome);
    const auto after = capture(*value);
    move_history(*value);
    CHECK(capture(*value).scene == document);
    CHECK(find(*value, 4000) == survivor);
    move_history(*value, true);
    CHECK(capture(*value).scene == after.scene);
    CHECK(take(live(*value).stats()).live_entities == source_maximum_entities);
}

TEST_CASE("history allocation failures retain live state metadata and both stack directions",
          "[authoring-history]") {
    for (bool forward : {false, true}) {
        bool exhausted = false;
        std::size_t faults = 0;
        for (std::size_t index = 0; index < 512; ++index) {
            auto value = owner();
            REQUIRE(take(value->execute(batch({DeleteEntityCommand{id<AuthoredEntityId>(30)},
                                               CreateEntityCommand{id<AuthoredEntityId>(30),
                                                                   {"replacement"}}}),
                                        status(*value).revision))
                        .outcome);
            if (forward)
                move_history(*value);
            const auto before = capture(*value);
            const auto old = history(*value);
            const auto handle = find(*value, 30);
            const auto ticket = forward ? old.next_redo->ticket : old.next_undo->ticket;
#ifdef _WIN32
            REQUIRE(detail::AuthoredSceneTestAccess::fail_history_after(*value, index));
#else
            shark::test::allocation_fault::arm(index);
#endif
            auto reply =
                forward ? value->redo(ticket, old.revision) : value->undo(ticket, old.revision);
#ifdef _WIN32
            const bool fired = !reply || !reply.value().outcome;
#else
            const bool fired = shark::test::allocation_fault::disarm();
#endif
            if (fired) {
                ++faults;
                CHECK_FALSE((reply && reply.value().outcome));
                same_capture(capture(*value), before);
                same_history(history(*value), old);
                CHECK(find(*value, 30) == handle);
            } else {
                REQUIRE(reply);
                REQUIRE(reply.value().outcome);
                exhausted = true;
                break;
            }
        }
        CHECK(exhausted);
        CHECK(faults >= 5);
    }
}

TEST_CASE("history capture retains the source aggregate extras limit for new entities",
          "[authoring-history]") {
    auto document = source();
    const std::string full =
        std::string{"{\"x\":\""} + std::string(source_maximum_extras_bytes - 8, 'a') + "\"}";
    REQUIRE(full.size() == source_maximum_extras_bytes);
    document.extras_json = full;
    document.entities.clear();
    for (std::uint32_t index = 1; index <= 15; ++index) {
        auto current = entity(index);
        current.extras_json = full;
        document.entities.push_back(std::move(current));
    }
    auto value = owner(document);
    command_rejected(*value, CreateEntityCommand{id<AuthoredEntityId>(1000)},
                     ErrorCode::invalid_configuration);
    CHECK(history(*value).undo_entries == 0);
}
