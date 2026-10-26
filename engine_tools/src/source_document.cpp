#include "shark/tools/source_document.hpp"
#include "asset_cache_format.hpp"
#include "shark/tools/asset_metadata.hpp"

#include "shark/schema.hpp"
#include "utf8.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <iomanip>
#include <limits>
#include <map>
#include <new>
#include <ostream>
#include <set>
#include <streambuf>
#include <utility>

namespace shark::tools {
namespace {

using Json = nlohmann::json;

// Validation unwinds only inside public Result boundaries below. Neither
// dependency exceptions nor caller-controlled exception text crosses that boundary.
struct SourceFailure {
    Error error;
};

[[noreturn]] void fail(std::string path, const char* message,
                       ErrorCode code = ErrorCode::invalid_configuration) {
    throw SourceFailure{Error{code, message, {.path = std::move(path)}}};
}

template <typename T> T checked(Result<T> result, std::string path, std::string entity = {}) {
    if (!result) {
        auto error = std::move(result).error();
        error.context.path = std::move(path);
        error.context.entity_id = std::move(entity);
        throw SourceFailure{std::move(error)};
    }
    return std::move(result).value();
}

void checked(Result<void> result, std::string path, std::string entity = {}) {
    if (!result) {
        auto error = std::move(result).error();
        error.context.path = std::move(path);
        error.context.entity_id = std::move(entity);
        throw SourceFailure{std::move(error)};
    }
}

std::string child_path(const std::string& parent, std::string_view key) {
    // Diagnostics never retain an arbitrarily long chain of attacker-owned keys.
    constexpr std::size_t maximum_pointer_bytes = 1024;
    std::string result = parent;
    result.push_back('/');
    for (const char character : key) {
        if (character == '~') {
            result += "~0";
        } else if (character == '/') {
            result += "~1";
        } else {
            result.push_back(character);
        }
        if (result.size() > maximum_pointer_bytes) {
            return parent;
        }
    }
    return result;
}

// SAX validates before the recursive DOM parser runs. Its lexer may retain at
// most the bounded input; duplicate decoded keys cannot be overwritten by a DOM.
// Nodes count containers, scalar values AND object keys. Root container depth=1.
class SourceSax final : public nlohmann::json_sax<Json> {
  public:
    explicit SourceSax(std::size_t enclosing_depth = 0) : enclosing_depth_(enclosing_depth) {}

    bool null() override {
        return scalar();
    }
    bool boolean(bool) override {
        return scalar();
    }
    bool number_integer(number_integer_t) override {
        return scalar();
    }
    bool number_unsigned(number_unsigned_t) override {
        return scalar();
    }
    bool number_float(number_float_t value, const string_t& token) override {
        if (!std::isfinite(value)) {
            return reject("JSON numbers must be finite");
        }
        if (token.find_first_of(".eE") == std::string::npos) {
            return reject("JSON integer exceeds signed or unsigned 64-bit storage");
        }
        return scalar();
    }
    bool string(string_t& value) override {
        return valid_string(value) && scalar();
    }
    bool binary(binary_t&) override {
        return reject("Binary JSON is unsupported");
    }
    bool start_object(std::size_t) override {
        return start(true);
    }
    bool start_array(std::size_t) override {
        return start(false);
    }
    bool end_object() override {
        return finish();
    }
    bool end_array() override {
        return finish();
    }
    bool key(string_t& value) override {
        if (!valid_string(value) || !node()) {
            return false;
        }
        auto& frame = frames_.back();
        frame.key = value;
        if (!frame.keys.insert(value).second) {
            return reject("Duplicate JSON object key");
        }
        return true;
    }
    bool parse_error(std::size_t, const std::string&, const Json::exception&) override {
        return reject("Malformed UTF-8 JSON document");
    }
    [[nodiscard]] const Error& error() const {
        return error_;
    }

  private:
    struct Frame {
        bool object;
        std::string path;
        std::string key;
        std::set<std::string, std::less<>> keys;
        std::size_t index{0};
    };
    std::vector<Frame> frames_;
    std::size_t enclosing_depth_;
    std::size_t nodes_{0};
    Error error_{ErrorCode::invalid_configuration, "Malformed UTF-8 JSON document", {}};

