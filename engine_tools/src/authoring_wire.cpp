#include "authoring_wire.hpp"

#include "shark/scene.hpp"
#include "shark/schema.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <new>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace shark::tools::authoring_wire {
namespace {

using Json = nlohmann::json;

struct WireFailure {
    Error error;
};

[[noreturn]] void fail(const char* path, const char* message,
                       ErrorCode code = ErrorCode::invalid_argument) {
    throw WireFailure{Error{code, message, {.path = path}}};
}

template <typename T> T checked(Result<T> result, const char* path) {
    if (!result) {
        auto error = std::move(result).error();
        error.context.path = path;
        throw WireFailure{std::move(error)};
    }
    return std::move(result).value();
}

template <typename T, typename Function>
Result<T> boundary(const char* operation, Function&& function) {
    try {
        return function();
    } catch (WireFailure& failure) {
        failure.error.context.operation = operation;
        return std::move(failure.error);
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::execution_failed,
                     "Authoring wire allocation failed",
                     {.operation = operation}};
    } catch (const Json::exception&) {
        return Error{ErrorCode::execution_failed,
                     "Authoring wire JSON conversion failed",
                     {.operation = operation}};
    }
}

void object(const Json& value, std::initializer_list<std::string_view> keys, const char* path) {
    if (!value.is_object()) {
        fail(path, "Expected a parameter object");
    }
    for (const auto& item : value.items()) {
        if (std::find(keys.begin(), keys.end(), item.key()) == keys.end()) {
            fail(path, "Unknown parameter field");
        }
    }
}

const std::string& text(const Json& value, const char* path) {
    if (!value.is_string()) {
        fail(path, "Expected a string");
    }
    return value.get_ref<const std::string&>();
}

std::uint64_t decimal(const Json& value, const char* path) {
    const auto& input = text(value, path);
    if (input.empty() || input.size() > 20 || (input.size() > 1 && input.front() == '0') ||
        !std::all_of(input.begin(), input.end(),
                     [](char digit) { return digit >= '0' && digit <= '9'; })) {
        fail(path, "Expected a canonical unsigned 64-bit decimal string");
    }
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size()) {
        fail(path, "Decimal string exceeds the unsigned 64-bit range");
    }
    return result;
}

std::optional<SceneRevision> expected_revision(const Json& params) {
    const auto found = params.find("expected_revision");
    if (found == params.end()) {
        return std::nullopt;
    }
    object(*found, {"scene_instance", "revision"}, "/params/expected_revision");
    if (!found->contains("scene_instance") || !found->contains("revision")) {
        fail("/params/expected_revision", "Both scene_instance and revision are required");
    }
    return SceneRevision{
        decimal((*found)["scene_instance"], "/params/expected_revision/scene_instance"),
        decimal((*found)["revision"], "/params/expected_revision/revision")};
}

SceneQueryFields fields(const Json& params) {
    const auto found = params.find("fields");
    if (found == params.end()) {
        return {};
    }
    if (!found->is_array() || found->size() > 5) {
        fail("/params/fields", "Expected at most five distinct field names");
    }
    SceneQueryFields result{false, false, false, false, false};
    for (const auto& item : *found) {
        const auto& key = text(item, "/params/fields");
        bool* selected = nullptr;
        if (key == "name") {
            selected = &result.name;
        } else if (key == "transform") {
            selected = &result.transform;
        } else if (key == "hierarchy") {
            selected = &result.hierarchy;
        } else if (key == "camera") {
            selected = &result.camera;
        } else if (key == "mesh_renderer") {
            selected = &result.mesh_renderer;
        } else {
            fail("/params/fields", "Unknown scene field name");
        }
        if (*selected) {
            fail("/params/fields", "Duplicate scene field name");
        }
        *selected = true;
    }
    return result;
}

std::size_t bounded_integer(const Json& value, std::size_t maximum, const char* path) {
    std::uint64_t number = 0;
    if (value.is_number_unsigned()) {
        number = value.get<std::uint64_t>();
    } else if (value.is_number_integer()) {
        const auto signed_number = value.get<std::int64_t>();
        if (signed_number <= 0) {
            fail(path, "Expected a positive integer within the declared bound");
        }
        number = static_cast<std::uint64_t>(signed_number);
    } else {
        fail(path, "Expected an integer, not a floating-point number or boolean");
    }
    if (number == 0 || number > maximum) {
        fail(path, "Integer exceeds the declared query bound");
    }
    return static_cast<std::size_t>(number);
}

