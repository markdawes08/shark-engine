#include "shark/tools/feature_world.hpp"

#include "shark/schema.hpp"
#include "shark/tools/asset_catalog.hpp"
#include "shark/tools/asset_import.hpp"
#include "shark/tools/gltf_asset.hpp"
#include "shark/tools/gltf_geometry.hpp"
#include "shark/tools/gltf_source.hpp"
#include "shark/tools/gltf_textures.hpp"
#include "shark/tools/project_files.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <utility>

namespace shark::tools {
namespace {
namespace fs = std::filesystem;
constexpr std::array<std::string_view, 13> case_ids{
    "featureworld.components",   "featureworld.commands",      "featureworld.batches-history",
    "featureworld.retries",      "featureworld.persistence",   "featureworld.asset-identity",
    "featureworld.asset-import", "featureworld.asset-jobs",    "featureworld.asset-pipeline",
    "featureworld.gltf-source",  "featureworld.gltf-geometry", "featureworld.gltf-textures",
    "featureworld.gltf-assets"};

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
            "{\"asset_reference\":\"20000000-0000-4000-8000-00000000000a\",\"fixture\":\"10b\"}"};
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
        for (const auto& name :
             {"scenes/FeatureWorld.sharkscene", "FeatureWorld.sharkproject", "one.txt",
              "one.txt.sharkasset", "moved/renamed.txt", "moved/renamed.txt.sharkasset", "moved",
              "dependency.txt", "dependency.txt.sharkasset", ".shark-development-cache",
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
void asset_identity(SceneRunContext& context, const fs::path& parent) {
    const auto before = take(context.owner().capture());
    std::error_code error;
    const auto canonical = fs::canonical(parent, error);
    require(context, !parent.empty() && !error && fs::is_directory(canonical, error) && !error,
            "Asset identity needs an existing scratch parent");
    const auto path = canonical / ("shark-featureworld-" + take(generate_stable_id()).to_string());
    require(context, fs::create_directory(path, error) && !error,
            "Cannot create an isolated asset identity project");
    Scratch scratch{path};
    require(context, fs::create_directory(path / "scenes", error) && !error,
            "Cannot create asset scene directory");
    require(context, fs::create_directory(path / "moved", error) && !error,
            "Cannot create asset destination directory");
    auto files = take(ProjectFiles::open(path));
    take(files->write_project(take(ProjectPath::parse("FeatureWorld.sharkproject")), before.project,
                              {}));
    take(files->write_scene(before.project, before.scene, {}));
    {
        std::ofstream source{path / "one.txt", std::ios::binary};
        source << "Shark authored identity fixture\n";
        source.close();
        require(context, source.good(), "Cannot create asset source fixture");
    }
    const auto reference = id<AssetId>('a');
    const AssetMetadata metadata{before.project.project,
                                 reference,
                                 "fixture",
                                 "Shark authored offline identity fixture",
                                 "MIT",
                                 "shark.fixture"};
    const auto sidecar = take(ProjectPath::parse("one.txt.sharkasset"));
    const auto written = take(files->write_asset_metadata(sidecar, metadata, {}));
    const auto read = take(files->read_asset_metadata(sidecar));
    require(context, read.source.document == metadata && read.revision == written.revision,
            "Persistent metadata changed identity, provenance or exact revision");
    auto catalog = take(AssetCatalog::create(*files, before.project.project));
    const std::array initial{sidecar};
    const auto first = take(catalog->refresh(initial, take(catalog->revision())));
    const auto page = take(catalog->query(first, 0, 1));
    require(context,
            page.total == 1 && page.records.size() == 1 && !page.next_offset &&
                page.records[0].metadata.asset == reference,
            "Asset query lost the saved reference");
    const auto resolved = take(catalog->resolve(reference, first));
    require(context, resolved.availability == AssetAvailability::source_present && resolved.record,
            "Asset reference did not resolve to its source");
    fs::rename(path / "one.txt", path / "moved/renamed.txt", error);
    require(context, !error, "Cannot move the asset fixture");
    fs::rename(path / "one.txt.sharkasset", path / "moved/renamed.txt.sharkasset", error);
    require(context, !error, "Cannot move the asset sidecar fixture");
    const std::array moved{take(ProjectPath::parse("moved/renamed.txt.sharkasset"))};
    const auto second = take(catalog->refresh(moved, first));
    const auto after = take(catalog->resolve(reference, second));
    require(context,
            second.value == first.value + 1 && after.record && after.record->metadata == metadata &&
                after.record->source.text() == "moved/renamed.txt" &&
                after.availability == AssetAvailability::source_present && !catalog->query(first),
            "Move changed identity or allowed stale catalog queries");
    require(context, fs::remove(path / "moved/renamed.txt", error) && !error,
            "Cannot remove fixture source for missing-asset control");
    const auto missing = take(catalog->refresh(moved, second));
    require(context,
            take(catalog->resolve(reference, missing)).availability ==
                    AssetAvailability::source_missing &&
                take(catalog->resolve(id<AssetId>('b'), missing)).availability ==
                    AssetAvailability::unknown_id,
            "Missing source and unknown asset identity were conflated");
    catalog.reset();
    files.reset();
    files = take(ProjectFiles::open(path));
    catalog = take(AssetCatalog::create(*files, before.project.project));
    const auto reopened = take(catalog->refresh(moved, take(catalog->revision())));
    const auto saved = take(files->read_scene(before.project, before.scene.scene));
    require(context,
            saved.source.document == before.scene &&
                take(catalog->resolve(reference, reopened)).record->metadata == metadata,
            "Saved scene reference or persistent metadata changed across reopen");
    unchanged(context, before);
}
void asset_import(SceneRunContext& context, const fs::path& parent) {
    const auto before = take(context.owner().capture());
    std::error_code error;
    const auto canonical = fs::canonical(parent, error);
    require(context, !parent.empty() && !error && fs::is_directory(canonical, error) && !error,
            "Asset import needs an existing scratch parent");
    const auto path = canonical / ("shark-featureworld-" + take(generate_stable_id()).to_string());
    require(context, fs::create_directory(path, error) && !error,
            "Cannot create asset import scratch");
    Scratch scratch{path};
    auto files = take(ProjectFiles::open(path));
    auto write_source = [&](const char* name, const char* value) {
        std::ofstream stream{path / name, std::ios::binary};
        stream << value;
        stream.close();
        require(context, stream.good(), "Cannot write offline asset fixture");
    };
    write_source("one.txt", "Root bytes");
    write_source("dependency.txt", "Leaf bytes");
    AssetMetadata root{before.project.project,
                       id<AssetId>('a'),
                       "bytes",
                       "Shark authored offline byte fixture",
                       "MIT",
                       "shark.bytes"};
    root.dependencies = {id<AssetId>('b')};
    const AssetMetadata leaf{before.project.project,
                             id<AssetId>('b'),
                             "bytes",
                             "Shark authored offline byte fixture",
                             "MIT",
                             "shark.bytes"};
    const std::array inventory{take(ProjectPath::parse("one.txt.sharkasset")),
                               take(ProjectPath::parse("dependency.txt.sharkasset"))};
    take(files->write_asset_metadata(inventory[0], root, {}));
    take(files->write_asset_metadata(inventory[1], leaf, {}));
    auto catalog = take(AssetCatalog::create(*files, before.project.project));
    auto cr = take(catalog->refresh(inventory, take(catalog->revision())));
    ByteAssetImporter importer;
    const std::array<AssetImporter*, 1> registry{&importer};
    auto imports = take(AssetImports::open(*files, *catalog, before.project.project, registry));
    const AssetTargetProfile target{"featureworld-x64", AssetAudience::client};
    require(context, take(imports->importers()).front().id == "shark.bytes",
            "Importer discovery did not reflect a real implementation");
    auto revision = take(imports->revision());
    require(context,
            take(imports->query(root.asset, target, cr, revision)).state ==
                AssetCacheState::missing,
            "Fresh scratch unexpectedly has a cached payload");
    auto prepared = take(imports->prepare(root.asset, target, cr, revision));
    const auto first = take(prepared.summary());
    require(context,
            first.nodes == 2 && first.imported == 2 && first.reused == 0 &&
                !fs::exists(path / std::string{asset_cache_filename}),
            "Import preview published output or lost the dependency closure");
    const auto commit = take(imports->publish(std::move(prepared), cr, revision));
    revision = commit.revision;
    require(context, commit.changed && commit.file.has_value(),
            "Native cache publication did not commit");
    const auto artifact = take(imports->read(first.root_key, revision));
    require(context,
            artifact.asset == root.asset && artifact.kind == "bytes" &&
                artifact.payload_version == 1 && artifact.dependencies.size() == 1 &&
                artifact.dependencies[0].asset == leaf.asset && artifact.payload.size() == 46,
            "Imported reference/payload metadata changed");
    require(context,
            take(imports->query(root.asset, target, cr, revision)).state == AssetCacheState::fresh,
            "Published import is not current");
    prepared = take(imports->prepare(root.asset, target, cr, revision));
    require(context, take(prepared.summary()).reused == 2 && take(prepared.summary()).imported == 0,
            "Matching work did not deduplicate");
    require(context, !take(imports->publish(std::move(prepared), cr, revision)).changed,
            "Matching import republished the cache");
    write_source("dependency.txt", "Changed leaf bytes");
    require(context,
            take(imports->query(root.asset, target, cr, revision)).state == AssetCacheState::stale,
            "Changed dependency did not invalidate its parent");
    prepared = take(imports->prepare(root.asset, target, cr, revision));
    const auto changed = take(prepared.summary());
    require(context, changed.imported == 2 && changed.root_key != first.root_key,
            "Dependency change failed to create a new root key");
    revision = take(imports->publish(std::move(prepared), cr, revision)).revision;
    require(context, take(imports->read(first.root_key, revision)) == artifact,
            "New import overwrote last-valid payload bytes");
    revision = take(imports->prune(revision)).revision;
    require(context, !imports->read(first.root_key, revision),
            "Prune retained an unreachable old generation");
    imports.reset();
    catalog.reset();
    files.reset();
    files = take(ProjectFiles::open(path));
    catalog = take(AssetCatalog::create(*files, before.project.project));
    cr = take(catalog->refresh(inventory, take(catalog->revision())));
    imports = take(AssetImports::open(*files, *catalog, before.project.project, registry));
    revision = take(imports->revision());
    require(context,
            take(imports->query(root.asset, target, cr, revision)).last_valid_key ==
                    changed.root_key &&
                take(imports->usage(revision)).artifacts == 2,
            "Cache reopen lost dependency/key identity");
    revision = take(imports->reload(revision));
    require(context, take(AssetImports::recover(*files)) == 0,
            "Clean cache unexpectedly required interrupted-stage recovery");
    revision = take(imports->clear(revision)).revision;
    require(context,
            take(imports->usage(revision)).artifacts == 0 &&
                take(files->read_asset_metadata(inventory[0])).source.document == root,
            "Explicit cache clear changed source metadata");
    unchanged(context, before);
}
void asset_jobs(SceneRunContext& context, const fs::path& parent) {
    const auto before = take(context.owner().capture());
    std::error_code error;
    const auto canonical = fs::canonical(parent, error);
    require(context, !parent.empty() && !error && fs::is_directory(canonical, error) && !error,
            "Asset jobs need an existing scratch parent");
    const auto path = canonical / ("shark-featureworld-" + take(generate_stable_id()).to_string());
    require(context, fs::create_directory(path, error) && !error,
            "Cannot create asset jobs scratch");
    Scratch scratch{path};
    auto files = take(ProjectFiles::open(path));
    auto write_source = [&](const char* name, const char* value) {
        std::ofstream stream{path / name, std::ios::binary};
        stream << value;
        stream.close();
        require(context, stream.good(), "Cannot write offline background import fixture");
    };
    write_source("one.txt", "Root bytes");
    write_source("dependency.txt", "Leaf bytes");
    AssetMetadata root{before.project.project,
                       id<AssetId>('a'),
                       "bytes",
                       "Shark authored offline background import fixture",
                       "MIT",
                       "shark.bytes"};
    root.dependencies = {id<AssetId>('b')};
    const AssetMetadata leaf{before.project.project,
                             id<AssetId>('b'),
                             "bytes",
                             "Shark authored offline background import fixture",
                             "MIT",
                             "shark.bytes"};
    const std::array inventory{take(ProjectPath::parse("one.txt.sharkasset")),
                               take(ProjectPath::parse("dependency.txt.sharkasset"))};
    take(files->write_asset_metadata(inventory[0], root, {}));
    take(files->write_asset_metadata(inventory[1], leaf, {}));
    auto catalog = take(AssetCatalog::create(*files, before.project.project));
    const auto cr = take(catalog->refresh(inventory, take(catalog->revision())));
    ByteAssetImporter importer;
    const std::array<AssetImporter*, 1> registry{&importer};
    auto imports = take(AssetImports::open(*files, *catalog, before.project.project, registry));
    const AssetTargetProfile target{"featureworld-x64", AssetAudience::client};
    auto revision = take(imports->revision());
    const auto first = take(imports->submit(root.asset, target, cr, revision));
    const auto duplicate = take(imports->submit(root.asset, target, cr, revision));
    require(context, !first.deduplicated && duplicate.deduplicated && first.id == duplicate.id,
            "Matching pending background work did not share one job");
    auto terminal = [](AssetImportJobState value) {
        return value == AssetImportJobState::succeeded || value == AssetImportJobState::failed ||
               value == AssetImportJobState::cancelled;
    };
    auto finish = [&](AssetImportJobId job) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        for (;;) {
            take(imports->drain_jobs());
            const auto statuses = take(imports->jobs());
            const auto found = std::find_if(statuses.begin(), statuses.end(),
                                            [&](const auto& item) { return item.id == job; });
            if (found != statuses.end() && terminal(found->state))
                return *found;
            if (std::chrono::steady_clock::now() >= deadline)
                throw CaseError{Error{ErrorCode::execution_failed,
                                      "Background import did not reach a terminal outcome",
                                      {.operation = "featureworld.asset-jobs"}}};
            std::this_thread::yield();
        }
    };
    const auto completed = finish(first.id);
    require(context,
            completed.state == AssetImportJobState::succeeded && completed.summary &&
                completed.commit && !completed.error && completed.completed_nodes == 2 &&
                completed.total_nodes == 2 && completed.summary->imported == 2 &&
                completed.summary->root == root.asset && completed.commit->changed &&
                completed.commit->file,
            "Background import lost progress, closure, output or committed outcome");
    revision = completed.commit->revision;
    const auto cancelled_after = take(imports->cancel_job(first.id));
    require(context,
            cancelled_after.state == AssetImportJobState::succeeded && cancelled_after.commit &&
                cancelled_after.commit->revision == revision,
            "Cancellation after publication misreported a committed import");
    require(context,
            take(imports->take_job(first.id)).state == AssetImportJobState::succeeded &&
                take(imports->jobs()).empty(),
            "Taking the terminal import did not release retained work");
    require(context,
            take(imports->query(root.asset, target, cr, revision)).state == AssetCacheState::fresh,
            "Background publication did not produce a fresh root binding");
    const auto cancelled = take(imports->submit(root.asset, target, cr, revision));
    take(imports->cancel_job(cancelled.id));
    const auto stopped = finish(cancelled.id);
    require(context,
            stopped.state == AssetImportJobState::cancelled && !stopped.commit &&
                take(imports->revision()) == revision,
            "Cancellation before owner publication replaced cache output");
    require(context, take(imports->take_job(cancelled.id)).state == AssetImportJobState::cancelled,
            "Cancelled job lost its retained terminal outcome");
    const auto retried = take(imports->submit(root.asset, target, cr, revision));
    const auto retry = finish(retried.id);
    require(context,
            retried.id != cancelled.id && retry.state == AssetImportJobState::succeeded &&
                retry.summary && retry.summary->reused == 2 && retry.summary->imported == 0 &&
                retry.commit && !retry.commit->changed && retry.commit->revision == revision,
            "Explicit background retry lost identity, matching reuse or no-op publication");
    take(imports->take_job(retried.id));
    take(imports->close_jobs());
    require(context,
            take(imports->jobs()).empty() && !imports->submit(root.asset, target, cr, revision),
            "Closed background jobs admitted new work or leaked retained outcomes");
    require(context,
            take(files->read_asset_metadata(inventory[0])).source.document == root &&
                take(files->read_asset_metadata(inventory[1])).source.document == leaf,
            "Background import modified authored metadata");
    unchanged(context, before);
}

// A deterministic native fixture around the real byte importer. Its one gate
// proves an owner edit happens during computation, without scheduler-dependent
// sleeps or an additional worker. Ordinary importers receive no owner reference.
class PipelineImporter final : public AssetImporter {
  public:
    const AssetImporterInfo& info() const noexcept override {
        return bytes_.info();
    }
    Result<std::string> normalize_settings(std::string_view json) const override {
        return bytes_.normalize_settings(json);
    }
    Result<void> validate_payload(const AssetArtifact& artifact) const override {
        return bytes_.validate_payload(artifact);
    }
    Result<std::vector<std::byte>> import(const AssetImportInput& input) override {
        {
            std::unique_lock lock{mutex_};
            if (armed_ && selected_ == input.asset) {
                armed_ = false;
                entered_ = true;
                entered_wake_.notify_all();
                if (!release_wake_.wait_until(lock, input.stop,
                                              std::chrono::steady_clock::now() +
                                                  std::chrono::seconds{10},
                                              [&] { return released_; }) &&
                    !input.stop.stop_requested())
                    return Error{ErrorCode::execution_failed,
                                 "Asset pipeline fixture gate timed out",
                                 {.operation = "featureworld.asset-pipeline"}};
            }
        }
        return bytes_.import(input);
    }
    void arm(AssetId selected) {
        const std::lock_guard lock{mutex_};
        selected_ = selected;
        armed_ = true;
        entered_ = false;
        released_ = false;
    }
    bool entered() {
        std::unique_lock lock{mutex_};
        return entered_wake_.wait_for(lock, std::chrono::seconds{10}, [&] { return entered_; });
    }
    void release() noexcept {
        {
            const std::lock_guard lock{mutex_};
            released_ = true;
        }
        release_wake_.notify_all();
    }

  private:
    ByteAssetImporter bytes_;
    std::mutex mutex_;
    std::condition_variable entered_wake_;
    std::condition_variable_any release_wake_;
    std::optional<AssetId> selected_;
    bool armed_{false};
    bool entered_{false};
    bool released_{false};
};

void asset_pipeline(SceneRunContext& context, const fs::path& parent) {
    const auto before = take(context.owner().capture());
    // The baseline has independently specified, canonical fixture extras. Read
    // its actual saved reference, rather than inventing a second fixture ID.
    const std::string_view marker{"\"asset_reference\":\""};
    const auto offset = before.scene.extras_json.find(marker);
    require(context, offset != std::string::npos, "Saved scene lost its asset reference");
    const auto first_character = offset + marker.size();
    const auto end = before.scene.extras_json.find('"', first_character);
    require(context, end != std::string::npos, "Saved asset reference is incomplete");
    const AssetId root_asset{take(StableId::parse(std::string_view{before.scene.extras_json}.substr(
        first_character, end - first_character)))};
    const auto leaf_asset = id<AssetId>('b');
    require(context, root_asset == id<AssetId>('a'),
            "Saved asset reference differs from the oracle");
    std::error_code error;
    const auto canonical = fs::canonical(parent, error);
    require(context, !parent.empty() && !error && fs::is_directory(canonical, error) && !error,
            "Asset pipeline needs an existing scratch parent");
    const auto path = canonical / ("shark-featureworld-" + take(generate_stable_id()).to_string());
    require(context, fs::create_directory(path, error) && !error,
            "Cannot create asset pipeline scratch");
    Scratch scratch{path};
    require(context,
            fs::create_directory(path / "scenes", error) && !error &&
                fs::create_directory(path / "moved", error) && !error,
            "Cannot create asset pipeline directories");
    auto write_source = [&](const char* name, std::string_view value) {
        std::ofstream stream{path / name, std::ios::binary | std::ios::trunc};
        stream.write(value.data(), static_cast<std::streamsize>(value.size()));
        stream.close();
        require(context, stream.good(), "Cannot write asset pipeline source");
    };
    auto read_bytes = [&](const char* name) {
        std::ifstream stream{path / name, std::ios::binary};
        require(context, stream.good(), "Cannot read asset pipeline evidence");
        return std::string{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
    };
    auto body = [&](const AssetArtifact& artifact) {
        require(context,
                artifact.payload.size() >= 36 && artifact.payload_version == 1 &&
                    artifact.importer_version == 1 && artifact.importer == "shark.bytes",
                "Pipeline artifact lost its byte payload version");
        return std::string{reinterpret_cast<const char*>(artifact.payload.data() + 36),
                           artifact.payload.size() - 36};
    };
    auto files = take(ProjectFiles::open(path));
    const auto project_path = take(ProjectPath::parse("FeatureWorld.sharkproject"));
    take(files->write_project(project_path, before.project, {}));
    take(files->write_scene(before.project, before.scene, {}));
    const auto saved_scene_bytes = read_bytes("scenes/FeatureWorld.sharkscene");
    write_source("one.txt", "Root pipeline");
    write_source("dependency.txt", "Initial leaf");
    AssetMetadata root_metadata{before.project.project,
                                root_asset,
                                "bytes",
                                "Shark authored connected pipeline fixture",
                                "MIT",
                                "shark.bytes"};
    root_metadata.dependencies = {leaf_asset};
    AssetMetadata leaf_metadata{before.project.project,
                                leaf_asset,
                                "bytes",
                                "Shark authored connected pipeline fixture",
                                "MIT",
                                "shark.bytes"};
    std::array inventory{take(ProjectPath::parse("one.txt.sharkasset")),
                         take(ProjectPath::parse("dependency.txt.sharkasset"))};
    take(files->write_asset_metadata(inventory[0], root_metadata, {}));
    take(files->write_asset_metadata(inventory[1], leaf_metadata, {}));
    auto catalog = take(AssetCatalog::create(*files, before.project.project));
    auto catalog_revision = take(catalog->refresh(inventory, take(catalog->revision())));
    PipelineImporter importer;
    const std::array<AssetImporter*, 1> registry{&importer};
    auto imports = take(AssetImports::open(*files, *catalog, before.project.project, registry));
    // Release before Imports joins during exception unwinding; importer itself
    // outlives every service borrowing it, including the explicit reopen below.
    struct ReleaseGate {
        PipelineImporter& importer;
        ~ReleaseGate() {
            importer.release();
        }
    } release_gate{importer};
    const AssetTargetProfile target{"featureworld-x64", AssetAudience::client};
    auto revision = take(imports->revision());
    const auto first_page = take(catalog->query(catalog_revision, 0, 1));
    const auto discovered = take(imports->importers());
    require(context,
            first_page.total == 2 && first_page.records.size() == 1 && first_page.next_offset &&
                first_page.records[0].metadata.asset == root_asset && discovered.size() == 1 &&
                discovered[0].id == "shark.bytes" && discovered[0].kind == "bytes" &&
                discovered[0].version == 1 && discovered[0].payload_version == 1,
            "Pipeline discovery lost its bounded catalog, importer kind or versions");
    auto submit = [&] {
        return take(imports->submit(root_asset, target, catalog_revision, revision));
    };
    auto terminal = [](AssetImportJobState state) {
        return state == AssetImportJobState::succeeded || state == AssetImportJobState::failed ||
               state == AssetImportJobState::cancelled;
    };
    auto wait = [&](AssetImportJobId id, bool drain) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
        for (;;) {
            if (drain)
                take(imports->drain_jobs());
            const auto statuses = take(imports->jobs());
            const auto found = std::find_if(statuses.begin(), statuses.end(),
                                            [&](const auto& status) { return status.id == id; });
            if (found != statuses.end() &&
                (drain ? terminal(found->state) : found->state == AssetImportJobState::ready))
                return *found;
            if (found == statuses.end() || std::chrono::steady_clock::now() >= deadline ||
                (!drain && terminal(found->state)))
                throw CaseError{Error{ErrorCode::execution_failed,
                                      "Asset pipeline job did not reach its expected phase",
                                      {.operation = "featureworld.asset-pipeline"}}};
            std::this_thread::yield();
        }
    };
    const auto initial = submit();
    require(context,
            take(imports->submit(root_asset, target, catalog_revision, revision)).id == initial.id,
            "Connected pipeline lost pending-work deduplication");
    const auto first = wait(initial.id, true);
    require(context,
            first.state == AssetImportJobState::succeeded && first.summary && first.commit &&
                first.summary->nodes == 2 && first.summary->imported == 2 &&
                first.completed_nodes == 2,
            "Initial connected job failed to import the complete closure");
    revision = first.commit->revision;
    const auto original_artifact = take(imports->read(first.summary->root_key, revision));
    require(context,
            original_artifact.asset == root_asset && body(original_artifact) == "Root pipeline" &&
                original_artifact.dependencies.size() == 1 &&
                original_artifact.dependencies[0].asset == leaf_asset &&
                body(take(imports->read(original_artifact.dependencies[0].key, revision))) ==
                    "Initial leaf",
            "Initial pipeline output differs from independent root/leaf expectations");
    take(imports->take_job(initial.id));
    fs::rename(path / "one.txt", path / "moved/renamed.txt", error);
    require(context, !error, "Cannot move connected root source");
    fs::rename(path / "one.txt.sharkasset", path / "moved/renamed.txt.sharkasset", error);
    require(context, !error, "Cannot move connected root sidecar");
    inventory[0] = take(ProjectPath::parse("moved/renamed.txt.sharkasset"));
    catalog_revision = take(catalog->refresh(inventory, catalog_revision));
    const auto resolved = take(catalog->resolve(root_asset, catalog_revision));
    require(context,
            resolved.record && resolved.record->metadata == root_metadata &&
                resolved.record->source.text() == "moved/renamed.txt",
            "Coordinated move changed the saved asset identity");
    const auto moved = submit();
    const auto reused = wait(moved.id, true);
    require(context,
            reused.state == AssetImportJobState::succeeded && reused.summary && reused.commit &&
                reused.summary->root_key == first.summary->root_key &&
                reused.summary->reused == 2 && reused.summary->imported == 0 &&
                !reused.commit->changed && reused.commit->revision == revision,
            "Move changed path-independent content keys or republished matching output");
    take(imports->take_job(moved.id));
    const auto valid_cache = read_bytes(".shark-development-cache");
    write_source("dependency.txt", "Captured leaf");
    importer.arm(leaf_asset);
    const auto stale = submit();
    require(context, importer.entered(), "Real import worker never entered the leaf gate");
    const auto running = take(imports->jobs());
    require(context,
            running.size() == 1 && running[0].id == stale.id &&
                running[0].state == AssetImportJobState::running,
            "Owner edit was not synchronized with a running import");
    write_source("dependency.txt", "Human leaf");
    auto leaf_source = take(files->read_asset_metadata(inventory[1]));
    leaf_metadata.settings_json = "{\"transform\":\"uppercase-ascii\"}";
    take(files->write_asset_metadata(inventory[1], leaf_metadata, leaf_source.revision));
    importer.release();
    const auto rejected = wait(stale.id, true);
    require(context,
            rejected.state == AssetImportJobState::failed && rejected.error &&
                rejected.error->code == ErrorCode::conflict && !rejected.commit &&
                read_bytes(".shark-development-cache") == valid_cache &&
                take(imports->read(first.summary->root_key, revision)) == original_artifact,
            "Human source/settings edits allowed stale publication or erased last-valid output");
    take(imports->take_job(stale.id));
    catalog_revision = take(catalog->refresh(inventory, catalog_revision));
    const auto retried = submit();
    const auto updated = wait(retried.id, true);
    require(context,
            retried.id != stale.id && updated.state == AssetImportJobState::succeeded &&
                updated.summary && updated.commit && updated.commit->changed &&
                updated.summary->imported == 2 &&
                updated.summary->root_key != first.summary->root_key,
            "Explicit retry did not invalidate the dependency closure with a fresh identity");
    revision = updated.commit->revision;
    const auto current_key = updated.summary->root_key;
    const auto current_artifact = take(imports->read(current_key, revision));
    require(context,
            body(current_artifact) == "Root pipeline" &&
                current_artifact.dependencies.size() == 1 &&
                current_artifact.dependencies[0].asset == leaf_asset &&
                current_artifact.dependencies[0].key != original_artifact.dependencies[0].key &&
                body(take(imports->read(current_artifact.dependencies[0].key, revision))) ==
                    "HUMAN LEAF" &&
                take(imports->read(first.summary->root_key, revision)) == original_artifact,
            "Parent/leaf keys, transformed bytes or retained old output disagree");
    const auto committed_cancel = take(imports->cancel_job(retried.id));
    require(context,
            committed_cancel.state == AssetImportJobState::succeeded && committed_cancel.commit &&
                committed_cancel.commit->revision == revision,
            "Cancellation misreported a published pipeline commit");
    take(imports->take_job(retried.id));
    const auto latest_cache = read_bytes(".shark-development-cache");
    write_source("dependency.txt", "Cancel leaf");
    const auto cancelled = submit();
    wait(cancelled.id, false);
    const auto pending_input = take(imports->query(root_asset, target, catalog_revision, revision));
    require(context,
            pending_input.state == AssetCacheState::stale && pending_input.required_key &&
                *pending_input.required_key != current_key &&
                pending_input.last_valid_key == current_key,
            "Ready cancellation did not contain a changed dependency closure");
    take(imports->cancel_job(cancelled.id));
    const auto stopped = wait(cancelled.id, true);
    require(context,
            stopped.state == AssetImportJobState::cancelled && !stopped.commit &&
                take(imports->revision()) == revision &&
                read_bytes(".shark-development-cache") == latest_cache &&
                take(imports->read(current_key, revision)) == current_artifact,
            "Ready cancellation published through the connected pipeline");
    take(imports->take_job(cancelled.id));
    require(context, fs::remove(path / "dependency.txt", error) && !error,
            "Cannot remove pipeline leaf for the missing-source control");
    catalog_revision = take(catalog->refresh(inventory, catalog_revision));
    const auto missing = imports->submit(root_asset, target, catalog_revision, revision);
    const auto blocked = take(imports->query(root_asset, target, catalog_revision, revision));
    require(context,
            !missing && blocked.state == AssetCacheState::stale && blocked.blocked_by &&
                blocked.last_valid_key == current_key &&
                read_bytes(".shark-development-cache") == latest_cache,
            "Missing dependency erased the connected last-valid binding");
    write_source("dependency.txt", "Human leaf");
    leaf_source = take(files->read_asset_metadata(inventory[1]));
    leaf_metadata.audience = AssetAudience::server;
    take(files->write_asset_metadata(inventory[1], leaf_metadata, leaf_source.revision));
    catalog_revision = take(catalog->refresh(inventory, catalog_revision));
    const auto private_leaf = imports->submit(root_asset, target, catalog_revision, revision);
    const auto private_status =
        take(imports->query(root_asset, target, catalog_revision, revision));
    require(context,
            !private_leaf && private_leaf.error().code == ErrorCode::invalid_configuration &&
                private_status.blocked_by && private_status.last_valid_key == current_key &&
                read_bytes(".shark-development-cache") == latest_cache,
            "Client pipeline admitted private server content or lost previous output");
    leaf_source = take(files->read_asset_metadata(inventory[1]));
    leaf_metadata.audience = AssetAudience::shared;
    take(files->write_asset_metadata(inventory[1], leaf_metadata, leaf_source.revision));
    catalog_revision = take(catalog->refresh(inventory, catalog_revision));
    const auto restored = submit();
    const auto restored_status = wait(restored.id, true);
    require(context,
            restored_status.state == AssetImportJobState::succeeded && restored_status.summary &&
                restored_status.commit && !restored_status.commit->changed &&
                restored_status.summary->root_key == current_key &&
                restored_status.summary->reused == 2,
            "Restoring a dependency did not reproduce its declared key and bytes");
    take(imports->take_job(restored.id));
    take(imports->close_jobs());
    imports.reset();
    catalog.reset();
    files.reset();
    files = take(ProjectFiles::open(path));
    catalog = take(AssetCatalog::create(*files, before.project.project));
    catalog_revision = take(catalog->refresh(inventory, take(catalog->revision())));
    imports = take(AssetImports::open(*files, *catalog, before.project.project, registry));
    revision = take(imports->revision());
    const auto foreign = imports->cancel_job(retried.id);
    require(
        context,
        revision.scope != retried.id.scope && !foreign &&
            foreign.error().code == ErrorCode::conflict && take(imports->jobs()).empty() &&
            take(imports->query(root_asset, target, catalog_revision, revision)).last_valid_key ==
                current_key &&
            take(imports->read(current_key, revision)) == current_artifact,
        "Reopen revived old job authority or lost persistent output identity");
    const auto saved_project = take(files->read_project(project_path));
    const auto saved_scene =
        take(files->read_scene(saved_project.source.document, before.scene.scene));
    const auto reopened_root = take(catalog->resolve(root_asset, catalog_revision));
    require(context,
            saved_project.source.document == before.project &&
                saved_scene.source.document == before.scene &&
                read_bytes("scenes/FeatureWorld.sharkscene") == saved_scene_bytes &&
                reopened_root.record && reopened_root.record->source.text() == "moved/renamed.txt",
            "Asset moves/recovery changed the saved scene reference or project metadata");
    revision = take(imports->reload(revision));
    require(context, take(AssetImports::recover(*files)) == 0,
            "Clean pipeline unexpectedly left an unpublished stage");
    revision = take(imports->prune(revision)).revision;
    require(context,
            !imports->read(first.summary->root_key, revision) &&
                take(imports->usage(revision)).artifacts == 2 &&
                take(imports->query(root_asset, target, catalog_revision, revision)).state ==
                    AssetCacheState::fresh,
            "Explicit prune failed to retain the current complete dependency closure");
    revision = take(imports->clear(revision)).revision;
    require(context,
            take(imports->usage(revision)).artifacts == 0 &&
                take(files->read_asset_metadata(inventory[0])).source.document == root_metadata &&
                take(files->read_asset_metadata(inventory[1])).source.document == leaf_metadata,
            "Disposable cache clear changed persistent asset metadata");
    take(imports->close_jobs());
    unchanged(context, before);
}
void gltf_source(SceneRunContext& context) {
    const auto before = take(context.owner().capture());
    const auto source_path = take(ProjectPath::parse("models/static.gltf"));
    const auto resource_path = take(ProjectPath::parse("sources/triangle.bin"));
    const auto resource_asset = id<AssetId>('a');
    const std::array<std::byte, 36> triangle{};
    const std::array resources{GltfExternalSource{resource_path, resource_asset, triangle}};
    const std::string json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
        R"("nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
        R"("buffers":[{"byteLength":36,"uri":"../sources/triangle.bin"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":36}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
        R"("min":[0,0,0],"max":[0,0,0]}]})";
    const auto source_bytes = [](const std::string& text) {
        return std::as_bytes(std::span{text.data(), text.size()});
    };
    const auto inspected = take(inspect_gltf_source({source_path, source_bytes(json), resources}));
    require(context, inspected.container == GltfContainer::json,
            "glTF source inspection lost its container kind");
    require(context, inspected.counts.scenes == 1 && inspected.counts.nodes == 1,
            "glTF source inspection changed scene/node counts");
    require(context,
            inspected.counts.meshes == 1 && inspected.counts.primitives == 1 &&
                inspected.counts.accessors == 1,
            "glTF source inspection changed geometry table counts");
    require(context, inspected.selected_scene == 0 && inspected.diagnostics.empty(),
            "glTF source selection or ordinary diagnostics changed");
    require(context, inspected.resources.size() == 1,
            "glTF source inspection lost its external buffer");
    const auto& resource = inspected.resources.front();
    require(context,
            resource.kind == GltfResourceKind::buffer && resource.index == 0 &&
                resource.storage == GltfResourceStorage::external,
            "glTF source buffer reference changed kind/index/storage");
    require(context, resource.path == resource_path && resource.asset == resource_asset,
            "Contained parent URI lost persistent external resource identity");
    require(context,
            resource.byte_count == 36 &&
                resource.digest.to_hex() ==
                    "6db65fd59fd356f6729140571b5bcd6bb3b83492a16e1bf0a3884442fc3c8a0e",
            "glTF source buffer differs from independently expected literal bytes");
    const auto repeated = take(inspect_gltf_source({source_path, source_bytes(json), resources}));
    require(context,
            repeated.resources.size() == 1 &&
                repeated.resources.front().digest == resource.digest &&
                repeated.resources.front().asset == resource_asset,
            "Repeated pure source inspection changed resource identity");
    const auto missing = inspect_gltf_source({source_path, source_bytes(json)});
    require(context, !missing && missing.error().context.operation == "asset.gltf.inspect",
            "Missing glTF resource did not report its owned operation failure");
    auto escaping = json;
    escaping.replace(escaping.find("../sources/triangle.bin"), 23, "../../escape.bin");
    require(context, !inspect_gltf_source({source_path, source_bytes(escaping), resources}),
            "glTF URI escaped the logical project root");
    const std::string required =
        R"({"asset":{"version":"2.0"},"extensionsRequired":["SHARK_unsupported"]})";
    const auto unsupported = inspect_gltf_source({source_path, source_bytes(required)});
    require(context,
            !unsupported &&
                unsupported.error().message.find("SHARK_unsupported") != std::string::npos,
            "Unsupported required extension lost its explicit name");
    const std::string animated = R"({"asset":{"version":"2.0"},"animations":[{}]})";
    require(context, !inspect_gltf_source({source_path, source_bytes(animated)}),
            "Static source inspection silently accepted animation content");
    std::stop_source stop;
    stop.request_stop();
    require(context,
            !inspect_gltf_source({source_path, source_bytes(json), resources, stop.get_token()}),
            "Cancelled source inspection returned success");
    unchanged(context, before);
}

void gltf_geometry(SceneRunContext& context) {
    const auto before = take(context.owner().capture());
    const auto source_path = take(ProjectPath::parse("models/static.gltf"));
    const auto resource_path = take(ProjectPath::parse("sources/triangle.bin"));
    const auto resource_asset = id<AssetId>('a');
    const auto source_bytes = [](const std::string& text) {
        return std::as_bytes(std::span{text.data(), text.size()});
    };
    const auto position_bytes = [](std::span<const float> values) {
        std::vector<std::byte> bytes;
        for (const auto value : values) {
            const auto bits = std::bit_cast<std::uint32_t>(value);
            for (unsigned shift = 0; shift != 32; shift += 8)
                bytes.push_back(static_cast<std::byte>((bits >> shift) & 0xffU));
        }
        return bytes;
    };
    constexpr std::array triangle_positions{0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    const auto triangle = position_bytes(triangle_positions);
    const std::array resources{GltfExternalSource{resource_path, resource_asset, triangle}};
    const std::string json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
        R"("nodes":[{"name":"parent","mesh":0,"translation":[1,2,3],"children":[1]},)"
        R"({"name":"mirrored","mesh":0,"translation":[4,0,0],"scale":[-2,3,1]}],)"
        R"("meshes":[{"name":"shared triangle","primitives":[{"attributes":{"POSITION":0}}]}],)"
        R"("buffers":[{"byteLength":36,"uri":"../sources/triangle.bin"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":36}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
        R"("min":[0,0,0],"max":[1,1,0]}]})";
    const auto converted =
        take(convert_gltf_geometry({source_path, source_bytes(json), resources}));
    require(context, converted.selected_scene == 0 && converted.source.selected_scene == 0,
            "Geometry conversion changed selected/default scene identity");
    require(context,
            converted.meshes.size() == 1 && converted.primitives.size() == 1 &&
                converted.nodes.size() == 2 && converted.roots == std::vector<std::size_t>{0},
            "Geometry conversion lost selected graph or duplicated a shared mesh");
    const auto& mesh = converted.meshes.front();
    const auto& primitive = converted.primitives.front();
    require(context,
            mesh.source_mesh == 0 && mesh.name == "shared triangle" &&
                mesh.primitives == std::vector<std::size_t>{0} && primitive.source_mesh == 0 &&
                primitive.source_primitive == 0 && !primitive.source_material,
            "Geometry conversion confused source indices with dense references");
    require(context,
            primitive.vertices.size() == 3 &&
                primitive.indices == std::vector<std::uint32_t>{0, 1, 2},
            "Geometry conversion changed triangle topology or flat-normal splitting");
    constexpr std::array expected_positions{
        std::array{0.0F, 0.0F, 0.0F}, std::array{1.0F, 0.0F, 0.0F}, std::array{0.0F, 1.0F, 0.0F}};
    for (std::size_t index = 0; index < expected_positions.size(); ++index) {
        require(context,
                primitive.vertices[index].position == expected_positions[index] &&
                    primitive.vertices[index].normal == std::array{0.0F, 0.0F, 1.0F} &&
                    primitive.vertices[index].color == std::array{1.0F, 1.0F, 1.0F, 1.0F},
                "Geometry differs from independent triangle positions, normals or color defaults");
    }
    require(context,
            primitive.generated_normals && !primitive.generated_tangents &&
                !primitive.has_tangents && !primitive.has_texcoord0 && !primitive.has_texcoord1 &&
                !primitive.has_color,
            "Geometry presence/generation flags do not describe the actual attributes");
    const GltfGeometryBounds local_bounds{math::Vec3{0.0, 0.0, 0.0}, math::Vec3{1.0, 1.0, 0.0}};
    require(context, primitive.bounds == local_bounds && mesh.bounds == local_bounds,
            "Geometry local bounds differ from the independent triangle bounds");
    const auto& parent = converted.nodes[0];
    const auto& mirrored = converted.nodes[1];
    require(context,
            parent.source_node == 0 && mirrored.source_node == 1 && !parent.parent &&
                parent.children == std::vector<std::size_t>{1} && mirrored.parent == 0 &&
                mirrored.children.empty() && parent.mesh == 0 && mirrored.mesh == 0,
            "Geometry hierarchy does not retain root/child/source order and shared dense meshes");
    require(context,
            parent.name == "parent" && mirrored.name == "mirrored" && !parent.mirrored &&
                mirrored.mirrored && parent.local.translation == math::Vec3{1.0, 2.0, 3.0} &&
                mirrored.local.translation == math::Vec3{4.0, 0.0, 0.0} &&
                mirrored.local.scale == math::Vec3{-2.0, 3.0, 1.0},
            "Geometry hierarchy changed local TRS or per-instance reflection");
    require(context,
            math::Vec3{mirrored.world_matrix[3]} == math::Vec3{5.0, 2.0, 3.0} &&
                mirrored.world_matrix[0][0] == -2.0 && mirrored.world_matrix[1][1] == 3.0 &&
                mirrored.world_matrix[2][2] == 1.0,
            "Geometry world matrix does not compose parent times local");
    require(context,
            parent.world_bounds ==
                    GltfGeometryBounds{math::Vec3{1.0, 2.0, 3.0}, math::Vec3{2.0, 3.0, 3.0}} &&
                mirrored.world_bounds ==
                    GltfGeometryBounds{math::Vec3{3.0, 2.0, 3.0}, math::Vec3{5.0, 5.0, 3.0}} &&
                converted.world_bounds ==
                    GltfGeometryBounds{math::Vec3{1.0, 2.0, 3.0}, math::Vec3{5.0, 5.0, 3.0}},
            "Geometry bounds do not include both ordinary and mirrored instances");
    require(context,
            mirrored.normal_matrix[0] == -0.5 && mirrored.normal_matrix[4] == 1.0 / 3.0 &&
                mirrored.normal_matrix[8] == 1.0,
            "Geometry inverse-transpose normal matrix lost nonuniform scale");
    require(context,
            converted.source.resources.size() == 1 &&
                converted.source.resources[0].path == resource_path &&
                converted.source.resources[0].asset == resource_asset,
            "Geometry conversion lost persistent captured-resource identity");
    const auto repeated = take(convert_gltf_geometry({source_path, source_bytes(json), resources}));
    require(context,
            repeated.primitives[0].vertices == primitive.vertices &&
                repeated.primitives[0].indices == primitive.indices &&
                repeated.world_bounds == converted.world_bounds &&
                repeated.nodes[1].world_matrix == mirrored.world_matrix,
            "Repeated geometry conversion changed owned values");

    const std::string sparse_json =
        R"({"asset":{"version":"2.0"},"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}],)"
        R"("buffers":[{"byteLength":40,"uri":"../sources/triangle.bin"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":3},{"buffer":0,"byteOffset":4,"byteLength":36}],)"
        R"("accessors":[{"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0],)"
        R"("sparse":{"count":3,"indices":{"bufferView":0,"componentType":5121},)"
        R"("values":{"bufferView":1}}}]})";
    std::vector<std::byte> sparse{std::byte{0}, std::byte{1}, std::byte{2}, std::byte{0}};
    sparse.insert(sparse.end(), triangle.begin(), triangle.end());
    const std::array sparse_resources{GltfExternalSource{resource_path, resource_asset, sparse}};
    const auto expanded =
        take(convert_gltf_geometry({source_path, source_bytes(sparse_json), sparse_resources}));
    require(context,
            expanded.primitives[0].vertices == primitive.vertices &&
                expanded.primitives[0].indices == primitive.indices &&
                expanded.primitives[0].bounds == local_bounds,
            "Sparse geometry does not reproduce the independent dense triangle");
    auto singular = json;
    singular.replace(singular.find("[-2,3,1]"), 8, "[0,3,1]");
    const auto rejected = convert_gltf_geometry({source_path, source_bytes(singular), resources});
    require(context, !rejected && rejected.error().context.operation == "asset.gltf.convert",
            "Singular geometry node did not return its owned conversion error");
    require(context,
            !convert_gltf_geometry({source_path, source_bytes(json), resources}, {.scene = 1}),
            "Geometry conversion accepted an absent explicitly selected scene");
    std::stop_source stop;
    stop.request_stop();
    require(context,
            !convert_gltf_geometry({source_path, source_bytes(json), resources, stop.get_token()}),
            "Cancelled geometry conversion returned success");
    unchanged(context, before);
}

