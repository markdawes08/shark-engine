#include "authoring_world_cells.hpp"

#include "authoring_asset_wire.hpp"
#include "authoring_wire.hpp"
#include "shark/tools/authoring_protocol.hpp"

#include <charconv>
#include <set>

namespace shark::tools::authoring_cell_wire {
namespace {
using Json = nlohmann::json;
struct Failure {
    Error error;
};
[[noreturn]] void fail(const char* message) {
    throw Failure{Error{ErrorCode::invalid_argument, message, {"authoring.cell.decode"}}};
}
template <class T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
const Json& required(const Json& object, const char* name) {
    if (!object.contains(name))
        fail("Required cell parameter is missing");
    return object.at(name);
}
std::uint64_t decimal(const Json& value) {
    if (!value.is_string())
        fail("Cell content revision must be a positive decimal string");
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t number{};
    if (text.empty() || text.size() > 20 || text.front() < '1' || text.front() > '9')
        fail("Cell content revision must be a positive canonical decimal string");
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || number == 0)
        fail("Cell content revision must fit uint64");
    return number;
}
std::int64_t axis(const Json& value) {
    if (!value.is_number_integer())
        fail("Cell axes must be signed 32-bit integers");
    if (value.is_number_unsigned() &&
        value.get<std::uint64_t>() > static_cast<std::uint64_t>(world_cell_max))
        fail("Cell axis is outside its representation bounds");
    const auto result = value.get<std::int64_t>();
    if (result < world_cell_min || result > world_cell_max)
        fail("Cell axis is outside its representation bounds");
    return result;
}
std::vector<AuthoredEntityId> selection(const Json& value, bool nonempty) {
    if (!value.is_array() || value.size() > 256 || (nonempty && value.empty()))
        fail("Cell mesh selections must contain at most 256 entity IDs, with collision nonempty");
    std::vector<AuthoredEntityId> result;
    std::set<AuthoredEntityId> seen;
    for (const auto& item : value) {
        if (!item.is_string())
            fail("Cell selection entries must be UUID strings");
        const auto id = AuthoredEntityId{take(StableId::parse(item.get_ref<const std::string&>()))};
        if (!seen.insert(id).second)
            fail("Cell selections cannot contain duplicate entity IDs");
        result.push_back(id);
    }
    return result;
}
Json object(Json properties, Json fields) {
    return {{"type", "object"},
            {"properties", std::move(properties)},
            {"required", std::move(fields)},
            {"additionalProperties", false}};
}
} // namespace

Result<AuthoringCellRequest> parse(const Json& parameters, bool publishing) {
    try {
        const auto checked =
            publishing ? authoring_asset_wire::require_fields(
                             parameters,
                             {"expected_revision", "expected_catalog", "expected_cache", "world_id",
                              "cell", "content_revision", "target", "collision_entities",
                              "navigation_entities", "path", "expected_digest"})
                       : authoring_asset_wire::require_fields(
                             parameters, {"expected_revision", "expected_catalog", "expected_cache",
                                          "world_id", "cell", "content_revision", "target",
                                          "collision_entities", "navigation_entities"});
        if (!checked)
            return checked.error();
        const auto& cell = required(parameters, "cell");
        const auto fields = authoring_asset_wire::require_fields(cell, {"x", "y", "z"});
        if (!fields)
            return fields.error();
        const auto& world = required(parameters, "world_id");
        if (!world.is_string())
            fail("Cell world_id must be a UUID string");
        return AuthoringCellRequest{
            take(authoring_wire::parse_revision(required(parameters, "expected_revision"))),
            take(authoring_asset_wire::parse_catalog_revision(
                required(parameters, "expected_catalog"))),
            take(
                authoring_asset_wire::parse_cache_revision(required(parameters, "expected_cache"))),
            {WorldId{take(StableId::parse(world.get_ref<const std::string&>()))},
             {axis(required(cell, "x")), axis(required(cell, "y")), axis(required(cell, "z"))}},
            decimal(required(parameters, "content_revision")),
            take(authoring_asset_wire::parse_target(required(parameters, "target"))),
            selection(required(parameters, "collision_entities"), true),
            selection(required(parameters, "navigation_entities"), false)};
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot decode bounded cell request",
                     {"authoring.cell.decode"}};
    }
}