    std::string path() const {
        if (frames_.empty()) {
            return {};
        }
        const auto& frame = frames_.back();
        return child_path(frame.path, frame.object ? frame.key : std::to_string(frame.index));
    }
    bool reject(const char* message) {
        error_ = Error{ErrorCode::invalid_configuration, message, {.path = path()}};
        return false;
    }
    bool valid_string(const std::string& value) {
        if (value.size() > source_maximum_string_bytes) {
            return reject("JSON string exceeds the source byte limit");
        }
        return true;
    }
    bool node() {
        if (nodes_ == source_maximum_json_nodes) {
            return reject("JSON node budget exceeded");
        }
        ++nodes_;
        return true;
    }
    void complete_value() {
        if (!frames_.empty() && !frames_.back().object) {
            ++frames_.back().index;
        }
    }
    bool scalar() {
        if (!node()) {
            return false;
        }
        complete_value();
        return true;
    }
    bool start(bool object) {
        if (frames_.size() + enclosing_depth_ == source_maximum_json_depth) {
            return reject("JSON nesting depth exceeded");
        }
        if (!node()) {
            return false;
        }
        frames_.push_back(Frame{object, path(), {}, {}, 0});
        return true;
    }
    bool finish() {
        frames_.pop_back();
        complete_value();
        return true;
    }
};

Json parse_json(std::string_view input, const std::string& prefix = {},
                std::size_t enclosing_depth = 0) {
    if (input.size() > source_maximum_bytes) {
        fail(prefix, "Source input exceeds 4 MiB");
    }
    if (input.find('\0') != std::string_view::npos) {
        fail(prefix, "Source JSON cannot contain raw NUL bytes");
    }
    SourceSax sax{enclosing_depth};
    if (!Json::sax_parse(input.begin(), input.end(), &sax)) {
        auto error = sax.error();
        if (!prefix.empty()) {
            error.context.path = prefix + error.context.path;
        }
        throw SourceFailure{std::move(error)};
    }
    return Json::parse(input.begin(), input.end());
}

const Json& required(const Json& object, std::string_view key, const std::string& path) {
    if (!object.is_object()) {
        fail(path, "Expected a JSON object");
    }
    const auto found = object.find(key);
    if (found == object.end()) {
        fail(child_path(path, key), "Required source field is missing");
    }
    return *found;
}

void members(const Json& object, std::initializer_list<std::string_view> mandatory,
             std::initializer_list<std::string_view> optional, const std::string& path) {
    if (!object.is_object()) {
        fail(path, "Expected a JSON object");
    }
    for (const auto key : mandatory) {
        static_cast<void>(required(object, key, path));
    }
    for (auto iterator = object.begin(); iterator != object.end(); ++iterator) {
        const auto& key = iterator.key();
        if (std::find(mandatory.begin(), mandatory.end(), key) == mandatory.end() &&
            std::find(optional.begin(), optional.end(), key) == optional.end()) {
            fail(child_path(path, key), "Unknown field outside extras");
        }
    }
}

const std::string& string_value(const Json& value, const std::string& path) {
    if (!value.is_string()) {
        fail(path, "Expected a JSON string");
    }
    return value.get_ref<const std::string&>();
}

std::uint32_t version(const Json& value, const std::string& path, bool legacy_allowed) {
    if (!value.is_number_integer()) {
        fail(path, "Schema version must be an integer");
    }
    if (value == Json(1)) {
        return 1;
    }
    if (legacy_allowed && value == Json(0)) {
        return 0;
    }
    fail(path, legacy_allowed ? "Unsupported source schema; supported versions are 0 and 1"
                              : "Unsupported component schema; supported version is 1");
}

std::uint32_t envelope(const Json& object, std::string_view format) {
    if (string_value(required(object, "format", {}), "/format") != format) {
        fail("/format", "Source format discriminator does not match the document kind");
    }
    return version(required(object, "schema_version", {}), "/schema_version", true);
}

template <typename Id> Id id_value(const Json& value, const std::string& path) {
    return Id{checked(StableId::parse(string_value(value, path)), path)};
}

template <std::size_t Count>
std::array<double, Count> numbers(const Json& value, const std::string& path) {
    if (!value.is_array() || value.size() != Count) {
        fail(path, "Numeric array has the wrong dimension");
    }
    std::array<double, Count> result{};
    for (std::size_t index = 0; index < Count; ++index) {
        if (!value[index].is_number()) {
            fail(child_path(path, std::to_string(index)), "Expected a finite JSON number");
        }
        result[index] = value[index].get<double>();
        if (!std::isfinite(result[index])) {
            fail(child_path(path, std::to_string(index)), "Expected a finite JSON number");
        }
    }
    return result;
}

Name validated_name(const Name& value, const std::string& path) {
    // Check before passing the value variant, so caller-owned unbounded strings
    // cannot force an unbounded defensive copy.
    checked(validate_name(value), path);
    return std::get<Name>(checked(validate_component(ComponentSchemaId::name, 1, value), path));
}

Transform validated_transform(const Transform& value, const std::string& path) {
    return std::get<Transform>(
        checked(validate_component(ComponentSchemaId::transform, 1, value), path));
}

Transform read_transform(const Json& object, const std::string& path, bool legacy) {
    if (legacy) {
        members(object, {}, {"position", "rotation_wxyz", "scale"}, path);
    } else {
        members(object, {"schema_version"}, {"translation", "rotation", "scale"}, path);
        static_cast<void>(version(object.at("schema_version"), path + "/schema_version", false));
    }
    Transform result;
    const auto translation_key = legacy ? "position" : "translation";
    if (object.contains(translation_key)) {
        const auto value =
            numbers<3>(object.at(translation_key), child_path(path, translation_key));
        result.translation = math::Vec3{value[0], value[1], value[2]};
    }
    const auto rotation_key = legacy ? "rotation_wxyz" : "rotation";
    if (object.contains(rotation_key)) {
        const auto value = numbers<4>(object.at(rotation_key), child_path(path, rotation_key));
        result.rotation = legacy ? math::Quat{value[0], value[1], value[2], value[3]}
                                 : math::Quat{value[3], value[0], value[1], value[2]};
    }
    if (object.contains("scale")) {
        const auto value = numbers<3>(object.at("scale"), path + "/scale");
        result.scale = math::Vec3{value[0], value[1], value[2]};
    }
    return validated_transform(result, path);
}

std::int64_t cell_value(const Json& value, const std::string& path) {
    if (!value.is_number_integer()) {
        fail(path, "World cell must be an integer");
    }
    if (value.is_number_unsigned()) {
        const auto integer = value.get<std::uint64_t>();
        if (integer > static_cast<std::uint64_t>(world_cell_max)) {
            fail(path, "World cell exceeds signed 32-bit representation bounds");
        }
        return static_cast<std::int64_t>(integer);
    }
    const auto integer = value.get<std::int64_t>();
    if (integer < world_cell_min || integer > world_cell_max) {
        fail(path, "World cell exceeds signed 32-bit representation bounds");
    }
    return integer;
}

WorldPosition read_origin(const Json& object, const std::string& path) {
    members(object, {"world_id", "cell", "offset"}, {}, path);
    const auto world = id_value<WorldId>(object.at("world_id"), path + "/world_id");
    const auto& cell = object.at("cell");
    if (!cell.is_array() || cell.size() != 3) {
        fail(path + "/cell", "World cell requires three integer coordinates");
    }
    WorldPosition result{world,
                         {cell_value(cell[0], path + "/cell/0"),
                          cell_value(cell[1], path + "/cell/1"),
                          cell_value(cell[2], path + "/cell/2")},
                         numbers<3>(object.at("offset"), path + "/offset")};
    checked(validate_world_position(result), path);
    return result;
}

struct ExtrasBudget {
    std::size_t bytes{0};
    std::size_t raw_bytes{0};

