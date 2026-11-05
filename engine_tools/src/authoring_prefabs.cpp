#include "authoring_prefabs.hpp"

#include "authoring_asset_wire.hpp"
#include "authoring_wire.hpp"
#include "shark/tools/prefab_instance.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace shark::tools {
namespace {
using Json = nlohmann::json;
constexpr std::size_t maximum_source_tokens = 32;
constexpr std::size_t maximum_preparations = 8;
constexpr std::size_t maximum_retained_bytes = 16 * 1024 * 1024;
constexpr std::array method_names{std::string_view{"prefab.read"},
                                  std::string_view{"prefab.write"},
                                  std::string_view{"prefab.capture"},
                                  std::string_view{"prefab.prepare"},
                                  std::string_view{"prefab.instantiate"},
                                  std::string_view{"prefab.release"},
                                  std::string_view{"prefab.release_preparation"}};

[[noreturn]] void fail(int code, ErrorCode error, const char* message, std::string path = {}) {
    throw AuthoringPrefabFailure{
        code, {error, message, {.operation = "authoring.prefab", .path = std::move(path)}}};
}

template <typename T> T take(Result<T> value, int code = -32000) {
    if (!value)
        throw AuthoringPrefabFailure{code, std::move(value).error()};
    return std::move(value).value();
}

void fields(const Json& value, std::initializer_list<std::string_view> allowed) {
    auto checked = authoring_asset_wire::require_fields(value, allowed);
    if (!checked)
        throw AuthoringPrefabFailure{-32602, std::move(checked).error()};
}

const Json& parameter(const Json& value, const char* name) {
    const auto found = value.find(name);
    if (found == value.end())
        fail(-32602, ErrorCode::invalid_argument, "Missing prefab parameter",
             std::string{"/params/"} + name);
    return *found;
}

const std::string& string(const Json& value, const char* path) {
    if (!value.is_string())
        fail(-32602, ErrorCode::invalid_argument, "Expected a string", path);
    return value.get_ref<const std::string&>();
}

StableId identity(const Json& value, const char* path) {
    return take(StableId::parse(string(value, path)), -32602);
}

std::string token(const Json& value, const char* path) {
    return identity(value, path).to_string();
}

std::string folded(std::string_view value) {
    std::string result(value);
    for (auto& character : result)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    return result;
}

bool overlaps(const ProjectPath& first, const ProjectPath& second) {
    const auto a = folded(first.text());
    const auto b = folded(second.text());
    return a == b || a.starts_with(b + "/") || b.starts_with(a + "/");
}

double scalar(const Json& value) {
    if (!value.is_number())
        fail(-32602, ErrorCode::invalid_argument, "Expected a finite transform number");
    const auto result = value.get<double>();
    if (!std::isfinite(result))
        fail(-32602, ErrorCode::invalid_argument, "Expected a finite transform number");
    return result;
}

void vector_size(const Json& value, std::size_t size) {
    if (!value.is_array() || value.size() != size)
        fail(-32602, ErrorCode::invalid_argument, "Transform vector has an invalid length");
}

math::Vec3 vector3(const Json& value) {
    vector_size(value, 3);
    return {scalar(value[0]), scalar(value[1]), scalar(value[2])};
}

Transform transform(const Json& value) {
    fields(value, {"translation", "rotation", "scale"});
    const auto& rotation = parameter(value, "rotation");
    vector_size(rotation, 4);
    return {vector3(parameter(value, "translation")),
            math::Quat{scalar(rotation[3]), scalar(rotation[0]), scalar(rotation[1]),
                       scalar(rotation[2])},
            vector3(parameter(value, "scale"))};
}

struct SourceObservation {
    ProjectPath path;
    std::optional<FileRevision> revision;
    std::size_t charge;
};
struct Preparation {
    InstantiatePrefabCommand command;
    SceneRevision revision;
    std::size_t charge;
};
using Sources = std::map<std::string, SourceObservation>;
using Preparations = std::map<std::string, Preparation>;

std::size_t source_charge(const ProjectPath& path, std::size_t bytes) {
    return sizeof(SourceObservation) + 256 + path.text().capacity() + bytes;
}

std::size_t preparation_charge(const InstantiatePrefabCommand& command) {
    std::size_t result = sizeof(Preparation) + 257 + command.prefab.extras_json.capacity();
    result += command.prefab.entities.capacity() * sizeof(SourceEntity);
    result += command.remap.capacity() * sizeof(PrefabEntityRemap);
    result += command.overrides.capacity() * sizeof(PrefabEntityOverride);
    for (const auto& entity : command.prefab.entities) {
        result += entity.name.value.capacity() + entity.extras_json.capacity() + 2;
        if (entity.script)
            result += entity.script->source.capacity() + 1;
    }
    for (const auto& value : command.overrides)
        if (value.name)
            result += value.name->value.capacity() + 1;
    return result;
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
    const Json decimal{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]{0,19})$"}};
    return object_schema({{"scene_instance", decimal}, {"revision", decimal}},
                         {"scene_instance", "revision"});
}
Json result_schema(std::initializer_list<const char*> names, const char* description) {
    Json properties = Json::object();
    Json required = Json::array();
    for (const auto* name : names) {
        properties[name] = Json::object();
        required.push_back(name);
    }
    auto result = object_schema(std::move(properties), std::move(required));
    result["description"] = description;
    return result;
}
} // namespace

