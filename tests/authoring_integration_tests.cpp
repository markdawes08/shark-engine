#include <shark/tools/project_files.hpp>

#include "project_files_native.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
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
template <typename T> void rejected(const Result<T>& result, ErrorCode code) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().message.empty());
    CHECK_FALSE(result.error().context.operation.empty());
}
template <typename T> T id(unsigned value) {
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    text[34] = digits[(value >> 4U) & 15U];
    text[35] = digits[value & 15U];
    return T{take(StableId::parse(text))};
}
std::string read_bytes(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input)
        throw std::runtime_error{"authoring integration read failed"};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
void write_bytes(const fs::path& path, std::string_view bytes) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output)
        throw std::runtime_error{"authoring integration write failed"};
}
std::string fixture_bytes(std::string_view name) {
    return read_bytes(fs::path{SHARK_SOURCE_FIXTURE_DIRECTORY} / name);
}
ProjectPath project_path() {
    return take(ProjectPath::parse("project.sharkproject"));
}

class Fixture final {
  public:
    Fixture()
        : parent(fs::current_path()),
          tree(parent / ("shark-authoring-integration-" + take(generate_stable_id()).to_string())),
          root(tree / fs::path{u8"Human agent \u4e16\u754c"}),
          project(take(decode_project_document(fixture_bytes("project-v1.sharkproject"))).document),
          initial(take(decode_scene_document(fixture_bytes("scene-v1.sharkscene"))).document) {
        REQUIRE(tree.is_absolute());
        REQUIRE(tree.parent_path() == parent);
        REQUIRE(fs::create_directory(tree));
        REQUIRE(fs::create_directory(root));
        REQUIRE(fs::create_directory(root / "scenes"));
        write_bytes(root / "project.sharkproject", fixture_bytes("project-v0.sharkproject"));
        write_bytes(scene_path(), fixture_bytes("scene-v0.sharkscene"));
    }
    ~Fixture() {
        if (tree.is_absolute() && tree.parent_path() == parent &&
            tree.filename().string().starts_with("shark-authoring-integration-")) {
            std::error_code error;
            fs::remove_all(tree, error);
        }
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    fs::path scene_path() const {
        return take(take(ProjectRoot::open(root)).resolve(project.scenes.front().path));
    }
    const fs::path parent, tree, root;
    const ProjectDocument project;
    const SceneDocument initial;
};

LoadedAuthoredScene load(ProjectFiles& files, const ProjectDocument& project,
                         std::optional<AuthoringMutationLimits> limits) {
    if (!limits)
        return take(files.load_scene(project, project.startup_scene));
    auto read = take(files.read_scene(project, project.startup_scene));
    auto owner = take(AuthoredScene::create(project, read.source.document, {}, {}, *limits));
    return {std::move(owner), read.revision, read.source.source_schema_version};
}
struct Session {
    explicit Session(std::optional<AuthoringMutationLimits> limits = std::nullopt)
        : files(take(ProjectFiles::open(fixture.root))),
          catalog(take(files->read_project(project_path()))),
          loaded(load(*files, catalog.source.document, limits)), file(loaded.file_revision) {
        CHECK(catalog.source.source_schema_version == 0);
        CHECK(loaded.source_schema_version == 0);
        CHECK(catalog.source.document == fixture.project);
        CHECK(take(loaded.owner->capture()).scene == fixture.initial);
    }
    AuthoredScene& owner() {
        return *loaded.owner;
    }
    Fixture fixture;
    std::unique_ptr<ProjectFiles> files;
    ReadSource<ProjectDocument> catalog;
    LoadedAuthoredScene loaded;
    FileRevision file;
};
SceneRevision revision(const AuthoredScene& owner) {
    return take(owner.status()).revision;
}
const Scene& scene(const AuthoredScene& owner) {
    return take(owner.scene()).get();
}
EntityHandle find(const AuthoredScene& owner, unsigned value) {
    return take(scene(owner).find(id<AuthoredEntityId>(value)));
}
SourceEntity& entity(SceneDocument& document, unsigned value) {
    const auto wanted = id<AuthoredEntityId>(value);
    auto found = std::find_if(document.entities.begin(), document.entities.end(),
                              [&](const auto& item) { return item.id == wanted; });
    REQUIRE(found != document.entities.end());
    return *found;
}
AuthoringCommand rename(unsigned value, std::string name) {
    return SetNameCommand{id<AuthoredEntityId>(value), {std::move(name)}};
}
AuthoringBatch batch(std::vector<AuthoringCommand> commands, unsigned correlation = 80) {
    return {id<AuthoringBatchId>(correlation), std::move(commands)};
}
struct Attempt {
    AuthoringMutationRequest request;
    std::shared_ptr<const AuthoringMutationOutcome> outcome;
};
Attempt submit(AuthoredScene& owner, AuthoringMutation mutation) {
    const auto status = take(owner.mutation_status());
    REQUIRE(status.next);
    AuthoringMutationRequest request{*status.next, status.revision, std::move(mutation)};
    auto reply = take(owner.mutate(request));
    REQUIRE(reply.state == AuthoringMutationState::recorded);
    REQUIRE(reply.outcome);
    CHECK(reply.id == request.id);
    CHECK(reply.observed_revision == revision(owner));
    return {std::move(request), std::move(reply.outcome).value()};
}
Attempt changed(AuthoredScene& owner, AuthoringMutation mutation) {
    const auto before = revision(owner);
    auto result = submit(owner, std::move(mutation));
    REQUIRE(result.outcome->result);
    CHECK(result.outcome->result.value().changed);
    CHECK(revision(owner).scene_instance == before.scene_instance);
    CHECK(revision(owner).revision == before.revision + 1);
    return result;
}
Attempt move(AuthoredScene& owner, bool forward = false) {
    const auto status = take(owner.history());
    const auto group = forward ? status.next_redo : status.next_undo;
    REQUIRE(group);
    if (forward)
        return changed(owner, RedoAuthoringMutation{group->ticket});
    return changed(owner, UndoAuthoringMutation{group->ticket});
}
void same_history(const AuthoringHistoryStatus& left, const AuthoringHistoryStatus& right) {
    CHECK(left.revision == right.revision);
    CHECK(left.synchronized == right.synchronized);
    CHECK(left.undo_entries == right.undo_entries);
    CHECK(left.redo_entries == right.redo_entries);
    CHECK(left.retained_bytes == right.retained_bytes);
    CHECK(left.next_undo.has_value() == right.next_undo.has_value());
    CHECK(left.next_redo.has_value() == right.next_redo.has_value());
    if (left.next_undo && right.next_undo)
        CHECK(left.next_undo->ticket == right.next_undo->ticket);
    if (left.next_redo && right.next_redo)
        CHECK(left.next_redo->ticket == right.next_redo->ticket);
}
struct Checkpoint {
    explicit Checkpoint(Session& session)
        : capture(take(session.owner().capture())), history(take(session.owner().history())),
          ledger(take(session.owner().mutation_status())),
          disk(read_bytes(session.fixture.scene_path())) {}
    void unchanged(Session& session, bool ledger_too = true) const {
        const auto actual = take(session.owner().capture());
        CHECK(actual.project == capture.project);
        CHECK(actual.scene == capture.scene);
        CHECK(actual.revision == capture.revision);
        CHECK(actual.dirty == capture.dirty);
        same_history(take(session.owner().history()), history);
        CHECK(read_bytes(session.fixture.scene_path()) == disk);
        const auto current = take(session.owner().mutation_status());
        CHECK(current.scope == ledger.scope);
        if (ledger_too) {
            CHECK(current.high_watermark == ledger.high_watermark);
            CHECK(current.next == ledger.next);
            CHECK(current.oldest_retained == ledger.oldest_retained);
            CHECK(current.retained_entries == ledger.retained_entries);
            CHECK(current.retained_bytes == ledger.retained_bytes);
        }
    }
    CapturedScene capture;
    AuthoringHistoryStatus history;
    AuthoringMutationStatus ledger;
    std::string disk;
};
void replay(Session& session, const Attempt& original) {
    const Checkpoint before{session};
    auto reply = take(session.owner().mutate(original.request));
    REQUIRE(reply.outcome);
    CHECK(reply.state == AuthoringMutationState::replayed);
    CHECK(reply.outcome.value() == original.outcome);
    CHECK(reply.observed_revision == revision(session.owner()));
    before.unchanged(session);
}
void save(Session& session) {
    const auto before = take(session.owner().capture());
    const auto history = take(session.owner().history());
    const auto ledger = take(session.owner().mutation_status());
    auto saved = take(session.files->save_scene(session.owner(), before.revision, session.file));
    REQUIRE(saved.current_scene_clean);
    CHECK(saved.saved_revision == before.revision);
    CHECK_FALSE(take(session.owner().status()).dirty);
    same_history(take(session.owner().history()), history);
    CHECK(take(session.owner().mutation_status()).high_watermark == ledger.high_watermark);
    session.file = saved.file.revision;
    const auto read = take(session.files->read_scene(before.project, before.scene.scene));
    CHECK(read.source.source_schema_version == source_document_version);
    CHECK(read.source.document == before.scene);
    CHECK(read.revision == session.file);
    CHECK(read_bytes(session.fixture.scene_path()) == take(encode_scene_document(before.scene)));
}
class Control final {
  public:
    explicit Control(detail::FileTestControl value)
        : previous(detail::set_file_test_control(value)) {}
    ~Control() {
        static_cast<void>(detail::set_file_test_control(previous));
    }
    Control(const Control&) = delete;
    Control& operator=(const Control&) = delete;