    void reserve_raw(std::string_view text, const std::string& path) {
        if (text.size() > source_maximum_bytes - raw_bytes) {
            fail(path, "Combined caller-owned extras input exceeds 4 MiB");
        }
        raw_bytes += text.size();
    }

    std::string take(const Json& value, const std::string& path) {
        if (!value.is_object()) {
            fail(path, "Extras must be a JSON object");
        }
        auto compact = value.dump();
        if (compact.size() > source_maximum_extras_bytes) {
            fail(path, "Compact extras exceeds 4096 bytes");
        }
        if (compact.size() > source_maximum_total_extras_bytes - bytes) {
            fail(path, "Total compact extras exceeds 65536 bytes");
        }
        bytes += compact.size();
        return compact;
    }
    std::string read(const Json& object, const std::string& path) {
        const auto found = object.find("extras");
        return take(found == object.end() ? Json::object() : *found, path + "/extras");
    }
    std::string parse(std::string_view text, const std::string& path, std::size_t enclosing_depth) {
        return take(parse_json(text, path, enclosing_depth), path);
    }
};

void project_references(const ProjectDocument& document, bool legacy = false) {
    std::set<SceneId> ids;
    std::set<std::string, std::less<>> paths;
    for (std::size_t index = 0; index < document.scenes.size(); ++index) {
        const auto& entry = document.scenes[index];
        const auto path = "/scenes/" + std::to_string(index);
        // ProjectPath owns a movable string: a legal moved-from value must pass
        // the same lexical checks before a caller-owned document can be emitted.
        static_cast<void>(checked(ProjectPath::parse(entry.path.text()), path + "/path"));
        if (!ids.insert(entry.scene).second) {
            fail(path + (legacy ? "/id" : "/scene_id"), "Duplicate scene ID");
        }
        if (!paths.insert(entry.path.text()).second) {
            fail(path + "/path", "Duplicate project scene path");
        }
    }
    if (!ids.contains(document.startup_scene)) {
        fail(legacy ? "/start_scene" : "/startup_scene",
             "Startup scene does not resolve within this project");
    }
}

[[noreturn]] void fail_entity(std::size_t index, std::string_view key, const char* message,
                              const AuthoredEntityId& id) {
    throw SourceFailure{
        Error{ErrorCode::invalid_configuration,
              message,
              {.path = "/entities/" + std::to_string(index) + "/" + std::string{key},
               .entity_id = id.value().to_string()}}};
}

void scene_references(const SceneDocument& document, bool legacy = false) {
    const auto count = document.entities.size();
    std::map<AuthoredEntityId, std::size_t> indices;
    for (std::size_t index = 0; index < count; ++index) {
        if (!indices.emplace(document.entities[index].id, index).second) {
            fail_entity(index, legacy ? "id" : "entity_id", "Duplicate entity ID",
                        document.entities[index].id);
        }
    }
    constexpr auto no_parent = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> parents(count, no_parent);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& entity = document.entities[index];
        if (entity.parent) {
            const auto parent = indices.find(*entity.parent);
            if (parent == indices.end()) {
                fail_entity(index, "parent", "Parent does not resolve within this scene",
                            entity.id);
            }
            parents[index] = parent->second;
        }
    }
    // Each link is visited once after O(n log n) ID resolution. No recursive
    // traversal or repeated walking of already-qualified ancestor chains.
    std::vector<std::size_t> depths(count, 0);
    std::vector<bool> visiting(count, false);
    std::vector<std::size_t> chain;
    chain.reserve(std::min(count, source_maximum_hierarchy_depth + 1));
    for (std::size_t index = 0; index < count; ++index) {
        if (depths[index] != 0) {
            continue;
        }
        chain.clear();
        auto cursor = index;
        while (cursor != no_parent && depths[cursor] == 0) {
            if (visiting[cursor]) {
                fail_entity(cursor, "parent", "Scene parent hierarchy contains a cycle",
                            document.entities[cursor].id);
            }
            visiting[cursor] = true;
            chain.push_back(cursor);
            if (chain.size() > source_maximum_hierarchy_depth) {
                fail_entity(cursor, "parent", "Scene parent hierarchy exceeds depth 256",
                            document.entities[cursor].id);
            }
            cursor = parents[cursor];
        }
        auto depth = cursor == no_parent ? 0 : depths[cursor];
        for (auto iterator = chain.rbegin(); iterator != chain.rend(); ++iterator) {
            ++depth;
            if (depth > source_maximum_hierarchy_depth) {
                fail_entity(*iterator, "parent", "Scene parent hierarchy exceeds depth 256",
                            document.entities[*iterator].id);
            }
            depths[*iterator] = depth;
            visiting[*iterator] = false;
        }
    }
}

ProjectDocument read_project(const Json& root, std::uint32_t input_version) {
    const bool legacy = input_version == 0;
    const auto project_key = legacy ? "id" : "project_id";
    const auto startup_key = legacy ? "start_scene" : "startup_scene";
    members(root,
            {"format", "schema_version", project_key, "world_id", "name", startup_key, "scenes"},
            {"extras"}, {});
    const auto& scenes = root.at("scenes");
    if (!scenes.is_array() || scenes.empty() || scenes.size() > source_maximum_scenes) {
        fail("/scenes", "Project requires between 1 and 256 scene references");
    }
    ExtrasBudget extras;
    ProjectDocument result{id_value<ProjectId>(root.at(project_key), child_path({}, project_key)),
                           id_value<WorldId>(root.at("world_id"), "/world_id"),
                           validated_name(Name{string_value(root.at("name"), "/name")}, "/name"),
                           id_value<SceneId>(root.at(startup_key), child_path({}, startup_key)),
                           {},
                           extras.read(root, {})};
    result.scenes.reserve(scenes.size());
    for (std::size_t index = 0; index < scenes.size(); ++index) {
        const auto path = "/scenes/" + std::to_string(index);
        const auto id_key = legacy ? "id" : "scene_id";
        const auto& scene = scenes[index];
        members(scene, {id_key, "path"}, {}, path);
        result.scenes.push_back(
            {id_value<SceneId>(scene.at(id_key), child_path(path, id_key)),
             checked(ProjectPath::parse(string_value(scene.at("path"), path + "/path")),
                     path + "/path")});
    }
    project_references(result, legacy);
    return result;
}

SourceEntity read_entity(const Json& object, const std::string& path, bool legacy,
                         ExtrasBudget& extras) {
    const auto id_key = legacy ? "id" : "entity_id";
    if (legacy) {
        members(object, {id_key}, {"parent", "name", "transform", "extras"}, path);
    } else {
        members(object, {id_key, "components"}, {"parent", "extras"}, path);
    }
    const auto id = id_value<AuthoredEntityId>(object.at(id_key), child_path(path, id_key));
    try {
        SourceEntity entity{id, std::nullopt, {}, {}, extras.read(object, path)};
        if (object.contains("parent") && !object.at("parent").is_null()) {
            entity.parent = id_value<AuthoredEntityId>(object.at("parent"), path + "/parent");
        }
        if (legacy) {
            if (object.contains("name")) {
                entity.name = validated_name(Name{string_value(object.at("name"), path + "/name")},
                                             path + "/name");
            }
            if (object.contains("transform")) {
                entity.transform =
                    read_transform(object.at("transform"), path + "/transform", true);
            }
        } else {
            const auto& components = object.at("components");
            const auto components_path = path + "/components";
            members(components, {"shark.name", "shark.transform"}, {}, components_path);
            const auto name_path = components_path + "/shark.name";
            const auto& name = components.at("shark.name");
            members(name, {"schema_version"}, {"value"}, name_path);
            static_cast<void>(
                version(name.at("schema_version"), name_path + "/schema_version", false));
            if (name.contains("value")) {
                entity.name =
                    validated_name(Name{string_value(name.at("value"), name_path + "/value")},
                                   name_path + "/value");
            }
            entity.transform = read_transform(components.at("shark.transform"),
                                              components_path + "/shark.transform", false);
        }
        return entity;
    } catch (SourceFailure& failure) {
        failure.error.context.entity_id = id.value().to_string();
        throw;
    }
}

SceneDocument read_scene(const Json& root, std::uint32_t input_version) {
    const bool legacy = input_version == 0;
    const auto scene_key = legacy ? "id" : "scene_id";
    const auto origin_key = legacy ? "origin" : "world_origin";
    members(root, {"format", "schema_version", "project_id", scene_key, "entities"},
            {origin_key, "extras"}, {});
    const auto& entities = root.at("entities");
    if (!entities.is_array() || entities.size() > source_maximum_entities) {
        fail("/entities", "Scene requires an array of at most 4096 entities");
    }
    ExtrasBudget extras;
    SceneDocument result{id_value<ProjectId>(root.at("project_id"), "/project_id"),
                         id_value<SceneId>(root.at(scene_key), child_path({}, scene_key)),
                         std::nullopt,
                         {},
                         extras.read(root, {})};
    if (root.contains(origin_key) && !root.at(origin_key).is_null()) {
        result.world_origin = read_origin(root.at(origin_key), child_path({}, origin_key));
    }
    result.entities.reserve(entities.size());
    for (std::size_t index = 0; index < entities.size(); ++index) {
        result.entities.push_back(
            read_entity(entities[index], "/entities/" + std::to_string(index), legacy, extras));
    }
    scene_references(result, legacy);
    return result;
}

ProjectDocument validate_project(const ProjectDocument& document) {
    if (document.scenes.empty() || document.scenes.size() > source_maximum_scenes) {
        fail("/scenes", "Project requires between 1 and 256 scene references");
    }
    ExtrasBudget extras;
    extras.reserve_raw(document.extras_json, "/extras");
    ProjectDocument result{document.project,
                           document.world,
                           validated_name(document.name, "/name"),
                           document.startup_scene,
                           {},
                           extras.parse(document.extras_json, "/extras", 1)};
    project_references(document);
    result.scenes = document.scenes;
    return result;
}

SceneDocument validate_scene(const SceneDocument& document) {
    if (document.entities.size() > source_maximum_entities) {
        fail("/entities", "Scene exceeds 4096 entities");
    }
    // Bound all caller-owned raw extras before parsing or copying any candidate
    // data. Independent whitespace padding must not multiply parser work by the
    // entity count while remaining small after compact serialization.
    ExtrasBudget extras;
    extras.reserve_raw(document.extras_json, "/extras");
    for (std::size_t index = 0; index < document.entities.size(); ++index) {
        extras.reserve_raw(document.entities[index].extras_json,
                           "/entities/" + std::to_string(index) + "/extras");
    }
    if (document.world_origin) {
        checked(validate_world_position(*document.world_origin), "/world_origin");
    }
    scene_references(document);
    SceneDocument result{document.project,
                         document.scene,
                         document.world_origin,
                         {},
                         extras.parse(document.extras_json, "/extras", 1)};
    result.entities.reserve(document.entities.size());
    for (std::size_t index = 0; index < document.entities.size(); ++index) {
        const auto& entity = document.entities[index];
        const auto path = "/entities/" + std::to_string(index);
        try {
            result.entities.push_back(
                {entity.id, entity.parent,
                 validated_name(entity.name, path + "/components/shark.name/value"),
                 validated_transform(entity.transform, path + "/components/shark.transform"),
                 extras.parse(entity.extras_json, path + "/extras", 3)});
        } catch (SourceFailure& failure) {
            failure.error.context.entity_id = entity.id.value().to_string();
            throw;
        }
    }
    return result;
}

Json project_json(const ProjectDocument& document) {
    Json scenes = Json::array();
    for (const auto& scene : document.scenes) {
        scenes.push_back(
            Json{{"scene_id", scene.scene.value().to_string()}, {"path", scene.path.text()}});
    }
    return Json{{"format", "shark.project"},
                {"schema_version", source_document_version},
                {"project_id", document.project.value().to_string()},
                {"world_id", document.world.value().to_string()},
                {"name", document.name.value},
                {"startup_scene", document.startup_scene.value().to_string()},
                {"scenes", std::move(scenes)},
                {"extras", Json::parse(document.extras_json)}};
}

Json scene_json(const SceneDocument& document) {
    Json entities = Json::array();
    for (const auto& entity : document.entities) {
        const auto& transform = entity.transform;
        Json components{
            {"shark.name",
             {{"schema_version", component_schema_version}, {"value", entity.name.value}}},
            {"shark.transform",
             {{"schema_version", component_schema_version},
              {"translation",
               {transform.translation.x, transform.translation.y, transform.translation.z}},
              {"rotation",
               {transform.rotation.x, transform.rotation.y, transform.rotation.z,
                transform.rotation.w}},
              {"scale", {transform.scale.x, transform.scale.y, transform.scale.z}}}}};
        entities.push_back(
            Json{{"entity_id", entity.id.value().to_string()},
                 {"parent", entity.parent ? Json(entity.parent->value().to_string()) : Json{}},
                 {"components", std::move(components)},
                 {"extras", Json::parse(entity.extras_json)}});
    }
    Json origin = nullptr;
    if (document.world_origin) {
        const auto& position = *document.world_origin;
        origin = Json{{"world_id", position.world.value().to_string()},
                      {"cell", {position.cell.x, position.cell.y, position.cell.z}},
                      {"offset", position.offset}};
    }
    return Json{{"format", "shark.scene"},
                {"schema_version", source_document_version},
                {"project_id", document.project.value().to_string()},
                {"scene_id", document.scene.value().to_string()},
                {"world_origin", std::move(origin)},
                {"entities", std::move(entities)},
                {"extras", Json::parse(document.extras_json)}};
}

std::string asset_text(const std::string& text, std::size_t maximum, const std::string& path,
                       bool token = false) {
    if (text.empty() || text.size() > maximum || !shark::detail::valid_utf8(text, true)) {
        fail(path, "Asset metadata requires bounded nonempty UTF-8 text without controls");
    }
    if (token && (text.front() < 'a' || text.front() > 'z' ||
                  !std::all_of(text.begin(), text.end(), [](char c) {
                      return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                             c == '_' || c == '-';
                  }))) {
        fail(path, "Asset kind/importer must be a lowercase identifier");
    }
    return text;
}

std::string asset_object(std::string_view text, const std::string& path) {
    if (text.size() > source_maximum_extras_bytes) {
        fail(path, "Asset settings/extras input exceeds 4096 bytes");
    }
    const auto object = parse_json(text, path, 1);
    if (!object.is_object()) {
        fail(path, "Asset settings/extras must be JSON objects");
    }
    const auto compact = object.dump();
    if (compact.size() > source_maximum_extras_bytes) {
        fail(path, "Canonical asset settings/extras exceeds 4096 bytes");
    }
    return compact;
}

AssetMetadata validate_asset(const AssetMetadata& value) {
    if (value.dependencies.size() > asset_maximum_dependencies) {
        fail("/dependencies", "Asset dependency limit is 256");
    }
    auto kind = asset_text(value.kind, 64, "/kind", true);
    auto origin = asset_text(value.origin, 1024, "/origin");
    auto license = asset_text(value.license, 128, "/license");
    auto importer = asset_text(value.importer, 64, "/importer", true);
    if (value.audience != AssetAudience::client && value.audience != AssetAudience::server &&
        value.audience != AssetAudience::shared) {
        fail("/audience", "Unknown asset audience");
    }
    auto settings = asset_object(value.settings_json, "/settings");
    auto extras = asset_object(value.extras_json, "/extras");
    auto dependencies = value.dependencies;
    std::sort(dependencies.begin(), dependencies.end());
    if (std::adjacent_find(dependencies.begin(), dependencies.end()) != dependencies.end() ||
        std::binary_search(dependencies.begin(), dependencies.end(), value.asset)) {
        fail("/dependencies", "Duplicate or self asset dependency");
    }
    return {value.project,           value.asset,         std::move(kind), std::move(origin),
            std::move(license),      std::move(importer), value.audience,  std::move(settings),
            std::move(dependencies), std::move(extras)};
}

AssetMetadata read_asset(const Json& root) {
    members(root,
            {"format", "schema_version", "project_id", "asset_id", "kind", "origin", "license",
             "importer", "audience", "settings", "dependencies"},
            {"extras"}, {});
    if (string_value(root.at("format"), "/format") != "shark.asset") {
        fail("/format", "Expected shark.asset metadata");
    }
    static_cast<void>(version(root.at("schema_version"), "/schema_version", false));
    const auto audience = string_value(root.at("audience"), "/audience");
    if (audience != "client" && audience != "server" && audience != "shared") {
        fail("/audience", "Unknown asset audience");
    }
    const auto& dependencies = root.at("dependencies");
    if (!dependencies.is_array() || dependencies.size() > asset_maximum_dependencies) {
        fail("/dependencies", "Expected at most 256 dependency IDs");
    }
    std::vector<AssetId> ids;
    for (std::size_t i = 0; i < dependencies.size(); ++i) {
        ids.push_back(id_value<AssetId>(dependencies[i], "/dependencies/" + std::to_string(i)));
    }
    return validate_asset(
        {id_value<ProjectId>(root.at("project_id"), "/project_id"),
         id_value<AssetId>(root.at("asset_id"), "/asset_id"),
         string_value(root.at("kind"), "/kind"), string_value(root.at("origin"), "/origin"),
         string_value(root.at("license"), "/license"),
         string_value(root.at("importer"), "/importer"),
         audience == "client"   ? AssetAudience::client
         : audience == "server" ? AssetAudience::server
                                : AssetAudience::shared,
         root.at("settings").dump(), std::move(ids), root.value("extras", Json::object()).dump()});
}

Json asset_json(const AssetMetadata& value) {
    Json dependencies = Json::array();
    for (const auto& dependency : value.dependencies) {
        dependencies.push_back(dependency.value().to_string());
    }
    const auto audience = value.audience == AssetAudience::client   ? "client"
                          : value.audience == AssetAudience::server ? "server"
                                                                    : "shared";
    return Json{{"format", "shark.asset"},
                {"schema_version", asset_metadata_version},
                {"project_id", value.project.value().to_string()},
                {"asset_id", value.asset.value().to_string()},
                {"kind", value.kind},
                {"origin", value.origin},
                {"license", value.license},
                {"importer", value.importer},
                {"audience", audience},
                {"settings", Json::parse(value.settings_json)},
                {"dependencies", std::move(dependencies)},
                {"extras", Json::parse(value.extras_json)}};
}

class BoundedOutput final : public std::streambuf {
  public:
    std::string text;

