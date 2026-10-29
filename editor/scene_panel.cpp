#include "scene_panel.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <thread>
#include <utility>
#include <vector>

namespace shark::editor {
namespace {
using Json = nlohmann::json;

struct Failure {
    Error error;
};

template <typename T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}

void take(Result<void> result) {
    if (!result)
        throw Failure{std::move(result).error()};
}

[[noreturn]] void fail(ErrorCode code, const char* message) {
    throw Failure{{code, message, {.operation = "editor.scene.panel"}}};
}

template <typename T, typename F> Result<T> boundary(F&& operation) {
    try {
        return operation();
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot complete scene panel operation; reconcile any pending mutation",
                     {.operation = "editor.scene.panel"}};
    }
}

Error decode_error(const Json& value) {
    constexpr std::array codes{
        ErrorCode::invalid_argument,    ErrorCode::invalid_configuration,  ErrorCode::invalid_id,
        ErrorCode::invalid_path,        ErrorCode::path_outside_root,      ErrorCode::io_failure,
        ErrorCode::entropy_unavailable, ErrorCode::capability_unavailable, ErrorCode::conflict,
        ErrorCode::execution_failed};
    const auto name = value.at("code").get<std::string>();
    const auto found = std::find_if(codes.begin(), codes.end(),
                                    [&](ErrorCode code) { return error_code_name(code) == name; });
    if (found == codes.end())
        fail(ErrorCode::execution_failed, "Authoring returned an unknown error code");
    const auto& context = value.at("context");
    return {*found,
            value.at("message").get<std::string>(),
            {context.value("operation", std::string{}), context.value("request_id", std::string{}),
             context.value("path", std::string{}), context.value("entity_id", std::string{}),
             context.value("asset_id", std::string{})}};
}

Json result_value(const tools::AuthoringProtocolReply& reply) {
    auto document = Json::parse(reply.json);
    if (document.contains("error")) {
        const auto& rpc = document.at("error");
        auto value = rpc.at("data");
        value["message"] = rpc.at("message");
        throw Failure{decode_error(value)};
    }
    return std::move(document.at("result"));
}

std::uint64_t decimal(const Json& value) {
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        fail(ErrorCode::execution_failed, "Authoring returned an invalid revision");
    return result;
}

SceneRevision revision_value(const Json& value) {
    return {decimal(value.at("scene_instance")), decimal(value.at("revision"))};
}

Json revision_json(SceneRevision value) {
    return {{"scene_instance", std::to_string(value.scene_instance)},
            {"revision", std::to_string(value.revision)}};
}

AuthoredEntityId entity_id(const Json& value) {
    return AuthoredEntityId{take(StableId::parse(value.get_ref<const std::string&>()))};
}

Json optional_id(const std::optional<AuthoredEntityId>& value) {
    return value ? Json(value->value().to_string()) : Json(nullptr);
}

Json vector_json(const math::Vec3& value) {
    return Json::array({value.x, value.y, value.z});
}

Json transform_command(AuthoredEntityId id, const Transform& transform) {
    const std::array numbers{transform.translation.x, transform.translation.y,
                             transform.translation.z, transform.rotation.x,
                             transform.rotation.y,    transform.rotation.z,
                             transform.rotation.w,    transform.scale.x,
                             transform.scale.y,       transform.scale.z};
    if (!std::all_of(numbers.begin(), numbers.end(),
                     [](double value) { return std::isfinite(value); }))
        fail(ErrorCode::invalid_argument, "Transform values must be finite");
    return {{"type", "set_transform"},
            {"entity_id", id.value().to_string()},
            {"transform",
             {{"translation", vector_json(transform.translation)},
              {"rotation", Json::array({transform.rotation.x, transform.rotation.y,
                                        transform.rotation.z, transform.rotation.w})},
              {"scale", vector_json(transform.scale)}}}};
}

