#include "authoring_physics.hpp"

#include "authoring_asset_wire.hpp"
#include "authoring_wire.hpp"
#include "shark/physics.hpp"
#include "shark/prepared_scene.hpp"
#include "shark/script_runtime.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <utility>

namespace shark::tools {
namespace {
using Json = nlohmann::json;
struct Failure {
    Error error;
};
template <class T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
void take(Result<void> result) {
    if (!result)
        throw Failure{std::move(result).error()};
}
[[noreturn]] void reject(const char* message, ErrorCode code = ErrorCode::conflict) {
    throw Failure{{code, message, {.operation = "authoring.physics"}}};
}
void fields(const Json& value, std::initializer_list<std::string_view> allowed) {
    take(authoring_asset_wire::require_fields(value, allowed));
}
const Json& required(const Json& value, const char* key) {
    if (!value.contains(key))
        reject("Missing physics operation parameter", ErrorCode::invalid_argument);
    return value.at(key);
}
StableId id(const Json& value) {
    if (!value.is_string())
        reject("Expected a stable UUID", ErrorCode::invalid_argument);
    return take(StableId::parse(value.get_ref<const std::string&>()));
}
std::uint64_t decimal(const Json& value) {
    if (!value.is_string())
        reject("Physics counters require canonical decimal strings", ErrorCode::invalid_argument);
    const auto& text = value.get_ref<const std::string&>();
    if (text.empty() || text.front() < '0' || text.front() > '9')
        reject("Invalid physics decimal counter", ErrorCode::invalid_argument);
    std::uint64_t number{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), number);
    if (text.empty() || (text.size() > 1 && text.front() == '0') || result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() || text.front() == '-')
        reject("Invalid physics decimal counter", ErrorCode::invalid_argument);
    return number;
}
std::size_t index(const Json& value, std::size_t maximum) {
    return take(authoring_asset_wire::parse_index(value, maximum, "/params"));
}
Json participant(const PhysicsContactParticipant& value) {
    return {{"entity_id", std::get<AuthoredEntityId>(value.entity.identity).value().to_string()},
            {"is_trigger", value.is_trigger}};
}
Json event_json(const PhysicsContactEvent& value) {
    return {
        {"sequence", std::to_string(value.sequence)},
        {"tick", std::to_string(value.completed_tick)},
        {"phase", value.phase == PhysicsContactPhase::enter ? "enter" : "exit"},
        {"cause", value.cause == PhysicsContactCause::simulation ? "simulation" : "body_removed"},
        {"first", participant(value.first)},
        {"second", participant(value.second)}};
}
Json script_logs_json(const std::vector<ScriptLogEntry>& logs) {
    Json result = Json::array();
    for (const auto& entry : logs) {
        const char* phase = "load";
        switch (entry.phase) {
        case ScriptPhase::load:
            phase = "load";
            break;
        case ScriptPhase::init:
            phase = "init";
            break;
        case ScriptPhase::update:
            phase = "update";
            break;
        case ScriptPhase::fixed_update:
            phase = "fixed_update";
            break;
        case ScriptPhase::contact:
            phase = "contact";
            break;
        case ScriptPhase::destroy:
            phase = "destroy";
            break;
        }
        result.push_back(
            {{"entity_id", std::get<AuthoredEntityId>(entry.entity.identity).value().to_string()},
             {"phase", phase},
             {"tick", std::to_string(entry.tick)},
             {"message", entry.message}});
    }
    return result;
}
constexpr double script_delta = 1.0 / 60.0;
constexpr std::size_t maximum_ticks = 8;
constexpr std::size_t maximum_events_per_tick = 128;
} // namespace

