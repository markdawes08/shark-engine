// Fixed CPU acceptance scenario over the actual Editor-private adapters. This
// executable is test-only: it is neither another authoring host nor a UI test.
#include "../editor/asset_panel.hpp"
#include "../editor/inspector_panel.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace shark;
using namespace shark::editor;
using namespace shark::tools;
using Json = nlohmann::json;

constexpr std::string_view project_path = "Terminal.sharkproject";
constexpr std::string_view triangle_id = "15e00000-0000-4000-8000-000000000005";
constexpr std::string_view camera_id = "15e00000-0000-4000-8000-000000000004";
constexpr std::string_view mesh_id = "15e00000-0000-4000-8000-000000000006";
constexpr std::array<std::string_view, 2> sidecars{"models/triangle.gltf.sharkasset",
                                                   "models/triangle.bin.sharkasset"};
const AssetTargetProfile target{"terminal-example", AssetAudience::client};

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error{std::string{message}};
}
template <typename T> T take(Result<T> result) {
    if (!result)
        throw std::runtime_error{result.error().context.operation + ": " + result.error().message};
    return std::move(result).value();
}
void accepted(Result<void> result) {
    if (!result)
        throw std::runtime_error{result.error().context.operation + ": " + result.error().message};
}
template <typename T> void conflict(const Result<T>& result) {
    require(!result && result.error().code == ErrorCode::conflict,
            "Expected revision/ownership conflict");
}
AuthoredEntityId entity(std::string_view text) {
    return AuthoredEntityId{take(StableId::parse(text))};
}
Json call(ProjectSession& owner, std::string_view method, Json params = Json::object()) {
    const auto reply = take(owner.request(method, params.dump()));
    require(!reply.failed, reply.json);
    const auto envelope = Json::parse(reply.json);
    require(!envelope.contains("error"), reply.json);
    return envelope.at("result");
}
void emit(Json row) {
    row["seed"] = 1337;
    const auto bytes = row.dump();
    require(bytes.size() <= 65536, "Fixture output exceeded its bound");
    std::cout << bytes << '\n' << std::flush;
    require(static_cast<bool>(std::cout), "Fixture output failed");
}
std::string_view durability(FileDurability value) {
    switch (value) {
    case FileDurability::file_flushed:
        return "file_flushed";
    case FileDurability::file_and_directory_flushed:
        return "file_and_directory_flushed";
    case FileDurability::directory_flush_failed:
        return "directory_flush_failed";
    }
    throw std::runtime_error{"Unknown file durability"};
}
Json publication(const AssetPublication& value) {
    return {{"published", value.published},
            {"bytes", value.bytes},
            {"durability", durability(value.durability)}};
}
Json values(const InspectorValues& value, const HierarchyRow& row) {
    const auto& local = value.transform;
    Json camera = nullptr;
    if (value.camera) {
        const auto& item = *value.camera;
        camera = {{"projection", item.projection == CameraProjection::orthographic ? "orthographic"
                                                                                   : "perspective"},
                  {"vertical_fov_radians", item.vertical_fov_radians},
                  {"orthographic_height", item.orthographic_height},
                  {"near_clip", item.near_clip},
                  {"far_clip", item.far_clip}};
    }
    Json mesh = nullptr;
    if (value.mesh_renderer) {
        const auto& item = *value.mesh_renderer;
        mesh = {{"asset_id", item.asset.value().to_string()},
                {"mesh_index", item.mesh_index},
                {"visible", item.visible}};
    }
    return {
        {"entity_id", value.selection.id.value().to_string()},
        {"name", value.name.value},
        {"hierarchy",
         {{"parent_id", row.parent ? Json(row.parent->value().to_string()) : Json(nullptr)}}},
        {"transform",
         {{"local",
           {{"translation", {local.translation.x, local.translation.y, local.translation.z}},
            {"rotation", {local.rotation.x, local.rotation.y, local.rotation.z, local.rotation.w}},
            {"scale", {local.scale.x, local.scale.y, local.scale.z}}}}}},
        {"camera", std::move(camera)},
        {"mesh_renderer", std::move(mesh)}};
}
Json observe(ProjectSession& session, ScenePanel& scene) {
    accepted(scene.refresh());
    Json entities = Json::array();
    for (const auto& row : scene.rows())
        entities.push_back(values(take(scene.inspect(row.id, scene.revision())), row));
    return {{"status", call(session, "project.status")},
            {"entities", std::move(entities)},
            {"history", call(session, "scene.history")},
            {"mutation_status", call(session, "scene.mutation_status")}};
}
void select(ScenePanel& scene, InspectorPanel& inspector, std::string_view id) {
    accepted(scene.select(entity(id)));
    accepted(inspector.refresh());
    require(inspector.draft().has_value(), "Inspector did not retain its selected draft");
}
void applied(InspectorPanel& inspector) {
    const auto receipt = take(inspector.apply());
    require(receipt.changed && !receipt.refresh_error, "Inspector edit did not fully refresh");
}
void inventory(AssetPanel& assets) {
    accepted(assets.refresh_catalog());
    accepted(assets.refresh_inventory(sidecars));
    accepted(assets.refresh_catalog());
    accepted(assets.refresh_cache());
    accepted(assets.select(AssetId{take(StableId::parse(mesh_id))}));
}
Json descriptor(const AssetArtifactInfo& value) {
    Json dependencies = Json::array();
    for (const auto& dependency : value.dependencies)
        dependencies.push_back(
            {{"asset_id", dependency.asset.value().to_string()}, {"key", dependency.key.to_hex()}});
    return {{"key", value.key.to_hex()},
            {"asset_id", value.asset.value().to_string()},
            {"source_digest", value.source_digest.to_hex()},
            {"settings_digest", value.settings_digest.to_hex()},
            {"payload_digest", value.payload_digest.to_hex()},
            {"payload_bytes", value.payload_bytes},
            {"dependencies", std::move(dependencies)}};
}
Json fresh(AssetPanel& assets) {
    const auto cache = take(assets.query_selected_cache(target));
    require(cache.state == AssetCacheState::fresh && cache.last_valid_key && cache.required_key &&
                cache.required_key == cache.last_valid_key && !cache.blocked_by,
            "Expected a fresh complete imported asset");
    auto result = descriptor(take(assets.inspect_artifact(*cache.last_valid_key)));
    result["state"] = "fresh";
    return result;
}
Json imported(AssetPanel& assets, const std::string& previous_key) {
    const auto admission = take(assets.submit_selected(target));
    require(!admission.deduplicated, "Unexpected pending job in a new owner");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    bool ready = false;
    while (std::chrono::steady_clock::now() < deadline) {
        accepted(assets.refresh_jobs());
        const auto jobs = assets.jobs();
        const auto found = std::find_if(jobs.begin(), jobs.end(),
                                        [&](const auto& job) { return job.id == admission.id; });
        require(found != jobs.end(), "Submitted import disappeared");
        if (found->state == AssetImportJobState::ready) {
            require(!found->commit, "Observing computation published the import");
            ready = true;
            break;
        }
        require(found->state == AssetImportJobState::queued ||
                    found->state == AssetImportJobState::running,
                "Import ended before explicit drain");
        std::this_thread::yield();
    }
    require(ready, "Import ready deadline expired");
    const auto drained = take(assets.drain());
    require(drained.drained == 1 && drained.jobs.size() == 1, "Expected one drained import");
    const auto& completed = drained.jobs.front();
    require(completed.id == admission.id && completed.state == AssetImportJobState::succeeded &&
                completed.summary && completed.commit && completed.commit->changed &&
                completed.commit->publication && completed.commit->publication->published &&
                !completed.error,
            "Import did not publish successfully");
    const auto taken = take(assets.take_job(admission.id));
    require(taken.state == completed.state && taken.summary &&
                taken.summary->root_key == completed.summary->root_key && taken.commit &&
                taken.commit->publication && taken.commit->publication->published,
            "Taking the outcome lost its publication");
    accepted(assets.refresh_jobs());
    require(assets.jobs().empty(), "Taking did not release the completed slot");
    accepted(assets.refresh_cache());
    auto result = fresh(assets);
    result["root_key"] = completed.summary->root_key.to_hex();
    result["previous_key"] = previous_key;
    result["imported"] = completed.summary->imported;
    result["reused"] = completed.summary->reused;
    result["nodes"] = completed.summary->nodes;
    result["job_id"] = {{"scope", admission.id.scope.to_string()},
                        {"sequence", std::to_string(admission.id.sequence)}};
    result["publication"] = publication(*completed.commit->publication);
    require(result.at("root_key") != previous_key, "Metadata settings did not change the root key");
    return result;
}
Json dirty_guards(ProjectSession& session) {
    const auto before = session.state();
    require(before.dirty, "Dirty guards require an unsaved scene");
    const auto history = call(session, "scene.history");
    conflict(session.close(CloseChoice::reject, before.revision));
    const auto cancelled = take(session.close(CloseChoice::cancel, before.revision));
    require(cancelled.cancelled && !cancelled.closed, "Cancel closed the dirty owner");
    conflict(session.open(project_path));
    require(session.state() == before && call(session, "scene.history") == history,
            "Rejected close/open changed the dirty owner or history");
    return {{"reject", "conflict"},
            {"cancelled", cancelled.cancelled},
            {"replace", "conflict"},
            {"state_preserved", true},
            {"history_preserved", true}};
}
void edit(const std::filesystem::path& root) {
    auto session = take(ProjectSession::create(root, project_path));
    ScenePanel scene{*session};
    InspectorPanel inspector{scene};
    AssetPanel assets{*session};
    const auto initial = observe(*session, scene);
    require(initial.at("history").at("undo_entries") == 0, "New editor owner retained old history");
    inventory(assets);
    const auto previous = fresh(assets);
    select(scene, inspector, triangle_id);
    inspector.draft()->name.value = "Editor harbor";
    inspector.draft()->transform.translation = {-2, -1, 0};
    applied(inspector);
    select(scene, inspector, camera_id);
    require(inspector.draft()->camera.has_value(), "Expected the authored Camera");
    inspector.draft()->camera->orthographic_height = 4;
    applied(inspector);
    const auto dirty = session->state();
    const auto history = call(*session, "scene.history");
    accepted(assets.read_metadata(sidecars.front()));
    auto metadata = Json::parse(assets.metadata()->json);
    metadata["origin"] = "Editor-authored continuation";
    require(metadata.at("settings").erase("scene") == 1,
            "Expected the explicit original scene setting");
    const auto written = take(assets.write_metadata(sidecars.front(), metadata.dump(), false));
    require(written.publication.published, "Metadata did not publish");
    inventory(assets);
    const auto imported_asset = imported(assets, previous.at("key").get<std::string>());
    require(session->state() == dirty && call(*session, "scene.history") == history,
            "Asset metadata/import changed scene state or history");
    auto ready = observe(*session, scene);
    ready["event"] = "ready";
    ready["initial"] = initial;
    ready["metadata"] = metadata;
    ready["metadata_publication"] = publication(written.publication);
    ready["import"] = imported_asset;
    ready["guards"] = dirty_guards(*session);
    emit(std::move(ready));

    // The Python parent supplies a bounded deadline and kills/drains this known
    // child on failure. Accept only this fixed barrier, never arbitrary methods.
    std::array<char, 32> command{};
    std::cin.getline(command.data(), static_cast<std::streamsize>(command.size()));
    require(static_cast<bool>(std::cin) && std::string_view{command.data()} == "save-close",
            "Expected fixed save-close barrier");
    const auto saved = take(session->save(session->state().revision));
    require(saved.published && saved.current_scene_clean, "Explicit editor save did not publish");
    const auto after_save = call(*session, "scene.history");
    require(after_save == history, "Saving cleared or changed editor history");
    accepted(assets.release_metadata());
    const auto closed = take(session->close(CloseChoice::reject, session->state().revision));
    require(closed.closed && !closed.error, "Explicit editor close did not release ownership");
    emit({{"event", "closed"},
          {"save",
           {{"published", saved.published},
            {"current_scene_clean", saved.current_scene_clean},
            {"bytes", saved.bytes},
            {"durability", saved.durability}}},
          {"history_before", history},
          {"history_after", after_save},
          {"status", call(*session, "project.status")}});
}
void continuation(const std::filesystem::path& root) {
    auto session = take(ProjectSession::create(root, project_path));
    ScenePanel scene{*session};
    InspectorPanel inspector{scene};
    AssetPanel assets{*session};
    auto reopened = observe(*session, scene);
    require(reopened.at("history").at("undo_entries") == 0 && !session->state().dirty,
            "Terminal handoff did not create a clean new history owner");
    inventory(assets);
    accepted(assets.read_metadata(sidecars.front()));
    reopened["event"] = "reopened";
    reopened["metadata"] = Json::parse(assets.metadata()->json);
    reopened["cache"] = fresh(assets);
    const auto initial_entities = reopened.at("entities");
    emit(std::move(reopened));
    select(scene, inspector, triangle_id);
    inspector.draft()->name.value = "Discarded editor scratch";
    applied(inspector);
    const auto before_undo = observe(*session, scene);
    auto history = take(scene.history(scene.revision()));
    require(history.next_undo.has_value(), "Temporary edit lacks undo");
    const auto undone = take(scene.undo(history.next_undo->ticket, history.revision));
    require(undone.changed && !undone.refresh_error, "Editor undo failed");
    const auto after_undo = observe(*session, scene);
    require(after_undo.at("entities") == initial_entities,
            "Undo did not restore terminal-authored values");
    history = take(scene.history(scene.revision()));
    require(history.next_redo.has_value(), "Editor undo lacks redo");
    const auto redone = take(scene.redo(history.next_redo->ticket, history.revision));
    require(redone.changed && !redone.refresh_error, "Editor redo failed");
    const auto after_redo = observe(*session, scene);
    require(after_redo.at("entities") == before_undo.at("entities"),
            "Redo did not restore the editor edit");
    const auto guards = dirty_guards(*session);
    accepted(assets.release_metadata());
    const auto closed = take(session->close(CloseChoice::discard, session->state().revision));
    require(closed.closed && !closed.saved && !closed.error,
            "Explicit discard did not release ownership");
    emit({{"event", "discarded"},
          {"before_undo", before_undo},
          {"after_undo", after_undo},
          {"after_redo", after_redo},
          {"guards", guards},
          {"status", call(*session, "project.status")}});
}
} // namespace

