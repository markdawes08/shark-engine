#include <shark/tools/authored_scene.hpp>

#include "authored_scene_test_access.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace shark;
using namespace shark::tools;

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
            {"Shared project"},
            id<SceneId>(3),
            {{id<SceneId>(3), take(ProjectPath::parse("scenes/start.sharkscene"))},
             {id<SceneId>(4), take(ProjectPath::parse("scenes/other.sharkscene"))}},
            R"({"project-note":"preserved"})"};
}

SourceEntity entity(const std::uint32_t value, std::string name = "Entity") {
    return {id<AuthoredEntityId>(value), std::nullopt, {std::move(name)}, {}, "{}"};
}

SceneDocument source() {
    auto parent = entity(10, "Parent");
    parent.transform.translation = {10.0, 20.0, 30.0};
    parent.transform.scale = {2.0, 2.0, 2.0};
    parent.extras_json = R"({"owner":"parent"})";
    auto child = entity(11, "Child");
    child.parent = parent.id;
    child.transform.translation = {1.0, 2.0, 3.0};
    child.extras_json = R"({"owner":"child"})";
    auto sibling = entity(12, "Sibling");
    sibling.parent = parent.id;
    sibling.transform.translation = {-1.0, 0.0, 0.0};
    // A child deliberately precedes its parent in the independently authored order.
    return {id<ProjectId>(1),
            id<SceneId>(3),
            WorldPosition{id<WorldId>(2), {-123456, 2048, world_cell_max}, {0.25, 512.0, 1023.5}},
            {child, parent, sibling},
            R"({"scene-note":["stable",7]})"};
}

std::unique_ptr<AuthoredScene> owner(const SceneDocument& document = source(),
                                     const SceneLimits limits = {}) {
    return take(AuthoredScene::create(project(), document, limits));
}

const Scene& live(const AuthoredScene& value) {
    return take(value.scene()).get();
}

Scene& fixture_scene(AuthoredScene& value) {
    return take(detail::AuthoredSceneTestAccess::scene(value)).get();
}

EntityHandle find(AuthoredScene& value, const std::uint32_t identity) {
    return take(live(value).find(id<AuthoredEntityId>(identity)));
}

AuthoredSceneStatus status(const AuthoredScene& value) {
    return take(value.status());
}

Result<AuthoringCommandEffect> execute(AuthoredScene& value, const AuthoringCommand& command) {
    auto reply = take(value.execute(command, status(value).revision));
    CHECK(reply.observed_revision == status(value).revision);
    return std::move(reply.outcome);
}

CapturedScene capture(const AuthoredScene& value) {
    return take(value.capture());
}

template <typename T> void rejected(const Result<T>& value, const ErrorCode code) {
    REQUIRE_FALSE(value);
    CHECK(value.error().code == code);
    CHECK_FALSE(value.error().message.empty());
    CHECK_FALSE(value.error().context.operation.empty());
}

void same_capture(const CapturedScene& actual, const CapturedScene& expected) {
    CHECK(actual.revision == expected.revision);
    CHECK(actual.dirty == expected.dirty);
    CHECK(actual.project == expected.project);
    CHECK(actual.scene == expected.scene);
}

std::vector<AuthoredEntityId> identities(const SceneDocument& document) {
    std::vector<AuthoredEntityId> values;
    for (const auto& current : document.entities) {
        values.push_back(current.id);
    }
    return values;
}

static_assert(!std::is_copy_constructible_v<AuthoredScene>);
static_assert(!std::is_move_constructible_v<AuthoredScene>);
static_assert(std::is_same_v<decltype(std::declval<const AuthoredScene&>().scene()),
                             Result<std::reference_wrapper<const Scene>>>);
static_assert(std::is_same_v<decltype(std::declval<AuthoredScene&>().scene()),
                             Result<std::reference_wrapper<const Scene>>>);
