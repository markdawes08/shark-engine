#include "shark/tools/feature_world.hpp"

#include "shark/schema.hpp"
#include "shark/tools/project_files.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace shark::tools {
namespace {
namespace fs = std::filesystem;
constexpr std::array<std::string_view, 5> case_ids{
    "featureworld.components", "featureworld.commands", "featureworld.batches-history",
    "featureworld.retries", "featureworld.persistence"};

// Short circuit only inside trusted native cases. Public failures remain Results
// with their original operation/entity/path context, including sticky assertions.
struct CaseError {
    Error error;
};
template <typename T> T take(Result<T> result) {
    if (!result)
        throw CaseError{result.error()};
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        throw CaseError{result.error()};
}
template <typename T> T id(char digit) {
    std::string text = "20000000-0000-4000-8000-000000000001";
    text.back() = digit;
    return T{take(StableId::parse(text))};
}
AuthoredEntityId root() {
    return id<AuthoredEntityId>('4');
}
AuthoredEntityId branch() {
    return id<AuthoredEntityId>('5');
}
AuthoredEntityId leaf() {
    return id<AuthoredEntityId>('6');
}
AuthoredEntityId other() {
    return id<AuthoredEntityId>('7');
}
AuthoredEntityId temporary() {
    return id<AuthoredEntityId>('8');
}
AuthoredEntityId child() {
    return id<AuthoredEntityId>('9');
}
Transform translated(double x) {
    Transform value;
    value.translation.x = x;
    return value;
}
ProjectDocument expected_project() {
    const auto scene = id<SceneId>('3');
    return {id<ProjectId>('1'),
            id<WorldId>('2'),
            Name{"FeatureWorld"},
            scene,
            {{scene, take(ProjectPath::parse("scenes/FeatureWorld.sharkscene"))}},
            "{\"fixture\":\"FeatureWorld\"}"};
}
SceneDocument expected_scene() {
    return {id<ProjectId>('1'),
            id<SceneId>('3'),
            WorldPosition{id<WorldId>('2'), {1234567890, -7, 42}, {0.125, 1023.875, 0.0}},
            {{root(), {}, Name{"Root"}, translated(10), "{\"role\":\"root\"}"},
             {branch(), root(), Name{"Branch"}, translated(2)},
             {leaf(), branch(), Name{"Waypoint 雪"}, translated(3), "{\"keep\":true}"},
             {other(), {}, Name{"Root"}, translated(-4)}},
            "{\"fixture\":\"10b\"}"};
}
void require(SceneRunContext& context, bool value, const char* message,
             std::optional<AuthoredEntityId> entity = {}) {
    take(context.require(value, message, entity));
}
SceneRevision revision(AuthoredScene& owner) {
    return take(owner.status()).revision;
}
SceneEntityState state(AuthoredScene& owner, AuthoredEntityId entity) {
    const auto borrowed_scene = take(owner.scene());
    const auto& scene = borrowed_scene.get();
    return take(scene.entity_state(take(scene.find(EntityIdentity{entity}))));
}
void unchanged(SceneRunContext& context, const CapturedScene& before) {
    const auto after = take(context.owner().capture());
    require(context,
            after.revision == before.revision && after.scene == before.scene &&
                after.project == before.project && after.dirty == before.dirty,
            "Rejected or observational work changed the committed source");
}
void apply(SceneRunContext& context, const AuthoringCommand& command) {
    auto& owner = context.owner();
    const auto before = revision(owner);
    auto reply = take(owner.execute(command, before));
    const auto effect = take(std::move(reply.outcome));
    require(context,
            effect.changed && reply.observed_revision.scene_instance == before.scene_instance &&
                reply.observed_revision.revision == before.revision + 1,
            "Changed command did not publish exactly one revision");
}
void baseline(SceneRunContext& context) {
    const auto capture = take(context.owner().capture());
    require(context,
            capture.project == expected_project() && capture.scene == expected_scene() &&
                !capture.dirty,
            "Saved FeatureWorld differs from its independently specified baseline");
    require(context, state(context.owner(), leaf()).world_matrix[3][0] == 15.0,
            "Saved hierarchy did not compose the leaf world transform", leaf());
}
void components(SceneRunContext& context) {
    baseline(context);
    auto& owner = context.owner();
    const auto schemas = component_schemas();
    require(context,
            schemas[0].stable_id == "shark.name" && schemas[1].stable_id == "shark.transform" &&
                schemas[0].version == 1 && schemas[1].version == 1 && schemas[0].required &&
                schemas[1].required && !schemas[0].removable && !schemas[1].removable,
            "Supported component schemas differ from saved version 1");
    require(context,
            std::get<Name>(take(validate_component(ComponentSchemaId::name, 1,
                                                   Name{"Waypoint 雪"}))) == Name{"Waypoint 雪"} &&
                !validate_component(ComponentSchemaId::transform, 2, Transform{}),
            "Shared component validation lost version/value constraints");
    SceneQueryOptions options;
    options.page_size = 1;
    options.scan_budget = 1;
    options.expected_revision = revision(owner);
    auto page = take(owner.query_entities(options));
    std::vector<AuthoredEntityId> seen;
    for (;;) {
        for (const auto& entity : page.entities()) {
            require(context,
                    entity.name.has_value() && entity.transform.has_value() &&
                        entity.hierarchy.has_value(),
                    "Component query omitted required owned values");
            const auto* authored = std::get_if<AuthoredEntityId>(&entity.reference.identity);
            require(context, authored != nullptr,
                    "Authored query returned a different identity domain");
            seen.push_back(*authored);
        }
        if (!page.next_cursor())
            break;
        page = take(owner.continue_query(*page.next_cursor()));
    }
    std::sort(seen.begin(), seen.end());
    require(context, seen == std::vector{root(), branch(), leaf(), other()},
            "Paged query lost or duplicated saved identities");
    const auto inspected = take(owner.inspect_entity(leaf(), {}, revision(owner)));
    require(context,
            inspected.entities().size() == 1 &&
                inspected.entities().front().name == Name{"Waypoint 雪"} &&
                inspected.entities().front().hierarchy->depth == 3,
            "Semantic inspection differs from the saved Name/hierarchy", leaf());
}
void commands(SceneRunContext& context) {
    auto& owner = context.owner();
    apply(context, CreateEntityCommand{temporary(), Name{"Temporary"}, {}, root()});
    apply(context, CreateEntityCommand{child(), Name{"Child"}, {}, temporary()});
    const auto stale_child = state(owner, child()).reference;
    apply(context, SetNameCommand{leaf(), Name{"Renamed 雪"}});
    apply(context, SetTransformCommand{branch(), translated(4)});
    apply(context, ReparentEntityCommand{leaf(), other(), ReparentMode::keep_world});
    require(context,
            state(owner, leaf()).local_transform == translated(21) &&
                state(owner, leaf()).world_matrix[3][0] == 17,
            "Keep-world reparent changed the world pose", leaf());
    apply(context, ReparentEntityCommand{leaf(), root(), ReparentMode::keep_local});
    require(context,
            state(owner, leaf()).local_transform == translated(21) &&
                state(owner, leaf()).world_matrix[3][0] == 31,
            "Keep-local reparent changed the local pose", leaf());
    const auto before = take(owner.capture());
    auto cycle = take(owner.execute(ReparentEntityCommand{root(), branch()}, before.revision));
    require(context, !cycle.outcome, "Cyclic hierarchy was admitted", root());
    auto invalid =
        take(owner.execute(SetNameCommand{leaf(), Name{"invalid\nname"}}, before.revision));
    require(context, !invalid.outcome, "Invalid Name was admitted", leaf());
    auto invalid_transform = translated(0);
    invalid_transform.scale.x = 0;
    require(context,
            !take(owner.execute(SetTransformCommand{leaf(), invalid_transform}, before.revision))
                 .outcome,
            "Zero-scale Transform was admitted", leaf());
    auto stale =
        take(owner.execute(SetNameCommand{leaf(), Name{"stale"}},
                           {before.revision.scene_instance, before.revision.revision - 1}));
    require(context, !stale.outcome && stale.outcome.error().code == ErrorCode::conflict,
            "Stale command did not report a revision conflict", leaf());
    unchanged(context, before);
    apply(context, DeleteEntityCommand{temporary()});
    require(context, !take(owner.scene()).get().resolve(stale_child),
            "Subtree deletion retained a deleted child handle", child());
    auto expected = expected_scene();
    expected.entities[1].transform = translated(4);
    expected.entities[2].name = Name{"Renamed 雪"};
    expected.entities[2].parent = root();
    expected.entities[2].transform = translated(21);
    require(context, take(owner.capture()).scene == expected,
            "Single commands changed unrelated saved values or metadata");
}
void batches_history(SceneRunContext& context) {
    auto& owner = context.owner();
    const auto before = take(owner.capture());
    const auto survivor = state(owner, leaf()).reference;
    AuthoringBatch batch{
        id<AuthoringBatchId>('8'),
        {SetNameCommand{leaf(), Name{"Batch leaf"}}, SetTransformCommand{branch(), translated(5)}}};
    auto preview = take(owner.preview(batch, before.revision));
    require(context, take(std::move(preview.outcome)).changed,
            "Valid batch preview did not predict a change");
    unchanged(context, before);
    require(context, take(owner.history()).undo_entries == 0, "Preview created undo history");
    auto invalid = batch;
    invalid.commands.push_back(ReparentEntityCommand{root(), leaf()});
    require(context, !take(owner.execute(invalid, before.revision)).outcome,
            "Late invalid batch command was admitted");
    unchanged(context, before);
    auto committed = take(owner.execute(batch, before.revision));
    require(context,
            take(std::move(committed.outcome)).changed &&
                committed.observed_revision.revision == before.revision.revision + 1,
            "Atomic batch did not publish once");
    auto expected = expected_scene();
    expected.entities[1].transform = translated(5);
    expected.entities[2].name = Name{"Batch leaf"};
    require(context, take(owner.capture()).scene == expected, "Committed batch state is incorrect");
    auto history = take(owner.history(revision(owner)));
    require(context,
            history.undo_entries == 1 && history.redo_entries == 0 && history.next_undo &&
                history.next_undo->command_count == 2,
            "Batch was not retained as one history group");
    auto undo = take(owner.undo(history.next_undo->ticket, revision(owner)));
    require(context,
            take(std::move(undo.outcome)).changed && take(owner.capture()).scene == before.scene,
            "Undo did not restore saved component values/metadata");
    history = take(owner.history());
    require(context, history.next_redo.has_value(), "Undo did not expose a redo ticket");
    auto redo = take(owner.redo(history.next_redo->ticket, history.revision));
    require(context,
            take(std::move(redo.outcome)).changed && take(owner.capture()).scene == expected &&
                take(take(owner.scene()).get().resolve(survivor)) == survivor.handle,
            "Redo changed survivor handles or failed to restore the group");
    const auto final = take(owner.capture());
    history = take(owner.clear_history(final.revision));
    require(context, history.undo_entries == 0 && history.redo_entries == 0 && history.synchronized,
            "Explicit history clearing left retained groups");
    unchanged(context, final);
}
void retries(SceneRunContext& context) {
    auto& owner = context.owner();
    const auto status = take(owner.mutation_status());
    require(context, status.high_watermark == 0 && status.next.has_value(),
            "Fresh case did not receive a fresh retry scope");
    AuthoringMutationRequest request{
        *status.next, status.revision,
        AuthoringCommand{SetNameCommand{leaf(), Name{"Retained leaf"}}}};
    const auto reply = take(owner.mutate(request));
    const auto outcome = take(reply.outcome);
    require(context,
            reply.state == AuthoringMutationState::recorded && take(outcome->result).changed,
            "Retained mutation did not record its original success");
    apply(context, SetTransformCommand{leaf(), translated(7)});
    const auto before = take(owner.capture());
    const auto lookup = take(owner.mutation_outcome(request.id));
    require(context,
            lookup.state == AuthoringMutationState::retained && take(lookup.outcome) == outcome &&
                take(take(lookup.outcome)->result).changed,
            "Retry query lost the original operation result");
    const auto replay = take(owner.mutate(request));
    require(context,
            replay.state == AuthoringMutationState::replayed && take(replay.outcome) == outcome &&
                take(take(replay.outcome)->result).changed,
            "Exact retry did not replay the immutable original result");
    auto mismatch = request;
    mismatch.mutation = AuthoringCommand{SetNameCommand{leaf(), Name{"different"}}};
    const auto rejected = take(owner.mutate(mismatch));
    require(context,
            rejected.state == AuthoringMutationState::payload_mismatch && !rejected.outcome,
            "Retry identity accepted a different payload");
    unchanged(context, before);
    const auto after = take(owner.mutation_status(before.revision));
    require(context,
            after.scope == status.scope && after.high_watermark == 1 &&
                after.retained_entries == 1 && after.next && after.next->sequence == 2,
            "Replay changed the sequence watermark or retained count");
    auto expected = expected_scene();
    expected.entities[2].name = Name{"Retained leaf"};
    expected.entities[2].transform = translated(7);
    require(context, before.scene == expected, "Retry overwrote a later component edit");
}

// Only remove explicitly owned names. Failed/unrecognized remnants remain for
// inspection; no recursive deletion of a caller-selected project is possible.
struct Scratch {
    fs::path path;
    ~Scratch() {
        std::error_code ignored;
        for (const auto& name : {"scenes/FeatureWorld.sharkscene", "FeatureWorld.sharkproject",
                                 ".shark-write.lock", "scenes"})
            fs::remove(path / name, ignored);
        fs::remove(path, ignored);
    }
};
void persistence(SceneRunContext& context, const fs::path& parent) {
    auto& owner = context.owner();
    std::error_code error;
    const auto canonical = fs::canonical(parent, error);
    require(context, !parent.empty() && !error && fs::is_directory(canonical, error) && !error,
            "FeatureWorld persistence needs an existing scratch parent");
    const auto path = canonical / ("shark-featureworld-" + take(generate_stable_id()).to_string());
    require(context, fs::create_directory(path, error) && !error,
            "Cannot create an isolated FeatureWorld scratch project");
    Scratch scratch{path};
    require(context, fs::create_directory(path / "scenes", error) && !error,
            "Cannot create the scratch scene directory");
    auto files = take(ProjectFiles::open(path));
    const auto project_path = take(ProjectPath::parse("FeatureWorld.sharkproject"));
    const auto before = take(owner.capture());
    const auto project_commit = take(files->write_project(project_path, before.project, {}));
    require(context, project_commit.revision.valid(), "Project publication has no file revision");
    const auto first = take(files->save_scene(owner, before.revision, {}));
    require(context, first.current_scene_clean && !take(owner.status()).dirty,
            "Initial source save did not acknowledge the captured baseline");
    const auto stale_handle = state(owner, leaf()).reference;
    apply(context, SetNameCommand{leaf(), Name{"Saved leaf"}});
    const auto edited = take(owner.capture());
    const auto saved = take(files->save_scene(owner, edited.revision, first.file.revision));
    require(context,
            saved.current_scene_clean && !take(owner.status()).dirty &&
                take(owner.history()).undo_entries == 1,
            "Saving cleared shared history or left the current snapshot dirty");
    const auto project = take(files->read_project(project_path));
    const auto scene = take(files->read_scene(project.source.document, before.scene.scene));
    require(context,
            project.source.document == expected_project() &&
                scene.source.document == edited.scene && scene.revision == saved.file.revision &&
                scene.source.source_schema_version == source_document_version,
            "Native saved files lost source identity, metadata or exact-content revision");
    const auto encoded = take(encode_scene_document(scene.source.document));
    require(context, take(decode_scene_document(encoded)).document == edited.scene,
            "Canonical codec round trip changed the saved source");
    auto expected = expected_scene();
    expected.entities[2].name = Name{"Saved leaf"};
    require(context, scene.source.document == expected, "Saved scene differs from expected values");
    files.reset();
    files = take(ProjectFiles::open(path));
    const auto reloaded = take(files->reload_scene(owner, revision(owner)));
    require(context,
            reloaded.file_revision == saved.file.revision &&
                reloaded.revision.scene_instance != edited.revision.scene_instance &&
                !take(owner.scene()).get().resolve(stale_handle),
            "Reload did not replace the saved scene incarnation");
    require(context,
            take(owner.capture()).scene == expected && !take(owner.status()).dirty &&
                take(owner.history()).undo_entries == 0,
            "Reload failed to restore saved values and clear history");
}
} // namespace