  private:
    detail::FileTestControl previous;
};
} // namespace

TEST_CASE("authoring connected lineage preserves shared history retries and external source edits",
          "[authoring-integration]") {
    Session session;
    auto& owner = session.owner();
    auto expected = session.fixture.initial;
    const auto survivor = find(owner, 16);
    const auto prior_child = take(scene(owner).reference(find(owner, 17)));
    const auto scope = take(owner.mutation_status()).scope;
    const auto project_commit = take(session.files->write_project(
        project_path(), session.fixture.project, session.catalog.revision));
    CHECK(take(session.files->read_project(project_path())).source.source_schema_version ==
          source_document_version);
    CHECK(project_commit.revision != session.catalog.revision);
    // Human-equivalent caller edits through the shared owner.
    auto human = changed(owner, rename(17, "Human coast"));
    entity(expected, 17).name.value = "Human coast";
    CHECK(take(owner.capture()).scene == expected);
    // Agent-equivalent caller previews a dependent create/reparent/transform batch.
    Transform moved;
    moved.translation = {4.0, 5.0, 6.0};
    const auto input =
        batch({CreateEntityCommand{
                   id<AuthoredEntityId>(32), {"Agent marker"}, moved, id<AuthoredEntityId>(16)},
               ReparentEntityCommand{id<AuthoredEntityId>(17), id<AuthoredEntityId>(32)},
               SetTransformCommand{id<AuthoredEntityId>(17), {}}});
    const Checkpoint before_preview{session};
    auto preview = take(owner.preview(input, revision(owner)));
    REQUIRE(preview.outcome);
    CHECK(preview.outcome.value().changed);
    before_preview.unchanged(session);
    auto agent = changed(owner, input);
    auto retained_child = find(owner, 17);
    expected.entities.push_back(
        {id<AuthoredEntityId>(32), id<AuthoredEntityId>(16), {"Agent marker"}, moved, "{}"});
    entity(expected, 17).parent = id<AuthoredEntityId>(32);
    entity(expected, 17).transform = {};
    CHECK(take(owner.capture()).scene == expected);
    CHECK(find(owner, 16) == survivor);
    CHECK(find(owner, 17) == prior_child.handle);
    CHECK(take(owner.history()).undo_entries == 2);
    CHECK(take(owner.history()).next_undo->command_count == 3);
    save(session);
    const auto agent_saved = expected;
    auto later = changed(owner, rename(32, "Human continuation"));
    entity(expected, 32).name.value = "Human continuation";
    replay(session, agent); // Original reply was lost before the later edit/save.
    auto undone = move(owner);
    CHECK(take(owner.capture()).scene == agent_saved);
    CHECK(take(owner.status()).dirty);
    auto redone = move(owner, true);
    CHECK(take(owner.capture()).scene == expected);
    replay(session, undone);
    replay(session, redone);
    auto invalid = submit(
        owner, batch({rename(32, "Must roll back"),
                      ReparentEntityCommand{id<AuthoredEntityId>(16), id<AuthoredEntityId>(17)}}));
    rejected(invalid.outcome->result, ErrorCode::conflict);
    CHECK(invalid.outcome->result.error().context.path == "/commands/1");
    CHECK(take(owner.capture()).scene == expected);
    save(session);
    auto local = changed(owner, rename(17, "Unsaved agent work"));
    auto external = expected;
    entity(external, 32).name.value = "External human text edit";
    write_bytes(session.fixture.scene_path(), take(encode_scene_document(external)));
    const Checkpoint conflict{session};
    rejected(session.files->save_scene(owner, revision(owner), session.file), ErrorCode::conflict);
    conflict.unchanged(session);
    rejected(session.files->reload_scene(owner, revision(owner)), ErrorCode::conflict);
    conflict.unchanged(session);
    const auto old_revision = revision(owner);
    const auto reload =
        take(session.files->reload_scene(owner, old_revision, UnsavedScenePolicy::discard));
    session.file = reload.file_revision;
    CHECK(reload.revision.scene_instance != old_revision.scene_instance);
    CHECK(take(owner.capture()).scene == external);
    CHECK_FALSE(take(owner.status()).dirty);
    CHECK(take(owner.history()).undo_entries == 0);
    CHECK(take(owner.history()).redo_entries == 0);
    CHECK(take(owner.mutation_status()).scope == scope);
    rejected(scene(owner).resolve(prior_child), ErrorCode::invalid_id);
    CHECK(find(owner, 17) != retained_child);
    for (const auto* original : {&human, &agent, &later, &undone, &redone, &invalid, &local})
        replay(session, *original);
    auto final = changed(owner, rename(17, "Reconciled agent intent"));
    entity(external, 17).name.value = "Reconciled agent intent";
    CHECK(take(owner.capture()).scene == external);
    save(session);
    replay(session, final);
    const auto bytes = read_bytes(session.fixture.scene_path());
    session.loaded.owner.reset();
    session.files.reset();
    auto reopened_files = take(ProjectFiles::open(session.fixture.root));
    auto reopened =
        take(reopened_files->load_scene(session.fixture.project, session.fixture.initial.scene));
    CHECK(take(reopened.owner->capture()).scene == external);
    CHECK(take(reopened.owner->history()).undo_entries == 0);
    CHECK(take(reopened.owner->mutation_status()).scope != scope);
    const auto before = take(reopened.owner->capture());
    auto old = final.request;
    old.expected_revision = before.revision;
    auto retry = take(reopened.owner->mutate(old));
    CHECK(retry.state == AuthoringMutationState::unknown_outcome);
    REQUIRE_FALSE(retry.outcome);
    CHECK(take(reopened.owner->capture()).scene == external);
    CHECK(revision(*reopened.owner) == before.revision);
    CHECK(take(reopened.owner->mutation_status()).high_watermark == 0);
    CHECK(read_bytes(session.fixture.scene_path()) == bytes);
}

