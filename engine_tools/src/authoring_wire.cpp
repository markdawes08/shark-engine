#include "authoring_wire.hpp"

#include "shark/scene.hpp"
#include "shark/schema.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
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

[[noreturn]] void fail(std::string_view path, const char* message,
                       ErrorCode code = ErrorCode::invalid_argument) {
    throw WireFailure{Error{code, message, {.path = std::string(path)}}};
}

template <typename T> T checked(Result<T> result, std::string_view path) {
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

void object(const Json& value, std::initializer_list<std::string_view> keys,
            std::string_view path) {
    if (!value.is_object()) {
        fail(path, "Expected a parameter object");
    }
    for (const auto& item : value.items()) {
        if (std::find(keys.begin(), keys.end(), item.key()) == keys.end()) {
            fail(path, "Unknown parameter field");
        }
    }
}

const std::string& text(const Json& value, std::string_view path) {
    if (!value.is_string()) {
        fail(path, "Expected a string");
    }
    return value.get_ref<const std::string&>();
}

std::uint64_t decimal(const Json& value, std::string_view path) {
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

AuthoredEntityId entity_id(const Json& value, std::string_view path) {
    return AuthoredEntityId{checked(StableId::parse(text(value, path)), path)};
}

const Json& required(const Json& value, const char* key, std::string_view path) {
    const auto found = value.find(key);
    if (found == value.end()) {
        fail(std::string(path) + '/' + key, "Required field is missing");
    }
    return *found;
}

SceneRevision revision(const Json& value, std::string_view path) {
    object(value, {"scene_instance", "revision"}, path);
    return {decimal(required(value, "scene_instance", path), std::string(path) + "/scene_instance"),
            decimal(required(value, "revision", path), std::string(path) + "/revision")};
}

AuthoringMutationId mutation_id(const Json& value, std::string_view path) {
    object(value, {"scope", "sequence"}, path);
    const auto scope_path = std::string(path) + "/scope";
    return {AuthoringMutationScope{checked(
                StableId::parse(text(required(value, "scope", path), scope_path)), scope_path)},
            decimal(required(value, "sequence", path), std::string(path) + "/sequence")};
}

double scalar(const Json& value, std::string_view path) {
    if (!value.is_number()) {
        fail(path, "Expected a finite number, not a boolean or string");
    }
    const auto result = value.get<double>();
    if (!std::isfinite(result)) {
        fail(path, "Expected a finite number");
    }
    return result;
}

void vector_size(const Json& value, std::size_t size, std::string_view path) {
    if (!value.is_array() || value.size() != size) {
        fail(path, "Expected an array with the declared vector length");
    }
}

math::Vec3 vector3(const Json& value, std::string_view path) {
    vector_size(value, 3, path);
    return {scalar(value[0], path), scalar(value[1], path), scalar(value[2], path)};
}

Transform transform(const Json& value, std::string_view path) {
    object(value, {"translation", "rotation", "scale"}, path);
    const auto rotation_path = std::string(path) + "/rotation";
    const auto& rotation = required(value, "rotation", path);
    vector_size(rotation, 4, rotation_path);
    return {vector3(required(value, "translation", path), std::string(path) + "/translation"),
            math::Quat{scalar(rotation[3], rotation_path), scalar(rotation[0], rotation_path),
                       scalar(rotation[1], rotation_path), scalar(rotation[2], rotation_path)},
            vector3(required(value, "scale", path), std::string(path) + "/scale")};
}

std::optional<AuthoredEntityId> parent(const Json& value, std::string_view path) {
    if (value.is_null()) {
        return std::nullopt;
    }
    return entity_id(value, path);
}

std::optional<Camera> camera(const Json& value, std::string_view path) {
    if (value.is_null()) {
        return std::nullopt;
    }
    object(value,
           {"projection", "vertical_fov_radians", "orthographic_height", "near_clip", "far_clip"},
           path);
    const auto projection_path = std::string(path) + "/projection";
    const auto& name = text(required(value, "projection", path), projection_path);
    CameraProjection projection;
    if (name == "perspective") {
        projection = CameraProjection::perspective;
    } else if (name == "orthographic") {
        projection = CameraProjection::orthographic;
    } else {
        fail(projection_path, "Unknown camera projection");
    }
    return Camera{projection,
                  scalar(required(value, "vertical_fov_radians", path),
                         std::string(path) + "/vertical_fov_radians"),
                  scalar(required(value, "orthographic_height", path),
                         std::string(path) + "/orthographic_height"),
                  scalar(required(value, "near_clip", path), std::string(path) + "/near_clip"),
                  scalar(required(value, "far_clip", path), std::string(path) + "/far_clip")};
}

std::optional<MeshRenderer> mesh_renderer(const Json& value, std::string_view path) {
    if (value.is_null()) {
        return std::nullopt;
    }
    object(value, {"asset_id", "mesh_index", "visible"}, path);
    const auto asset_path = std::string(path) + "/asset_id";
    const auto asset = AssetId{
        checked(StableId::parse(text(required(value, "asset_id", path), asset_path)), asset_path)};
    const auto& index_value = required(value, "mesh_index", path);
    const auto index_path = std::string(path) + "/mesh_index";
    if (!index_value.is_number_integer() ||
        (index_value.is_number_integer() && !index_value.is_number_unsigned() &&
         index_value.get<std::int64_t>() < 0)) {
        fail(index_path, "Expected an unsigned 32-bit integer");
    }
    const auto index = index_value.get<std::uint64_t>();
    if (index > std::numeric_limits<std::uint32_t>::max()) {
        fail(index_path, "Mesh index exceeds the unsigned 32-bit range");
    }
    const auto& visible = required(value, "visible", path);
    if (!visible.is_boolean()) {
        fail(std::string(path) + "/visible", "Expected a boolean");
    }
    return MeshRenderer{asset, static_cast<std::uint32_t>(index), visible.get<bool>()};
}

Name command_name(const Json& value, std::string_view path) {
    const auto& name = text(value, path);
    if (name.size() > authoring_maximum_mutation_name_bytes) {
        fail(path, "Name exceeds the retained input byte limit");
    }
    return Name{name};
}

AuthoringCommand command(const Json& value, std::string_view path) {
    if (!value.is_object()) {
        fail(path, "Expected a command object");
    }
    const auto& type = text(required(value, "type", path), std::string(path) + "/type");
    const auto id = entity_id(required(value, "entity_id", path), std::string(path) + "/entity_id");
    if (type == "create_entity") {
        object(value,
               {"type", "entity_id", "name", "transform", "parent_id", "camera", "mesh_renderer"},
               path);
        CreateEntityCommand result{id};
        if (value.contains("name")) {
            result.name = command_name(value["name"], std::string(path) + "/name");
        }
        if (value.contains("transform")) {
            result.transform = transform(value["transform"], std::string(path) + "/transform");
        }
        if (value.contains("parent_id")) {
            result.parent = parent(value["parent_id"], std::string(path) + "/parent_id");
        }
        if (value.contains("camera")) {
            result.camera = camera(value["camera"], std::string(path) + "/camera");
        }
        if (value.contains("mesh_renderer")) {
            result.mesh_renderer =
                mesh_renderer(value["mesh_renderer"], std::string(path) + "/mesh_renderer");
        }
        return result;
    }
    if (type == "delete_entity") {
        object(value, {"type", "entity_id"}, path);
        return DeleteEntityCommand{id};
    }
    if (type == "set_name") {
        object(value, {"type", "entity_id", "name"}, path);
        return SetNameCommand{
            id, command_name(required(value, "name", path), std::string(path) + "/name")};
    }
    if (type == "set_transform") {
        object(value, {"type", "entity_id", "transform"}, path);
        return SetTransformCommand{
            id, transform(required(value, "transform", path), std::string(path) + "/transform")};
    }
    if (type == "set_camera") {
        object(value, {"type", "entity_id", "camera"}, path);
        return SetCameraCommand{
            id, camera(required(value, "camera", path), std::string(path) + "/camera")};
    }
    if (type == "set_mesh_renderer") {
        object(value, {"type", "entity_id", "mesh_renderer"}, path);
        return SetMeshRendererCommand{id, mesh_renderer(required(value, "mesh_renderer", path),
                                                        std::string(path) + "/mesh_renderer")};
    }
    if (type == "reparent_entity") {
        object(value, {"type", "entity_id", "parent_id", "mode"}, path);
        const auto mode_path = std::string(path) + "/mode";
        const auto& mode = text(required(value, "mode", path), mode_path);
        ReparentMode native_mode;
        if (mode == "keep_local") {
            native_mode = ReparentMode::keep_local;
        } else if (mode == "keep_world") {
            native_mode = ReparentMode::keep_world;
        } else {
            fail(mode_path, "Unknown reparent mode");
        }
        return ReparentEntityCommand{
            id, parent(required(value, "parent_id", path), std::string(path) + "/parent_id"),
            native_mode};
    }
    fail(std::string(path) + "/type", "Unknown authoring command type");
}

AuthoringBatch batch(const Json& value, std::string_view path) {
    object(value, {"id", "commands"}, path);
    const auto id_path = std::string(path) + "/id";
    const auto id = AuthoringBatchId{
        checked(StableId::parse(text(required(value, "id", path), id_path)), id_path)};
    const auto& commands = required(value, "commands", path);
    const auto commands_path = std::string(path) + "/commands";
    if (!commands.is_array() || commands.size() > authoring_maximum_batch_commands) {
        fail(commands_path, "Expected at most 256 ordered commands");
    }
    AuthoringBatch result{id, {}};
    result.commands.reserve(commands.size());
    for (std::size_t index = 0; index < commands.size(); ++index) {
        result.commands.push_back(
            command(commands[index], commands_path + '/' + std::to_string(index)));
    }
    return result;
}

AuthoringMutation operation(const Json& value, std::string_view path) {
    if (!value.is_object()) {
        fail(path, "Expected an operation object");
    }
    const auto& type = text(required(value, "type", path), std::string(path) + "/type");
    if (type == "command") {
        object(value, {"type", "command"}, path);
        return command(required(value, "command", path), std::string(path) + "/command");
    }
    if (type == "batch") {
        object(value, {"type", "batch"}, path);
        return batch(required(value, "batch", path), std::string(path) + "/batch");
    }
    if (type == "undo" || type == "redo") {
        object(value, {"type", "ticket"}, path);
        const AuthoringHistoryTicket ticket{
            decimal(required(value, "ticket", path), std::string(path) + "/ticket")};
        if (type == "undo") {
            return UndoAuthoringMutation{ticket};
        }
        return RedoAuthoringMutation{ticket};
    }
    if (type == "clear_history") {
        object(value, {"type"}, path);
        return ClearAuthoringHistoryMutation{};
    }
    fail(std::string(path) + "/type", "Unknown authoring mutation type");
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

Json id_json(AuthoringMutationId value) {
    return {{"scope", value.scope.value().to_string()},
            {"sequence", std::to_string(value.sequence)}};
}

Json error_json(const Error& value) {
    return {{"code", error_code_name(value.code)},
            {"message", value.message},
            {"context",
             {{"operation", value.context.operation},
              {"request_id", value.context.request_id},
              {"path", value.context.path},
              {"entity_id", value.context.entity_id},
              {"asset_id", value.context.asset_id}}}};
}

Json effect_json(const Result<AuthoringCommandEffect>& value) {
    if (!value) {
        return {{"ok", false}, {"error", error_json(value.error())}};
    }
    Json affected = Json::array();
    for (const auto id : value.value().affected_entities) {
        affected.push_back(id.value().to_string());
    }
    return {{"ok", true},
            {"value",
             {{"changed", value.value().changed}, {"affected_entities", std::move(affected)}}}};
}

Json group_json(const std::optional<AuthoringHistoryGroup>& value) {
    if (!value) {
        return nullptr;
    }
    return {{"ticket", std::to_string(value->ticket.value)},
            {"command_count", value->command_count}};
}

Json object_schema(Json properties, Json required_keys) {
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties", std::move(properties)},
            {"required", std::move(required_keys)}};
}

Json nullable_schema(Json value) {
    return {{"oneOf", {std::move(value), Json{{"type", "null"}}}}};
}

Json id_schema() {
    return {{"type", "string"},
            {"format", "uuid"},
            {"pattern", "^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"}};
}

Json decimal_schema() {
    return {{"type", "string"},
            {"pattern", "^(0|[1-9][0-9]{0,19})$"},
            {"maximum_decimal", "18446744073709551615"}};
}

Json revision_schema() {
    return object_schema({{"scene_instance", decimal_schema()}, {"revision", decimal_schema()}},
                         {"scene_instance", "revision"});
}

Json mutation_id_schema() {
    return object_schema({{"scope", id_schema()}, {"sequence", decimal_schema()}},
                         {"scope", "sequence"});
}

Json scalar_schema() {
    return {{"type", "number"}, {"finite_only", true}};
}

Json vector_schema(std::size_t size) {
    return {{"type", "array"}, {"minItems", size}, {"maxItems", size}, {"items", scalar_schema()}};
}

Json transform_schema() {
    return object_schema({{"translation", vector_schema(3)},
                          {"rotation", vector_schema(4)},
                          {"scale", vector_schema(3)}},
                         {"translation", "rotation", "scale"});
}

Json camera_schema() {
    return nullable_schema(object_schema(
        {{"projection", {{"enum", {"perspective", "orthographic"}}}},
         {"vertical_fov_radians", scalar_schema()},
         {"orthographic_height", scalar_schema()},
         {"near_clip", scalar_schema()},
         {"far_clip", scalar_schema()}},
        {"projection", "vertical_fov_radians", "orthographic_height", "near_clip", "far_clip"}));
}

Json mesh_schema() {
    return nullable_schema(object_schema(
        {{"asset_id", id_schema()},
         {"mesh_index", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295ULL}}},
         {"visible", {{"type", "boolean"}}}},
        {"asset_id", "mesh_index", "visible"}));
}