struct AuthoringPhysics::Impl {
    struct Runtime {
        StableId session;
        SceneRevision source;
        AuthoredEntityId camera;
        AssetTargetProfile target;
        std::unique_ptr<LoadedPreparedScene> loaded;
        std::vector<AuthoredEntityId> entities;
        std::map<AuthoredEntityId, PhysicsBodyHandle> bodies;
        std::unique_ptr<PhysicsWorld> world; // Destroy native lease before Core Scene.
        std::unique_ptr<ScriptRuntime> scripts;
        Runtime(StableId session_id, SceneRevision source_revision, AuthoredEntityId camera_id,
                AssetTargetProfile target_profile, std::unique_ptr<LoadedPreparedScene> scene,
                std::unique_ptr<PhysicsWorld> physics)
            : session(session_id), source(source_revision), camera(camera_id),
              target(std::move(target_profile)), loaded(std::move(scene)),
              world(std::move(physics)) {}
        ~Runtime() {
            if (scripts) {
                auto stopped = scripts->stop(loaded->scene(), *world);
                if (!stopped)
                    (void)scripts->close();
                scripts.reset();
            }
            world.reset();
        }
        std::uint64_t tick{0};
        std::uint64_t next_sequence{1};
        bool faulted{false};
    };
    struct Operation {
        StableId session;
        std::uint64_t sequence;
        Json input;
        std::uint64_t tick;
        std::vector<PhysicsContactEvent> events;
        std::vector<ScriptLogEntry> script_logs;
        std::optional<Error> error;
        bool faulted{false};
    };
    std::uint64_t generation{0};
    std::unique_ptr<Runtime> runtime;
    std::optional<Json> last_start_input;
    std::optional<Json> last_start;
    std::optional<Operation> last_operation;
    std::optional<Json> last_stop;