AuthoredEntityId entity_id(const Json& value, const char* path) {
    return AuthoredEntityId{checked(StableId::parse(text(value, path)), path)};
}

const char* property_type(PropertyType value) {
    switch (value) {
    case PropertyType::utf8_text:
        return "utf8_text";
    case PropertyType::float64_vector3:
        return "float64_vector3";
    case PropertyType::float64_quaternion_xyzw:
        return "float64_quaternion_xyzw";
    case PropertyType::enumeration:
        return "enumeration";
    case PropertyType::float64:
        return "float64";
    case PropertyType::asset_id:
        return "asset_id";
    case PropertyType::uint32:
        return "uint32";
    case PropertyType::boolean:
        return "boolean";
    }
    fail("", "Unsupported Core property type", ErrorCode::execution_failed);
}

const char* property_unit(PropertyUnit value) {
    switch (value) {
    case PropertyUnit::none:
        return "none";
    case PropertyUnit::meters:
        return "meters";
    case PropertyUnit::dimensionless:
        return "dimensionless";
    case PropertyUnit::radians:
        return "radians";
    }
    fail("", "Unsupported Core property unit", ErrorCode::execution_failed);
}

Json default_json(const PropertyDefault& value) {
    return std::visit(
        [](const auto& item) -> Json {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return nullptr;
            } else if constexpr (std::is_same_v<T, AssetId>) {
                return item.value().to_string();
            } else {
                return item;
            }
        },
        value);
}

Json constraints_json(const PropertyConstraints& value) {
    return std::visit(
        [](const auto& item) -> Json {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, TextConstraints>) {
                return {{"minimum_bytes", item.minimum_bytes},
                        {"maximum_bytes", item.maximum_bytes},
                        {"require_utf8", item.require_utf8},
                        {"forbid_c0_c1_del", item.forbid_c0_c1_del},
                        {"unique", item.unique},
                        {"normalizes_unicode", item.normalizes_unicode}};
            } else if constexpr (std::is_same_v<T, Vector3Constraints>) {
                return {{"minimum_component", item.minimum_component},
                        {"maximum_component", item.maximum_component},
                        {"minimum_absolute_component", item.minimum_absolute_component
                                                           ? Json(*item.minimum_absolute_component)
                                                           : Json(nullptr)},
                        {"finite_only", item.finite_only}};
            } else if constexpr (std::is_same_v<T, QuaternionConstraints>) {
                if (item.canonical_sign != QuaternionSignPolicy::positive_w_then_xyz) {
                    fail("", "Unsupported Core quaternion policy", ErrorCode::execution_failed);
                }
                return {{"minimum_component", item.minimum_component},
                        {"maximum_component", item.maximum_component},
                        {"finite_only", item.finite_only},
                        {"require_nonzero", item.require_nonzero},
                        {"normalizes_input", item.normalizes_input},
                        {"canonical_sign", "positive_w_then_xyz"}};
            } else if constexpr (std::is_same_v<T, EnumerationConstraints>) {
                Json values = Json::array();
                for (const auto& entry : item.values) {
                    values.push_back({{"value", entry.value}, {"key", entry.key}});
                }
                return {{"values", std::move(values)}};
            } else if constexpr (std::is_same_v<T, ScalarConstraints>) {
                return {{"minimum", item.minimum},
                        {"maximum", item.maximum},
                        {"minimum_inclusive", item.minimum_inclusive},
                        {"maximum_inclusive", item.maximum_inclusive},
                        {"finite_only", item.finite_only}};
            } else if constexpr (std::is_same_v<T, AssetIdConstraints>) {
                return {{"required", item.required}, {"uuid_v4", item.uuid_v4}};
            } else if constexpr (std::is_same_v<T, Uint32Constraints>) {
                return {{"minimum", item.minimum}, {"maximum", item.maximum}};
            } else {
                static_assert(std::is_same_v<T, BooleanConstraints>);
                return Json::object();
            }
        },
        value);
}