TEST_CASE("authoring preview and rejected commit preserve redo until a changed group publishes",
          "[authoring-integration]") {
    Session session;
    auto& owner = session.owner();
    changed(owner, rename(17, "First"));
    changed(owner, rename(17, "Second"));
    auto undo = move(owner);
    auto noop = submit(owner, rename(17, "First"));
    REQUIRE(noop.outcome->result);
    CHECK_FALSE(noop.outcome->result.value().changed);
    const Checkpoint before{session};
    const auto stale = batch({rename(17, "Preview only")});
    REQUIRE(take(owner.preview(stale, revision(owner))).outcome);
    before.unchanged(session);
    auto invalid = submit(
        owner, batch({rename(17, "Rollback"), DeleteEntityCommand{id<AuthoredEntityId>(255)}}));
    rejected(invalid.outcome->result, ErrorCode::invalid_id);
    before.unchanged(session, false);
    CHECK(take(owner.history()).next_redo->ticket == before.history.next_redo->ticket);
    auto branch = changed(owner, rename(17, "New agent branch"));
    const auto branched = take(owner.capture());
    CHECK(take(owner.history()).redo_entries == 0);
    replay(session, undo);
    replay(session, invalid);
    replay(session, noop);
    CHECK(take(owner.capture()).scene == branched.scene);
    save(session);
    replay(session, branch);
}