static_assert(std::is_same_v<decltype(CreateEntityCommand::id), AuthoredEntityId>);

} // namespace

TEST_CASE("authored loading resolves forward parents and preserves complete source identity",
          "[authored-scene]") {
    const auto document = source();
    auto value = owner(document);
    const auto initial = status(*value);
    CHECK(initial.revision.scene_instance != 0);
    CHECK_FALSE(initial.dirty);
    CHECK(initial.pending_mutations == 0);
    const auto saved = take(value->capture(initial.revision));
    CHECK(saved.revision == initial.revision);
    CHECK(saved.project == project());
    CHECK(saved.scene == document);
    CHECK_FALSE(saved.dirty);
    const auto& const_owner = *std::as_const(value);
    const auto const_access = take(const_owner.scene());
    const auto& current = const_access.get();
    const auto child = take(current.find(id<AuthoredEntityId>(11)));
    const auto parent = take(current.find(id<AuthoredEntityId>(10)));
    const auto state = take(current.entity_state(child));
    CHECK(state.parent == parent);
    CHECK(state.depth == 2);
    CHECK(state.world_matrix[3][0] == 12.0);
    CHECK(state.world_matrix[3][1] == 24.0);
    CHECK(state.world_matrix[3][2] == 36.0);
    CHECK(state.reference.scope == SceneScope{AuthoredSceneScope{id<SceneId>(3)}});
    CHECK(std::holds_alternative<AuthoredEntityId>(state.reference.identity));
    CHECK_FALSE(state.reference.authority);
    CHECK(take(current.stats()).live_entities == 3);
    const auto encoded = take(encode_scene_document(saved.scene));
    CHECK(encoded.find("scene_instance") == std::string::npos);
    CHECK(encoded.find("generation") == std::string::npos);
    CHECK(encoded.find("authority") == std::string::npos);
    // Exercise the underlying Core scope invariant explicitly; the public typed
    // authoring command cannot represent either foreign identity or authority.
    rejected(fixture_scene(*value).create_entity(
                 {id<WorldEntityId>(90), std::nullopt, {}, {}, std::nullopt}),
             ErrorCode::invalid_id);
    rejected(fixture_scene(*value).create_entity(
                 {id<AuthoredEntityId>(90),
                  AuthorityStamp{id<RegionId>(91), id<RegionInstanceId>(92), 1},
                  {},
                  {},
                  std::nullopt}),
             ErrorCode::invalid_argument);
    same_capture(capture(*value), saved);
}

TEST_CASE("authored owner and capture retain independent values without borrowing caller data",
          "[authored-scene]") {
    auto input_project = project();
    auto input_scene = source();
    auto value = take(AuthoredScene::create(input_project, input_scene));
    auto saved = capture(*value);
    input_project.name.value = "Changed outside owner";
    input_project.scenes.clear();
    input_scene.entities.clear();
    input_scene.extras_json = R"({"replacement":true})";
    saved.scene.entities.front().name.value = "Changed captured copy";
    saved.project.scenes.clear();
    const auto fresh = capture(*value);
    CHECK(fresh.project == project());
    CHECK(fresh.scene == source());
    REQUIRE(execute(*value, SetNameCommand{id<AuthoredEntityId>(11), {"Committed name"}}));
    CHECK(saved.scene.entities.front().name.value == "Changed captured copy");
    CHECK(fresh.scene.entities.front().name.value == "Child");
    value.reset();
    CHECK(fresh.scene == source());
}