    Json operation_json() const {
        if (!last_operation)
            return nullptr;
        const auto& value = *last_operation;
        Json events = Json::array();
        for (const auto& event : value.events)
            events.push_back(event_json(event));
        return {{"success", !value.error},
                {"faulted", value.faulted},
                {"session_id", value.session.to_string()},
                {"sequence", std::to_string(value.sequence)},
                {"tick", std::to_string(value.tick)},
                {"next_sequence", std::to_string(value.sequence + 1)},
                {"events", std::move(events)},
                {"script_logs", script_logs_json(value.script_logs)},
                {"error",
                 value.error ? authoring_asset_wire::error_json(*value.error) : Json(nullptr)}};
    }
    Json status(const AuthoredScene& owner) const {
        const auto mutations = take(owner.mutation_status());
        return {{"owner_scope", mutations.scope.value().to_string()},
                {"generation", std::to_string(generation)},
                {"active", runtime != nullptr},
                {"session_id", runtime ? Json(runtime->session.to_string()) : Json(nullptr)},
                {"source_revision",
                 runtime ? authoring_wire::revision_json(runtime->source) : Json(nullptr)},
                {"tick", std::to_string(runtime ? runtime->tick : 0)},
                {"next_sequence", std::to_string(runtime ? runtime->next_sequence : 1)},
                {"faulted", runtime && runtime->faulted},
                {"bodies", runtime ? runtime->bodies.size() : 0},
                {"entity_count", runtime ? runtime->entities.size() : 0},
                {"limits",
                 {{"tick_hz", 60},
                  {"bodies", 128},
                  {"contact_pairs", 64},
                  {"events_per_operation", 1024},
                  {"ticks_per_operation", maximum_ticks},
                  {"query_entities", 128},
                  {"script_instances", 16},
                  {"script_sources", script_runtime_maximum_sources},
                  {"script_source_bytes", script_runtime_maximum_source_bytes},
                  {"script_total_source_bytes", script_runtime_maximum_total_source_bytes},
                  {"script_logs_per_operation", script_runtime_maximum_logs},
                  {"script_log_bytes_per_operation", script_runtime_maximum_log_bytes},
                  {"script_log_message_bytes", script_runtime_maximum_log_message_bytes}}},
                {"last_start", last_start ? *last_start : Json(nullptr)},
                {"last_operation", operation_json()}};
    }
    Runtime& current(const Json& parameters) {
        const auto session = id(required(parameters, "session_id"));
        if (!runtime || runtime->session != session)
            reject("Physics preview session is no longer active");
        return *runtime;
    }
    Json
    start(const Json& parameters, const AuthoredScene& owner, AuthoringAssets& assets,
          bool& changed, const std::function<Result<void>()>& validate_source,
          const std::function<Result<std::vector<ScriptSourceSnapshot>>(Scene&)>& capture_scripts,
          const std::function<Result<void>()>& validate_scripts) {
        fields(parameters, {"op", "owner_scope", "expected_generation", "expected_revision",
                            "camera", "target"});
        const auto scope = id(required(parameters, "owner_scope"));
        const auto expected_generation = decimal(required(parameters, "expected_generation"));
        const auto expected =
            take(authoring_wire::parse_revision(required(parameters, "expected_revision")));
        const AuthoredEntityId camera{id(required(parameters, "camera"))};
        const auto target =
            take(authoring_asset_wire::parse_target(required(parameters, "target")));
        if (scope != take(owner.mutation_status()).scope.value())
            reject("Physics start belongs to another authored owner");
        if (last_start_input && *last_start_input == parameters)
            return *last_start;
        if (runtime || expected_generation != generation ||
            generation == std::numeric_limits<std::uint64_t>::max())
            reject("Stop the active preview and observe its current generation before starting");
        const auto candidate = assets.prepare_current_export(owner, camera, expected, target);
        std::vector<PreparedAssetInput> inputs;
        inputs.reserve(candidate.meshes().size());
        for (const auto& mesh : candidate.meshes())
            inputs.push_back({mesh.asset, mesh.payload});
        const auto prepared = take(PreparedAssets::create(inputs));
        auto loaded = take(LoadedPreparedScene::load(candidate.scene_payload(), prepared));
        PhysicsWorldConfig config;
        config.max_bodies = 128;
        config.max_body_pairs = 512;
        config.max_contact_constraints = 256;
        config.max_contact_pairs = 64;
        config.max_queued_events = 128;
        auto next =
            std::make_unique<Runtime>(take(generate_stable_id()), expected, camera, target,
                                      std::move(loaded), take(PhysicsWorld::create(config)));
        auto& scene = next->loaded->scene();
        next->entities.reserve(take(scene.stats()).live_entities);
        take(scene.visit_entities([&](const EntityReference& entity) -> Result<void> {
            const auto state = scene.entity_state(entity.handle);
            if (!state)
                return state.error();
            const auto entity_id = std::get<AuthoredEntityId>(entity.identity);
            next->entities.push_back(entity_id);
            if (state.value().rigid_body) {
                auto body = next->world->create_body(scene, entity.handle);
                if (!body)
                    return body.error();
                next->bodies.emplace(entity_id, body.value());
            }
            return {};
        }));
        take(next->world->begin_simulation(scene));
        const auto sources = take(capture_scripts(scene));
        next->scripts = take(ScriptRuntime::create(scene, sources));
        take(next->scripts->start(scene, *next->world));
        const auto logs = take(next->scripts->take_logs());
        take(assets.validate_export(candidate, owner));
        take(validate_source());
        take(validate_scripts());
        Json receipt{{"success", true},
                     {"session_id", next->session.to_string()},
                     {"generation", std::to_string(generation + 1)},
                     {"source_revision", authoring_wire::revision_json(expected)},
                     {"tick", "0"},
                     {"next_sequence", "1"},
                     {"script_logs", script_logs_json(logs)}};
        auto input = parameters;
        // All receipt/input allocations precede publication. A lost output is reconciled by status
        // or exact start replay.
        last_start_input.emplace(std::move(input));
        last_start.emplace(std::move(receipt));
        last_operation.reset();
        ++generation;
        runtime = std::move(next);
        changed = true;
        return *last_start;
    }
    Json execute(const Json& parameters, bool removing, bool& changed) {
        if (removing)
            fields(parameters, {"op", "session_id", "sequence", "expected_tick", "entity_id"});
        else
            fields(parameters, {"op", "session_id", "sequence", "expected_tick", "ticks"});
        auto& value = current(parameters);
        const auto sequence = decimal(required(parameters, "sequence"));
        const auto expected = decimal(required(parameters, "expected_tick"));
        const auto ticks =
            removing ? std::size_t{0} : index(required(parameters, "ticks"), maximum_ticks);
        if (!removing && ticks == 0)
            reject("Physics step batch must contain 1..8 ticks", ErrorCode::invalid_argument);
        const auto entity =
            removing ? std::optional{AuthoredEntityId{id(required(parameters, "entity_id"))}}
                     : std::nullopt;
        if (last_operation && last_operation->session == value.session &&
            last_operation->sequence == sequence) {
            if (last_operation->input != parameters)
                reject("Physics sequence was already used with different input");
            return operation_json();
        }
        if (value.faulted || sequence != value.next_sequence ||
            sequence == std::numeric_limits<std::uint64_t>::max() || expected != value.tick)
            reject("Physics operation is stale, expired, out of sequence or faulted; reconcile "
                   "status");
        Operation operation{
            value.session,
            sequence,
            parameters,
            value.tick,
            {},
            {},
            Error{ErrorCode::execution_failed,
                  "Physics operation completion was interrupted; stop and reconcile",
                  {.operation = "authoring.physics"}}};
        operation.events.reserve((removing ? 1 : ticks) * maximum_events_per_tick);
        last_operation.emplace(std::move(operation));
        ++value.next_sequence;
        changed = true;
        bool native_changed = false;
        bool scripts_changed = false;
        try {
            // Preserve consumed native observations even if an already-invoked
            // callback faults. Exact operation replay must not dispatch twice.
            const auto deliver_contacts = [&] {
                auto report = take(value.scripts->dispatch_contacts(
                    value.loaded->scene(), *value.world, maximum_events_per_tick));
                last_operation->events.insert(last_operation->events.end(), report.events.begin(),
                                              report.events.end());
                if (report.error)
                    throw Failure{std::move(*report.error)};
            };
            if (removing) {
                const auto found = value.bodies.find(*entity);
                if (found == value.bodies.end())
                    reject("Entity has no active runtime RigidBody", ErrorCode::invalid_id);
                take(value.world->remove_body(value.loaded->scene(), found->second));
                native_changed = true;
                value.bodies.erase(found);
                deliver_contacts();
            } else {
                for (std::size_t tick = 0; tick < ticks; ++tick) {
                    scripts_changed = true;
                    take(value.scripts->update(value.loaded->scene(), *value.world, script_delta));
                    take(value.scripts->fixed_update(value.loaded->scene(), *value.world,
                                                     value.tick, script_delta));
                    take(value.world->step(value.loaded->scene()));
                    native_changed = true;
                    ++value.tick;
                    last_operation->tick = value.tick;
                    deliver_contacts();
                }
            }
            last_operation->error.reset();
        } catch (Failure& failure) {
            last_operation->error = std::move(failure.error);
            const auto state = value.world->stats();
            value.faulted =
                native_changed || scripts_changed || !state || state.value().faulted ||
                (!removing && last_operation->error->code == ErrorCode::execution_failed);
        } catch (...) {
            value.faulted = true;
        }
        auto logs = value.scripts->take_logs();
        if (!logs) {
            value.faulted = true;
            last_operation->error = std::move(logs).error();
        } else {
            last_operation->script_logs = std::move(logs).value();
        }
        last_operation->faulted = value.faulted;
        return operation_json();
    }
    Json query(const Json& parameters) {
        fields(parameters, {"op", "session_id", "expected_tick", "offset", "limit"});
        auto& value = current(parameters);
        if (decimal(required(parameters, "expected_tick")) != value.tick)
            reject("Physics query tick is stale");
        const auto offset = parameters.contains("offset")
                                ? index(parameters.at("offset"), value.entities.size())
                                : 0;
        const auto limit = parameters.contains("limit") ? index(parameters.at("limit"), 128) : 64;
        if (limit == 0)
            reject("Physics query limit must be 1..128", ErrorCode::invalid_argument);
        const auto end = std::min(value.entities.size(), offset + limit);
        Json entities = Json::array();
        auto& scene = value.loaded->scene();
        const SceneQueryFields selected{true, true, true, true, true, true};
        for (auto position = offset; position < end; ++position) {
            const auto handle = take(scene.find(value.entities[position]));
            const auto page = take(scene.inspect_entity(handle, selected));
            auto encoded = take(authoring_wire::page_json(scene, page, selected));
            entities.push_back(std::move(encoded.at("entities").at(0)));
        }
        return {{"session_id", value.session.to_string()},
                {"tick", std::to_string(value.tick)},
                {"entities", std::move(entities)},
                {"next_offset", end < value.entities.size() ? Json(end) : Json(nullptr)},
                {"complete", end == value.entities.size()}};
    }
};

