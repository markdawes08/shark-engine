#include "shark/tools/authoring_protocol.hpp"

#include "authoring_asset_wire.hpp"
#include "authoring_assets.hpp"
#include "authoring_execution_wire.hpp"
#include "authoring_export.hpp"
#include "authoring_run.hpp"
#include "authoring_wire.hpp"
#include "shark/tools/project_files.hpp"
#include "source_json.hpp"
#include "utf8.hpp"

#include <algorithm>
#include <initializer_list>
#include <map>
#include <optional>
#include <thread>
#include <utility>

namespace shark::tools {
namespace {
using Json = nlohmann::json;
namespace wire = authoring_wire;
constexpr std::uint64_t maximum_request_id = 9007199254740991ULL;
constexpr std::size_t maximum_json_nodes = 8192;
constexpr std::size_t maximum_text_revisions = 32;

struct ProtocolFailure {
    int code;
    Error error;
};

[[noreturn]] void fail(int code, ErrorCode engine_code, const char* message,
                       std::string path = {}) {
    throw ProtocolFailure{code,
                          {engine_code,
                           message,
                           {.operation = "authoring.protocol.handle", .path = std::move(path)}}};
}

template <typename T> T take(Result<T> result, int code = -32000) {
    if (!result)
        throw ProtocolFailure{code, std::move(result).error()};
    return std::move(result).value();
}

bool text(const Json& value, std::size_t maximum) {
    if (!value.is_string())
        return false;
    const auto& string = value.get_ref<const std::string&>();
    return !string.empty() && string.size() <= maximum && shark::detail::valid_utf8(string, true);
}

bool request_id(const Json& value) {
    if (value.is_null() || text(value, 128))
        return true;
    if (value.is_number_unsigned())
        return value.get<std::uint64_t>() <= maximum_request_id;
    return value.is_number_integer() && value.get<std::int64_t>() >= 0 &&
           static_cast<std::uint64_t>(value.get<std::int64_t>()) <= maximum_request_id;
}

bool fields(const Json& object, std::initializer_list<std::string_view> allowed) {
    if (!object.is_object())
        return false;
    for (auto item = object.begin(); item != object.end(); ++item) {
        if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end())
            return false;
    }
    return true;
}

void empty_parameters(const Json& parameters) {
    if (!parameters.is_object() || !parameters.empty())
        fail(-32602, ErrorCode::invalid_argument, "This method requires an empty params object",
             "/params");
}

Json object_schema(Json properties, Json required = Json::array()) {
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties", std::move(properties)},
            {"required", std::move(required)}};
}

Json id_schema() {
    return {{"type", "string"},
            {"format", "uuid"},
            {"pattern", "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"}};
}

Json revision_schema() {
    const Json integer{{"type", "string"},
                       {"pattern", "^(0|[1-9][0-9]{0,19})$"},
                       {"maximum_decimal", "18446744073709551615"}};
    return object_schema({{"scene_instance", integer}, {"revision", integer}},
                         {"scene_instance", "revision"});
}

Json status_schema() {
    const Json count{{"type", "integer"}, {"minimum", 0}};
    return {
        {"oneOf",
         {object_schema({{"open", {{"const", false}}}}, {"open"}),
          object_schema(
              {{"open", {{"const", true}}},
               {"project_id", id_schema()},
               {"world_id", id_schema()},
               {"project_name", {{"type", "string"}}},
               {"project_path", {{"type", "string"}}},
               {"scene_id", id_schema()},
               {"revision", revision_schema()},
               {"dirty", {{"type", "boolean"}}},
               {"pending_mutations", {{"type", "integer"}, {"minimum", 0}}},
               {"source_schema_version",
                object_schema({{"project", count}, {"scene", count}}, {"project", "scene"})},
               {"file_revisions", object_schema({{"project_bytes", count}, {"scene_bytes", count}},
                                                {"project_bytes", "scene_bytes"})},
               {"cursors",
                object_schema({{"retained",
                                {{"type", "integer"},
                                 {"minimum", 0},
                                 {"maximum", authoring_protocol_maximum_cursors}}},
                               {"maximum", {{"const", authoring_protocol_maximum_cursors}}}},
                              {"retained", "maximum"})}},
              {"open", "project_id", "world_id", "project_name", "project_path", "scene_id",
               "revision", "dirty", "pending_mutations", "source_schema_version", "file_revisions",
               "cursors"})}}};
}

Json page_schema() {
    const Json count{{"type", "integer"}, {"minimum", 0}};
    const Json scalar{{"type", "number"}, {"finite_only", true}};
    const auto vector = [&](std::size_t size) {
        return Json{{"type", "array"}, {"minItems", size}, {"maxItems", size}, {"items", scalar}};
    };
    const Json scope = object_schema({{"kind", {{"const", "authored"}}}, {"scene_id", id_schema()}},
                                     {"kind", "scene_id"});
    const Json camera = object_schema(
        {{"projection", {{"enum", {"perspective", "orthographic"}}}},
         {"vertical_fov_radians", scalar},
         {"orthographic_height", scalar},
         {"near_clip", scalar},
         {"far_clip", scalar}},
        {"projection", "vertical_fov_radians", "orthographic_height", "near_clip", "far_clip"});
    const Json mesh = object_schema(
        {{"asset_id", id_schema()},
         {"mesh_index", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295ULL}}},
         {"visible", {{"type", "boolean"}}}},
        {"asset_id", "mesh_index", "visible"});
    const Json entity = object_schema(
        {{"entity_id", id_schema()},
         {"scope", scope},
         {"authority", {{"type", "null"}}},
         {"name",
          {{"type", "string"},
           {"minimum_utf8_bytes", 1},
           {"maximum_utf8_bytes", 256},
           {"forbid_controls", true}}},
         {"transform",
          object_schema({{"local", object_schema({{"translation", vector(3)},
                                                  {"rotation", vector(4)},
                                                  {"scale", vector(3)}},
                                                 {"translation", "rotation", "scale"})},
                         {"world_matrix", object_schema({{"layout", {{"const", "column_major"}}},
                                                         {"values", vector(16)}},
                                                        {"layout", "values"})}},
                        {"local", "world_matrix"})},
         {"hierarchy",
          object_schema({{"parent_id", {{"oneOf", {id_schema(), Json{{"type", "null"}}}}}},
                         {"child_count", count},
                         {"depth", count}},
                        {"parent_id", "child_count", "depth"})},
         {"camera", {{"oneOf", {camera, Json{{"type", "null"}}}}}},
         {"mesh_renderer", {{"oneOf", {mesh, Json{{"type", "null"}}}}}}},
        {"entity_id", "scope", "authority"});
    return object_schema(
        {{"revision", revision_schema()},
         {"scope", scope},
         {"entities",
          {{"type", "array"}, {"maxItems", scene_query_maximum_page_size}, {"items", entity}}},
         {"scanned_slots",
          {{"type", "integer"}, {"minimum", 0}, {"maximum", scene_query_maximum_scan_budget}}},
         {"complete", {{"type", "boolean"}}},
         {"next_cursor", {{"oneOf", {id_schema(), Json{{"type", "null"}}}}}}},
        {"revision", "scope", "entities", "scanned_slots", "complete", "next_cursor"});
}