Json component_commands(const InspectorValues& values) {
    const auto id = values.selection.id.value().to_string();
    const auto& transform = values.transform;
    // JSON cannot represent nonfinite doubles. Reject before serializing rather
    // than silently converting an invalid input to null.
    const std::array numbers{transform.translation.x, transform.translation.y,
                             transform.translation.z, transform.rotation.x,
                             transform.rotation.y,    transform.rotation.z,
                             transform.rotation.w,    transform.scale.x,
                             transform.scale.y,       transform.scale.z};
    if (!std::all_of(numbers.begin(), numbers.end(),
                     [](double value) { return std::isfinite(value); }))
        fail(ErrorCode::invalid_argument, "Inspector transform values must be finite");
    Json camera = nullptr;
    if (values.camera) {
        const auto& value = *values.camera;
        const std::array camera_numbers{value.vertical_fov_radians, value.orthographic_height,
                                        value.near_clip, value.far_clip};
        if (!std::all_of(camera_numbers.begin(), camera_numbers.end(),
                         [](double number) { return std::isfinite(number); }) ||
            (value.projection != CameraProjection::perspective &&
             value.projection != CameraProjection::orthographic))
            fail(ErrorCode::invalid_argument, "Inspector Camera fields cannot be represented");
        camera = {{"projection", value.projection == CameraProjection::perspective
                                     ? "perspective"
                                     : "orthographic"},
                  {"vertical_fov_radians", value.vertical_fov_radians},
                  {"orthographic_height", value.orthographic_height},
                  {"near_clip", value.near_clip},
                  {"far_clip", value.far_clip}};
    }
    Json mesh = nullptr;
    if (values.mesh_renderer) {
        const auto& value = *values.mesh_renderer;
        mesh = {{"asset_id", value.asset.value().to_string()},
                {"mesh_index", value.mesh_index},
                {"visible", value.visible}};
    }
    return Json::array(
        {{{"type", "set_name"}, {"entity_id", id}, {"name", values.name.value}},
         {{"type", "set_transform"},
          {"entity_id", id},
          {"transform",
           {{"translation", vector_json(transform.translation)},
            {"rotation", Json::array({transform.rotation.x, transform.rotation.y,
                                      transform.rotation.z, transform.rotation.w})},
            {"scale", vector_json(transform.scale)}}}},
         {{"type", "set_camera"}, {"entity_id", id}, {"camera", std::move(camera)}},
         {{"type", "set_mesh_renderer"}, {"entity_id", id}, {"mesh_renderer", std::move(mesh)}}});
}

math::Vec3 vector_value(const Json& value) {
    return {value.at(0).get<double>(), value.at(1).get<double>(), value.at(2).get<double>()};
}

InspectorValues inspector_value(const Json& value, SceneRevision revision) {
    InspectorValues result{{entity_id(value.at("entity_id")), revision},
                           {value.at("name").get<std::string>()},
                           {},
                           {},
                           {}};
    const auto& transform = value.at("transform").at("local");
    const auto& rotation = transform.at("rotation");
    result.transform = {vector_value(transform.at("translation")),
                        {rotation.at(3).get<double>(), rotation.at(0).get<double>(),
                         rotation.at(1).get<double>(), rotation.at(2).get<double>()},
                        vector_value(transform.at("scale"))};
    if (!value.at("camera").is_null()) {
        const auto& camera = value.at("camera");
        const auto projection = camera.at("projection").get<std::string>();
        if (projection != "perspective" && projection != "orthographic")
            fail(ErrorCode::execution_failed, "Inspector received an unknown Camera projection");
        result.camera =
            Camera{projection == "perspective" ? CameraProjection::perspective
                                               : CameraProjection::orthographic,
                   camera.at("vertical_fov_radians").get<double>(),
                   camera.at("orthographic_height").get<double>(),
                   camera.at("near_clip").get<double>(), camera.at("far_clip").get<double>()};
    }
    if (!value.at("mesh_renderer").is_null()) {
        const auto& mesh = value.at("mesh_renderer");
        result.mesh_renderer = MeshRenderer{
            AssetId{take(StableId::parse(mesh.at("asset_id").get<std::string>()))},
            mesh.at("mesh_index").get<std::uint32_t>(), mesh.at("visible").get<bool>()};
    }
    return result;
}