  private:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (count < 0 || static_cast<std::uintmax_t>(count) > source_maximum_bytes - text.size()) {
            fail({}, "Encoded source exceeds 4 MiB");
        }
        text.append(data, static_cast<std::size_t>(count));
        return count;
    }
    int_type overflow(int_type character) override {
        if (traits_type::eq_int_type(character, traits_type::eof())) {
            return traits_type::not_eof(character);
        }
        if (text.size() == source_maximum_bytes) {
            fail({}, "Encoded source exceeds 4 MiB");
        }
        text.push_back(traits_type::to_char_type(character));
        return character;
    }
};

std::string encode_json(const Json& value) {
    BoundedOutput buffer;
    std::ostream output{&buffer};
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << std::setw(2) << value << '\n';
    // The typed envelope plus independently parsed extras still share the same
    // overall syntax/node/depth/string budget as a subsequently decoded file.
    SourceSax sax;
    if (!Json::sax_parse(buffer.text.begin(), buffer.text.end(), &sax)) {
        throw SourceFailure{sax.error()};
    }
    return std::move(buffer.text);
}

template <typename T, typename Function>
Result<T> boundary(const char* operation, Function&& function) {
    try {
        return function();
    } catch (SourceFailure& failure) {
        failure.error.context.operation = operation;
        return std::move(failure.error);
    } catch (const std::bad_alloc&) {
        return Error{ErrorCode::execution_failed, "Source document allocation failed", {operation}};
    } catch (const Json::exception&) {
        return Error{ErrorCode::invalid_configuration,
                     "Source JSON value cannot be represented",
                     {operation}};
    } catch (const std::exception&) {
        return Error{ErrorCode::execution_failed, "Source document operation failed", {operation}};
    } catch (...) {
        return Error{ErrorCode::execution_failed, "Source document operation failed", {operation}};
    }
}

} // namespace