TEST_CASE("authored loading canonicalizes supported values before clean capture",
          "[authored-scene]") {
    auto document = source();
    document.entities.front().transform.rotation = {-8.0, -0.0, -0.0, -0.0};
    document.entities.front().extras_json = " { \"z\": 2, \"a\": [true] } ";
    document.extras_json = " { \"z\": null, \"a\": 1 } ";
    const auto input = document;
    auto value = owner(document);
    const auto saved = capture(*value);
    CHECK(document == input);
    CHECK(saved.scene.entities.front().transform.rotation == math::Quat(1.0, 0.0, 0.0, 0.0));
    CHECK(saved.scene.entities.front().extras_json == R"({"a":[true],"z":2})");
    CHECK(saved.scene.extras_json == R"({"a":1,"z":null})");
    CHECK_FALSE(saved.dirty);
    const auto decoded = take(decode_scene_document(take(encode_scene_document(saved.scene))));
    CHECK(decoded.document == saved.scene);
}

TEST_CASE("authored capture reflects shared component and hierarchy mutations",
          "[authored-scene]") {
    auto value = owner();
    const auto parent = find(*value, 10);
    REQUIRE(execute(*value, SetNameCommand{id<AuthoredEntityId>(11), {"Edited child"}}));
    Transform replacement;
    replacement.translation = {4.0, 5.0, 6.0};
    REQUIRE(execute(*value, SetTransformCommand{id<AuthoredEntityId>(11), replacement}));
    REQUIRE(execute(*value, ReparentEntityCommand{id<AuthoredEntityId>(11), std::nullopt,
                                                  ReparentMode::keep_world}));
    auto saved = capture(*value);
    REQUIRE(saved.scene.entities.size() == 3);
    CHECK(saved.dirty);
    CHECK(saved.scene.entities.front().name.value == "Edited child");
    CHECK_FALSE(saved.scene.entities.front().parent);
    CHECK(saved.scene.entities.front().transform.translation == math::Vec3(18.0, 30.0, 42.0));
    CHECK(saved.scene.entities.front().transform.scale == math::Vec3(2.0, 2.0, 2.0));
    CHECK(saved.scene.entities.front().extras_json == R"({"owner":"child"})");
    CHECK(saved.scene.world_origin == source().world_origin);
    CHECK(take(live(*value).entity_state(parent)).children.size() == 1);
    auto decoded = take(decode_scene_document(take(encode_scene_document(saved.scene))));
    auto restored = take(AuthoredScene::create(saved.project, decoded.document));
    const auto round_trip = capture(*restored);
    CHECK(round_trip.scene == saved.scene);
    CHECK_FALSE(round_trip.dirty);
    CHECK(round_trip.revision.scene_instance != saved.revision.scene_instance);
}

TEST_CASE("authored scene catalog resolves nonstartup scenes without mutating project settings",
          "[authored-scene]") {
    auto alternate = source();
    alternate.scene = id<SceneId>(4);
    alternate.entities.clear();
    alternate.world_origin.reset();
    auto value = owner(alternate);
    CHECK(capture(*value).project.startup_scene == id<SceneId>(3));
    CHECK(capture(*value).scene == alternate);
    const auto replacement = take(value->activate(source(), status(*value).revision));
    CHECK(capture(*value).project == project());
    CHECK(capture(*value).scene == source());
    CHECK(status(*value).revision == replacement);
}

TEST_CASE("authored loading rejects project catalog and world mismatches without altering input",
          "[authored-scene]") {
    for (int scenario = 0; scenario < 4; ++scenario) {
        CAPTURE(scenario);
        auto catalog = project();
        auto document = source();
        if (scenario == 0) {
            document.project = id<ProjectId>(999);
        } else if (scenario == 1) {
            document.scene = id<SceneId>(999);
        } else if (scenario == 2) {
            document.world_origin->world = id<WorldId>(999);
        } else {
            catalog.startup_scene = id<SceneId>(999);
        }
        const auto project_before = catalog;
        const auto scene_before = document;
        rejected(AuthoredScene::create(catalog, document), ErrorCode::invalid_configuration);
        CHECK(catalog == project_before);
        CHECK(document == scene_before);
    }
}