std::optional<tools::AuthoringHistoryGroup> history_group(const Json& value) {
    if (value.is_null())
        return {};
    return tools::AuthoringHistoryGroup{{decimal(value.at("ticket"))},
                                        value.at("command_count").get<std::size_t>()};
}

HierarchyRow decode_row(const Json& value) {
    const auto& hierarchy = value.at("hierarchy");
    HierarchyRow result{entity_id(value.at("entity_id")),
                        value.at("name").get<std::string>(),
                        {},
                        hierarchy.at("depth").get<std::size_t>(),
                        hierarchy.at("child_count").get<std::size_t>(),
                        !value.at("camera").is_null(),
                        !value.at("mesh_renderer").is_null()};
    if (!hierarchy.at("parent_id").is_null())
        result.parent = entity_id(hierarchy.at("parent_id"));
    if (result.name.size() > 256 || result.depth == 0 || result.depth > 1024 ||
        result.child_count > scene_panel_maximum_rows || !value.at("authority").is_null())
        fail(ErrorCode::execution_failed, "Authoring returned an invalid hierarchy row");
    return result;
}

std::vector<HierarchyRow> hierarchy_order(std::vector<HierarchyRow> rows) {
    std::map<AuthoredEntityId, std::size_t> indices;
    for (std::size_t index = 0; index < rows.size(); ++index)
        if (!indices.emplace(rows[index].id, index).second)
            fail(ErrorCode::execution_failed, "Authoring returned duplicate hierarchy IDs");
    std::vector<std::vector<std::size_t>> children(rows.size());
    std::vector<std::size_t> roots;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (!rows[index].parent) {
            roots.push_back(index);
        } else {
            const auto parent = indices.find(*rows[index].parent);
            if (parent == indices.end() || rows[index].depth != rows[parent->second].depth + 1)
                fail(ErrorCode::execution_failed, "Authoring returned an incomplete hierarchy");
            children[parent->second].push_back(index);
        }
    }
    std::vector<std::size_t> pending(roots.rbegin(), roots.rend());
    std::vector<HierarchyRow> ordered;
    ordered.reserve(rows.size());
    while (!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        if (children[index].size() != rows[index].child_count || ordered.size() >= rows.size())
            fail(ErrorCode::execution_failed, "Authoring returned inconsistent hierarchy links");
        pending.insert(pending.end(), children[index].rbegin(), children[index].rend());
        ordered.push_back(std::move(rows[index]));
    }
    if (ordered.size() != rows.size())
        fail(ErrorCode::execution_failed, "Authoring returned an incomplete hierarchy");
    return ordered;
}
} // namespace

struct ScenePanel::Impl {
    struct Pending {
        Json identity;
        std::string input;
        std::optional<AuthoredEntityId> entity;
        bool select_after;
    };

    ProjectSession& session;
    const std::thread::id thread{std::this_thread::get_id()};
    std::vector<HierarchyRow> rows;
    SceneRevision revision{};
    std::optional<SceneSelection> selection;
    std::uint64_t selection_epoch{0};
    std::optional<Pending> pending;
    std::optional<std::string> cursor;
    SceneRevision cursor_revision{};
    std::size_t page{0};
    bool stale{true};

    explicit Impl(ProjectSession& value) : session(value) {}

    void check_thread() const {
        if (thread != std::this_thread::get_id())
            fail(ErrorCode::conflict, "Scene panel operations require the creating thread");
    }

    void select(std::optional<SceneSelection> value) noexcept {
        if ((!selection && !value) || (selection && value && selection->id == value->id &&
                                       selection->revision == value->revision))
            return;
        if (selection_epoch != std::numeric_limits<std::uint64_t>::max())
            ++selection_epoch;
        selection = value;
    }

    bool current() const noexcept {
        return !stale && session.state_current() && session.state().open &&
               revision == session.state().revision;
    }

