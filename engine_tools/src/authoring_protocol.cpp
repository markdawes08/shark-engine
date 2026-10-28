#include "shark/tools/authoring_protocol.hpp"

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

Json discovery() {
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
        const bool requires_open_project = name.starts_with("scene.");
        const bool requires_project_root = name == "project.open";
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
    add("project.close", empty, object_schema({{"open", {{"const", false}}}}, {"open"}),
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
    return {{"protocol_version", authoring_protocol_version},
            {"transport",
             {{"framing", "newline-delimited-json"}, {"batch", false}, {"notifications", true}}},
            {"limits",
             {{"request_bytes", authoring_protocol_maximum_request_bytes},
              {"response_bytes", authoring_protocol_maximum_response_bytes},
              {"retained_cursors", authoring_protocol_maximum_cursors}}},
            {"capabilities",
             {{"cpu_authoring", true}, {"device_capture", false}, {"mutations", false}}},
            {"methods", std::move(methods)},
            {"components", components}};
}

struct Cursor {
    SceneQueryCursor value;
    SceneQueryFields fields;
};

struct Session {
    // Reverse destruction releases scene/cursors before the native project lock.
    std::unique_ptr<ProjectFiles> files;
    ProjectPath path;
    ReadSource<ProjectDocument> source;
    LoadedAuthoredScene loaded;
    SceneId scene;
    std::map<std::string, Cursor> cursors;
};

Json status(const Session* session) {
    if (session == nullptr)
        return {{"open", false}};
    const auto state = take(session->loaded.owner->status());
    const auto& project = session->source.source.document;
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
         {{"project", session->source.source.source_schema_version},
          {"scene", session->loaded.source_schema_version}}},
        {"file_revisions",
         {{"project_bytes", session->source.revision.byte_count()},
          {"scene_bytes", session->loaded.file_revision.byte_count()}}},
        {"cursors",
         {{"retained", session->cursors.size()}, {"maximum", authoring_protocol_maximum_cursors}}}};
}

struct CursorPublication {
    std::string token;
    Cursor cursor;
};

struct Dispatch {
    Json result;
    std::unique_ptr<Session> opening{};
    bool closing{false};
    std::optional<CursorPublication> cursor{};
    std::optional<std::string> consumed_cursor{};
};

} // namespace

struct AuthoringProtocol::Impl {
    const std::thread::id thread{std::this_thread::get_id()};
    std::filesystem::path root;
    std::unique_ptr<Session> session;

    explicit Impl(std::filesystem::path value) : root(std::move(value)) {}

    Session& require_session() {
        if (!session)
            fail(-32000, ErrorCode::conflict,
                 "No project is open; open a project and restart the query at its observed "
                 "revision");
        return *session;
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
        const auto scene = take(current.loaded.owner->scene());
        result.result = take(wire::page_json(scene.get(), value, selected, token));
        return result;
    }

    Dispatch dispatch(std::string_view method, const Json& parameters) {
        if (method == "protocol.discover") {
            empty_parameters(parameters);
            return {discovery()};
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
            empty_parameters(parameters);
            if (session) {
                const auto state = take(session->loaded.owner->status());
                if (state.dirty || state.pending_mutations != 0)
                    fail(-32000, ErrorCode::conflict,
                         "Cannot close a project with unsaved or pending authoring changes");
            }
            return {Json{{"open", false}}, {}, true};
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
                                                               std::move(source),
                                                               std::move(loaded),
                                                               scene,
                                                               {}});
            auto reply = status(candidate.get());
            return {std::move(reply), std::move(candidate)};
        }
        if (method == "scene.query") {
            auto parsed = take(wire::parse_query(parameters), -32602);
            auto& current = require_session();
            if (current.cursors.size() >= authoring_protocol_maximum_cursors)
                fail(-32000, ErrorCode::conflict,
                     "Outstanding cursor limit reached; finish a query or close the project");
            if (parsed.parent_id) {
                const auto scene = take(current.loaded.owner->scene());
                parsed.options.parent = take(scene.get().find(*parsed.parent_id));
            }
            const auto result = take(current.loaded.owner->query_entities(parsed.options));
            return page(result, parsed.options.fields);
        }
        if (method == "scene.inspect") {
            const auto parsed = take(wire::parse_inspect(parameters), -32602);
            auto& current = require_session();
            const auto result = take(current.loaded.owner->inspect_entity(
                parsed.entity_id, parsed.fields, parsed.expected_revision));
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
            const auto result = take(current.loaded.owner->continue_query(found->second.value));
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
            const auto state = session->loaded.owner->status();
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
        try {
            if (request.size() > authoring_protocol_maximum_request_bytes)
                fail(-32600, ErrorCode::invalid_argument, "Authoring request exceeds 64 KiB");
            auto parsed =
                detail::parse_source_json(request, source_maximum_string_bytes, maximum_json_nodes);
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
            if (prepared.opening)
                session = std::move(prepared.opening);
            if (prepared.closing)
                session.reset();
            return AuthoringProtocolReply{notification ? std::string{} : std::move(encoded), false};
        } catch (ProtocolFailure& rejected) {
            return failure(rejected.code, std::move(rejected.error), id, notification);
        }
    }
};

AuthoringProtocol::AuthoringProtocol(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
AuthoringProtocol::~AuthoringProtocol() = default;

Result<std::unique_ptr<AuthoringProtocol>> AuthoringProtocol::create(std::filesystem::path root) {
    try {
        if (!root.empty()) {
            auto resolved = ProjectRoot::open(root, {.operation = "authoring.protocol.create"});
            if (!resolved)
                return std::move(resolved).error();
            root = resolved.value().native_path();
        }
        return std::unique_ptr<AuthoringProtocol>{
            new AuthoringProtocol(std::make_unique<Impl>(std::move(root)))};
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