struct AuthoringPrefabAdmission {
    Sources::node_type source;
    Preparations::node_type preparation;
    std::optional<std::string> release_source;
    std::optional<std::string> release_preparation;
};

AuthoringPrefabReply::AuthoringPrefabReply(Json value) : result(std::move(value)) {}
AuthoringPrefabReply::~AuthoringPrefabReply() = default;
AuthoringPrefabReply::AuthoringPrefabReply(AuthoringPrefabReply&&) noexcept = default;
AuthoringPrefabReply& AuthoringPrefabReply::operator=(AuthoringPrefabReply&&) noexcept = default;

struct AuthoringPrefabs::Impl {
    ProjectFiles& files;
    const ProjectDocument& project;
    const ProjectPath& project_path;
    AuthoredScene& owner;
    Sources sources;
    Preparations preparations;
    std::size_t retained_bytes{0};

    Impl(ProjectFiles& storage, const ProjectDocument& document, const ProjectPath& path,
         AuthoredScene& scene)
        : files(storage), project(document), project_path(path), owner(scene) {}

    void check_path(const ProjectPath& path) const {
        const auto sidecar = take(asset_sidecar_path(path), -32602);
        for (const auto& candidate : {path, sidecar}) {
            if (overlaps(candidate, project_path))
                fail(-32000, ErrorCode::conflict, "Prefab access overlaps the owned project source",
                     candidate.text());
            for (const auto& scene : project.scenes)
                if (overlaps(candidate, scene.path))
                    fail(-32000, ErrorCode::conflict,
                         "Prefab access overlaps an owned scene source", candidate.text());
        }
    }

    void check_project(const PrefabDocument& prefab) const {
        if (prefab.project != project.project)
            fail(-32000, ErrorCode::conflict, "Prefab belongs to a different project");
    }

    void capacity(std::size_t added, std::size_t removed = 0) const {
        if (added > maximum_retained_bytes ||
            retained_bytes - removed > maximum_retained_bytes - added)
            fail(
                -32000, ErrorCode::conflict,
                "Prefab retained input limit reached; release unused observations or preparations");
    }

    std::string fresh_token() const {
        auto result = take(generate_stable_id()).to_string();
        if (sources.contains(result) || preparations.contains(result))
            fail(-32000, ErrorCode::conflict, "Generated prefab token already exists");
        return result;
    }
};

AuthoringPrefabs::AuthoringPrefabs(ProjectFiles& files, const ProjectDocument& project,
                                   const ProjectPath& project_path, AuthoredScene& owner)
    : impl_(std::make_unique<Impl>(files, project, project_path, owner)) {}
AuthoringPrefabs::~AuthoringPrefabs() = default;

bool AuthoringPrefabs::supports(std::string_view method) noexcept {
    return std::find(method_names.begin(), method_names.end(), method) != method_names.end();
}