    const HierarchyRow* find(AuthoredEntityId id) const noexcept {
        const auto found =
            std::find_if(rows.begin(), rows.end(), [&](const auto& row) { return row.id == id; });
        return found == rows.end() ? nullptr : &*found;
    }

    Json call(std::string_view method, const Json& parameters = Json::object()) {
        auto reply = take(session.request(method, parameters.dump()));
        auto value = result_value(reply);
        if (reply.failed)
            fail(ErrorCode::execution_failed, "Authoring rejected the scene observation");
        return value;
    }

    void remember_cursor(const Json& value, SceneRevision expected) {
        if (value.at("next_cursor").is_null())
            cursor.reset();
        else
            cursor = value.at("next_cursor").get<std::string>();
        cursor_revision = expected;
    }

    void finish_previous_query(SceneRevision expected) {
        if (cursor_revision != expected) {
            // Changed mutations/reload clear the protocol's retained cursors.
            cursor.reset();
            return;
        }
        std::size_t scanned = 0;
        for (std::size_t pages = 0; cursor && pages < 64; ++pages) {
            try {
                const auto value = call("scene.continue", {{"cursor", *cursor}});
                remember_cursor(value, expected);
                const auto visited = value.at("scanned_slots").get<std::size_t>();
                if (visited > 4096 || visited > 65536 - scanned)
                    fail(ErrorCode::execution_failed,
                         "Previous hierarchy query exceeds bounded cleanup scans");
                scanned += visited;
            } catch (Failure& failure) {
                // A lost/consumed token is not reusable. Other failures retain
                // the token, preventing refresh from leaking another query.
                if (failure.error.code == ErrorCode::conflict)
                    cursor.reset();
                throw;
            }
        }
        if (cursor)
            fail(ErrorCode::conflict, "Previous hierarchy query exceeds bounded cleanup work");
    }

    void refresh() {
        check_thread();
        stale = true;
        take(session.refresh());
        const auto expected = session.state().revision;
        if (expected != revision || !session.state().open)
            select({});
        if (!session.state().open) {
            rows.clear();
            revision = {};
            page = 0;
            cursor.reset();
            return;
        }
        finish_previous_query(expected);
        std::vector<HierarchyRow> candidate;
        auto value =
            call("scene.query", {{"fields", {"name", "hierarchy", "camera", "mesh_renderer"}},
                                 {"page_size", 256},
                                 {"scan_budget", 4096},
                                 {"expected_revision", revision_json(expected)}});
        std::size_t pages = 0;
        std::size_t scanned = 0;
        for (;;) {
            remember_cursor(value, expected);
            if (++pages > 64 || revision_value(value.at("revision")) != expected ||
                value.at("scope").at("kind") != "authored" ||
                value.at("scope").at("scene_id") != session.state().scene_id)
                fail(ErrorCode::conflict, "Hierarchy query exceeded its scope or bounded work");
            const auto visited = value.at("scanned_slots").get<std::size_t>();
            if (visited > 4096 || visited > 65536 - scanned)
                fail(ErrorCode::conflict, "Hierarchy query exceeded its scan budget");
            scanned += visited;
            const auto& entities = value.at("entities");
            if (!entities.is_array() || entities.size() > 256 ||
                entities.size() > scene_panel_maximum_rows - candidate.size())
                fail(ErrorCode::conflict, "Hierarchy query exceeds 4096 display rows");
            for (const auto& entity : entities)
                candidate.push_back(decode_row(entity));
            if (value.at("complete").get<bool>()) {
                if (!value.at("next_cursor").is_null())
                    fail(ErrorCode::execution_failed, "Complete hierarchy retained a cursor");
                break;
            }
            if (pages == 64 || scanned == 65536 || value.at("next_cursor").is_null())
                fail(ErrorCode::conflict, "Hierarchy query cannot complete within its work bound");
            // Every successful continuation consumes its old token. No token
            // is retained merely because the UI displays only 64 rows at once.
            value = call("scene.continue", {{"cursor", value.at("next_cursor")}});
        }
        candidate = hierarchy_order(std::move(candidate));
        rows = std::move(candidate);
        revision = expected;
        if (selection && !find(selection->id))
            select({});
        const auto pages_available = std::max<std::size_t>(1, (rows.size() + 63) / 64);
        page = std::min(page, pages_available - 1);
        stale = false;
    }