Json command_schema() {
    const Json name{{"type", "string"},
                    {"maximum_utf8_bytes", authoring_maximum_mutation_name_bytes}};
    const Json entity = id_schema();
    const Json transform = transform_schema();
    const Json parent = nullable_schema(id_schema());
    const Json camera = camera_schema();
    const Json mesh = mesh_schema();
    Json options = Json::array();
    options.push_back(object_schema({{"type", {{"const", "create_entity"}}},
                                     {"entity_id", entity},
                                     {"name", name},
                                     {"transform", transform},
                                     {"parent_id", parent},
                                     {"camera", camera},
                                     {"mesh_renderer", mesh}},
                                    {"type", "entity_id"}));
    options.push_back(object_schema({{"type", {{"const", "delete_entity"}}}, {"entity_id", entity}},
                                    {"type", "entity_id"}));
    options.push_back(
        object_schema({{"type", {{"const", "set_name"}}}, {"entity_id", entity}, {"name", name}},
                      {"type", "entity_id", "name"}));
    options.push_back(object_schema(
        {{"type", {{"const", "set_transform"}}}, {"entity_id", entity}, {"transform", transform}},
        {"type", "entity_id", "transform"}));
    options.push_back(object_schema(
        {{"type", {{"const", "set_camera"}}}, {"entity_id", entity}, {"camera", camera}},
        {"type", "entity_id", "camera"}));
    options.push_back(object_schema({{"type", {{"const", "set_mesh_renderer"}}},
                                     {"entity_id", entity},
                                     {"mesh_renderer", mesh}},
                                    {"type", "entity_id", "mesh_renderer"}));
    options.push_back(object_schema({{"type", {{"const", "reparent_entity"}}},
                                     {"entity_id", entity},
                                     {"parent_id", parent},
                                     {"mode", {{"enum", {"keep_local", "keep_world"}}}}},
                                    {"type", "entity_id", "parent_id", "mode"}));
    return {{"oneOf", std::move(options)},
            {"description",
             "Typed commands are structurally decoded here; shared component and scene "
             "validators decide the retained operation outcome. Create defaults use the "
             "native Name and Transform; optional components are absent."}};
}