TEST_CASE("authored activation failure preserves dirty live data and metadata",
          "[authored-scene]") {
    auto value = owner();
    const auto handle = find(*value, 11);
    REQUIRE(execute(*value, SetNameCommand{id<AuthoredEntityId>(11), {"Unsaved work"}}));
    const auto before = capture(*value);
    for (int scenario = 0; scenario < 7; ++scenario) {
        CAPTURE(scenario);
        auto candidate = source();
        if (scenario == 0) {
            candidate.project = id<ProjectId>(888);
        } else if (scenario == 1) {
            candidate.scene = id<SceneId>(888);
        } else if (scenario == 2) {
            candidate.world_origin->world = id<WorldId>(888);
        } else if (scenario == 3) {
            candidate.entities.front().parent = id<AuthoredEntityId>(888);
        } else if (scenario == 4) {
            candidate.entities[1].parent = candidate.entities.front().id;
        } else if (scenario == 5) {
            candidate.entities.back().id = candidate.entities.front().id;
        } else {
            candidate.entities.front().extras_json = R"({"duplicate":1,"duplicate":2})";
        }
        const auto unchanged = candidate;
        rejected(value->activate(candidate, before.revision, UnsavedScenePolicy::discard),
                 ErrorCode::invalid_configuration);
        same_capture(capture(*value), before);
        CHECK(candidate == unchanged);
        CHECK(take(live(*value).entity_state(handle)).name.value == "Unsaved work");
    }
}

TEST_CASE("authored candidate validates composed transforms before replacing the active scene",
          "[authored-scene]") {
    auto value = owner();
    const auto before = capture(*value);
    auto candidate = source();
    candidate.entities.front().transform.scale = {1e200, 1e200, 1e200};
    candidate.entities[1].transform.scale = {1e200, 1e200, 1e200};
    REQUIRE(validate_scene_document(candidate));
    rejected(AuthoredScene::create(project(), candidate), ErrorCode::invalid_argument);
    rejected(value->activate(candidate, before.revision), ErrorCode::invalid_argument);
    same_capture(capture(*value), before);
    CHECK(find(*value, 11).scene_instance == before.revision.scene_instance);
}

TEST_CASE("authored loading retains valid world shear as local authored transforms",
          "[authored-scene]") {
    auto document = source();
    document.entities[1].transform.scale = {2.0, 3.0, 4.0};
    const double half_angle = 0.39269908169872415481;
    document.entities.front().transform.rotation = {std::cos(half_angle), 0.0, 0.0,
                                                    std::sin(half_angle)};
    auto value = owner(document);
    const auto child = take(live(*value).entity_state(find(*value, 11)));
    const math::Vec3 first{child.world_matrix[0]};
    const math::Vec3 second{child.world_matrix[1]};
    // Rotation beneath nonuniform parent scale yields nonorthogonal world columns.
    CHECK(std::abs(first.x * second.x + first.y * second.y + first.z * second.z) > 2.0);
    CHECK(child.world_matrix[3][0] == 12.0);
    CHECK(child.world_matrix[3][1] == 26.0);
    CHECK(child.world_matrix[3][2] == 42.0);
    const auto saved = capture(*value);
    CHECK(saved.scene == document);
    CHECK_FALSE(saved.dirty);
}

TEST_CASE("authored activation requires current revision even with explicit discard",
          "[authored-scene]") {
    auto value = owner();
    const auto initial = status(*value).revision;
    REQUIRE(execute(*value, SetNameCommand{id<AuthoredEntityId>(10), {"New revision"}}));
    const auto before = capture(*value);
    for (const auto expected :
         {SceneRevision{}, initial,
          SceneRevision{before.revision.scene_instance + 1, before.revision.revision},
          SceneRevision{before.revision.scene_instance, before.revision.revision + 1}}) {
        rejected(value->activate(source(), expected, UnsavedScenePolicy::discard),
                 ErrorCode::conflict);
        rejected(value->capture(expected), ErrorCode::conflict);
        same_capture(capture(*value), before);
    }
}