    void require_revision(SceneRevision expected) {
        check_thread();
        if (pending)
            fail(ErrorCode::conflict, "Reconcile the pending mutation before another edit");
        if (!current() || expected != revision)
            fail(ErrorCode::conflict, "Hierarchy changed; refresh and confirm the edit again");
    }

    void require_entity(AuthoredEntityId id) const {
        if (!find(id))
            fail(ErrorCode::conflict, "Entity is absent from this observed hierarchy");
    }

    void require_selection(SceneSelection expected) {
        require_revision(expected.revision);
        require_entity(expected.id);
        if (!selection || selection->id != expected.id || selection->revision != expected.revision)
            fail(ErrorCode::conflict, "Transform selection changed; begin a new drag");
    }

    Json transform_observation(AuthoredEntityId id, SceneRevision expected) {
        const auto value = call("scene.inspect", {{"entity_id", id.value().to_string()},
                                                  {"fields", {"transform", "hierarchy"}},
                                                  {"expected_revision", revision_json(expected)}});
        if (revision_value(value.at("revision")) != expected || value.at("entities").size() != 1 ||
            !value.at("complete").get<bool>() || !value.at("next_cursor").is_null() ||
            value.at("scope").at("kind") != "authored" ||
            value.at("scope").at("scene_id") != session.state().scene_id)
            fail(ErrorCode::execution_failed, "Transform received an inconsistent observation");
        auto entity = value.at("entities").at(0);
        if (entity_id(entity.at("entity_id")) != id || !entity.at("authority").is_null())
            fail(ErrorCode::execution_failed, "Transform received a different entity");
        return entity;
    }

    SceneEditReceipt decode_mutation(const Json& value) {
        if (!pending || value.at("mutation_id") != pending->identity)
            fail(ErrorCode::execution_failed, "Authoring returned another mutation identity");
        const auto state = value.at("state").get<std::string>();
        const auto& outcome = value.at("outcome");
        if (!outcome.at("ok").get<bool>()) {
            const auto error = decode_error(outcome.at("error"));
            if (state == "not_admitted" || state == "not_recorded")
                pending.reset();
            throw Failure{error};
        }
        const auto& retained = outcome.at("value");
        if ((state != "recorded" && state != "replayed" && state != "retained") ||
            retained.at("mutation_id") != pending->identity)
            fail(ErrorCode::execution_failed,
                 "Authoring returned an inconsistent retained outcome");
        const auto& operation = retained.at("result");
        if (!operation.at("ok").get<bool>()) {
            auto error = decode_error(operation.at("error"));
            pending.reset();
            throw Failure{std::move(error)};
        }
        SceneEditReceipt receipt{pending->entity,
                                 operation.at("value").at("changed").get<bool>(),
                                 revision_value(retained.at("observed_revision")),
                                 {}};
        const bool select_after = pending->select_after;
        pending.reset();
        stale = true;
        select({});
        try {
            refresh();
            // Successful create/reparent/component edits may reselect their
            // known surviving entity. External refresh never rebinds selection.
            if (select_after && receipt.entity && revision == receipt.revision &&
                find(*receipt.entity))
                select(SceneSelection{*receipt.entity, revision});
        } catch (Failure& failure) {
            receipt.refresh_error = std::move(failure.error);
        } catch (...) {
            receipt.refresh_error = Error{ErrorCode::execution_failed,
                                          "Edit completed; hierarchy refresh failed",
                                          {.operation = "editor.scene.refresh"}};
        }
        return receipt;
    }