AuthoringPrefabReply AuthoringPrefabs::dispatch(std::string_view method, const Json& parameters,
                                                bool& outcome_may_have_changed) {
    if (method == "prefab.read") {
        fields(parameters, {"path"});
        auto path =
            take(ProjectPath::parse(string(parameter(parameters, "path"), "/params/path")), -32602);
        impl_->check_path(path);
        if (impl_->sources.size() >= maximum_source_tokens)
            fail(-32000, ErrorCode::conflict, "Prefab source token limit reached");
        auto observed = take(impl_->files.read_prefab(path));
        impl_->check_project(observed.source.document);
        auto encoded = take(encode_prefab_document(observed.source.document));
        const auto charge = source_charge(path, observed.revision.byte_count());
        impl_->capacity(charge);
        auto identity = impl_->fresh_token();
        const auto& prefab = observed.source.document;
        AuthoringPrefabReply reply{Json{{"path", path.text()},
                                        {"document", std::move(encoded)},
                                        {"revision", identity},
                                        {"asset_id", prefab.asset.value().to_string()},
                                        {"root_entity_id", prefab.root.value().to_string()},
                                        {"entity_count", prefab.entities.size()}}};
        Sources staged;
        staged.emplace(identity,
                       SourceObservation{std::move(path), std::move(observed.revision), charge});
        reply.admission_ = std::make_unique<AuthoringPrefabAdmission>();
        reply.admission_->source = staged.extract(identity);
        return reply;
    }
    if (method == "prefab.release" || method == "prefab.release_preparation") {
        const bool source = method == "prefab.release";
        fields(parameters, source ? std::initializer_list<std::string_view>{"revision"}
                                  : std::initializer_list<std::string_view>{"preparation"});
        auto identity = token(parameter(parameters, source ? "revision" : "preparation"),
                              source ? "/params/revision" : "/params/preparation");
        if ((source && !impl_->sources.contains(identity)) ||
            (!source && !impl_->preparations.contains(identity)))
            fail(-32000, ErrorCode::conflict,
                 "Prefab token was released or belongs to another owner");
        AuthoringPrefabReply reply{Json{{"released", true}}};
        reply.admission_ = std::make_unique<AuthoringPrefabAdmission>();
        if (source)
            reply.admission_->release_source = std::move(identity);
        else
            reply.admission_->release_preparation = std::move(identity);
        return reply;
    }
    if (method == "prefab.write") {
        fields(parameters, {"path", "document", "expected_revision"});
        auto path =
            take(ProjectPath::parse(string(parameter(parameters, "path"), "/params/path")), -32602);
        impl_->check_path(path);
        auto decoded = take(
            decode_prefab_document(string(parameter(parameters, "document"), "/params/document")),
            -32602);
        impl_->check_project(decoded.document);
        // Encode now to determine and reserve the final retained byte charge.
        const auto encoded = take(encode_prefab_document(decoded.document));
        const auto charge = source_charge(path, encoded.size());
        std::optional<std::string> consumed;
        std::optional<FileRevision> expected;
        std::size_t removed = 0;
        const auto& requested = parameter(parameters, "expected_revision");
        if (!requested.is_null()) {
            consumed = token(requested, "/params/expected_revision");
            const auto found = impl_->sources.find(*consumed);
            if (found == impl_->sources.end() || found->second.path != path ||
                !found->second.revision)
                fail(-32000, ErrorCode::conflict,
                     "Prefab source revision was consumed or belongs to another path/owner");
            expected = found->second.revision;
            removed = found->second.charge;
        }
        if (!consumed && impl_->sources.size() >= maximum_source_tokens)
            fail(-32000, ErrorCode::conflict, "Prefab source token limit reached");
        impl_->capacity(charge, removed);
        auto identity = impl_->fresh_token();
        // Allocate the durable observation slot before a native publication.
        const auto inserted = impl_->sources.emplace(identity, SourceObservation{path, {}, charge});
        auto committed = [&] {
            try {
                return impl_->files.write_prefab(path, decoded.document, expected);
            } catch (...) {
                impl_->sources.erase(inserted.first);
                throw;
            }
        }();
        if (!committed) {
            impl_->sources.erase(inserted.first);
            static_cast<void>(take(std::move(committed)));
        }
        outcome_may_have_changed = true;
        inserted.first->second.revision = committed.value().revision;
        if (consumed)
            impl_->sources.erase(*consumed);
        impl_->retained_bytes = impl_->retained_bytes - removed + charge;
        return AuthoringPrefabReply{
            Json{{"path", path.text()},
                 {"revision", identity},
                 {"publication", authoring_asset_wire::publication_json(committed.value())}}};
    }
    if (method == "prefab.capture") {
        fields(parameters, {"entity_id", "asset_id", "expected_revision"});
        const AuthoredEntityId root{
            identity(parameter(parameters, "entity_id"), "/params/entity_id")};
        const AssetId asset{identity(parameter(parameters, "asset_id"), "/params/asset_id")};
        const auto revision = take(
            authoring_wire::parse_revision(parameter(parameters, "expected_revision")), -32602);
        auto captured = take(impl_->owner.capture(revision));
        std::multimap<AuthoredEntityId, AuthoredEntityId> children;
        bool found = false;
        for (const auto& entity : captured.scene.entities) {
            found = found || entity.id == root;
            if (entity.parent)
                children.emplace(*entity.parent, entity.id);
        }
        if (!found)
            fail(-32000, ErrorCode::invalid_id, "Prefab capture root is absent from the scene");
        std::set<AuthoredEntityId> included;
        std::vector<AuthoredEntityId> pending{root};
        while (!pending.empty()) {
            const auto id = pending.back();
            pending.pop_back();
            if (!included.insert(id).second)
                continue;
            const auto [first, last] = children.equal_range(id);
            for (auto iterator = first; iterator != last; ++iterator)
                pending.push_back(iterator->second);
        }
        PrefabDocument prefab{impl_->project.project, asset, root, {}};
        prefab.entities.reserve(included.size());
        for (auto& entity : captured.scene.entities)
            if (included.contains(entity.id)) {
                if (entity.id == root)
                    entity.parent.reset();
                prefab.entities.push_back(std::move(entity));
            }
        auto encoded = take(encode_prefab_document(prefab));
        return AuthoringPrefabReply{Json{{"document", std::move(encoded)},
                                         {"root_entity_id", root.value().to_string()},
                                         {"entity_count", prefab.entities.size()}}};
    }
    if (method == "prefab.prepare") {
        fields(parameters, {"revision", "expected_revision", "parent_id", "overrides"});
        const auto source_token = token(parameter(parameters, "revision"), "/params/revision");
        const auto found = impl_->sources.find(source_token);
        if (found == impl_->sources.end() || !found->second.revision)
            fail(-32000, ErrorCode::conflict,
                 "Prefab source revision was consumed or belongs to another owner");
        const auto revision = take(
            authoring_wire::parse_revision(parameter(parameters, "expected_revision")), -32602);
        const auto state = take(impl_->owner.status());
        if (state.revision != revision)
            fail(-32000, ErrorCode::conflict, "Scene revision changed before prefab preparation");
        if (impl_->preparations.size() >= maximum_preparations)
            fail(-32000, ErrorCode::conflict, "Prefab preparation limit reached");
        impl_->check_path(found->second.path);
        auto observed = take(impl_->files.read_prefab(found->second.path));
        impl_->check_project(observed.source.document);
        if (observed.revision != *found->second.revision)
            fail(-32000, ErrorCode::conflict,
                 "Prefab source changed; read its exact current revision");
        PrefabInstanceOptions options;
        const auto& parent = parameter(parameters, "parent_id");
        if (!parent.is_null())
            options.parent = AuthoredEntityId{identity(parent, "/params/parent_id")};
        const auto& overrides = parameter(parameters, "overrides");
        if (!overrides.is_array() || overrides.size() > source_maximum_entities)
            fail(-32602, ErrorCode::invalid_argument, "Prefab overrides exceed their array bound");
        options.overrides.reserve(overrides.size());
        for (const auto& value : overrides) {
            fields(value, {"entity_id", "name", "transform"});
            PrefabEntityOverride entry{AuthoredEntityId{
                identity(parameter(value, "entity_id"), "/params/overrides/entity_id")}};
            if (value.contains("name"))
                entry.name = Name{string(value.at("name"), "/params/overrides/name")};
            if (value.contains("transform"))
                entry.transform = transform(value.at("transform"));
            options.overrides.push_back(std::move(entry));
        }
        auto command = take(prepare_prefab_instance(observed.source.document, options));
        const auto charge = preparation_charge(command);
        impl_->capacity(charge);
        auto identity = impl_->fresh_token();
        Json remap = Json::array();
        for (const auto& mapping : command.remap)
            remap.push_back({{"source", mapping.source.value().to_string()},
                             {"instance", mapping.instance.value().to_string()}});
        AuthoringPrefabReply reply{Json{{"preparation", identity},
                                        {"root_entity_id", command.id.value().to_string()},
                                        {"remap", std::move(remap)},
                                        {"revision", authoring_wire::revision_json(revision)}}};
        Preparations staged;
        staged.emplace(identity, Preparation{std::move(command), revision, charge});
        reply.admission_ = std::make_unique<AuthoringPrefabAdmission>();
        reply.admission_->preparation = staged.extract(identity);
        return reply;
    }
    if (method == "prefab.instantiate") {
        fields(parameters, {"preparation", "mutation_id"});
        const auto preparation = token(parameter(parameters, "preparation"), "/params/preparation");
        const auto found = impl_->preparations.find(preparation);
        if (found == impl_->preparations.end())
            fail(-32000, ErrorCode::conflict,
                 "Prefab preparation was released or belongs to another owner");
        const auto mutation =
            take(authoring_wire::parse_mutation_id(parameter(parameters, "mutation_id")), -32602);
        // A preparation is frozen intent: later file edits cannot alter retries.
        const AuthoringMutationRequest request{mutation, found->second.revision,
                                               AuthoringCommand{found->second.command}};
        const auto before = take(impl_->owner.status()).revision;
        outcome_may_have_changed = true;
        const auto outcome = take(impl_->owner.mutate(request));
        AuthoringPrefabReply reply{authoring_wire::mutation_reply_json(outcome)};
        reply.failed = !outcome.outcome || !outcome.outcome.value()->result;
        reply.scene_changed = outcome.observed_revision != before;
        return reply;
    }
    fail(-32601, ErrorCode::invalid_argument, "Unknown prefab method");
}