Result<DecodedSource<ProjectDocument>> decode_project_document(std::string_view text) {
    return boundary<DecodedSource<ProjectDocument>>("source.project.decode", [&] {
        const auto root = parse_json(text);
        const auto input_version = envelope(root, "shark.project");
        return DecodedSource<ProjectDocument>{read_project(root, input_version), input_version};
    });
}

Result<DecodedSource<SceneDocument>> decode_scene_document(std::string_view text) {
    return boundary<DecodedSource<SceneDocument>>("source.scene.decode", [&] {
        const auto root = parse_json(text);
        const auto input_version = envelope(root, "shark.scene");
        return DecodedSource<SceneDocument>{read_scene(root, input_version), input_version};
    });
}

Result<ProjectDocument> validate_project_document(const ProjectDocument& document) {
    return boundary<ProjectDocument>("source.project.validate",
                                     [&] { return validate_project(document); });
}

Result<SceneDocument> validate_scene_document(const SceneDocument& document) {
    return boundary<SceneDocument>("source.scene.validate",
                                   [&] { return validate_scene(document); });
}

Result<std::string> encode_project_document(const ProjectDocument& document) {
    return boundary<std::string>("source.project.encode", [&] {
        return encode_json(project_json(validate_project(document)));
    });
}

Result<std::string> encode_scene_document(const SceneDocument& document) {
    return boundary<std::string>("source.scene.encode",
                                 [&] { return encode_json(scene_json(validate_scene(document))); });
}