TEST_CASE("authoring deletion history restores source metadata without reviving old handles",
          "[authoring-integration]") {
    Session session;
    auto& owner = session.owner();
    const auto parent = find(owner, 16);
    const auto child = find(owner, 17);
    auto deletion = changed(owner, AuthoringCommand{DeleteEntityCommand{id<AuthoredEntityId>(16)}});
    REQUIRE(take(owner.capture()).scene.entities.empty());
    save(session);
    auto undo = move(owner);
    CHECK(take(owner.capture()).scene == session.fixture.initial);
    CHECK(find(owner, 16) != parent);
    CHECK(find(owner, 17) != child);
    rejected(scene(owner).entity_state(parent), ErrorCode::invalid_id);
    rejected(scene(owner).entity_state(child), ErrorCode::invalid_id);
    replay(session, deletion);
    CHECK(take(owner.capture()).scene == session.fixture.initial);
    save(session);
    auto redo = move(owner, true);
    CHECK(take(owner.capture()).scene.entities.empty());
    replay(session, undo);
    replay(session, redo);
    save(session);
}

TEST_CASE("authoring native save failures preserve scene history retries and old file bytes",
          "[authoring-integration]") {
    for (auto fault :
         {detail::FileFault::read, detail::FileFault::stage_create, detail::FileFault::stage_write,
          detail::FileFault::stage_flush, detail::FileFault::publish}) {
        CAPTURE(static_cast<int>(fault));
        Session session;
        auto edit = changed(session.owner(), rename(17, "Retained unsaved edit"));
        const Checkpoint before{session};
        {
            Control control{{fault, nullptr, nullptr}};
            rejected(
                session.files->save_scene(session.owner(), revision(session.owner()), session.file),
                ErrorCode::io_failure);
        }
        before.unchanged(session);
        replay(session, edit);
        auto undo = move(session.owner());
        CHECK(take(session.owner().capture()).scene == session.fixture.initial);
        move(session.owner(), true);
        save(session);
        replay(session, undo);
        replay(session, edit);
        for (const auto& path : fs::directory_iterator(session.fixture.root / "scenes"))
            CHECK_FALSE(path.path().filename().u8string().starts_with(u8".shark-stage-"));
    }
}