Json batch_schema() {
    return object_schema({{"id", id_schema()},
                          {"commands",
                           {{"type", "array"},
                            {"maxItems", authoring_maximum_batch_commands},
                            {"items", command_schema()}}}},
                         {"id", "commands"});
}

Json operation_schema() {
    Json options = Json::array();
    options.push_back(object_schema(
        {{"type", {{"const", "command"}}}, {"command", command_schema()}}, {"type", "command"}));
    options.push_back(object_schema({{"type", {{"const", "batch"}}}, {"batch", batch_schema()}},
                                    {"type", "batch"}));
    options.push_back(
        object_schema({{"type", {{"enum", {"undo", "redo"}}}}, {"ticket", decimal_schema()}},
                      {"type", "ticket"}));
    options.push_back(object_schema({{"type", {{"const", "clear_history"}}}}, {"type"}));
    return {{"oneOf", std::move(options)}};
}

Json error_schema() {
    const Json text{{"type", "string"}};
    return object_schema(
        {{"code", text},
         {"message", text},
         {"context", object_schema({{"operation", text},
                                    {"request_id", text},
                                    {"path", text},
                                    {"entity_id", text},
                                    {"asset_id", text}},
                                   {"operation", "request_id", "path", "entity_id", "asset_id"})}},
        {"code", "message", "context"});
}