void AuthoringPrefabs::admit(AuthoringPrefabReply& reply) {
    if (!reply.admission_)
        return;
    auto& admission = *reply.admission_;
    if (!admission.source.empty()) {
        const auto charge = admission.source.mapped().charge;
        const auto inserted = impl_->sources.insert(std::move(admission.source));
        if (!inserted.inserted)
            fail(-32000, ErrorCode::conflict, "Prefab source token admission collision");
        impl_->retained_bytes += charge;
    }
    if (!admission.preparation.empty()) {
        const auto charge = admission.preparation.mapped().charge;
        const auto inserted = impl_->preparations.insert(std::move(admission.preparation));
        if (!inserted.inserted)
            fail(-32000, ErrorCode::conflict, "Prefab preparation token admission collision");
        impl_->retained_bytes += charge;
    }
    if (admission.release_source) {
        const auto found = impl_->sources.find(*admission.release_source);
        if (found != impl_->sources.end()) {
            impl_->retained_bytes -= found->second.charge;
            impl_->sources.erase(found);
        }
    }
    if (admission.release_preparation) {
        const auto found = impl_->preparations.find(*admission.release_preparation);
        if (found != impl_->preparations.end()) {
            impl_->retained_bytes -= found->second.charge;
            impl_->preparations.erase(found);
        }
    }
    reply.admission_.reset();
}