TEST_CASE("authoring postpublication edits remain dirty and retain independently retryable groups",
          "[authoring-integration]") {
    Session session;
    auto& owner = session.owner();
    auto first = changed(owner, rename(17, "Saved snapshot"));
    const auto snapshot = take(owner.capture());
    struct Callback {
        AuthoredScene* owner;
        bool called{false};
        bool success{false};
        std::optional<Attempt> result{};
    } context{&owner};
    const auto hook = [](detail::FilePhase phase, void* pointer) noexcept {
        if (phase != detail::FilePhase::published)
            return;
        auto& value = *static_cast<Callback*>(pointer);
        value.called = true;
        try {
            auto status = value.owner->mutation_status();
            if (!status || !status.value().next)
                return;
            AuthoringMutationRequest request{*status.value().next, status.value().revision,
                                             rename(17, "Edit after publication")};
            auto reply = value.owner->mutate(request);
            if (!reply || !reply.value().outcome || !reply.value().outcome.value()->result)
                return;
            value.result = Attempt{std::move(request), reply.value().outcome.value()};
            value.success = true;
        } catch (...) {
            // The native fixture hook cannot let exceptions cross its noexcept boundary.
        }
    };
    auto saved = [&] {
        Control control{{detail::FileFault::none, hook, &context}};
        return take(session.files->save_scene(owner, snapshot.revision, session.file));
    }();
    REQUIRE(context.called);
    REQUIRE(context.success);
    REQUIRE(context.result);
    CHECK_FALSE(saved.current_scene_clean);
    CHECK(saved.saved_revision == snapshot.revision);
    CHECK(take(owner.status()).dirty);
    CHECK(take(session.files->read_scene(session.fixture.project, session.fixture.initial.scene))
              .source.document == snapshot.scene);
    session.file = saved.file.revision;
    replay(session, first);
    replay(session, *context.result);
    move(owner);
    CHECK(take(owner.capture()).scene == snapshot.scene);
    move(owner, true);
    CHECK(take(owner.history()).undo_entries == 2);
    save(session);
}