Json discovery(bool export_available, bool capture_available) {
    const auto components = take(wire::component_schemas_json());
    const Json empty = object_schema(Json::object());
    const Json selected{
        {"type", "array"},
        {"uniqueItems", true},
        {"maxItems", 5},
        {"items", {{"enum", {"name", "transform", "hierarchy", "camera", "mesh_renderer"}}}},
        {"default", {"name", "transform", "hierarchy"}}};
    auto query = object_schema(
        {{"selection", {{"enum", {"all", "roots", "children"}}, {"default", "all"}}},
         {"parent_id", id_schema()},
         {"name_prefix",
          {{"type", "string"},
           {"maximum_utf8_bytes", 256},
           {"default", ""},
           {"forbid_controls", true}}},
         {"fields", selected},
         {"page_size", {{"type", "integer"}, {"minimum", 1}, {"maximum", 256}, {"default", 64}}},
         {"scan_budget",
          {{"type", "integer"}, {"minimum", 1}, {"maximum", 4096}, {"default", 256}}},
         {"expected_revision", revision_schema()}});
    query["allOf"] = {
        {{"if",
          {{"properties", {{"selection", {{"const", "children"}}}}}, {"required", {"selection"}}}},
         {"then", {{"required", {"parent_id"}}}},
         {"else", {{"not", {{"required", {"parent_id"}}}}}}}};
    Json methods = Json::array();
    const auto add = [&](std::string name, Json parameters, Json result, Json example) {
        const bool requires_open_project = name.starts_with("scene.") || name.starts_with("text.");
        const bool requires_project_root = name == "project.open" || name == "project.create";
        Json request{{"jsonrpc", "2.0"},
                     {"protocol_version", authoring_protocol_version},
                     {"id", "example"},
                     {"method", name},
                     {"params", std::move(example)}};
        methods.push_back({{"name", std::move(name)},
                           {"execution_context", "cpu_owner_thread"},
                           {"requires_open_project", requires_open_project},
                           {"requires_project_root", requires_project_root},
                           {"params", std::move(parameters)},
                           {"result", std::move(result)},
                           {"example", std::move(request)}});
    };
    add("protocol.discover", empty,
        {{"type", "object"},
         {"required",
          {"protocol_version", "transport", "limits", "capabilities", "methods", "components"}}},
        Json::object());
    add("schema.components", empty,
        object_schema({{"components", {{"type", "array"}, {"minItems", 4}, {"maxItems", 4}}}},
                      {"components"}),
        Json::object());
    add("project.open",
        object_schema({{"path",
                        {{"type", "string"},
                         {"maximum_utf8_bytes", 4096},
                         {"format", "shark-project-relative-path"}}},
                       {"scene_id", id_schema()}},
                      {"path"}),
        status_schema(), {{"path", "FeatureWorld.sharkproject"}});
    add("project.status", empty, status_schema(), Json::object());
    auto close_parameters =
        object_schema({{"expected_revision", revision_schema()},
                       {"unsaved", {{"enum", {"reject", "discard"}}, {"default", "reject"}}}});
    close_parameters["allOf"] = {
        {{"if", {{"properties", {{"unsaved", {{"const", "discard"}}}}}, {"required", {"unsaved"}}}},
         {"then", {{"required", {"expected_revision"}}}}}};
    add("project.close", close_parameters, object_schema({{"open", {{"const", false}}}}, {"open"}),
        Json::object());
    add("scene.query", std::move(query), page_schema(),
        {{"selection", "roots"},
         {"fields", {"name", "camera", "mesh_renderer"}},
         {"page_size", 64}});
    add("scene.inspect",
        object_schema({{"entity_id", id_schema()},
                       {"fields", selected},
                       {"expected_revision", revision_schema()}},
                      {"entity_id"}),
        page_schema(),
        {{"entity_id", "00000000-0000-4000-8000-000000000001"},
         {"fields", {"name", "transform", "hierarchy", "camera", "mesh_renderer"}}});
    add("scene.continue", object_schema({{"cursor", id_schema()}}, {"cursor"}), page_schema(),
        {{"cursor", "00000000-0000-4000-8000-000000000002"}});
    const Json path_schema{{"type", "string"},
                           {"maximum_utf8_bytes", 4096},
                           {"format", "shark-project-relative-path"}};
    const Json publication = object_schema(
        {{"published", {{"const", true}}},
         {"bytes", {{"type", "integer"}, {"minimum", 0}}},
         {"durability",
          {{"enum", {"file_flushed", "file_and_directory_flushed", "directory_flush_failed"}}}}},
        {"published", "bytes", "durability"});
    const Json nullable_publication{{"oneOf", {publication, Json{{"type", "null"}}}}};
    const Json example_revision{{"scene_instance", "1"}, {"revision", "0"}};
    const Json example_identity{{"scope", "00000000-0000-4000-8000-000000000003"},
                                {"sequence", "1"}};
    const Json example_command{{"type", "set_name"},
                               {"entity_id", "00000000-0000-4000-8000-000000000001"},
                               {"name", "Renamed entity"}};
    const Json example_batch{{"id", "00000000-0000-4000-8000-000000000004"},
                             {"commands", Json::array({example_command})}};
    add("project.create",
        object_schema({{"path", path_schema},
                       {"scene_path", path_schema},
                       {"name",
                        {{"type", "string"},
                         {"minimum_utf8_bytes", 1},
                         {"maximum_utf8_bytes", 256},
                         {"forbid_controls", true}}},
                       {"project_id", id_schema()},
                       {"world_id", id_schema()},
                       {"scene_id", id_schema()}},
                      {"path", "scene_path", "name", "project_id", "world_id", "scene_id"}),
        object_schema({{"success", {{"type", "boolean"}}},
                       {"published", object_schema({{"scene", nullable_publication},
                                                    {"project", nullable_publication}},
                                                   {"scene", "project"})},
                       {"project", {{"oneOf", {status_schema(), Json{{"type", "null"}}}}}},
                       {"error", {{"type", {"object", "null"}}}}},
                      {"success", "published", "project", "error"}),
        {{"path", "Game.sharkproject"},
         {"scene_path", "Start.sharkscene"},
         {"name", "Game"},
         {"project_id", "00000000-0000-4000-8000-000000000005"},
         {"world_id", "00000000-0000-4000-8000-000000000006"},
         {"scene_id", "00000000-0000-4000-8000-000000000007"}});
    add("scene.preview", wire::preview_params_schema(), wire::batch_reply_schema(),
        {{"batch", example_batch}, {"expected_revision", example_revision}});
    add("scene.mutate", wire::mutation_params_schema(), wire::mutation_reply_schema(),
        {{"mutation_id", example_identity},
         {"expected_revision", example_revision},
         {"operation", {{"type", "command"}, {"command", example_command}}}});
    const Json observed_params = object_schema({{"expected_revision", revision_schema()}});
    add("scene.history", observed_params, wire::history_schema(), Json::object());
    add("scene.mutation_status", observed_params, wire::mutation_status_schema(), Json::object());
    const Json mutation_identity = object_schema(
        {{"scope", id_schema()}, {"sequence", revision_schema().at("properties").at("revision")}},
        {"scope", "sequence"});
    add("scene.mutation_outcome",
        object_schema({{"mutation_id", mutation_identity}}, {"mutation_id"}),
        wire::mutation_reply_schema(), {{"mutation_id", example_identity}});
    add("scene.save",
        object_schema({{"expected_revision", revision_schema()}}, {"expected_revision"}),
        object_schema({{"success", {{"const", true}}},
                       {"saved_revision", revision_schema()},
                       {"current_scene_clean", {{"type", "boolean"}}},
                       {"publication", publication}},
                      {"success", "saved_revision", "current_scene_clean", "publication"}),
        {{"expected_revision", example_revision}});
    add("scene.reload",
        object_schema({{"expected_revision", revision_schema()},
                       {"unsaved", {{"enum", {"reject", "discard"}}, {"default", "reject"}}}},
                      {"expected_revision"}),
        status_schema(), {{"expected_revision", example_revision}, {"unsaved", "reject"}});
    const Json script_path{{"type", "string"},
                           {"maximum_utf8_bytes", 4096},
                           {"format", "shark-project-relative-path"},
                           {"scope", "scripts/**/*.lua"}};
    const Json script_text{{"type", "string"},
                           {"maximum_utf8_bytes", project_text_maximum_bytes},
                           {"forbid_nul", true}};
    add("text.read", object_schema({{"path", script_path}}, {"path"}),
        object_schema({{"path", script_path}, {"text", script_text}, {"revision", id_schema()}},
                      {"path", "text", "revision"}),
        {{"path", "scripts/main.lua"}});
    add("text.write",
        object_schema({{"path", script_path},
                       {"text", script_text},
                       {"expected_revision", {{"oneOf", {id_schema(), Json{{"type", "null"}}}}}}},
                      {"path", "text", "expected_revision"}),
        object_schema(
            {{"path", script_path}, {"revision", id_schema()}, {"publication", publication}},
            {"path", "revision", "publication"}),
        {{"path", "scripts/main.lua"},
         {"text", "-- Project script source\n"},
         {"expected_revision", nullptr}});
    add("text.release", object_schema({{"revision", id_schema()}}, {"revision"}),
        object_schema({{"released", {{"const", true}}}}, {"released"}),
        {{"revision", "00000000-0000-4000-8000-000000000008"}});
    for (auto& method : authoring_asset_wire::methods_json())
        methods.push_back(std::move(method));
    for (auto& method : authoring_execution_wire::methods(export_available, capture_available))
        methods.push_back(std::move(method));
    return {{"protocol_version", authoring_protocol_version},
            {"transport",
             {{"framing", "newline-delimited-json"}, {"batch", false}, {"notifications", true}}},
            {"limits",
             {{"request_bytes", authoring_protocol_maximum_request_bytes},
              {"response_bytes", authoring_protocol_maximum_response_bytes},
              {"retained_cursors", authoring_protocol_maximum_cursors},
              {"decoded_string_bytes", project_text_maximum_bytes},
              {"script_text_bytes", project_text_maximum_bytes},
              {"retained_text_revisions", maximum_text_revisions},
              {"retained_metadata_revisions", authoring_asset_maximum_metadata_revisions},
              {"retained_import_jobs", asset_import_maximum_jobs}}},
            {"capabilities",
             {{"cpu_authoring", true},
              {"device_capture", capture_available},
              {"mutations", true},
              {"imports", true},
              {"background_jobs", true},
              {"scene_runs", true},
              {"standalone_export", export_available}}},
            {"methods", std::move(methods)},
            {"components", components}};
}