TEST_CASE("authored dirty work requires explicit discard and capture does not acknowledge a save",
          "[authored-scene]") {
    auto value = owner();
    REQUIRE(execute(*value, SetNameCommand{id<AuthoredEntityId>(10), {"Unsaved work"}}));
    const auto before = capture(*value);
    CHECK(before.dirty);
    CHECK(status(*value).dirty);
    rejected(value->activate(source(), before.revision), ErrorCode::conflict);
    same_capture(capture(*value), before);
    CHECK(status(*value).dirty);
    const auto revision =
        take(value->activate(source(), before.revision, UnsavedScenePolicy::discard));
    CHECK(revision.scene_instance != before.revision.scene_instance);
    CHECK_FALSE(status(*value).dirty);
    CHECK(capture(*value).scene == source());
}

TEST_CASE("authored dirty tracking respects noops and conservatively retains reverted edits",
          "[authored-scene]") {
    auto value = owner();
    const auto initial = status(*value);
    const auto child = find(*value, 11);
    const auto state = take(live(*value).entity_state(child));
    REQUIRE(execute(*value, SetNameCommand{id<AuthoredEntityId>(11), state.name}));
    REQUIRE(execute(*value, SetTransformCommand{id<AuthoredEntityId>(11), state.local_transform}));
    REQUIRE(
        execute(*value, ReparentEntityCommand{id<AuthoredEntityId>(11), id<AuthoredEntityId>(10),
                                              ReparentMode::keep_local}));
    CHECK(status(*value).revision == initial.revision);
    CHECK_FALSE(status(*value).dirty);
    REQUIRE(execute(*value, SetNameCommand{id<AuthoredEntityId>(11), {"Temporary change"}}));
    REQUIRE(execute(*value, SetNameCommand{id<AuthoredEntityId>(11), state.name}));
    CHECK(capture(*value).scene == source());
    CHECK(status(*value).dirty);
    CHECK(status(*value).revision != initial.revision);
    rejected(value->activate(source(), status(*value).revision), ErrorCode::conflict);
}

TEST_CASE("authored pending writes reject capture and replacement without consuming queue outcomes",
          "[authored-scene]") {
    auto value = owner();
    const auto child = find(*value, 11);
    const auto initial = status(*value);
    const auto first = take(fixture_scene(*value).defer_destroy(child));
    const auto second = take(fixture_scene(*value).defer_destroy(child));
    CHECK(status(*value).pending_mutations == 2);
    CHECK(status(*value).revision == initial.revision);
    rejected(value->capture(), ErrorCode::conflict);
    rejected(value->activate(source(), initial.revision), ErrorCode::conflict);
    rejected(value->activate(source(), initial.revision, UnsavedScenePolicy::discard),
             ErrorCode::conflict);
    CHECK(status(*value).pending_mutations == 2);
    CHECK(take(live(*value).entity_state(child)).name.value == "Child");
    const auto report = take(fixture_scene(*value).apply_deferred());
    REQUIRE(report.outcomes.size() == 2);
    CHECK(report.outcomes[0].ticket == first);
    CHECK(report.outcomes[1].ticket == second);
    REQUIRE(report.outcomes[0].result);
    REQUIRE_FALSE(report.outcomes[1].result);
    CHECK(status(*value).pending_mutations == 0);
    CHECK(status(*value).dirty);
    CHECK(capture(*value).scene.entities.size() == 2);
}

TEST_CASE("authored activation rejects active visits while capture remains a committed read",
          "[authored-scene]") {
    auto value = owner();
    const auto before = capture(*value);
    std::size_t visited = 0;
    REQUIRE(fixture_scene(*value).visit_entities([&](const EntityReference&) -> Result<void> {
        ++visited;
        same_capture(capture(*value), before);
        rejected(value->activate(source(), before.revision), ErrorCode::conflict);
        rejected(value->activate(source(), before.revision, UnsavedScenePolicy::discard),
                 ErrorCode::conflict);
        return {};
    }));
    CHECK(visited == 3);
    same_capture(capture(*value), before);
    REQUIRE(value->activate(source(), before.revision));
}