Json result_schema(Json value) {
    Json options = Json::array();
    options.push_back(
        object_schema({{"ok", {{"const", true}}}, {"value", std::move(value)}}, {"ok", "value"}));
    options.push_back(
        object_schema({{"ok", {{"const", false}}}, {"error", error_schema()}}, {"ok", "error"}));
    return {{"oneOf", std::move(options)}};
}

Json effect_schema() {
    return result_schema(object_schema(
        {{"changed", {{"type", "boolean"}}},
         {"affected_entities",
          {{"type", "array"}, {"maxItems", 8192}, {"uniqueItems", true}, {"items", id_schema()}}}},
        {"changed", "affected_entities"}));
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

Result<SceneRevision> parse_revision(const Json& value) {
    return boundary<SceneRevision>("authoring.protocol.revision",
                                   [&] { return revision(value, "/params/expected_revision"); });
}

Result<std::optional<SceneRevision>> parse_optional_revision(const Json& params) {
    return boundary<std::optional<SceneRevision>>("authoring.protocol.revision", [&] {
        object(params, {"expected_revision"}, "/params");
        return expected_revision(params);
    });
}

Result<AuthoringMutationId> parse_mutation_id(const Json& value) {
    return boundary<AuthoringMutationId>("scene.mutation_outcome",
                                         [&] { return mutation_id(value, "/params/mutation_id"); });
}

Result<AuthoringMutationRequest> parse_mutation(const Json& params) {
    return boundary<AuthoringMutationRequest>("scene.mutate", [&] {
        object(params, {"mutation_id", "expected_revision", "operation"}, "/params");
        return AuthoringMutationRequest{
            mutation_id(required(params, "mutation_id", "/params"), "/params/mutation_id"),
            revision(required(params, "expected_revision", "/params"), "/params/expected_revision"),
            operation(required(params, "operation", "/params"), "/params/operation")};
    });
}

Result<Preview> parse_preview(const Json& params) {
    return boundary<Preview>("scene.preview", [&] {
        object(params, {"batch", "expected_revision"}, "/params");
        return Preview{batch(required(params, "batch", "/params"), "/params/batch"),
                       revision(required(params, "expected_revision", "/params"),
                                "/params/expected_revision")};
    });
}

Json history_json(const AuthoringHistoryStatus& value) {
    return {{"revision", revision_json(value.revision)},
            {"synchronized", value.synchronized},
            {"undo_entries", value.undo_entries},
            {"redo_entries", value.redo_entries},
            {"retained_bytes", value.retained_bytes},
            {"limits",
             {{"max_entries", value.limits.max_entries},
              {"max_retained_bytes", value.limits.max_retained_bytes}}},
            {"next_undo", group_json(value.next_undo)},
            {"next_redo", group_json(value.next_redo)}};
}

Json mutation_status_json(const AuthoringMutationStatus& value) {
    return {{"scope", value.scope.value().to_string()},
            {"revision", revision_json(value.revision)},
            {"high_watermark", std::to_string(value.high_watermark)},
            {"next", value.next ? id_json(*value.next) : Json(nullptr)},
            {"oldest_retained",
             value.oldest_retained ? id_json(*value.oldest_retained) : Json(nullptr)},
            {"retained_entries", value.retained_entries},
            {"retained_bytes", value.retained_bytes},
            {"limits",
             {{"max_entries", value.limits.max_entries},
              {"max_retained_bytes", value.limits.max_retained_bytes}}}};
}

Json mutation_reply_json(const AuthoringMutationReply& value) {
    Json outcome;
    if (!value.outcome) {
        outcome = {{"ok", false}, {"error", error_json(value.outcome.error())}};
    } else {
        const auto& original = *value.outcome.value();
        Json record{{"mutation_id", id_json(original.id)},
                    {"observed_revision", revision_json(original.observed_revision)},
                    {"result", effect_json(original.result)},
                    {"history_after", original.history_after ? history_json(*original.history_after)
                                                             : Json(nullptr)}};
        outcome = {{"ok", true}, {"value", std::move(record)}};
    }
    return {{"mutation_id", id_json(value.id)},
            {"state", authoring_mutation_state_name(value.state)},
            {"observed_revision", revision_json(value.observed_revision)},
            {"outcome", std::move(outcome)}};
}

Json batch_reply_json(const AuthoringBatchReply& value) {
    return {{"batch_id", value.id.value().to_string()},
            {"observed_revision", revision_json(value.observed_revision)},
            {"outcome", effect_json(value.outcome)}};
}

Json mutation_params_schema() {
    return object_schema({{"mutation_id", mutation_id_schema()},
                          {"expected_revision", revision_schema()},
                          {"operation", operation_schema()}},
                         {"mutation_id", "expected_revision", "operation"});
}

Json preview_params_schema() {
    return object_schema({{"batch", batch_schema()}, {"expected_revision", revision_schema()}},
                         {"batch", "expected_revision"});
}

Json history_schema() {
    const Json count{{"type", "integer"}, {"minimum", 0}};
    const Json group = nullable_schema(object_schema(
        {{"ticket", decimal_schema()}, {"command_count", count}}, {"ticket", "command_count"}));
    return object_schema(
        {{"revision", revision_schema()},
         {"synchronized", {{"type", "boolean"}}},
         {"undo_entries", count},
         {"redo_entries", count},
         {"retained_bytes", count},
         {"limits", object_schema({{"max_entries",
                                    {{"type", "integer"},
                                     {"minimum", 1},
                                     {"maximum", authoring_maximum_history_entries}}},
                                   {"max_retained_bytes",
                                    {{"type", "integer"},
                                     {"minimum", 1},
                                     {"maximum", authoring_maximum_history_bytes}}}},
                                  {"max_entries", "max_retained_bytes"})},
         {"next_undo", group},
         {"next_redo", group}},
        {"revision", "synchronized", "undo_entries", "redo_entries", "retained_bytes", "limits",
         "next_undo", "next_redo"});
}

Json mutation_status_schema() {
    const Json count{{"type", "integer"}, {"minimum", 0}};
    return object_schema(
        {{"scope", id_schema()},
         {"revision", revision_schema()},
         {"high_watermark", decimal_schema()},
         {"next", nullable_schema(mutation_id_schema())},
         {"oldest_retained", nullable_schema(mutation_id_schema())},
         {"retained_entries", count},
         {"retained_bytes", count},
         {"limits", object_schema({{"max_entries",
                                    {{"type", "integer"},
                                     {"minimum", 1},
                                     {"maximum", authoring_maximum_mutation_entries}}},
                                   {"max_retained_bytes",
                                    {{"type", "integer"},
                                     {"minimum", 1},
                                     {"maximum", authoring_maximum_mutation_bytes}}}},
                                  {"max_entries", "max_retained_bytes"})}},
        {"scope", "revision", "high_watermark", "next", "oldest_retained", "retained_entries",
         "retained_bytes", "limits"});
}

Json mutation_reply_schema() {
    const Json original =
        object_schema({{"mutation_id", mutation_id_schema()},
                       {"observed_revision", revision_schema()},
                       {"result", effect_schema()},
                       {"history_after", nullable_schema(history_schema())}},
                      {"mutation_id", "observed_revision", "result", "history_after"});
    return object_schema({{"mutation_id", mutation_id_schema()},
                          {"state",
                           {{"enum",
                             {"recorded", "retained", "replayed", "not_recorded", "unknown_outcome",
                              "payload_mismatch", "out_of_order", "not_admitted"}}}},
                          {"observed_revision", revision_schema()},
                          {"outcome", result_schema(original)}},
                         {"mutation_id", "state", "observed_revision", "outcome"});
}

Json batch_reply_schema() {
    return object_schema({{"batch_id", id_schema()},
                          {"observed_revision", revision_schema()},
                          {"outcome", effect_schema()}},
                         {"batch_id", "observed_revision", "outcome"});
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