struct Cursor {
    SceneQueryCursor value;
    SceneQueryFields fields;
};

Json error_json(const Error& error) {
    return {{"code", error_code_name(error.code)},
            {"message", error.message},
            {"context",
             {{"operation", error.context.operation},
              {"request_id", error.context.request_id},
              {"path", error.context.path},
              {"entity_id", error.context.entity_id},
              {"asset_id", error.context.asset_id}}}};
}

Json publication_json(const FileCommit& commit) {
    const char* durability = "directory_flush_failed";
    if (commit.durability == FileDurability::file_flushed)
        durability = "file_flushed";
    else if (commit.durability == FileDurability::file_and_directory_flushed)
        durability = "file_and_directory_flushed";
    return {
        {"published", true}, {"bytes", commit.revision.byte_count()}, {"durability", durability}};
}

std::string folded_path(const ProjectPath& path) {
    auto value = path.text();
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    }
    return value;
}

bool overlaps(const ProjectPath& left, const ProjectPath& right) {
    const auto a = folded_path(left);
    const auto b = folded_path(right);
    return a == b || a.starts_with(b + '/') || b.starts_with(a + '/');
}

ProjectPath parameter_path(const Json& parameters, const char* key) {
    if (!parameters.contains(key) || !text(parameters.at(key), 4096))
        fail(-32602, ErrorCode::invalid_path, "Expected a bounded relative project path",
             std::string{"/params/"} + key);
    return take(ProjectPath::parse(parameters.at(key).get_ref<const std::string&>()), -32602);
}

StableId parameter_id(const Json& parameters, const char* key) {
    if (!parameters.contains(key) || !parameters.at(key).is_string())
        fail(-32602, ErrorCode::invalid_id, "Expected a stable UUID string",
             std::string{"/params/"} + key);
    return take(StableId::parse(parameters.at(key).get_ref<const std::string&>()), -32602);
}

struct Session {
    // Reverse destruction joins asset work and releases scene/cursors before the
    // native project lock. Asset destruction never drains/publishes a job.
    std::unique_ptr<ProjectFiles> files;
    ProjectPath path;
    ProjectDocument project;
    std::uint32_t project_schema_version;
    std::optional<FileRevision> project_revision;
    std::unique_ptr<AuthoredScene> owner;
    std::uint32_t scene_schema_version;
    std::optional<FileRevision> scene_revision;
    SceneId scene;
    std::map<std::string, Cursor> cursors;
    struct TextRevision {
        ProjectPath path;
        std::optional<FileRevision> revision;
    };
    std::map<std::string, TextRevision> text_revisions;
    std::unique_ptr<AuthoringAssets> assets{};
};