Json AuthoringPrefabs::methods() {
    const auto id = id_schema();
    const auto revision = revision_schema();
    const Json path{{"type", "string"}, {"pattern", "\\.sharkprefab$"}};
    const Json document{{"type", "string"},
                        {"maximum_utf8_bytes", source_maximum_bytes},
                        {"description", "Schema-1 source JSON string; the complete request remains "
                                        "limited to 64 KiB and reply to 1 MiB"}};
    const Json nullable_id{{"oneOf", {id, Json{{"type", "null"}}}}};
    const Json number{{"type", "number"}, {"finite_only", true}};
    const auto vector = [&](std::size_t count) {
        return Json{{"type", "array"}, {"minItems", count}, {"maxItems", count}, {"items", number}};
    };
    const auto local_transform =
        object_schema({{"translation", vector(3)}, {"rotation", vector(4)}, {"scale", vector(3)}},
                      {"translation", "rotation", "scale"});
    auto override_value = object_schema({{"entity_id", id},
                                         {"name",
                                          {{"type", "string"},
                                           {"minimum_utf8_bytes", 1},
                                           {"maximum_utf8_bytes", maximum_name_bytes},
                                           {"forbid_controls", true}}},
                                         {"transform", local_transform}},
                                        {"entity_id"});
    override_value["description"] =
        "Replace complete Name and/or local Transform; empty and duplicate targets reject";
    const Json mutation = object_schema(
        {{"scope", id}, {"sequence", {{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}}}},
        {"scope", "sequence"});
    const Json example_revision{{"scene_instance", "1"}, {"revision", "0"}};
    constexpr auto sample_id = "30000000-0000-4000-8000-000000000003";
    Json result = Json::array();
    const auto publication = object_schema(
        {{"published", {{"const", true}}},
         {"bytes", {{"type", "integer"}, {"minimum", 0}, {"maximum", source_maximum_bytes}}},
         {"durability",
          {{"enum", {"file_flushed", "file_and_directory_flushed", "directory_flush_failed"}}}}},
        {"published", "bytes", "durability"});
    const Json output_fields{
        {"path", path},
        {"document", document},
        {"revision", id},
        {"asset_id", id},
        {"root_entity_id", id},
        {"preparation", id},
        {"entity_count",
         {{"type", "integer"}, {"minimum", 1}, {"maximum", source_maximum_entities}}},
        {"remap",
         {{"type", "array"},
          {"minItems", 1},
          {"maxItems", source_maximum_entities},
          {"items", object_schema({{"source", id}, {"instance", id}}, {"source", "instance"})}}},
        {"released", {{"type", "boolean"}, {"const", true}}},
        {"publication", publication}};
    const auto add = [&](const char* name, Json params, Json output, Json example) {
        // Mutation outcomes already use their complete existing wire schema.
        if (std::string_view{name} != "prefab.instantiate")
            for (auto& [key, value] : output["properties"].items())
                value = std::string_view{name} == "prefab.prepare" && key == "revision"
                            ? revision
                            : output_fields.at(key);
        result.push_back({{"name", name},
                          {"execution_context", "cpu_owner_thread"},
                          {"requires_open_project", true},
                          {"requires_project_root", false},
                          {"params", std::move(params)},
                          {"result", std::move(output)},
                          {"example",
                           {{"jsonrpc", "2.0"},
                            {"protocol_version", 1},
                            {"id", "example"},
                            {"method", name},
                            {"params", std::move(example)}}}});
    };
    add("prefab.read", object_schema({{"path", path}}, {"path"}),
        result_schema(
            {"path", "document", "revision", "asset_id", "root_entity_id", "entity_count"},
            "Exact source observation and session/path-bound token; at most 32 source tokens; "
            "adjacent metadata identity must match"),
        {{"path", "assets/actor.sharkprefab"}});
    add("prefab.write",
        object_schema({{"path", path}, {"document", document}, {"expected_revision", nullable_id}},
                      {"path", "document", "expected_revision"}),
        result_schema({"path", "revision", "publication"},
                      "Single source file publication; metadata must already exist and Scene "
                      "undo/save is independent"),
        {{"path", "assets/actor.sharkprefab"},
         {"document", "<schema-1 prefab JSON>"},
         {"expected_revision", nullptr}});
    add("prefab.capture",
        object_schema({{"entity_id", id}, {"asset_id", id}, {"expected_revision", revision}},
                      {"entity_id", "asset_id", "expected_revision"}),
        result_schema({"document", "root_entity_id", "entity_count"},
                      "Read-only rooted subtree capture; detaches the external parent and "
                      "preserves local transforms"),
        {{"entity_id", sample_id},
         {"asset_id", sample_id},
         {"expected_revision", example_revision}});
    add("prefab.prepare",
        object_schema({{"revision", id},
                       {"expected_revision", revision},
                       {"parent_id", nullable_id},
                       {"overrides",
                        {{"type", "array"},
                         {"maxItems", source_maximum_entities},
                         {"items", override_value}}}},
                      {"revision", "expected_revision", "parent_id", "overrides"}),
        result_schema({"preparation", "root_entity_id", "remap", "revision"},
                      "Frozen command and fresh explicit map; at most eight preparations and "
                      "16 MiB retained inputs; preparing again is a new intent"),
        {{"revision", sample_id},
         {"expected_revision", example_revision},
         {"parent_id", nullptr},
         {"overrides", Json::array()}});
    add("prefab.instantiate",
        object_schema({{"preparation", id}, {"mutation_id", mutation}},
                      {"preparation", "mutation_id"}),
        authoring_wire::mutation_reply_schema(),
        {{"preparation", sample_id}, {"mutation_id", {{"scope", sample_id}, {"sequence", "1"}}}});
    add("prefab.release", object_schema({{"revision", id}}, {"revision"}),
        result_schema({"released"}, "Release one source observation token"),
        {{"revision", sample_id}});
    add("prefab.release_preparation", object_schema({{"preparation", id}}, {"preparation"}),
        result_schema(
            {"released"},
            "Release frozen intent; existing mutation outcomes remain on the authored owner"),
        {{"preparation", sample_id}});
    return result;
}

} // namespace shark::tools