AuthoringPhysics::AuthoringPhysics() : impl_(std::make_unique<Impl>()) {}
AuthoringPhysics::~AuthoringPhysics() = default;
bool AuthoringPhysics::active() const noexcept {
    return impl_->runtime != nullptr;
}
bool AuthoringPhysics::replays_start(const Json& parameters) const {
    return impl_->last_start_input && *impl_->last_start_input == parameters;
}
Result<Json> AuthoringPhysics::dispatch(
    const Json& parameters, const AuthoredScene& owner, AuthoringAssets& assets, bool& changed,
    const std::function<Result<void>()>& validate_source,
    const std::function<Result<std::vector<ScriptSourceSnapshot>>(Scene&)>& capture_scripts,
    const std::function<Result<void>()>& validate_scripts) {
    try {
        if (!parameters.is_object() || !parameters.contains("op") ||
            !parameters.at("op").is_string())
            reject("scene.physics requires a tagged operation", ErrorCode::invalid_argument);
        const auto& op = parameters.at("op").get_ref<const std::string&>();
        if (op == "status") {
            fields(parameters, {"op"});
            return impl_->status(owner);
        }
        if (op == "start")
            return impl_->start(parameters, owner, assets, changed, validate_source,
                                capture_scripts, validate_scripts);
        if (op == "step" || op == "remove")
            return impl_->execute(parameters, op == "remove", changed);
        if (op == "query")
            return impl_->query(parameters);
        if (op == "stop") {
            fields(parameters, {"op", "session_id"});
            const auto session = id(required(parameters, "session_id"));
            if (!impl_->runtime && impl_->last_stop &&
                impl_->last_stop->at("session_id") == session.to_string())
                return *impl_->last_stop;
            auto& current = impl_->current(parameters);
            Json receipt{{"success", false},
                         {"active", false},
                         {"session_id", current.session.to_string()},
                         {"tick", std::to_string(current.tick)},
                         {"script_logs", Json::array()},
                         {"script_errors", Json::array()},
                         {"script_discarded", 0}};
            impl_->last_stop.emplace(std::move(receipt));
            changed = true;
            try {
                auto stopped = current.scripts->stop(current.loaded->scene(), *current.world);
                if (!stopped) {
                    auto discarded = take(current.scripts->close());
                    (*impl_->last_stop)["script_discarded"] = discarded;
                    (*impl_->last_stop)["script_errors"].push_back(
                        authoring_asset_wire::error_json(stopped.error()));
                } else {
                    (*impl_->last_stop)["script_discarded"] = stopped.value().discarded;
                    for (const auto& error : stopped.value().errors)
                        (*impl_->last_stop)["script_errors"].push_back(
                            authoring_asset_wire::error_json(error));
                }
                (*impl_->last_stop)["script_logs"] =
                    script_logs_json(take(current.scripts->take_logs()));
                (*impl_->last_stop)["success"] = true;
                impl_->runtime.reset();
            } catch (...) {
                impl_->runtime.reset();
                throw;
            }
            return *impl_->last_stop;
        }
        reject("Unknown scene.physics operation", ErrorCode::invalid_argument);
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (AuthoringAssetFailure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot complete physics preview operation; reconcile retained status",
                     {.operation = "authoring.physics"}};
    }
}