Json summary(const PreparedWorldCell& cell, ContentDigest digest, std::size_t bytes) {
    return {
        {"world_id", cell.key.world.value().to_string()},
        {"cell", {{"x", cell.key.cell.x}, {"y", cell.key.cell.y}, {"z", cell.key.cell.z}}},
        {"content_revision", std::to_string(cell.content_revision)},
        {"scene_id", cell.scene.scene.value().to_string()},
        {"entity_count", cell.scene.entities.size()},
        {"vertices", cell.geometry ? cell.geometry->vertices.size() : 0},
        {"collision_triangles", cell.geometry ? cell.geometry->collision_triangles.size() : 0},
        {"navigation_triangles", cell.geometry ? cell.geometry->navigation_triangles.size() : 0},
        {"payload_bytes", bytes},
        {"payload_digest", digest.to_hex()},
        {"payload_version", cell.geometry ? 2 : 1},
        {"runtime_assets_embedded", false}};
}

Json methods() {
    const Json id{{"type", "string"}, {"format", "uuid"}};
    const Json number{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]{0,19})$"}};
    const auto revision =
        object({{"scene_instance", number}, {"revision", number}}, {"scene_instance", "revision"});
    const auto asset_revision = object({{"scope", id}, {"value", number}}, {"scope", "value"});
    const Json signed_axis{
        {"type", "integer"}, {"minimum", world_cell_min}, {"maximum", world_cell_max}};
    const auto cell =
        object({{"x", signed_axis}, {"y", signed_axis}, {"z", signed_axis}}, {"x", "y", "z"});
    const Json path{{"type", "string"}, {"maxLength", 4096}, {"pattern", "\\.sharkcell$"}};
    const Json selected{{"type", "array"}, {"maxItems", 256}, {"uniqueItems", true}, {"items", id}};
    auto collision = selected;
    collision["minItems"] = 1;
    const auto target = object({{"id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 64}}},
                                {"side", {{"enum", {"client", "server"}}}}},
                               {"id", "side"});
    auto positive = number;
    positive["pattern"] = "^[1-9][0-9]{0,19}$";
    auto params =
        object({{"expected_revision", revision},
                {"expected_catalog", asset_revision},
                {"expected_cache", asset_revision},
                {"world_id", id},
                {"cell", cell},
                {"content_revision", positive},
                {"target", target},
                {"collision_entities", collision},
                {"navigation_entities", selected}},
               {"expected_revision", "expected_catalog", "expected_cache", "world_id", "cell",
                "content_revision", "target", "collision_entities", "navigation_entities"});
    const Json sample_revision{{"scope", "30000000-0000-4000-8000-000000000006"}, {"value", "1"}};
    Json result = Json::array();
    const auto add = [&](const char* name, Json p, Json required_fields, Json sample) {
        result.push_back(
            {{"name", name},
             {"requires_project_root", false},
             {"requires_open_project", true},
             {"execution_context", "cpu_owner_thread"},
             {"params", std::move(p)},
             {"result", {{"type", "object"}, {"required", std::move(required_fields)}}},
             {"example",
              {{"jsonrpc", "2.0"},
               {"protocol_version", authoring_protocol_version},
               {"id", "example"},
               {"method", name},
               {"params", std::move(sample)}}}});
    };
    Json sample{{"expected_revision", {{"scene_instance", "1"}, {"revision", "1"}}},
                {"expected_catalog", sample_revision},
                {"expected_cache", sample_revision},
                {"world_id", "30000000-0000-4000-8000-000000000002"},
                {"cell", {{"x", 0}, {"y", 0}, {"z", 0}}},
                {"content_revision", "1"},
                {"target", {{"id", "local-server"}, {"side", "server"}}},
                {"collision_entities", {"30000000-0000-4000-8000-000000000003"}},
                {"navigation_entities", Json::array()}};
    add("scene.cell.prepare", params,
        {"payload_digest", "source_revision", "dirty", "mesh_closure_checked"}, sample);
    params["properties"]["path"] = path;
    params["properties"]["expected_digest"] = {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}};
    params["required"].push_back("path");
    params["required"].push_back("expected_digest");
    sample["path"] = "World.sharkcell";
    sample["expected_digest"] = std::string(64, '0');
    add("scene.cell.export", params,
        {"path", "payload_digest", "publication", "source_revision", "dirty"}, sample);
    add("cell.inspect", object({{"path", path}}, {"path"}),
        {"path", "payload_digest", "content_revision", "mesh_closure_checked"},
        {{"path", "World.sharkcell"}});
    return result;
}
} // namespace shark::tools::authoring_cell_wire