int run(const std::filesystem::path& root, std::string_view phase) {
    try {
        if (phase == "edit")
            edit(root);
        else if (phase == "continue")
            continuation(root);
        else if (phase == "busy") {
            auto session = take(ProjectSession::create(root));
            const auto opened = session->open(project_path);
            conflict(opened);
            emit({{"event", "busy"},
                  {"code", "conflict"},
                  {"message", opened.error().message},
                  {"operation", opened.error().context.operation}});
        } else
            throw std::runtime_error{"Unknown fixed fixture phase"};
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    if (argc != 5 || std::wstring_view{argv[1]} != L"--root" ||
        std::wstring_view{argv[3]} != L"--phase")
        return 2;
    const std::wstring_view phase{argv[4]};
    return run(std::filesystem::path{argv[2]}, phase == L"edit"       ? "edit"
                                               : phase == L"continue" ? "continue"
                                               : phase == L"busy"     ? "busy"
                                                                      : "invalid");
}
#else
int main(int argc, char** argv) {
    if (argc != 5 || std::string_view{argv[1]} != "--root" ||
        std::string_view{argv[3]} != "--phase")
        return 2;
    const std::string root_text{argv[2]};
    const std::u8string root_utf8{root_text.begin(), root_text.end()};
    return run(std::filesystem::path{root_utf8}, argv[4]);
}
#endif