Json scope_json(const SceneScope& scope) {
    const auto* authored = std::get_if<AuthoredSceneScope>(&scope);
    if (authored == nullptr) {
        fail("", "Authoring queries require an authored scene scope", ErrorCode::execution_failed);
    }
    return {{"kind", "authored"}, {"scene_id", authored->scene.value().to_string()}};
}

std::string authored_id(const EntityReference& reference) {
    const auto* id = std::get_if<AuthoredEntityId>(&reference.identity);
    if (id == nullptr || reference.authority) {
        fail("", "Authoring query returned a non-authored identity", ErrorCode::execution_failed);
    }
    return id->value().to_string();
}

Json vector_json(const math::Vec3& vector) {
    return Json::array({vector.x, vector.y, vector.z});
}

const char* projection_name(CameraProjection projection) {
    switch (projection) {
    case CameraProjection::perspective:
        return "perspective";
    case CameraProjection::orthographic:
        return "orthographic";
    }
    fail("", "Unsupported Core camera projection", ErrorCode::execution_failed);
}

Json entity_json(const Scene& scene, const SceneInspectedEntity& entity,
                 SceneQueryFields selected) {
    Json result{{"entity_id", authored_id(entity.reference)},
                {"scope", scope_json(entity.reference.scope)},
                {"authority", nullptr}};
    if (selected.name) {
        if (!entity.name) {
            fail("", "Selected Name is missing from the query page", ErrorCode::execution_failed);
        }
        result["name"] = entity.name->value;
    }
    if (selected.transform) {
        if (!entity.transform) {
            fail("", "Selected Transform is missing from the query page",
                 ErrorCode::execution_failed);
        }
        const auto& transform = *entity.transform;
        const auto& local = transform.local;
        Json matrix = Json::array();
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                matrix.push_back(transform.world_matrix[column][row]);
            }
        }
        result["transform"] = {
            {"local",
             {{"translation", vector_json(local.translation)},
              {"rotation", Json::array({local.rotation.x, local.rotation.y, local.rotation.z,
                                        local.rotation.w})},
              {"scale", vector_json(local.scale)}}},
            {"world_matrix", {{"layout", "column_major"}, {"values", std::move(matrix)}}}};
    }
    if (selected.hierarchy) {
        if (!entity.hierarchy) {
            fail("", "Selected hierarchy is missing from the query page",
                 ErrorCode::execution_failed);
        }
        const auto& hierarchy = *entity.hierarchy;
        Json parent = nullptr;
        if (hierarchy.parent) {
            parent =
                authored_id(checked(scene.reference(*hierarchy.parent), "/hierarchy/parent_id"));
        }
        result["hierarchy"] = {{"parent_id", std::move(parent)},
                               {"child_count", hierarchy.child_count},
                               {"depth", hierarchy.depth}};
    }
    if (selected.camera) {
        result["camera"] = nullptr;
        if (entity.camera) {
            const auto& camera = *entity.camera;
            result["camera"] = {{"projection", projection_name(camera.projection)},
                                {"vertical_fov_radians", camera.vertical_fov_radians},
                                {"orthographic_height", camera.orthographic_height},
                                {"near_clip", camera.near_clip},
                                {"far_clip", camera.far_clip}};
        }
    }
    if (selected.mesh_renderer) {
        result["mesh_renderer"] = nullptr;
        if (entity.mesh_renderer) {
            const auto& mesh = *entity.mesh_renderer;
            result["mesh_renderer"] = {{"asset_id", mesh.asset.value().to_string()},
                                       {"mesh_index", mesh.mesh_index},
                                       {"visible", mesh.visible}};
        }
    }
    return result;
}

} // namespace