Json status(const Session* session) {
    if (session == nullptr)
        return {{"open", false}};
    const auto state = take(session->owner->status());
    const auto& project = session->project;
    return {
        {"open", true},
        {"project_id", project.project.value().to_string()},
        {"world_id", project.world.value().to_string()},
        {"project_name", project.name.value},
        {"project_path", session->path.text()},
        {"scene_id", session->scene.value().to_string()},
        {"revision", wire::revision_json(state.revision)},
        {"dirty", state.dirty},
        {"pending_mutations", state.pending_mutations},
        {"source_schema_version",
         {{"project", session->project_schema_version}, {"scene", session->scene_schema_version}}},
        {"file_revisions",
         {{"project_bytes",
           session->project_revision ? session->project_revision->byte_count() : 0},
          {"scene_bytes", session->scene_revision ? session->scene_revision->byte_count() : 0}}},
        {"cursors",
         {{"retained", session->cursors.size()}, {"maximum", authoring_protocol_maximum_cursors}}}};
}

struct CursorPublication {
    std::string token;
    Cursor cursor;
};

struct TextPublication {
    std::string token;
    Session::TextRevision value;
};

struct Dispatch {
    Json result;
    std::unique_ptr<Session> opening{};
    bool closing{false};
    std::optional<CursorPublication> cursor{};
    std::optional<std::string> consumed_cursor{};
    std::optional<TextPublication> text_revision{};
    std::optional<std::string> consumed_text_revision{};
    bool failed{false};
    std::optional<AuthoringAssetReply> assets{};
};

} // namespace

struct AuthoringProtocol::Impl {
    const std::thread::id thread{std::this_thread::get_id()};
    std::filesystem::path root;
    std::filesystem::path export_python;
    std::filesystem::path export_tool;
    AuthoringCaptureProvider* capture_provider;
    std::unique_ptr<Session> session;
    bool outcome_may_have_changed{false};

    explicit Impl(AuthoringProtocolOptions value)
        : root(std::move(value.root)), export_python(std::move(value.export_python)),
          export_tool(std::move(value.export_tool)), capture_provider(value.capture) {}

    bool export_available() const noexcept {
#ifdef _WIN32
        return !export_python.empty() && !export_tool.empty();
#else
        return false;
#endif
    }

    AuthoringAssets& assets(Session& current) {
        if (!current.assets)
            current.assets =
                std::make_unique<AuthoringAssets>(*current.files, current.project, current.path);
        return *current.assets;
    }

    void check_source_files(Session& current) {
        check_project_file(current);
        const auto observed = take(current.files->read_scene(current.project, current.scene));
        if (!current.scene_revision || observed.revision != *current.scene_revision)
            fail(-32000, ErrorCode::conflict,
                 "Scene source changed externally; preserve current edits and reconcile before "
                 "producing artifacts");
    }

    void check_output_path(const std::filesystem::path& output) {
        try {
            if (output.empty())
                fail(-32602, ErrorCode::invalid_path,
                     "Artifact output must name a fresh directory");
            const auto absolute = std::filesystem::absolute(output).lexically_normal();
            if (absolute.filename().empty() || std::filesystem::exists(absolute))
                fail(-32000, ErrorCode::conflict,
                     "Artifact output already exists or has no directory name");
            auto parent = std::filesystem::canonical(absolute.parent_path());
            while (!parent.empty()) {
                if (std::filesystem::equivalent(parent, root))
                    fail(-32000, ErrorCode::path_outside_root,
                         "Artifact output must be outside the source project root");
                const auto next = parent.parent_path();
                if (next == parent)
                    break;
                parent = next;
            }
        } catch (const std::filesystem::filesystem_error&) {
            const auto path = output.u8string();
            fail(-32000, ErrorCode::invalid_path,
                 "Artifact output needs an accessible existing parent directory",
                 {reinterpret_cast<const char*>(path.data()), path.size()});
        }
    }

    Session& require_session() {
        if (!session)
            fail(-32000, ErrorCode::conflict,
                 "No project is open; open a project and restart the query at its observed "
                 "revision");
        return *session;
    }

    void check_project_file(Session& current) {
        const auto observed = take(current.files->read_project(current.path));
        if (!current.project_revision || observed.revision != *current.project_revision)
            fail(-32000, ErrorCode::conflict,
                 "Project source changed externally; preserve unsaved state and explicitly "
                 "close/open",
                 current.path.text());
    }

    void check_text_path(const Session& current, const ProjectPath& path) {
        if (overlaps(path, current.path))
            fail(-32000, ErrorCode::conflict, "Text access overlaps an owned project source path",
                 path.text());
        for (const auto& scene : current.project.scenes) {
            if (overlaps(path, scene.path))
                fail(-32000, ErrorCode::conflict, "Text access overlaps an owned scene source path",
                     path.text());
        }
    }

    Dispatch page(const SceneQueryPage& value, SceneQueryFields selected,
                  std::optional<std::string> consumed = std::nullopt) {
        auto& current = require_session();
        Dispatch result;
        result.consumed_cursor = std::move(consumed);
        std::optional<std::string_view> token;
        if (value.next_cursor()) {
            if (!result.consumed_cursor &&
                current.cursors.size() >= authoring_protocol_maximum_cursors)
                fail(-32000, ErrorCode::conflict,
                     "Outstanding cursor limit reached; finish a query or close the project");
            auto identity = take(generate_stable_id()).to_string();
            if (current.cursors.contains(identity))
                fail(-32000, ErrorCode::conflict, "Generated cursor identity already exists");
            result.cursor.emplace(
                CursorPublication{std::move(identity), Cursor{*value.next_cursor(), selected}});
            token = result.cursor->token;
        }
        const auto scene = take(current.owner->scene());
        result.result = take(wire::page_json(scene.get(), value, selected, token));
        return result;
    }