TEST_CASE("authored identical reload invalidates handles references cursors and expected revisions",
          "[authored-scene]") {
    auto value = owner();
    const auto handle = find(*value, 11);
    const auto reference = take(live(*value).reference(handle));
    SceneQueryOptions options;
    options.page_size = 1;
    const auto old_page = take(live(*value).query_entities(options));
    REQUIRE(old_page.next_cursor());
    const auto old_cursor = *old_page.next_cursor();
    const auto before = capture(*value);
    const auto after = take(value->activate(before.scene, before.revision));
    CHECK(after.scene_instance != before.revision.scene_instance);
    rejected(live(*value).entity_state(handle), ErrorCode::invalid_id);
    rejected(live(*value).resolve(reference), ErrorCode::invalid_id);
    rejected(live(*value).continue_query(old_cursor), ErrorCode::conflict);
    options.expected_revision = before.revision;
    rejected(live(*value).query_entities(options), ErrorCode::conflict);
    rejected(value->capture(before.revision), ErrorCode::conflict);
    CHECK(find(*value, 11) != handle);
    CHECK(capture(*value).scene == before.scene);
    CHECK(old_page.entities().size() == 1);
    CHECK(old_page.revision() == before.revision);
    CHECK(before.scene == source());
}

TEST_CASE("authored metadata follows an entity incarnation instead of a reused durable id",
          "[authored-scene]") {
    auto document = source();
    document.entities = {entity(30), entity(10), entity(20), entity(40)};
    document.entities[0].extras_json = R"({"kept":30})";
    document.entities[1].extras_json = R"({"do-not-inherit":10})";
    auto value = owner(document);
    const auto old = find(*value, 10);
    REQUIRE(execute(*value, DeleteEntityCommand{id<AuthoredEntityId>(10)}));
    REQUIRE(execute(*value, CreateEntityCommand{id<AuthoredEntityId>(10), {"Recreated"}}));
    const auto replacement = find(*value, 10);
    REQUIRE(execute(*value, CreateEntityCommand{id<AuthoredEntityId>(50), {"New entity"}}));
    CHECK(replacement.slot == old.slot);
    CHECK(replacement.generation != old.generation);
    auto saved = capture(*value);
    CHECK(identities(saved.scene) == std::vector{id<AuthoredEntityId>(30), id<AuthoredEntityId>(20),
                                                 id<AuthoredEntityId>(40), id<AuthoredEntityId>(10),
                                                 id<AuthoredEntityId>(50)});
    CHECK(saved.scene.entities.front().extras_json == R"({"kept":30})");
    CHECK(saved.scene.entities[3].extras_json == "{}");
    CHECK(saved.scene.entities[4].extras_json == "{}");
    REQUIRE(execute(*value, DeleteEntityCommand{id<AuthoredEntityId>(10)}));
    REQUIRE(
        execute(*value, CreateEntityCommand{id<AuthoredEntityId>(60), {"Another incarnation"}}));
    saved = capture(*value);
    CHECK(identities(saved.scene) == std::vector{id<AuthoredEntityId>(30), id<AuthoredEntityId>(20),
                                                 id<AuthoredEntityId>(40), id<AuthoredEntityId>(60),
                                                 id<AuthoredEntityId>(50)});
    CHECK(saved.scene.entities[3].extras_json == "{}");
}