void gltf_textures(SceneRunContext& context) {
    const auto before = take(context.owner().capture());
    const auto source_path = take(ProjectPath::parse("models/textures.gltf"));
    const auto buffer_path = take(ProjectPath::parse("sources/triangle.bin"));
    const auto image_path = take(ProjectPath::parse("images/semantics.png"));
    const auto image_asset = id<AssetId>('b');
    const auto source_bytes = [](const std::string& text) {
        return std::as_bytes(std::span{text.data(), text.size()});
    };
    constexpr std::array triangle_values{0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 1.0F,
                                         0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F};
    std::vector<std::byte> triangle;
    for (const auto value : triangle_values) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned shift = 0; shift != 32; shift += 8)
            triangle.push_back(static_cast<std::byte>((bits >> shift) & 0xffU));
    }
    // Original MIT-licensed Shark 2x1 RGBA8 PNG: transparent black then opaque white.
    // Inline bytes keep this executable independent of test fixtures and paths.
    constexpr std::array<unsigned char, 71> png{
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
        0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0xf4,
        0x22, 0x7f, 0x8a, 0x00, 0x00, 0x00, 0x0e, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60,
        0x00, 0x82, 0xff, 0x40, 0x00, 0x00, 0x09, 0xff, 0x03, 0xfd, 0xcb, 0xeb, 0x0f, 0x61, 0x00,
        0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    const auto pixels = std::as_bytes(std::span{png});
    const std::array resources{GltfExternalSource{buffer_path, id<AssetId>('a'), triangle},
                               GltfExternalSource{image_path, image_asset, pixels}};
    const std::string json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"material":0}]}],)"
        R"("buffers":[{"byteLength":60,"uri":"../sources/triangle.bin"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24}],)"
        R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},)"
        R"({"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"}],)"
        R"("images":[{"uri":"../images/semantics.png"}],"textures":[{"source":0},{"source":0,"sampler":0}],)"
        R"("samplers":[{"magFilter":9728,"minFilter":9984,"wrapS":33071,"wrapT":33648}],)"
        R"("materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0},)"
        R"("metallicRoughnessTexture":{"index":1}},"normalTexture":{"index":1},)"
        R"("occlusionTexture":{"index":1},"emissiveTexture":{"index":0}}]})";
    const auto converted =
        take(convert_gltf_textures({source_path, source_bytes(json), resources}));
    require(context, converted.selected_scene == 0 && converted.source.selected_scene == 0,
            "Texture conversion changed selected scene identity");
    require(context, converted.images.size() == 3 && converted.usages.size() == 5,
            "One source image did not produce three shared semantic variants for five roles");
    constexpr std::array semantics{GltfTextureSemantic::srgb_color,
                                   GltfTextureSemantic::linear_data, GltfTextureSemantic::normal};
    const std::vector<std::byte> expected_pixels{std::byte{0},   std::byte{0},   std::byte{0},
                                                 std::byte{0},   std::byte{255}, std::byte{255},
                                                 std::byte{255}, std::byte{255}};
    const std::array expected_mips{
        std::vector<std::byte>{std::byte{188}, std::byte{188}, std::byte{188}, std::byte{128}},
        std::vector<std::byte>{std::byte{128}, std::byte{128}, std::byte{128}, std::byte{128}},
        std::vector<std::byte>{std::byte{128}, std::byte{128}, std::byte{255}, std::byte{128}}};
    for (std::size_t index = 0; index < semantics.size(); ++index) {
        const auto& image = converted.images[index];
        require(context,
                image.source_image == 0 && image.semantic == semantics[index] &&
                    image.component_bits == 8 && image.mips.size() == 2,
                "Texture variant lost its source index, precision, semantic or full mip chain");
        require(context,
                image.mips[0] == GltfTextureMip{2, 1, expected_pixels} &&
                    image.mips[1] == GltfTextureMip{1, 1, expected_mips[index]},
                "Texture pixels differ from independent sRGB, linear, normal or alpha results");
    }
    constexpr std::array roles{GltfTextureRole::base_color, GltfTextureRole::metallic_roughness,
                               GltfTextureRole::normal, GltfTextureRole::occlusion,
                               GltfTextureRole::emissive};
    constexpr std::array<std::size_t, 5> variants{0, 1, 2, 1, 0};
    const GltfTextureSampler explicit_sampler{
        GltfTextureFilter::nearest, GltfTextureFilter::nearest, GltfTextureMipFilter::nearest,
        GltfTextureWrap::clamp_to_edge, GltfTextureWrap::mirrored_repeat};
    for (std::size_t index = 0; index < roles.size(); ++index) {
        const auto& usage = converted.usages[index];
        const bool color = index == 0 || index == 4;
        require(context,
                usage.source_material == 0 && usage.role == roles[index] &&
                    usage.source_texture == (color ? 0U : 1U) && usage.texcoord == 0 &&
                    usage.image == variants[index],
                "Texture usage lost material/role/UV correspondence or semantic sharing");
        require(context, usage.sampler == (color ? GltfTextureSampler{} : explicit_sampler),
                "Texture usage lost default filtering/repeat or explicit nearest/clamp/mirror");
    }
    const auto image =
        std::find_if(converted.source.resources.begin(), converted.source.resources.end(),
                     [](const auto& resource) { return resource.kind == GltfResourceKind::image; });
    require(context,
            image != converted.source.resources.end() && converted.source.resources.size() == 2,
            "Texture conversion lost its captured external image mapping");
    require(context,
            image->index == 0 && image->storage == GltfResourceStorage::external &&
                image->path == image_path && image->asset == image_asset &&
                image->byte_count == 71 &&
                image->digest.to_hex() ==
                    "27f7e7462919f63872ab4ec74a1e532e39aacb4d00b5553d0203c6c21c964e02",
            "Texture conversion lost persistent image identity or exact captured byte digest");
    const auto repeated = take(convert_gltf_textures({source_path, source_bytes(json), resources}));
    require(context,
            repeated.images == converted.images && repeated.usages == converted.usages &&
                repeated.selected_scene == converted.selected_scene,
            "Repeated texture conversion changed pixels, sharing, samplers or scene identity");
    const std::array missing_image{resources[0]};
    const auto missing = convert_gltf_textures({source_path, source_bytes(json), missing_image});
    require(context, !missing && missing.error().context.operation == "asset.gltf.textures",
            "Missing texture image did not return its owned operation failure");
    const std::array truncated{resources[0],
                               GltfExternalSource{image_path, image_asset, pixels.first(70)}};
    require(context, !convert_gltf_textures({source_path, source_bytes(json), truncated}),
            "Texture conversion accepted a truncated captured PNG");
    auto no_uv = json;
    constexpr std::string_view uv_attribute = R"(,"TEXCOORD_0":1)";
    no_uv.erase(no_uv.find(uv_attribute), uv_attribute.size());
    require(context, !convert_gltf_textures({source_path, source_bytes(no_uv), resources}),
            "Texture conversion accepted a material use without its referenced UV set");
    require(context,
            !convert_gltf_textures({source_path, source_bytes(json), resources}, {.scene = 1}),
            "Texture conversion accepted an absent explicit scene");
    std::stop_source stop;
    stop.request_stop();
    require(context,
            !convert_gltf_textures({source_path, source_bytes(json), resources, stop.get_token()}),
            "Cancelled texture conversion returned success");
    unchanged(context, before);
}
void gltf_assets(SceneRunContext& context) {
    const auto before = take(context.owner().capture());
    const auto path = take(ProjectPath::parse("models/material.gltf"));
    // Original Shark unit triangle and material; no external files or decoders.
    const std::string json =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
        R"("nodes":[{"mesh":0,"translation":[2,3,4]}],"meshes":[{"primitives":[)"
        R"({"attributes":{"POSITION":0},"material":0}]}],"buffers":[{"byteLength":36,)"
        R"("uri":"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAA"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[)"
        R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],)"
        R"("materials":[{"name":"Paint","pbrMetallicRoughness":{)"
        R"("baseColorFactor":[0.25,0.5,0.75,0.5],"metallicFactor":0.25,"roughnessFactor":0.75},)"
        R"("emissiveFactor":[0.125,0.25,0.5],"alphaMode":"MASK","alphaCutoff":0.25,"doubleSided":true}]})";
    const GltfSourceInput input{path, std::as_bytes(std::span{json.data(), json.size()})};
    const auto material = take(convert_gltf_materials(input));
    require(context, material.materials.size() == 1, "Selected material inventory differs");
    const auto& paint = material.materials.front();
    require(context, paint.source_material == 0 && paint.name == "Paint",
            "Material identity differs");
    require(context, paint.base_color == std::array{0.25F, 0.5F, 0.75F, 0.5F},
            "Base color factors differ from authored values");
    require(context, paint.metallic == 0.25F && paint.roughness == 0.75F,
            "Metallic-roughness factors differ");
    require(context, paint.emissive == std::array{0.125F, 0.25F, 0.5F}, "Emissive factors differ");
    require(context,
            paint.alpha_mode == GltfAlphaMode::mask && paint.alpha_cutoff == 0.25F &&
                paint.double_sided,
            "Alpha/culling material values differ");
    require(context, paint.normal_scale == 1.0F && paint.occlusion_strength == 1.0F,
            "Absent normal/occlusion factors lost defaults");
    const auto asset = take(convert_gltf_asset(input));
    const auto payload = take(encode_gltf_asset(asset));
    const auto restored = take(decode_gltf_asset(payload));
    require(context, restored.materials.materials == material.materials,
            "Engine payload lost material values");
    require(context,
            restored.geometry.primitives.size() == 1 && restored.geometry.nodes.size() == 1,
            "Engine payload lost geometry/hierarchy");
    require(context,
            restored.geometry.primitives[0].vertices.size() == 3 &&
                restored.geometry.primitives[0].indices == std::vector<std::uint32_t>{0, 1, 2},
            "Engine payload lost triangle topology");
    require(context,
            restored.geometry.primitives[0].vertices[1].position == std::array{1.0F, 0.0F, 0.0F},
            "Engine payload changed independent vertex position");
    require(context, restored.geometry.nodes[0].local.translation == math::Vec3{2, 3, 4},
            "Engine payload changed node transform");
    require(context, restored.textures.images.empty() && restored.textures.usages.empty(),
            "Engine payload invented texture data");
    require(context, take(encode_gltf_asset(restored)) == payload,
            "Engine payload is not deterministic after decode");
    auto damaged = payload;
    damaged.back() ^= std::byte{1};
    require(context, !decode_gltf_asset(damaged), "Changed engine payload was accepted");
    std::stop_source stop;
    stop.request_stop();
    require(context, !decode_gltf_asset(payload, stop.get_token()), "Cancelled decode succeeded");
    unchanged(context, before);
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
                    case 5:
                        asset_identity(context, scratch_parent);
                        break;
                    case 6:
                        asset_import(context, scratch_parent);
                        break;
                    case 7:
                        asset_jobs(context, scratch_parent);
                        break;
                    case 8:
                        asset_pipeline(context, scratch_parent);
                        break;
                    case 9:
                        gltf_source(context);
                        break;
                    case 10:
                        gltf_geometry(context);
                        break;
                    case 11:
                        gltf_textures(context);
                        break;
                    case 12:
                        gltf_assets(context);
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