    Dispatch dispatch(std::string_view method, const Json& parameters) {
        if (method == "scene.run") {
            auto& current = require_session();
            const auto request = take(authoring_execution_wire::parse_run(parameters), -32602);
            auto result = take(run_authoring_scene(*current.owner, request));
            Dispatch reply{
                Json{{"observed_revision", wire::revision_json(result.observed_revision)},
                     {"dirty", result.dirty},
                     {"entity_count", result.entity_count},
                     {"source_digest", result.source_digest},
                     {"report", Json::parse(result.report_json)}}};
            reply.failed = result.status != SceneRunStatus::passed;
            return reply;
        }
        if (method == "scene.export" || method == "scene.capture") {
            if ((method == "scene.export" && !export_available()) ||
                (method == "scene.capture" && !capture_provider))
                fail(-32000, ErrorCode::capability_unavailable,
                     "This host has no configured adapter for the requested artifact operation");
            auto& current = require_session();
            const auto request = take(authoring_execution_wire::parse_content(parameters), -32602);
            const bool exporting = method == "scene.export";
            const auto export_request =
                exporting ? std::optional{take(authoring_execution_wire::parse_export(parameters),
                                               -32602)}
                          : std::nullopt;
            const auto capture_request =
                !exporting ? std::optional{take(authoring_execution_wire::parse_capture(parameters),
                                                -32602)}
                           : std::nullopt;
            check_output_path(exporting ? export_request->output : capture_request->output);
            check_source_files(current);
            auto& asset_owner = assets(current);
            const auto candidate = asset_owner.prepare_export(*current.owner, request);
            const auto validate = [&]() -> Result<void> {
                try {
                    check_source_files(current);
                    return asset_owner.validate_export(candidate, *current.owner);
                } catch (ProtocolFailure& failure) {
                    return std::move(failure.error);
                } catch (AuthoringAssetFailure& failure) {
                    return std::move(failure.error);
                } catch (...) {
                    return Error{ErrorCode::execution_failed,
                                 "Cannot validate retained artifact owner",
                                 {.operation = "authoring.artifact.validate"}};
                }
            };
            outcome_may_have_changed = true;
            auto artifact =
                take(exporting ? export_authoring_scene(candidate, root, export_python, export_tool,
                                                        *export_request, validate)
                               : capture_provider->capture(candidate, *capture_request, validate));
            if (artifact.json.size() > authoring_protocol_maximum_response_bytes)
                fail(-32000, ErrorCode::execution_failed,
                     "Artifact report exceeds response bounds; inspect output before retrying");
            auto encoded = Json::parse(artifact.json);
            if (!encoded.is_object() || !encoded.contains("published") ||
                !encoded.at("published").is_boolean() ||
                encoded.at("published").get<bool>() != artifact.published)
                fail(-32000, ErrorCode::execution_failed,
                     "Artifact adapter returned an inconsistent publication outcome; reconcile "
                     "output");
            Dispatch reply{std::move(encoded)};
            reply.failed = !artifact.succeeded;
            return reply;
        }
        if (AuthoringAssets::supports(method)) {
            auto& current = require_session();
            if (AuthoringAssets::requires_project_fence(method))
                check_project_file(current);
            auto reply = assets(current).dispatch(method, parameters, outcome_may_have_changed);
            Dispatch prepared{std::move(reply.result)};
            prepared.assets = std::move(reply);
            return prepared;
        }
        if (method == "protocol.discover") {
            empty_parameters(parameters);
            return {discovery(export_available(), capture_provider != nullptr)};
        }
        if (method == "schema.components") {
            empty_parameters(parameters);
            return {Json{{"components", take(wire::component_schemas_json())}}};
        }
        if (method == "project.status") {
            empty_parameters(parameters);
            return {status(session.get())};
        }
        if (method == "project.close") {
            if (!fields(parameters, {"expected_revision", "unsaved"}))
                fail(-32602, ErrorCode::invalid_argument, "Unknown close parameter", "/params");
            std::optional<SceneRevision> expected;
            if (parameters.contains("expected_revision"))
                expected = take(wire::parse_revision(parameters.at("expected_revision")), -32602);
            bool discard = false;
            if (parameters.contains("unsaved")) {
                if (parameters.at("unsaved") == "discard")
                    discard = true;
                else if (parameters.at("unsaved") != "reject")
                    fail(-32602, ErrorCode::invalid_argument, "Unknown unsaved policy",
                         "/params/unsaved");
            }
            if (discard && !expected)
                fail(-32602, ErrorCode::invalid_argument,
                     "Discard close requires expected_revision", "/params/expected_revision");
            if (session) {
                const auto state = take(session->owner->status());
                if (expected && *expected != state.revision)
                    fail(-32000, ErrorCode::conflict,
                         "Close revision is stale or belongs to another scene");
                if ((state.dirty && !discard) || state.pending_mutations != 0)
                    fail(-32000, ErrorCode::conflict,
                         "Cannot close a project with unsaved or pending authoring changes");
            } else if (expected) {
                fail(-32000, ErrorCode::conflict,
                     "No project is open at the expected close revision");
            }
            return {Json{{"open", false}}, {}, true};
        }
        if (method == "project.create") {
            if (!fields(parameters,
                        {"path", "scene_path", "name", "project_id", "world_id", "scene_id"}) ||
                !parameters.contains("name") || !text(parameters.at("name"), 256))
                fail(-32602, ErrorCode::invalid_argument,
                     "project.create requires paths, project/world/scene IDs and a name",
                     "/params");
            auto path = parameter_path(parameters, "path");
            const auto scene_path = parameter_path(parameters, "scene_path");
            if (overlaps(path, scene_path))
                fail(-32602, ErrorCode::invalid_path, "Project and scene paths overlap",
                     "/params/scene_path");
            const ProjectId project_id{parameter_id(parameters, "project_id")};
            const WorldId world_id{parameter_id(parameters, "world_id")};
            const SceneId scene_id{parameter_id(parameters, "scene_id")};
            if (session)
                fail(-32000, ErrorCode::conflict,
                     "Close the active project before creating another");
            if (root.empty())
                fail(-32000, ErrorCode::invalid_configuration,
                     "project.create requires a configured root");
            ProjectDocument project{project_id,
                                    world_id,
                                    {parameters.at("name").get<std::string>()},
                                    scene_id,
                                    {{scene_id, scene_path}}};
            SceneDocument scene{project_id, scene_id, {}, {}};
            project = take(validate_project_document(project));
            scene = take(validate_scene_document(scene));
            auto owner = take(AuthoredScene::create(project, scene));
            auto files = take(ProjectFiles::open(root));
            if (take(files->asset_source_exists(path)) ||
                take(files->asset_source_exists(scene_path)))
                fail(-32000, ErrorCode::conflict,
                     "Project creation requires both destination files to be absent");
            auto candidate = std::make_unique<Session>(Session{std::move(files),
                                                               std::move(path),
                                                               std::move(project),
                                                               source_document_version,
                                                               {},
                                                               std::move(owner),
                                                               source_document_version,
                                                               {},
                                                               scene_id,
                                                               {},
                                                               {}});
            Json result{{"success", false},
                        {"published", {{"scene", nullptr}, {"project", nullptr}}},
                        {"project", nullptr},
                        {"error", nullptr}};
            auto scene_commit =
                candidate->files->write_scene(candidate->project, scene, std::nullopt);
            if (!scene_commit) {
                result["error"] = error_json(scene_commit.error());
                Dispatch rejected{std::move(result)};
                rejected.failed = true;
                return rejected;
            }
            outcome_may_have_changed = true;
            candidate->scene_revision = scene_commit.value().revision;
            result["published"]["scene"] = publication_json(scene_commit.value());
            auto project_commit =
                candidate->files->write_project(candidate->path, candidate->project, std::nullopt);
            if (!project_commit) {
                result["error"] = error_json(project_commit.error());
                Dispatch rejected{std::move(result)};
                rejected.failed = true;
                return rejected;
            }
            candidate->project_revision = project_commit.value().revision;
            result["published"]["project"] = publication_json(project_commit.value());
            result["success"] = true;
            result["project"] = status(candidate.get());
            return {std::move(result), std::move(candidate)};
        }
        if (method == "project.open") {
            if (!fields(parameters, {"path", "scene_id"}) || !parameters.contains("path") ||
                !text(parameters.at("path"), 4096))
                fail(-32602, ErrorCode::invalid_argument,
                     "project.open requires a relative path and optional scene_id", "/params");
            auto path = take(
                ProjectPath::parse(parameters.at("path").get_ref<const std::string&>()), -32602);
            std::optional<SceneId> selected;
            if (parameters.contains("scene_id")) {
                if (!parameters.at("scene_id").is_string())
                    fail(-32602, ErrorCode::invalid_id, "scene_id must be a UUID string",
                         "/params/scene_id");
                selected = SceneId{
                    take(StableId::parse(parameters.at("scene_id").get_ref<const std::string&>()),
                         -32602)};
            }
            if (session)
                fail(-32000, ErrorCode::conflict,
                     "A project is already open; close it explicitly first");
            if (root.empty())
                fail(-32000, ErrorCode::invalid_configuration,
                     "project.open requires a project root configured at service creation");
            auto files = take(ProjectFiles::open(root));
            auto source = take(files->read_project(path));
            const auto scene = selected.value_or(source.source.document.startup_scene);
            auto loaded = take(files->load_scene(source.source.document, scene));
            auto candidate = std::make_unique<Session>(Session{std::move(files),
                                                               std::move(path),
                                                               std::move(source.source.document),
                                                               source.source.source_schema_version,
                                                               std::move(source.revision),
                                                               std::move(loaded.owner),
                                                               loaded.source_schema_version,
                                                               std::move(loaded.file_revision),
                                                               scene,
                                                               {},
                                                               {}});
            auto reply = status(candidate.get());
            return {std::move(reply), std::move(candidate)};
        }
        if (method == "scene.preview") {
            const auto parsed = take(wire::parse_preview(parameters), -32602);
            auto& current = require_session();
            const auto reply = take(current.owner->preview(parsed.batch, parsed.expected_revision));
            Dispatch result{wire::batch_reply_json(reply)};
            result.failed = !reply.outcome;
            return result;
        }
        if (method == "scene.mutate") {
            const auto parsed = take(wire::parse_mutation(parameters), -32602);
            auto& current = require_session();
            const auto before = take(current.owner->status()).revision;
            outcome_may_have_changed = true;
            const auto reply = take(current.owner->mutate(parsed));
            if (reply.observed_revision != before)
                current.cursors.clear();
            Dispatch result{wire::mutation_reply_json(reply)};
            result.failed = !reply.outcome || !reply.outcome.value()->result;
            return result;
        }
        if (method == "scene.history") {
            const auto expected = take(wire::parse_optional_revision(parameters), -32602);
            auto& current = require_session();
            return {wire::history_json(take(current.owner->history(expected)))};
        }
        if (method == "scene.mutation_status") {
            const auto expected = take(wire::parse_optional_revision(parameters), -32602);
            auto& current = require_session();
            return {wire::mutation_status_json(take(current.owner->mutation_status(expected)))};
        }
        if (method == "scene.mutation_outcome") {
            if (!fields(parameters, {"mutation_id"}) || !parameters.contains("mutation_id"))
                fail(-32602, ErrorCode::invalid_argument, "Expected one mutation_id", "/params");
            const auto identity =
                take(wire::parse_mutation_id(parameters.at("mutation_id")), -32602);
            auto& current = require_session();
            const auto reply = take(current.owner->mutation_outcome(identity));
            Dispatch result{wire::mutation_reply_json(reply)};
            result.failed = !reply.outcome || !reply.outcome.value()->result;
            return result;
        }
        if (method == "scene.save") {
            if (!fields(parameters, {"expected_revision"}) ||
                !parameters.contains("expected_revision"))
                fail(-32602, ErrorCode::invalid_argument, "Saving requires expected_revision",
                     "/params");
            const auto expected =
                take(wire::parse_revision(parameters.at("expected_revision")), -32602);
            auto& current = require_session();
            check_project_file(current);
            outcome_may_have_changed = true;
            const auto saved =
                take(current.files->save_scene(*current.owner, expected, current.scene_revision));
            current.scene_revision = saved.file.revision;
            current.scene_schema_version = source_document_version;
            return {Json{{"success", true},
                         {"saved_revision", wire::revision_json(saved.saved_revision)},
                         {"current_scene_clean", saved.current_scene_clean},
                         {"publication", publication_json(saved.file)}}};
        }
        if (method == "scene.reload") {
            if (!fields(parameters, {"expected_revision", "unsaved"}) ||
                !parameters.contains("expected_revision"))
                fail(-32602, ErrorCode::invalid_argument,
                     "Reload requires expected_revision and optional unsaved policy", "/params");
            const auto expected =
                take(wire::parse_revision(parameters.at("expected_revision")), -32602);
            auto unsaved = UnsavedScenePolicy::reject;
            if (parameters.contains("unsaved")) {
                if (parameters.at("unsaved") == "discard")
                    unsaved = UnsavedScenePolicy::discard;
                else if (parameters.at("unsaved") != "reject")
                    fail(-32602, ErrorCode::invalid_argument, "Unknown unsaved policy",
                         "/params/unsaved");
            }
            auto& current = require_session();
            check_project_file(current);
            outcome_may_have_changed = true;
            const auto loaded =
                take(current.files->reload_scene(*current.owner, expected, unsaved));
            current.scene_revision = loaded.file_revision;
            current.scene_schema_version = loaded.source_schema_version;
            current.cursors.clear();
            return {status(&current)};
        }
        if (method == "text.read") {
            if (!fields(parameters, {"path"}))
                fail(-32602, ErrorCode::invalid_argument, "Text read requires one path", "/params");
            auto path = parameter_path(parameters, "path");
            auto& current = require_session();
            check_project_file(current);
            check_text_path(current, path);
            if (current.text_revisions.size() >= maximum_text_revisions)
                fail(-32000, ErrorCode::conflict,
                     "Text revision limit reached; release an unused token or reuse a retained "
                     "token");
            auto value = take(current.files->read_text(path));
            auto token = take(generate_stable_id()).to_string();
            if (current.text_revisions.contains(token))
                fail(-32000, ErrorCode::conflict, "Generated text revision already exists");
            Json result{
                {"path", path.text()}, {"text", std::move(value.text)}, {"revision", token}};
            Dispatch prepared{std::move(result)};
            prepared.text_revision.emplace(
                TextPublication{std::move(token), {std::move(path), std::move(value.revision)}});
            return prepared;
        }
        if (method == "text.release") {
            if (!fields(parameters, {"revision"}) || !parameters.contains("revision") ||
                !parameters.at("revision").is_string())
                fail(-32602, ErrorCode::invalid_argument,
                     "Text release requires one revision token", "/params/revision");
            const auto token =
                take(StableId::parse(parameters.at("revision").get_ref<const std::string&>()),
                     -32602)
                    .to_string();
            auto& current = require_session();
            if (!current.text_revisions.contains(token))
                fail(-32000, ErrorCode::conflict,
                     "Text revision was consumed, closed or belongs to another owner");
            Dispatch result{Json{{"released", true}}};
            result.consumed_text_revision = token;
            return result;
        }
        if (method == "text.write") {
            if (!fields(parameters, {"path", "text", "expected_revision"}) ||
                !parameters.contains("text") || !parameters.at("text").is_string() ||
                !parameters.contains("expected_revision"))
                fail(-32602, ErrorCode::invalid_argument,
                     "Text write requires path, text and expected_revision (token or null)",
                     "/params");
            auto path = parameter_path(parameters, "path");
            const auto& contents = parameters.at("text").get_ref<const std::string&>();
            if (contents.size() > project_text_maximum_bytes)
                fail(-32602, ErrorCode::invalid_argument, "Script text exceeds 32768 bytes",
                     "/params/text");
            auto& current = require_session();
            check_project_file(current);
            check_text_path(current, path);
            std::optional<std::string> consumed;
            std::optional<FileRevision> expected;
            if (!parameters.at("expected_revision").is_null()) {
                if (!parameters.at("expected_revision").is_string())
                    fail(-32602, ErrorCode::invalid_argument,
                         "Expected a text revision token or null", "/params/expected_revision");
                consumed =
                    take(StableId::parse(
                             parameters.at("expected_revision").get_ref<const std::string&>()),
                         -32602)
                        .to_string();
                const auto found = current.text_revisions.find(*consumed);
                if (found == current.text_revisions.end() || found->second.path != path ||
                    !found->second.revision)
                    fail(-32000, ErrorCode::conflict,
                         "Text revision was consumed, closed or belongs to another path/owner",
                         path.text());
                expected = found->second.revision;
            }
            if (!consumed && current.text_revisions.size() >= maximum_text_revisions)
                fail(-32000, ErrorCode::conflict,
                     "Text revision limit reached; release an unused token or reuse a retained "
                     "token");
            auto token = take(generate_stable_id()).to_string();
            if (current.text_revisions.contains(token))
                fail(-32000, ErrorCode::conflict, "Generated text revision already exists");
            const auto inserted =
                current.text_revisions.emplace(token, Session::TextRevision{path, {}});
            auto committed = [&] {
                try {
                    return current.files->write_text(path, contents, expected);
                } catch (...) {
                    current.text_revisions.erase(inserted.first);
                    throw;
                }
            }();
            if (!committed) {
                current.text_revisions.erase(inserted.first);
                static_cast<void>(take(std::move(committed)));
            }
            outcome_may_have_changed = true;
            inserted.first->second.revision = committed.value().revision;
            if (consumed)
                current.text_revisions.erase(*consumed);
            return {Json{{"path", path.text()},
                         {"revision", token},
                         {"publication", publication_json(committed.value())}}};
        }
        if (method == "scene.query") {
            auto parsed = take(wire::parse_query(parameters), -32602);
            auto& current = require_session();
            if (current.cursors.size() >= authoring_protocol_maximum_cursors)
                fail(-32000, ErrorCode::conflict,
                     "Outstanding cursor limit reached; finish a query or close the project");
            if (parsed.parent_id) {
                const auto scene = take(current.owner->scene());
                parsed.options.parent = take(scene.get().find(*parsed.parent_id));
            }
            const auto result = take(current.owner->query_entities(parsed.options));
            return page(result, parsed.options.fields);
        }
        if (method == "scene.inspect") {
            const auto parsed = take(wire::parse_inspect(parameters), -32602);
            auto& current = require_session();
            const auto result = take(current.owner->inspect_entity(parsed.entity_id, parsed.fields,
                                                                   parsed.expected_revision));
            return page(result, parsed.fields);
        }
        if (method == "scene.continue") {
            if (!fields(parameters, {"cursor"}) || !parameters.contains("cursor") ||
                !parameters.at("cursor").is_string())
                fail(-32602, ErrorCode::invalid_argument,
                     "scene.continue requires one opaque cursor string", "/params/cursor");
            const auto identity =
                take(StableId::parse(parameters.at("cursor").get_ref<const std::string&>()), -32602)
                    .to_string();
            auto& current = require_session();
            const auto found = current.cursors.find(identity);
            if (found == current.cursors.end())
                fail(-32000, ErrorCode::conflict,
                     "Cursor was consumed, closed or belongs to another query owner; restart the "
                     "query at the observed revision");
            const auto result = take(current.owner->continue_query(found->second.value));
            return page(result, found->second.fields, identity);
        }
        fail(-32601, ErrorCode::invalid_argument, "Unknown authoring protocol method", "/method");
    }