Result<AssetMetadata> decode_asset_metadata(std::string_view text) {
    return boundary<AssetMetadata>("asset.metadata.decode", [&] {
        if (text.size() > asset_metadata_maximum_bytes) {
            fail({}, "Asset metadata exceeds 64 KiB");
        }
        return read_asset(parse_json(text));
    });
}

Result<AssetMetadata> validate_asset_metadata(const AssetMetadata& metadata) {
    return boundary<AssetMetadata>("asset.metadata.validate",
                                   [&] { return validate_asset(metadata); });
}

Result<std::string> encode_asset_metadata(const AssetMetadata& metadata) {
    return boundary<std::string>("asset.metadata.encode", [&] {
        auto text = encode_json(asset_json(validate_asset(metadata)));
        if (text.size() > asset_metadata_maximum_bytes) {
            fail({}, "Encoded asset metadata exceeds 64 KiB");
        }
        return text;
    });
}

Result<ProjectPath> asset_sidecar_path(const ProjectPath& source) {
    return boundary<ProjectPath>("asset.path.sidecar", [&] {
        auto path = checked(ProjectPath::parse(source.text()), "/source");
        auto folded = path.text();
        std::transform(folded.begin(), folded.end(), folded.begin(), [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        });
        if (folded == asset_development_cache_name || folded.starts_with(".shark-cache-stage-")) {
            fail("/source", "Development cache paths are reserved");
        }
        if (folded.ends_with(".sharkasset")) {
            fail("/source", "An asset sidecar cannot itself be an asset source");
        }
        return checked(ProjectPath::parse(path.text() + ".sharkasset"), "/source");
    });
}