TEST_CASE("authored subtree deletion removes metadata and capture excludes pending tombstones",
          "[authored-scene]") {
    auto value = owner();
    REQUIRE(execute(*value, DeleteEntityCommand{id<AuthoredEntityId>(10)}));
    const auto empty = capture(*value);
    CHECK(empty.scene.entities.empty());
    CHECK(empty.scene.extras_json == source().extras_json);
    CHECK(empty.scene.world_origin == source().world_origin);
    REQUIRE(execute(*value, CreateEntityCommand{id<AuthoredEntityId>(10), {"New root"}}));
    const auto recreated = capture(*value);
    REQUIRE(recreated.scene.entities.size() == 1);
    CHECK(recreated.scene.entities.front().id == id<AuthoredEntityId>(10));
    CHECK(recreated.scene.entities.front().extras_json == "{}");
}

TEST_CASE("authored empty scenes load and replace without requiring a world anchor",
          "[authored-scene]") {
    auto empty = source();
    empty.entities.clear();
    empty.world_origin.reset();
    auto value = owner(empty);
    CHECK(capture(*value).scene == empty);
    CHECK(status(*value).revision.revision == 0);
    CHECK_FALSE(status(*value).dirty);
    const auto old = status(*value).revision;
    REQUIRE(value->activate(empty, old));
    CHECK(status(*value).revision.scene_instance != old.scene_instance);
    CHECK(capture(*value).scene == empty);
    REQUIRE(value->activate(source(), status(*value).revision));
    CHECK(capture(*value).scene == source());
}

TEST_CASE("authored complete capture covers maximum-size scenes across bounded query pages",
          "[authored-scene]") {
    auto document = source();
    document.entities.clear();
    document.entities.reserve(source_maximum_entities);
    for (std::size_t index = 0; index < source_maximum_entities; ++index) {
        auto current =
            entity(static_cast<std::uint32_t>(index + 100), "Entity " + std::to_string(index));
        current.transform.translation = {static_cast<double>(index), 2.0, -3.0};
        document.entities.push_back(std::move(current));
    }
    auto value = owner(document);
    const auto complete = capture(*value);
    CHECK(complete.scene == document);
    CHECK(complete.scene.entities.size() == 4096);
    SceneQueryOptions options;
    options.page_size = scene_query_maximum_page_size;
    options.scan_budget = scene_query_maximum_scan_budget;
    auto page = take(live(*value).query_entities(options));
    std::size_t count = page.entities().size();
    std::size_t pages = 1;
    while (page.next_cursor()) {
        page = take(live(*value).continue_query(*page.next_cursor()));
        CHECK(page.revision() == complete.revision);
        count += page.entities().size();
        ++pages;
    }
    CHECK(count == 4096);
    CHECK(pages == 16);
    const auto extra = execute(*value, CreateEntityCommand{id<AuthoredEntityId>(99999)});
    rejected(extra, ErrorCode::conflict);
    same_capture(capture(*value), complete);

    // The survivor is beyond several entirely empty bounded scan pages.
    for (std::size_t index = 0; index + 1 < source_maximum_entities; ++index) {
        REQUIRE(execute(*value, DeleteEntityCommand{id<AuthoredEntityId>(
                                    static_cast<std::uint32_t>(index + 100))}));
    }
    options.scan_budget = 256;
    auto sparse_page = take(live(*value).query_entities(options));
    CHECK(sparse_page.entities().empty());
    REQUIRE(sparse_page.next_cursor());
    std::size_t sparse_pages = 1;
    std::size_t sparse_entities = 0;
    while (sparse_page.next_cursor()) {
        sparse_page = take(live(*value).continue_query(*sparse_page.next_cursor()));
        sparse_entities += sparse_page.entities().size();
        ++sparse_pages;
    }
    CHECK(sparse_pages == 16);
    CHECK(sparse_entities == 1);
    REQUIRE(sparse_page.entities().size() == 1);
    CHECK(sparse_page.entities().front().reference.identity ==
          EntityIdentity{document.entities.back().id});
    const auto sparse = capture(*value);
    REQUIRE(sparse.scene.entities.size() == 1);
    CHECK(sparse.scene.entities.front() == document.entities.back());
    CHECK(sparse.dirty);
    CHECK(sparse.scene.world_origin == document.world_origin);
}