    Result<AuthoringProtocolReply> failure(int code, Error error, const Json& id,
                                           bool notification) const {
        if (notification)
            return AuthoringProtocolReply{{}, true};
        if (error.context.operation.empty())
            error.context.operation = "authoring.protocol.handle";
        if (error.context.request_id.empty() && !id.is_null())
            error.context.request_id = id.is_string() ? id.get<std::string>() : id.dump();
        Json data{{"code", error_code_name(error.code)},
                  {"context",
                   {{"operation", error.context.operation},
                    {"request_id", error.context.request_id},
                    {"path", error.context.path},
                    {"entity_id", error.context.entity_id},
                    {"asset_id", error.context.asset_id}}},
                  {"protocol_version", authoring_protocol_version}};
        if (code == -32001)
            data["supported_versions"] = Json::array({authoring_protocol_version});
        if (session) {
            const auto state = session->owner->status();
            if (state)
                data["current_revision"] = wire::revision_json(state.value().revision);
        }
        Json response{
            {"jsonrpc", "2.0"},
            {"protocol_version", authoring_protocol_version},
            {"id", id},
            {"error",
             {{"code", code}, {"message", std::move(error.message)}, {"data", std::move(data)}}}};
        auto encoded = response.dump();
        if (encoded.size() > authoring_protocol_maximum_response_bytes)
            return Error{ErrorCode::execution_failed,
                         "Authoring error response exceeds its byte bound",
                         {.operation = "authoring.protocol.handle"}};
        return AuthoringProtocolReply{std::move(encoded), true};
    }