TEST_CASE("authoring failed reload and stale preview commit keep history and retained decisions",
          "[authoring-integration]") {
    Session session;
    auto& owner = session.owner();
    auto first = changed(owner, rename(17, "First"));
    const auto commands = batch({rename(17, "Stale agent preview")});
    const auto expected = revision(owner);
    REQUIRE(take(owner.preview(commands, expected)).outcome);
    auto human = changed(owner, rename(16, "Intervening human"));
    auto state = take(owner.mutation_status());
    REQUIRE(state.next);
    AuthoringMutationRequest stale{*state.next, expected, commands};
    const Checkpoint before{session};
    auto reply = take(owner.mutate(stale));
    REQUIRE(reply.outcome);
    rejected(reply.outcome.value()->result, ErrorCode::conflict);
    before.unchanged(session, false);
    const auto retained = reply.outcome.value();
    write_bytes(session.fixture.scene_path(),
                "{\"format\":\"shark.scene\",\"schema_version\":999}");
    const Checkpoint invalid_file{session};
    REQUIRE_FALSE(session.files->reload_scene(owner, revision(owner), UnsavedScenePolicy::discard));
    invalid_file.unchanged(session);
    CHECK(take(owner.mutate(stale)).outcome.value() == retained);
    write_bytes(session.fixture.scene_path(), before.disk);
    replay(session, first);
    replay(session, human);
    move(owner);
    move(owner, true);
    save(session);
}

TEST_CASE("authoring eviction never reexecutes expired edits or history moves after reload",
          "[authoring-integration]") {
    Session session{AuthoringMutationLimits{2, authoring_maximum_mutation_bytes}};
    auto& owner = session.owner();
    auto first = changed(owner, rename(17, "First"));
    auto undo = move(owner);
    auto redo = move(owner, true);
    auto history_clear = submit(owner, ClearAuthoringHistoryMutation{});
    REQUIRE(history_clear.outcome->result);
    REQUIRE(history_clear.outcome->history_after);
    CHECK(take(owner.history()).undo_entries == 0);
    CHECK(take(owner.mutation_status()).retained_entries == 2);
    save(session);
    const auto loaded = take(session.files->reload_scene(owner, revision(owner)));
    session.file = loaded.file_revision;
    const Checkpoint before{session};
    for (const auto* original : {&first, &undo}) {
        auto input = original->request;
        input.expected_revision = revision(owner);
        const auto reply = take(owner.mutate(input));
        CHECK(reply.state == AuthoringMutationState::unknown_outcome);
        REQUIRE_FALSE(reply.outcome);
        CHECK(take(owner.mutation_outcome(input.id)).state ==
              AuthoringMutationState::unknown_outcome);
        before.unchanged(session);
    }
    replay(session, redo);
    replay(session, history_clear);
    CHECK(take(owner.mutation_status()).high_watermark == 4);
    CHECK(first.outcome->result.value().changed);
    CHECK(undo.outcome->result.value().changed);
}

TEST_CASE("authoring publication durability limitations remain commits and preserve retry history",
          "[authoring-integration]") {
    Session session;
    auto edit = changed(session.owner(), rename(17, "Committed despite durability limit"));
    const auto expected = take(session.owner().capture());
    auto saved = [&] {
        Control control{{detail::FileFault::directory_flush, nullptr, nullptr}};
        return take(session.files->save_scene(session.owner(), expected.revision, session.file));
    }();
    CHECK(saved.file.durability == FileDurability::directory_flush_failed);
    REQUIRE(saved.current_scene_clean);
    session.file = saved.file.revision;
    CHECK(take(session.files->read_scene(session.fixture.project, session.fixture.initial.scene))
              .source.document == expected.scene);
    CHECK(take(session.owner().history()).undo_entries == 1);
    replay(session, edit);
    move(session.owner());
    CHECK(take(session.owner().capture()).scene == session.fixture.initial);
    CHECK(take(session.owner().status()).dirty);
    CHECK(read_bytes(session.fixture.scene_path()) == take(encode_scene_document(expected.scene)));
    save(session);
}