Result<ProjectPath> asset_source_path(const ProjectPath& sidecar) {
    return boundary<ProjectPath>("asset.path.source", [&] {
        const auto path = checked(ProjectPath::parse(sidecar.text()), "/sidecar");
        constexpr std::string_view suffix = ".sharkasset";
        if (!path.text().ends_with(suffix)) {
            fail("/sidecar", "Asset sidecar requires the exact .sharkasset suffix");
        }
        auto source =
            checked(ProjectPath::parse(path.text().substr(0, path.text().size() - suffix.size())),
                    "/sidecar");
        static_cast<void>(checked(asset_sidecar_path(source), "/sidecar"));
        return source;
    });
}

namespace detail {
namespace {
ContentDigest cache_digest(const Json& value, const std::string& path) {
    return checked(ContentDigest::parse(string_value(value, path)), path);
}
AssetAudience cache_audience(const Json& value, const std::string& path) {
    const auto& text = string_value(value, path);
    if (text == "client")
        return AssetAudience::client;
    if (text == "server")
        return AssetAudience::server;
    if (text == "shared")
        return AssetAudience::shared;
    fail(path, "Unknown cache audience");
}
const char* audience_text(AssetAudience audience) {
    if (audience == AssetAudience::client)
        return "client";
    if (audience == AssetAudience::server)
        return "server";
    if (audience == AssetAudience::shared)
        return "shared";
    fail("/audience", "Unknown cache audience");
}
std::uint32_t cache_u32(const Json& value, const std::string& path) {
    if (!value.is_number_integer() || value <= Json(0) ||
        value > Json(std::numeric_limits<std::uint32_t>::max())) {
        fail(path, "Cache versions require a nonzero uint32 integer");
    }
    return value.get<std::uint32_t>();
}
std::size_t cache_size(const Json& value, const std::string& path) {
    if (!value.is_number_integer() || value < Json(0) ||
        value > Json(asset_cache_maximum_file_bytes)) {
        fail(path, "Cache offset/length is outside its byte limit");
    }
    return value.get<std::size_t>();
}
AssetTargetProfile cache_target(const Json& object, const std::string& path) {
    members(object, {"id", "side"}, {}, path);
    AssetTargetProfile target{string_value(object.at("id"), path + "/id"),
                              cache_audience(object.at("side"), path + "/side")};
    checked(validate_target(target), path);
    return target;
}
Json target_json(const AssetTargetProfile& target) {
    return {{"id", target.id}, {"side", audience_text(target.side)}};
}
} // namespace

Result<std::string> canonical_import_object(std::string_view text) {
    return boundary<std::string>("asset.settings.canonical",
                                 [&] { return asset_object(text, "/settings"); });
}
Result<std::string> normalize_byte_settings(std::string_view text) {
    return boundary<std::string>("asset.bytes.settings", [&] {
        const auto canonical = asset_object(text, "/settings");
        const auto object = parse_json(canonical);
        members(object, {}, {"transform"}, "/settings");
        const auto transform = object.contains("transform")
                                   ? string_value(object.at("transform"), "/settings/transform")
                                   : "identity";
        if (transform != "identity" && transform != "uppercase-ascii")
            fail("/settings/transform", "Unsupported byte transform");
        return Json{{"transform", transform}}.dump();
    });
}
Result<std::string> encode_cache_index(const CacheIndex& index) {
    return boundary<std::string>("asset.cache.index.encode", [&] {
        if (index.entries.size() > asset_cache_maximum_entries ||
            index.bindings.size() > asset_cache_maximum_entries) {
            fail({}, "Cache index entry limit exceeded");
        }
        Json entries = Json::array();
        for (const auto& entry : index.entries) {
            const auto& a = entry.artifact;
            Json dependencies = Json::array();
            for (const auto& dependency : a.dependencies) {
                dependencies.push_back({{"asset_id", dependency.asset.value().to_string()},
                                        {"key", dependency.key.to_hex()}});
            }
            entries.push_back({{"asset_id", a.asset.value().to_string()},
                               {"key", a.key.to_hex()},
                               {"kind", a.kind},
                               {"audience", audience_text(a.audience)},
                               {"target", target_json(a.target)},
                               {"importer", a.importer},
                               {"importer_version", a.importer_version},
                               {"payload_version", a.payload_version},
                               {"source_digest", a.source_digest.to_hex()},
                               {"settings_digest", a.settings_digest.to_hex()},
                               {"dependencies", std::move(dependencies)},
                               {"payload_digest", a.payload_digest.to_hex()},
                               {"offset", entry.offset},
                               {"length", entry.length}});
        }
        Json bindings = Json::array();
        for (const auto& binding : index.bindings) {
            bindings.push_back({{"asset_id", binding.asset.value().to_string()},
                                {"target", target_json(binding.target)},
                                {"key", binding.key.to_hex()}});
        }
        return encode_json({{"format", "shark.development-cache"},
                            {"schema_version", 1},
                            {"project_id", index.project.value().to_string()},
                            {"entries", std::move(entries)},
                            {"bindings", std::move(bindings)}});
    });
}
Result<CacheIndex> decode_cache_index(std::string_view text) {
    return boundary<CacheIndex>("asset.cache.index.decode", [&] {
        const auto root = parse_json(text);
        members(root, {"format", "schema_version", "project_id", "entries", "bindings"}, {}, {});
        if (string_value(root.at("format"), "/format") != "shark.development-cache")
            fail("/format", "Wrong development cache format");
        static_cast<void>(version(root.at("schema_version"), "/schema_version", false));
        const auto& entries = root.at("entries");
        const auto& bindings = root.at("bindings");
        if (!entries.is_array() || !bindings.is_array() ||
            entries.size() > asset_cache_maximum_entries ||
            bindings.size() > asset_cache_maximum_entries) {
            fail({}, "Cache entries/bindings must be bounded arrays");
        }
        CacheIndex index{id_value<ProjectId>(root.at("project_id"), "/project_id"), {}, {}};
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto path = "/entries/" + std::to_string(i);
            const auto& e = entries[i];
            members(e,
                    {"asset_id", "key", "kind", "audience", "target", "importer",
                     "importer_version", "payload_version", "source_digest", "settings_digest",
                     "dependencies", "payload_digest", "offset", "length"},
                    {}, path);
            const auto& dependencies = e.at("dependencies");
            if (!dependencies.is_array() || dependencies.size() > asset_maximum_dependencies)
                fail(path + "/dependencies", "Cache dependency bound exceeded");
            std::vector<AssetArtifactDependency> ids;
            for (std::size_t d = 0; d < dependencies.size(); ++d) {
                const auto dp = path + "/dependencies/" + std::to_string(d);
                const auto& value = dependencies[d];
                members(value, {"asset_id", "key"}, {}, dp);
                ids.push_back({id_value<AssetId>(value.at("asset_id"), dp + "/asset_id"),
                               cache_digest(value.at("key"), dp + "/key")});
            }
            index.entries.push_back(
                {{id_value<AssetId>(e.at("asset_id"), path + "/asset_id"),
                  cache_digest(e.at("key"), path + "/key"),
                  asset_text(string_value(e.at("kind"), path + "/kind"), 64, path + "/kind", true),
                  cache_audience(e.at("audience"), path + "/audience"),
                  cache_target(e.at("target"), path + "/target"),
                  asset_text(string_value(e.at("importer"), path + "/importer"), 64,
                             path + "/importer", true),
                  cache_u32(e.at("importer_version"), path + "/importer_version"),
                  cache_u32(e.at("payload_version"), path + "/payload_version"),
                  cache_digest(e.at("source_digest"), path + "/source_digest"),
                  cache_digest(e.at("settings_digest"), path + "/settings_digest"),
                  std::move(ids),
                  cache_digest(e.at("payload_digest"), path + "/payload_digest"),
                  {}},
                 cache_size(e.at("offset"), path + "/offset"),
                 cache_size(e.at("length"), path + "/length")});
        }
        for (std::size_t i = 0; i < bindings.size(); ++i) {
            const auto path = "/bindings/" + std::to_string(i);
            const auto& b = bindings[i];
            members(b, {"asset_id", "target", "key"}, {}, path);
            index.bindings.push_back({id_value<AssetId>(b.at("asset_id"), path + "/asset_id"),
                                      cache_target(b.at("target"), path + "/target"),
                                      cache_digest(b.at("key"), path + "/key")});
        }
        return index;
    });
}
} // namespace detail

} // namespace shark::tools