    SceneEditReceipt mutate_operation(std::optional<AuthoredEntityId> entity, Json operation,
                                      SceneRevision expected, bool select_after) {
        require_revision(expected);
        const auto status =
            call("scene.mutation_status", {{"expected_revision", revision_json(expected)}});
        if (status.at("next").is_null())
            fail(ErrorCode::conflict, "Authoring mutation identities are exhausted");
        Json parameters{{"mutation_id", status.at("next")},
                        {"expected_revision", revision_json(expected)},
                        {"operation", std::move(operation)}};
        pending.emplace(Pending{status.at("next"), parameters.dump(), entity, select_after});
        auto reply = take(session.request("scene.mutate", pending->input));
        auto document = Json::parse(reply.json);
        if (document.contains("error")) {
            const auto& error = document.at("error");
            auto fields = error.at("data");
            fields["message"] = error.at("message");
            auto decoded = decode_error(fields);
            if (decoded.context.operation != "authoring.protocol.reconcile")
                pending.reset();
            throw Failure{std::move(decoded)};
        }
        return decode_mutation(document.at("result"));
    }

    SceneEditReceipt mutate(AuthoredEntityId entity, Json command, SceneRevision expected,
                            bool select_after) {
        return mutate_operation(entity, {{"type", "command"}, {"command", std::move(command)}},
                                expected, select_after);
    }
};

ScenePanel::ScenePanel(ProjectSession& session) : impl_(std::make_unique<Impl>(session)) {}
ScenePanel::~ScenePanel() = default;

Result<void> ScenePanel::refresh() {
    return boundary<void>([&] {
        impl_->refresh();
        return Result<void>{};
    });
}
bool ScenePanel::current() const noexcept {
    return impl_->current();
}
SceneRevision ScenePanel::revision() const noexcept {
    return impl_->revision;
}
std::span<const HierarchyRow> ScenePanel::rows() const noexcept {
    return impl_->rows;
}
std::span<const HierarchyRow> ScenePanel::page() const noexcept {
    const auto offset = std::min(impl_->page * scene_panel_page_size, impl_->rows.size());
    return std::span<const HierarchyRow>{impl_->rows}.subspan(
        offset, std::min(scene_panel_page_size, impl_->rows.size() - offset));
}
std::size_t ScenePanel::page_index() const noexcept {
    return impl_->page;
}
std::size_t ScenePanel::page_count() const noexcept {
    return std::max<std::size_t>(1, (impl_->rows.size() + scene_panel_page_size - 1) /
                                        scene_panel_page_size);
}
bool ScenePanel::next_page() noexcept {
    if (impl_->page + 1 >= page_count())
        return false;
    ++impl_->page;
    return true;
}
bool ScenePanel::previous_page() noexcept {
    if (impl_->page == 0)
        return false;
    --impl_->page;
    return true;
}
const std::optional<SceneSelection>& ScenePanel::selection() const noexcept {
    return impl_->selection;
}
std::uint64_t ScenePanel::selection_epoch() const noexcept {
    return impl_->selection_epoch;
}
Result<void> ScenePanel::select(AuthoredEntityId id) {
    return boundary<void>([&] {
        impl_->check_thread();
        if (!impl_->current())
            fail(ErrorCode::conflict, "Refresh the hierarchy before selecting an entity");
        impl_->require_entity(id);
        impl_->select(SceneSelection{id, impl_->revision});
        return Result<void>{};
    });
}
void ScenePanel::clear_selection() noexcept {
    impl_->select({});
}
Result<SceneEditReceipt> ScenePanel::create(std::string_view name,
                                            std::optional<AuthoredEntityId> parent,
                                            SceneRevision expected) {
    return boundary<SceneEditReceipt>([&] {
        impl_->require_revision(expected);
        if (name.size() > 4096)
            fail(ErrorCode::invalid_argument,
                 "Entity name exceeds the retained command input bound");
        if (parent)
            impl_->require_entity(*parent);
        const auto id = AuthoredEntityId{take(generate_stable_id())};
        return impl_->mutate(id,
                             {{"type", "create_entity"},
                              {"entity_id", id.value().to_string()},
                              {"name", name},
                              {"parent_id", optional_id(parent)}},
                             expected, true);
    });
}
Result<SceneEditReceipt> ScenePanel::erase(AuthoredEntityId id, SceneRevision expected) {
    return boundary<SceneEditReceipt>([&] {
        impl_->require_revision(expected);
        impl_->require_entity(id);
        return impl_->mutate(id, {{"type", "delete_entity"}, {"entity_id", id.value().to_string()}},
                             expected, false);
    });
}
Result<SceneEditReceipt> ScenePanel::reparent(AuthoredEntityId id,
                                              std::optional<AuthoredEntityId> parent,
                                              ReparentMode mode, SceneRevision expected) {
    return boundary<SceneEditReceipt>([&] {
        impl_->require_revision(expected);
        impl_->require_entity(id);
        if (parent)
            impl_->require_entity(*parent);
        if (mode != ReparentMode::keep_local && mode != ReparentMode::keep_world)
            fail(ErrorCode::invalid_argument, "Unknown reparent mode");
        return impl_->mutate(
            id,
            {{"type", "reparent_entity"},
             {"entity_id", id.value().to_string()},
             {"parent_id", optional_id(parent)},
             {"mode", mode == ReparentMode::keep_local ? "keep_local" : "keep_world"}},
            expected, true);
    });
}
bool ScenePanel::needs_reconciliation() const noexcept {
    return impl_->pending.has_value();
}