std::span<const std::string_view> feature_world_case_ids() noexcept {
    return case_ids;
}

std::vector<SceneRunCase> feature_world_cases(const fs::path& scratch_parent) {
    std::vector<SceneRunCase> cases;
    for (std::size_t index = 0; index < case_ids.size(); ++index) {
        SceneRunCase test{.id = std::string{case_ids[index]}};
        test.setup = [](SceneRunContext& context) -> Result<void> {
            try {
                baseline(context);
                return {};
            } catch (const CaseError& failure) {
                return failure.error;
            }
        };
        test.tick = [index, scratch_parent](SceneRunContext& context,
                                            const SceneRunTick& tick) -> Result<void> {
            try {
                if (tick.index == 0) {
                    switch (index) {
                    case 0:
                        components(context);
                        break;
                    case 1:
                        commands(context);
                        break;
                    case 2:
                        batches_history(context);
                        break;
                    case 3:
                        retries(context);
                        break;
                    case 4:
                        persistence(context, scratch_parent);
                        break;
                    default:
                        break;
                    }
                }
                const auto capture = take(context.owner().capture());
                require(context,
                        capture.scene.project == id<ProjectId>('1') &&
                            capture.scene.scene == id<SceneId>('3') &&
                            capture.scene.entities.size() == 4,
                        "Tick lost stable FeatureWorld identity or entity count");
                return {};
            } catch (const CaseError& failure) {
                return failure.error;
            }
        };
        cases.push_back(std::move(test));
    }
    return cases;
}

} // namespace shark::tools