Result<RenderScene> AuthoringPhysics::snapshot(const AuthoringViewportRequest& request,
                                               const AuthoredScene& owner) const {
    try {
        if (!impl_->runtime || !request.physics_session ||
            *request.physics_session != impl_->runtime->session)
            reject("Viewport physics session is no longer active");
        const auto& value = *impl_->runtime;
        if (request.transform_preview || request.expected_revision != value.source ||
            take(owner.status()).revision != value.source || request.camera != value.camera ||
            request.target != value.target)
            reject("Viewport request does not match its frozen physics source");
        if (value.faulted)
            reject("Physics preview is faulted; stop and reconcile");
        const auto& scene = value.loaded->scene();
        const auto state = take(scene.stats());
        auto poses = take(value.world->render_poses(scene, request.physics_alpha));
        return extract_render_scene(
            scene, *value.loaded->assets(),
            {take(scene.find(value.camera)),
             {state.instance, state.revision},
             request.viewport,
             {request.local_origin, request.origin_generation, value.loaded->world_origin()},
             true,
             request.view_camera,
             std::nullopt,
             std::move(poses)});
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot extract physics preview",
                     {.operation = "authoring.physics.snapshot"}};
    }
}

Json AuthoringPhysics::method_schema() {
    const Json decimal{{"type", "string"},
                       {"pattern", "^(0|[1-9][0-9]{0,19})$"},
                       {"maximum_decimal", "18446744073709551615"}};
    const Json uuid{{"type", "string"}, {"format", "uuid"}};
    const auto object = [](Json properties, Json required) {
        return Json{{"type", "object"},
                    {"additionalProperties", false},
                    {"properties", std::move(properties)},
                    {"required", std::move(required)}};
    };
    const auto operation = [&](const char* op, Json properties, Json required) {
        properties["op"] = {{"const", op}};
        required.push_back("op");
        return object(std::move(properties), std::move(required));
    };
    const Json revision = object({{"scene_instance", decimal}, {"revision", decimal}},
                                 {"scene_instance", "revision"});
    Json variants = Json::array();
    variants.push_back(operation("status", Json::object(), Json::array()));
    variants.push_back(operation(
        "start",
        {{"owner_scope", uuid},
         {"expected_generation", decimal},
         {"expected_revision", revision},
         {"camera", uuid},
         {"target",
          object({{"id", {{"type", "string"}}}, {"side", {{"const", "client"}}}}, {"id", "side"})}},
        {"owner_scope", "expected_generation", "expected_revision", "camera", "target"}));
    variants.push_back(
        operation("step",
                  {{"session_id", uuid},
                   {"sequence", decimal},
                   {"expected_tick", decimal},
                   {"ticks", {{"type", "integer"}, {"minimum", 1}, {"maximum", maximum_ticks}}}},
                  {"session_id", "sequence", "expected_tick", "ticks"}));
    variants.push_back(operation("remove",
                                 {{"session_id", uuid},
                                  {"sequence", decimal},
                                  {"expected_tick", decimal},
                                  {"entity_id", uuid}},
                                 {"session_id", "sequence", "expected_tick", "entity_id"}));
    variants.push_back(
        operation("query",
                  {{"session_id", uuid},
                   {"expected_tick", decimal},
                   {"offset", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4096}}},
                   {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 128}}}},
                  {"session_id", "expected_tick"}));
    variants.push_back(operation("stop", {{"session_id", uuid}}, {"session_id"}));
    return {{"name", "scene.physics"},
            {"execution_context", "cpu_owner_thread"},
            {"requires_open_project", true},
            {"requires_project_root", false},
            {"params", {{"oneOf", std::move(variants)}}},
            {"result",
             {{"type", "object"},
              {"properties",
               {{"script_logs",
                 {{"type", "array"},
                  {"maxItems", script_runtime_maximum_logs},
                  {"items",
                   object({{"entity_id", uuid},
                           {"phase",
                            {{"enum",
                              {"load", "init", "update", "fixed_update", "contact", "destroy"}}}},
                           {"tick", decimal},
                           {"message",
                            {{"type", "string"},
                             {"maximum_utf8_bytes", script_runtime_maximum_log_message_bytes}}}},
                          {"entity_id", "phase", "tick", "message"})}}},
                {"script_errors", {{"type", "array"}, {"items", {{"type", "object"}}}}},
                {"script_discarded",
                 {{"type", "integer"},
                  {"minimum", 0},
                  {"maximum", script_runtime_maximum_scripts}}}}}}},
            {"example",
             {{"jsonrpc", "2.0"},
              {"protocol_version", 1},
              {"id", "example"},
              {"method", "scene.physics"},
              {"params", {{"op", "status"}}}}}};
}
} // namespace shark::tools