Result<InspectorValues> ScenePanel::inspect(AuthoredEntityId id, SceneRevision expected) {
    return boundary<InspectorValues>([&] {
        impl_->check_thread();
        if (!impl_->current() || expected != impl_->revision)
            fail(ErrorCode::conflict, "Inspector observation changed; refresh explicitly");
        impl_->require_entity(id);
        const auto value = impl_->call(
            "scene.inspect", {{"entity_id", id.value().to_string()},
                              {"fields", {"name", "transform", "camera", "mesh_renderer"}},
                              {"expected_revision", revision_json(expected)}});
        if (revision_value(value.at("revision")) != expected || value.at("entities").size() != 1 ||
            !value.at("complete").get<bool>() || !value.at("next_cursor").is_null() ||
            value.at("scope").at("kind") != "authored" ||
            value.at("scope").at("scene_id") != impl_->session.state().scene_id)
            fail(ErrorCode::execution_failed, "Inspector received an inconsistent observation");
        auto result = inspector_value(value.at("entities").at(0), expected);
        if (result.selection.id != id || !value.at("entities").at(0).at("authority").is_null())
            fail(ErrorCode::execution_failed, "Inspector received a different entity");
        return result;
    });
}

Result<SceneEditReceipt> ScenePanel::apply_components(const InspectorValues& values) {
    return boundary<SceneEditReceipt>([&] {
        impl_->require_revision(values.selection.revision);
        impl_->require_entity(values.selection.id);
        if (values.name.value.size() > 4096)
            fail(ErrorCode::invalid_argument, "Inspector Name exceeds the retained input bound");
        auto commands = component_commands(values);
        return impl_->mutate_operation(
            values.selection.id,
            {{"type", "batch"},
             {"batch",
              {{"id", take(generate_stable_id()).to_string()}, {"commands", std::move(commands)}}}},
            values.selection.revision, true);
    });
}

Result<TransformObservation> ScenePanel::observe_transform(SceneSelection expected) {
    return boundary<TransformObservation>([&] {
        impl_->require_selection(expected);
        const auto value = impl_->transform_observation(expected.id, expected.revision);
        const auto& local = value.at("transform").at("local");
        const auto& rotation = local.at("rotation");
        TransformObservation result{expected,
                                    {vector_value(local.at("translation")),
                                     {rotation.at(3).get<double>(), rotation.at(0).get<double>(),
                                      rotation.at(1).get<double>(), rotation.at(2).get<double>()},
                                     vector_value(local.at("scale"))},
                                    math::Mat4{1.0}};
        const auto& parent = value.at("hierarchy").at("parent_id");
        if (!parent.is_null()) {
            const auto observed =
                impl_->transform_observation(entity_id(parent), expected.revision);
            const auto& world = observed.at("transform").at("world_matrix");
            if (world.at("layout") != "column_major" || world.at("values").size() != 16)
                fail(ErrorCode::execution_failed, "Transform received an invalid parent matrix");
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    result.parent_world[column][row] =
                        world.at("values")
                            .at(static_cast<std::size_t>(column * 4 + row))
                            .get<double>();
        }
        take(validate_transform_matrix(result.parent_world));
        impl_->require_selection(expected);
        return result;
    });
}