    Result<AuthoringProtocolReply> handle(std::string_view request) {
        Json id = nullptr;
        bool notification = false;
        outcome_may_have_changed = false;
        try {
            if (request.size() > authoring_protocol_maximum_request_bytes)
                fail(-32600, ErrorCode::invalid_argument, "Authoring request exceeds 64 KiB");
            auto parsed =
                detail::parse_source_json(request, project_text_maximum_bytes, maximum_json_nodes);
            if (!parsed)
                return failure(-32700, std::move(parsed).error(), id, false);
            auto envelope = std::move(parsed).value();
            if (!fields(envelope, {"jsonrpc", "protocol_version", "id", "method", "params"}) ||
                !envelope.contains("jsonrpc") || envelope.at("jsonrpc") != "2.0" ||
                !envelope.contains("protocol_version") ||
                !envelope.at("protocol_version").is_number_integer() ||
                !envelope.contains("method") || !text(envelope.at("method"), 128) ||
                (envelope.contains("id") && !request_id(envelope.at("id"))))
                fail(-32600, ErrorCode::invalid_argument, "Invalid authoring JSON-RPC envelope");
            if (envelope.contains("id"))
                id = envelope.at("id");
            else
                notification = true;
            if (envelope.at("protocol_version") != authoring_protocol_version)
                fail(-32001, ErrorCode::invalid_argument, "Unsupported authoring protocol version",
                     "/protocol_version");
            const auto parameters = envelope.value("params", Json::object());
            if (!parameters.is_object())
                fail(-32602, ErrorCode::invalid_argument, "Method params must be an object",
                     "/params");
            auto prepared =
                dispatch(envelope.at("method").get_ref<const std::string&>(), parameters);
            Json response{{"jsonrpc", "2.0"},
                          {"protocol_version", authoring_protocol_version},
                          {"id", id},
                          {"result", std::move(prepared.result)}};
            auto encoded = response.dump();
            if (encoded.size() > authoring_protocol_maximum_response_bytes)
                fail(-32000, ErrorCode::execution_failed,
                     "Authoring response exceeds 1 MiB; request a smaller page or field selection");

            // Allocate complete replies and candidate cursor nodes before any
            // ownership transition. Failed encoding/admission retains old owners
            // and leaves the prior continuation token usable.
            if (prepared.cursor) {
                auto& current = require_session();
                const auto inserted = current.cursors.emplace(std::move(prepared.cursor->token),
                                                              std::move(prepared.cursor->cursor));
                if (!inserted.second)
                    fail(-32000, ErrorCode::conflict, "Cursor publication identity already exists");
            }
            if (prepared.consumed_cursor)
                require_session().cursors.erase(*prepared.consumed_cursor);
            if (prepared.text_revision) {
                auto& current = require_session();
                const auto inserted =
                    current.text_revisions.emplace(std::move(prepared.text_revision->token),
                                                   std::move(prepared.text_revision->value));
                if (!inserted.second)
                    fail(-32000, ErrorCode::conflict,
                         "Text revision publication identity already exists");
            }
            if (prepared.consumed_text_revision)
                require_session().text_revisions.erase(*prepared.consumed_text_revision);
            if (prepared.assets) {
                if (prepared.assets->taken_job || prepared.assets->released_token)
                    outcome_may_have_changed = true;
                require_session().assets->admit(*prepared.assets);
            }
            if (prepared.opening)
                session = std::move(prepared.opening);
            if (prepared.closing)
                session.reset();
            return AuthoringProtocolReply{notification ? std::string{} : std::move(encoded),
                                          prepared.failed};
        } catch (ProtocolFailure& rejected) {
            return failure(rejected.code, std::move(rejected.error), id, notification);
        } catch (AuthoringAssetFailure& rejected) {
            return failure(rejected.code, std::move(rejected.error), id, notification);
        } catch (...) {
            if (outcome_may_have_changed)
                return failure(-32603,
                               {ErrorCode::execution_failed,
                                "Reply preparation failed after operation dispatch; inspect state "
                                "and retained outcomes before retrying",
                                {.operation = "authoring.protocol.reconcile"}},
                               id, notification);
            throw;
        }
    }
};