Result<Query> parse_query(const Json& params) {
    return boundary<Query>("scene.query", [&] {
        object(params,
               {"selection", "parent_id", "name_prefix", "fields", "page_size", "scan_budget",
                "expected_revision"},
               "/params");
        Query result;
        if (params.contains("selection")) {
            const auto& selection = text(params["selection"], "/params/selection");
            if (selection == "all") {
                result.options.selection = SceneQuerySelection::all;
            } else if (selection == "roots") {
                result.options.selection = SceneQuerySelection::roots;
            } else if (selection == "children") {
                result.options.selection = SceneQuerySelection::children;
            } else {
                fail("/params/selection", "Unknown scene query selection");
            }
        }
        if (params.contains("parent_id")) {
            result.parent_id = entity_id(params["parent_id"], "/params/parent_id");
        }
        if ((result.options.selection == SceneQuerySelection::children) !=
            result.parent_id.has_value()) {
            fail("/params/parent_id", "parent_id is required only for children selection");
        }
        if (params.contains("name_prefix")) {
            result.options.name_prefix = text(params["name_prefix"], "/params/name_prefix");
            if (!result.options.name_prefix.empty()) {
                auto valid = validate_name(Name{result.options.name_prefix});
                if (!valid) {
                    auto error = std::move(valid).error();
                    error.context.path = "/params/name_prefix";
                    throw WireFailure{std::move(error)};
                }
            }
        }
        result.options.fields = fields(params);
        result.options.expected_revision = expected_revision(params);
        if (params.contains("page_size")) {
            result.options.page_size = bounded_integer(
                params["page_size"], scene_query_maximum_page_size, "/params/page_size");
        }
        if (params.contains("scan_budget")) {
            result.options.scan_budget = bounded_integer(
                params["scan_budget"], scene_query_maximum_scan_budget, "/params/scan_budget");
        }
        return result;
    });
}

Result<Inspect> parse_inspect(const Json& params) {
    return boundary<Inspect>("scene.inspect", [&] {
        object(params, {"entity_id", "fields", "expected_revision"}, "/params");
        if (!params.contains("entity_id")) {
            fail("/params/entity_id", "entity_id is required");
        }
        return Inspect{entity_id(params["entity_id"], "/params/entity_id"), fields(params),
                       expected_revision(params)};
    });
}

Result<Json> component_schemas_json() {
    return boundary<Json>("schema.components", [] {
        Json result = Json::array();
        for (const auto& component : component_schemas()) {
            Json properties = Json::array();
            for (const auto& property : component.properties) {
                if (property.mutability != PropertyMutability::scene_owner_write) {
                    fail("", "Unsupported Core property mutability", ErrorCode::execution_failed);
                }
                properties.push_back({{"id", static_cast<std::uint32_t>(property.id)},
                                      {"key", property.key},
                                      {"display_name", property.display_name},
                                      {"type", property_type(property.type)},
                                      {"unit", property_unit(property.unit)},
                                      {"mutability", "scene_owner_write"},
                                      {"default", default_json(property.default_value)},
                                      {"constraints", constraints_json(property.constraints)}});
            }
            result.push_back({{"id", static_cast<std::uint32_t>(component.id)},
                              {"stable_id", component.stable_id},
                              {"display_name", component.display_name},
                              {"version", component.version},
                              {"required", component.required},
                              {"removable", component.removable},
                              {"properties", std::move(properties)}});
        }
        return result;
    });
}

Json revision_json(SceneRevision revision) {
    return {{"scene_instance", std::to_string(revision.scene_instance)},
            {"revision", std::to_string(revision.revision)}};
}

Result<Json> page_json(const Scene& scene, const SceneQueryPage& page, SceneQueryFields selected,
                       std::optional<std::string_view> next_token) {
    return boundary<Json>("scene.query.serialize", [&] {
        if (page.next_cursor().has_value() != next_token.has_value() ||
            (next_token && next_token->empty())) {
            fail("", "Query continuation token does not match the page",
                 ErrorCode::execution_failed);
        }
        const auto stats = checked(scene.stats(), "");
        if (stats.closed || stats.instance != page.revision().scene_instance ||
            stats.revision != page.revision().revision) {
            fail("", "Scene changed before query serialization", ErrorCode::conflict);
        }
        Json entities = Json::array();
        for (const auto& entity : page.entities()) {
            entities.push_back(entity_json(scene, entity, selected));
        }
        return Json{{"revision", revision_json(page.revision())},
                    {"scope", scope_json(page.scope())},
                    {"entities", std::move(entities)},
                    {"scanned_slots", page.scanned_slots()},
                    {"complete", !page.next_cursor().has_value()},
                    {"next_cursor", next_token ? Json(std::string(*next_token)) : Json(nullptr)}};
    });
}

} // namespace shark::tools::authoring_wire