Result<void> ScenePanel::preview_transform(SceneSelection expected, const Transform& transform) {
    return boundary<void>([&] {
        impl_->require_selection(expected);
        const auto batch_id = take(generate_stable_id()).to_string();
        const auto parameters =
            Json{{"batch",
                  {{"id", batch_id},
                   {"commands", Json::array({transform_command(expected.id, transform)})}}},
                 {"expected_revision", revision_json(expected.revision)}};
        const auto value =
            result_value(take(impl_->session.request("scene.preview", parameters.dump())));
        if (value.at("batch_id") != batch_id ||
            revision_value(value.at("observed_revision")) != expected.revision)
            fail(ErrorCode::conflict, "Transform preview observation changed");
        const auto& outcome = value.at("outcome");
        if (!outcome.at("ok").get<bool>())
            throw Failure{decode_error(outcome.at("error"))};
        impl_->require_selection(expected);
        return Result<void>{};
    });
}

Result<SceneEditReceipt> ScenePanel::apply_transform(SceneSelection expected,
                                                     const Transform& transform) {
    return boundary<SceneEditReceipt>([&] {
        impl_->require_selection(expected);
        return impl_->mutate(expected.id, transform_command(expected.id, transform),
                             expected.revision, true);
    });
}

Result<tools::AuthoringHistoryStatus> ScenePanel::history(SceneRevision expected) {
    return boundary<tools::AuthoringHistoryStatus>([&] {
        impl_->check_thread();
        const auto value =
            impl_->call("scene.history", {{"expected_revision", revision_json(expected)}});
        if (revision_value(value.at("revision")) != expected)
            fail(ErrorCode::execution_failed, "History received another scene revision");
        const auto& limits = value.at("limits");
        return tools::AuthoringHistoryStatus{expected,
                                             value.at("synchronized").get<bool>(),
                                             value.at("undo_entries").get<std::size_t>(),
                                             value.at("redo_entries").get<std::size_t>(),
                                             value.at("retained_bytes").get<std::size_t>(),
                                             {limits.at("max_entries").get<std::size_t>(),
                                              limits.at("max_retained_bytes").get<std::size_t>()},
                                             history_group(value.at("next_undo")),
                                             history_group(value.at("next_redo"))};
    });
}

Result<SceneEditReceipt> ScenePanel::undo(tools::AuthoringHistoryTicket ticket,
                                          SceneRevision expected) {
    return boundary<SceneEditReceipt>([&] {
        return impl_->mutate_operation(
            {}, {{"type", "undo"}, {"ticket", std::to_string(ticket.value)}}, expected, false);
    });
}
Result<SceneEditReceipt> ScenePanel::redo(tools::AuthoringHistoryTicket ticket,
                                          SceneRevision expected) {
    return boundary<SceneEditReceipt>([&] {
        return impl_->mutate_operation(
            {}, {{"type", "redo"}, {"ticket", std::to_string(ticket.value)}}, expected, false);
    });
}
Result<SceneEditReceipt> ScenePanel::reconcile() {
    return boundary<SceneEditReceipt>([&] {
        impl_->check_thread();
        if (!impl_->pending)
            fail(ErrorCode::conflict, "There is no pending mutation to reconcile");
        auto reply = take(impl_->session.request(
            "scene.mutation_outcome", Json{{"mutation_id", impl_->pending->identity}}.dump()));
        return impl_->decode_mutation(result_value(reply));
    });
}

} // namespace shark::editor