AuthoringProtocol::AuthoringProtocol(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
AuthoringProtocol::~AuthoringProtocol() = default;

Result<std::unique_ptr<AuthoringProtocol>> AuthoringProtocol::create(std::filesystem::path root) {
    return create(AuthoringProtocolOptions{std::move(root), {}, {}, nullptr});
}

Result<std::unique_ptr<AuthoringProtocol>>
AuthoringProtocol::create(AuthoringProtocolOptions options) {
    try {
        auto& root = options.root;
        if (!root.empty()) {
            auto resolved = ProjectRoot::open(root, {.operation = "authoring.protocol.create"});
            if (!resolved)
                return std::move(resolved).error();
            root = resolved.value().native_path();
        }
        if (options.export_python.empty() != options.export_tool.empty())
            return Error{ErrorCode::invalid_argument,
                         "Export requires both trusted host helper paths",
                         {.operation = "authoring.protocol.create"}};
        for (auto* path : {&options.export_python, &options.export_tool}) {
            if (path->empty())
                continue;
            if (!std::filesystem::is_regular_file(*path))
                return Error{ErrorCode::invalid_path,
                             "Export helper must be an existing regular file",
                             {.operation = "authoring.protocol.create"}};
            *path = std::filesystem::canonical(*path);
        }
        return std::unique_ptr<AuthoringProtocol>{
            new AuthoringProtocol(std::make_unique<Impl>(std::move(options)))};
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot create authoring protocol service",
                     {.operation = "authoring.protocol.create"}};
    }
}

Result<AuthoringProtocolReply> AuthoringProtocol::handle(std::string_view request) {
    if (impl_->thread != std::this_thread::get_id())
        return Error{ErrorCode::conflict,
                     "Authoring protocol requires its creating thread",
                     {.operation = "authoring.protocol.handle"}};
    try {
        return impl_->handle(request);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Authoring protocol operation failed",
                     {.operation = "authoring.protocol.handle"}};
    }
}

} // namespace shark::tools