TEST_CASE("authored loading honors the maximum hierarchy and lower caller limits",
          "[authored-scene]") {
    auto document = source();
    document.entities.clear();
    for (std::size_t index = 0; index < source_maximum_hierarchy_depth; ++index) {
        auto current = entity(static_cast<std::uint32_t>(index + 100));
        if (index + 1 < source_maximum_hierarchy_depth) {
            current.parent = id<AuthoredEntityId>(static_cast<std::uint32_t>(index + 101));
        }
        document.entities.push_back(std::move(current));
    }
    auto value = owner(document);
    CHECK(take(live(*value).entity_state(find(*value, 100))).depth == 256);
    CHECK(capture(*value).scene == document);
    auto limits = SceneLimits{};
    limits.max_hierarchy_depth = 255;
    rejected(AuthoredScene::create(project(), document, limits), ErrorCode::conflict);
    limits = {};
    limits.max_entities = 255;
    rejected(AuthoredScene::create(project(), document, limits), ErrorCode::conflict);
    limits = {3, 1, 2};
    auto limited = owner(source(), limits);
    const auto before = capture(*limited);
    rejected(limited->activate(document, before.revision), ErrorCode::conflict);
    same_capture(capture(*limited), before);
    REQUIRE(fixture_scene(*limited).defer_set_name(find(*limited, 11), {"One pending"}));
    rejected(fixture_scene(*limited).defer_set_name(find(*limited, 12), {"Queue full"}),
             ErrorCode::conflict);
    CHECK(status(*limited).pending_mutations == 1);
}

TEST_CASE("authored owner rejects unsupported scene limits and malformed discard policies",
          "[authored-scene]") {
    for (const auto limits :
         {SceneLimits{0, 1, 1}, SceneLimits{4097, 1, 1}, SceneLimits{1, 0, 1},
          SceneLimits{1, 65537, 1}, SceneLimits{1, 1, 0}, SceneLimits{1, 1, 257}}) {
        rejected(AuthoredScene::create(project(), source(), limits),
                 ErrorCode::invalid_configuration);
    }
    auto value = owner();
    const auto before = capture(*value);
    rejected(value->activate(source(), before.revision, static_cast<UnsavedScenePolicy>(99)),
             ErrorCode::invalid_argument);
    same_capture(capture(*value), before);
}

TEST_CASE("authored owner checks its thread before accessing scene state", "[authored-scene]") {
    auto value = owner();
    const auto before = capture(*value);
    const auto candidate = source();
    std::array<bool, 5> denied{};
    std::thread worker([&] {
        const auto queried = value->status();
        denied[0] = !queried && queried.error().code == ErrorCode::conflict;
        const auto accessed = value->scene();
        denied[1] = !accessed && accessed.error().code == ErrorCode::conflict;
        const auto const_accessed = std::as_const(*value).scene();
        denied[2] = !const_accessed && const_accessed.error().code == ErrorCode::conflict;
        const auto captured = value->capture();
        denied[3] = !captured && captured.error().code == ErrorCode::conflict;
        const auto activated = value->activate(candidate, before.revision);
        denied[4] = !activated && activated.error().code == ErrorCode::conflict;
    });
    worker.join();
    for (const bool result : denied) {
        CHECK(result);
    }
    same_capture(capture(*value), before);
}

TEST_CASE("authored owner rejects access after its borrowed scene is closed", "[authored-scene]") {
    auto value = owner();
    const auto before = status(*value).revision;
    REQUIRE(fixture_scene(*value).close());
    rejected(value->status(), ErrorCode::conflict);
    rejected(value->scene(), ErrorCode::conflict);
    rejected(std::as_const(*value).scene(), ErrorCode::conflict);
    rejected(value->capture(), ErrorCode::conflict);
    rejected(value->activate(source(), before, UnsavedScenePolicy::discard), ErrorCode::conflict);
}
