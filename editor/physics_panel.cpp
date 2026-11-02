#include "physics_panel.hpp"

#include <array>
#include <charconv>
#include <nlohmann/json.hpp>
#include <vector>

namespace shark::editor {
namespace {
using Json = nlohmann::json;
struct Failure {
    Error error;
    bool certain{false};
};
[[noreturn]] void fail(const char* message) {
    throw Failure{{ErrorCode::conflict, message, {.operation = "editor.physics"}}};
}
template <typename T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
template <typename F> Result<void> boundary(F&& function) {
    try {
        function();
        return {};
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot observe physics result; reconcile the original operation",
                     {.operation = "editor.physics"}};
    }
}
std::uint64_t decimal(const Json& value) {
    const auto& text = value.get_ref<const std::string&>();
    std::uint64_t number{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        fail("Invalid physics counter in response");
    return number;
}
Error decode_error(const Json& value) {
    constexpr std::array codes{
        ErrorCode::invalid_argument,    ErrorCode::invalid_configuration,  ErrorCode::invalid_id,
        ErrorCode::invalid_path,        ErrorCode::path_outside_root,      ErrorCode::io_failure,
        ErrorCode::entropy_unavailable, ErrorCode::capability_unavailable, ErrorCode::conflict,
        ErrorCode::execution_failed};
    const auto code = value.at("code").get<std::string>();
    for (const auto item : codes)
        if (error_code_name(item) == code) {
            const auto& context = value.at("context");
            return {item,
                    value.at("message").get<std::string>(),
                    {context.value("operation", std::string{}),
                     context.value("request_id", std::string{}),
                     context.value("path", std::string{}),
                     context.value("entity_id", std::string{}),
                     context.value("asset_id", std::string{})}};
        }
    fail("Unknown physics error code");
}
Json revision_json(SceneRevision value) {
    return {{"scene_instance", std::to_string(value.scene_instance)},
            {"revision", std::to_string(value.revision)}};
}
SceneRevision revision_value(const Json& value) {
    return {decimal(value.at("scene_instance")), decimal(value.at("revision"))};
}
math::Vec3 vector_value(const Json& value) {
    if (!value.is_array() || value.size() != 3)
        fail("Invalid physics vector in response");
    return {value.at(0).get<double>(), value.at(1).get<double>(), value.at(2).get<double>()};
}
Json vector_json(math::Vec3 value) {
    return Json::array({value.x, value.y, value.z});
}
std::string vector_text(math::Vec3 value) {
    return "(" + std::to_string(value.x) + ", " + std::to_string(value.y) + ", " +
           std::to_string(value.z) + ")";
}
PhysicsDebugBody body_value(const Json& row) {
    const auto& value = row.at("rigid_body");
    const auto& pose = row.at("physics_body").at("pose");
    PhysicsDebugBody result{};
    result.pose.translation = vector_value(pose.at("translation"));
    const auto& rotation = pose.at("rotation");
    if (!rotation.is_array() || rotation.size() != 4)
        fail("Invalid physics rotation in response");
    result.pose.rotation = {rotation.at(3).get<double>(), rotation.at(0).get<double>(),
                            rotation.at(1).get<double>(), rotation.at(2).get<double>()};
    result.pose.scale = vector_value(pose.at("scale"));
    const auto motion = value.at("motion").get<std::string>();
    if (motion == "static")
        result.settings.motion = BodyMotion::static_body;
    else if (motion == "dynamic")
        result.settings.motion = BodyMotion::dynamic_body;
    else if (motion == "kinematic")
        result.settings.motion = BodyMotion::kinematic_body;
    else
        fail("Unknown physics motion in response");
    const auto shape = value.at("shape").get<std::string>();
    if (shape == "box")
        result.settings.shape = ColliderShape::box;
    else if (shape == "sphere")
        result.settings.shape = ColliderShape::sphere;
    else if (shape == "capsule")
        result.settings.shape = ColliderShape::capsule;
    else
        fail("Unknown physics shape in response");
    result.settings.half_extents = vector_value(value.at("half_extents"));
    result.settings.radius = value.at("radius").get<double>();
    result.settings.half_height = value.at("half_height").get<double>();
    result.settings.mass = value.at("mass").get<double>();
    result.settings.friction = value.at("friction").get<double>();
    result.settings.restitution = value.at("restitution").get<double>();
    result.settings.collision_layer = value.at("collision_layer").get<std::uint32_t>();
    result.settings.collision_mask = value.at("collision_mask").get<std::uint32_t>();
    result.settings.is_trigger = value.at("is_trigger").get<bool>();
    return result;
}
} // namespace

struct PhysicsPanel::Impl {
    ProjectSession& session;
    PhysicsPreviewState state;
    std::unique_ptr<OfflineScheduler> scheduler;
    // Retain exact typed-wire input before the operation can reach the owner.
    // A failed display/transport read cannot authorize a replacement identity.
    std::optional<std::string> pending;
    std::string pending_op;
    std::vector<std::string> contacts;
    std::vector<std::string> scripts;
    std::optional<PhysicsDebugQuery> debug_query;
    std::vector<std::string> query_details;
    std::vector<std::string> body_details;

    explicit Impl(ProjectSession& value) : session(value) {}
    Json call(const Json& parameters) {
        const auto reply = take(session.request("scene.physics", parameters.dump()));
        const auto envelope = Json::parse(reply.json);
        if (envelope.contains("error")) {
            auto error = envelope.at("error").at("data");
            error["message"] = envelope.at("error").at("message");
            auto decoded = decode_error(error);
            const bool script_prepublication = parameters.value("op", std::string{}) == "start" &&
                                               (decoded.context.operation == "script.create" ||
                                                decoded.context.operation == "script.start");
            const bool certain =
                script_prepublication || (decoded.code != ErrorCode::execution_failed &&
                                          decoded.code != ErrorCode::io_failure);
            throw Failure{std::move(decoded), certain};
        }
        return envelope.at("result");
    }
    void observe_scripts(const Json& value, std::string_view operation) noexcept {
        // Diagnostics are observational and must never change a known receipt.
        try {
            if (operation == "start")
                scripts.clear();
            const auto append = [&](std::string text) {
                constexpr std::size_t limit = 256;
                if (scripts.size() == limit)
                    scripts.erase(scripts.begin());
                scripts.push_back(std::move(text));
            };
            if (value.contains("script_logs")) {
                const auto& logs = value.at("script_logs");
                if (!logs.is_array() || logs.size() > 256)
                    return;
                for (const auto& log : logs) {
                    const auto phase = log.at("phase").get<std::string>();
                    const auto tick = log.at("tick").get<std::string>();
                    const auto entity = log.at("entity_id").get<std::string>();
                    const auto message = log.at("message").get<std::string>();
                    if (phase.size() > 32 || tick.size() > 20 || entity.size() > 36 ||
                        message.size() > 256)
                        continue;
                    append("Tick " + tick + " " + phase + " / " + entity + ": " + message);
                }
            }
            if (value.contains("script_errors")) {
                const auto& errors = value.at("script_errors");
                if (errors.is_array() && errors.size() <= 64)
                    for (const auto& error : errors) {
                        const auto decoded = decode_error(error);
                        append("Script teardown: " + decoded.message + " " + decoded.context.path +
                               " " + decoded.context.entity_id);
                    }
            }
            if (value.value("script_discarded", std::size_t{0}) > 0)
                append("Script teardown discarded " +
                       std::to_string(value.at("script_discarded").get<std::size_t>()) +
                       " callbacks.");
        } catch (...) {
            // Preserve the original play/stop result even on display allocation failure.
        }
    }
    void observe(const Json& value, std::string_view operation) {
        observe_scripts(value, operation);
        if (operation == "stop") {
            const bool succeeded = value.at("success").get<bool>();
            state = {};
            scheduler.reset();
            pending.reset();
            debug_query.reset();
            query_details.clear();
            body_details.clear();
            if (!succeeded)
                throw Failure{{ErrorCode::execution_failed,
                               "Preview stopped, but script teardown did not complete; inspect its "
                               "retained details",
                               {.operation = "editor.physics"}},
                              true};
            return;
        }
        const auto id = take(StableId::parse(value.at("session_id").get<std::string>()));
        const auto tick = decimal(value.at("tick"));
        const auto next = decimal(value.at("next_sequence"));
        if (operation != "start" && (!state.session_id || *state.session_id != id))
            fail("Physics reply belongs to a different preview");
        std::vector<std::string> observed;
        if (operation != "start") {
            const auto& events = value.at("events");
            if (!events.is_array() || events.size() > 128)
                fail("Physics operation returned too many contact observations");
            observed.reserve(events.size());
            for (const auto& event : events) {
                const auto phase = event.at("phase").get<std::string>();
                const auto cause = event.at("cause").get<std::string>();
                if ((phase != "enter" && phase != "exit") ||
                    (cause != "simulation" && cause != "body_removed"))
                    fail("Physics operation returned an unknown contact observation");
                const auto first =
                    take(StableId::parse(event.at("first").at("entity_id").get<std::string>()));
                const auto second =
                    take(StableId::parse(event.at("second").at("entity_id").get<std::string>()));
                const auto tick_value = decimal(event.at("tick"));
                const auto event_sequence = decimal(event.at("sequence"));
                observed.push_back(
                    "Tick " + std::to_string(tick_value) + " #" + std::to_string(event_sequence) +
                    " " + phase + " (" + cause + "): " + first.to_string() +
                    (event.at("first").at("is_trigger").get<bool>() ? " [trigger]" : "") + " / " +
                    second.to_string() +
                    (event.at("second").at("is_trigger").get<bool>() ? " [trigger]" : ""));
            }
        }
        if (operation == "start" || state.tick != tick || state.next_sequence != next) {
            debug_query.reset();
            query_details.clear();
            body_details.clear();
        }
        state.session_id = id;
        state.active = true;
        state.tick = tick;
        state.next_sequence = next;
        if (operation == "start") {
            contacts.clear();
            state.source_revision = revision_value(value.at("source_revision"));
            state.events = 0;
            state.faulted = false;
        } else {
            state.events = value.at("events").size();
            if (!observed.empty())
                contacts = std::move(observed);
            if (!value.at("success").get<bool>()) {
                state.running = false;
                if (value.contains("faulted"))
                    state.faulted = value.at("faulted").get<bool>();
                // The owned receipt itself is a certain outcome, including any
                // completed ticks before its error. Never blindly rerun it.
                const auto error = decode_error(value.at("error"));
                pending.reset();
                state.uncertain = false;
                throw Failure{error};
            }
        }
    }
    void send(Json parameters, std::string operation) {
        if (pending)
            fail("Reconcile the original physics operation first");
        debug_query.reset();
        query_details.clear();
        body_details.clear();
        pending = parameters.dump();
        pending_op = std::move(operation);
        state.uncertain = true;
        Json result;
        try {
            result = call(parameters);
        } catch (const Failure& failure) {
            if (failure.certain) {
                pending.reset();
                state.uncertain = false;
            }
            throw;
        }
        observe(result, pending_op);
        pending.reset();
        state.uncertain = false;
    }
    void step_once() {
        if (!state.active || !state.session_id || state.faulted)
            fail("Start a healthy physics preview before stepping");
        send({{"op", "step"},
              {"session_id", state.session_id->to_string()},
              {"sequence", std::to_string(state.next_sequence)},
              {"expected_tick", std::to_string(state.tick)},
              {"ticks", 1}},
             "step");
    }
    void reset_schedule(MonotonicTime now) {
        scheduler = take(OfflineScheduler::create(now, state.tick));
        state.alpha = 1;
    }
};

PhysicsPanel::PhysicsPanel(ProjectSession& session) : impl_(std::make_unique<Impl>(session)) {}
PhysicsPanel::~PhysicsPanel() = default;
const PhysicsPreviewState& PhysicsPanel::state() const noexcept {
    return impl_->state;
}
std::span<const std::string> PhysicsPanel::contact_details() const noexcept {
    return impl_->contacts;
}
std::span<const std::string> PhysicsPanel::script_details() const noexcept {
    return impl_->scripts;
}
Result<std::vector<PhysicsDebugBody>> PhysicsPanel::debug_bodies() {
    auto& value = *impl_;
    value.body_details.clear();
    try {
        if (!value.state.active || !value.state.session_id || value.state.uncertain ||
            value.state.faulted)
            fail("A healthy reconciled physics preview is required for inspection");
        const auto response =
            value.call({{"op", "query"},
                        {"session_id", value.state.session_id->to_string()},
                        {"expected_tick", std::to_string(value.state.tick)},
                        {"expected_sequence", std::to_string(value.state.next_sequence)},
                        {"bodies_only", true},
                        {"limit", 128}});
        if (response.at("session_id").get<std::string>() != value.state.session_id->to_string() ||
            decimal(response.at("tick")) != value.state.tick ||
            decimal(response.at("next_sequence")) != value.state.next_sequence ||
            !response.at("complete").get<bool>())
            fail("Physics body observation is incomplete or stale");
        const auto& rows = response.at("entities");
        if (!rows.is_array() || rows.size() > 128)
            fail("Physics body observation exceeds its limit");
        std::vector<PhysicsDebugBody> result;
        std::vector<std::string> details;
        result.reserve(rows.size());
        details.reserve(rows.size());
        for (const auto& row : rows) {
            const auto id = take(StableId::parse(row.at("entity_id").get<std::string>()));
            auto body = body_value(row);
            const auto& settings = row.at("rigid_body");
            const auto& velocity = row.at("physics_body").at("velocity");
            auto text =
                id.to_string() + " / " + settings.at("motion").get<std::string>() + " " +
                settings.at("shape").get<std::string>() +
                (body.settings.is_trigger ? " [trigger]" : "") + " / layer " +
                std::to_string(body.settings.collision_layer) + " mask " +
                std::to_string(body.settings.collision_mask) + " / position " +
                vector_text(body.pose.translation) + " / rotation xyzw (" +
                std::to_string(body.pose.rotation.x) + ", " + std::to_string(body.pose.rotation.y) +
                ", " + std::to_string(body.pose.rotation.z) + ", " +
                std::to_string(body.pose.rotation.w) + ") / scale " + vector_text(body.pose.scale);
            if (velocity.is_null())
                text += " / velocity unavailable for this motion";
            else
                text += " / linear velocity " + vector_text(vector_value(velocity.at("linear"))) +
                        " / angular velocity " + vector_text(vector_value(velocity.at("angular")));
            result.push_back(std::move(body));
            details.push_back(std::move(text));
        }
        value.body_details = std::move(details);
        return result;
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "Cannot observe physics bodies",
                     {.operation = "editor.physics.debug_bodies"}};
    }
}
std::span<const std::string> PhysicsPanel::body_details() const noexcept {
    return impl_->body_details;
}
Result<void> PhysicsPanel::inspect_query(PhysicsDebugQuery query, std::uint32_t layer,
                                         std::uint32_t mask, bool include_triggers,
                                         std::optional<AuthoredEntityId> ignore,
                                         std::size_t max_hits) {
    auto& value = *impl_;
    return boundary([&] {
        value.debug_query.reset();
        value.query_details.clear();
        if (!value.state.active || !value.state.session_id || value.state.uncertain ||
            value.state.faulted || value.state.running)
            fail("Pause a healthy reconciled physics preview before inspecting a query");
        if (max_hits == 0 || max_hits > 128)
            fail("Physics query hit limit must be between 1 and 128");
        Json request{{"session_id", value.state.session_id->to_string()},
                     {"expected_tick", std::to_string(value.state.tick)},
                     {"expected_sequence", std::to_string(value.state.next_sequence)},
                     {"layer", layer},
                     {"mask", mask},
                     {"include_triggers", include_triggers},
                     {"max_hits", max_hits}};
        if (ignore)
            request["ignore_entity_id"] = ignore->value().to_string();
        switch (query.kind) {
        case PhysicsDebugQueryKind::raycast:
            request["op"] = "raycast";
            request["from"] = vector_json(query.first);
            request["to"] = vector_json(query.second);
            break;
        case PhysicsDebugQueryKind::overlap_sphere:
            request["op"] = "overlap_sphere";
            request["center"] = vector_json(query.first);
            request["radius"] = query.radius;
            break;
        case PhysicsDebugQueryKind::overlap_box:
            request["op"] = "overlap_box";
            request["center"] = vector_json(query.first);
            request["half_extents"] = vector_json(query.second);
            break;
        }
        const auto response = value.call(request);
        if (response.at("session_id").get<std::string>() != value.state.session_id->to_string() ||
            decimal(response.at("tick")) != value.state.tick ||
            decimal(response.at("next_sequence")) != value.state.next_sequence)
            fail("Physics query observation is stale");
        const auto& hits = response.at("hits");
        if (!hits.is_array() || hits.size() > max_hits || hits.size() > physics_debug_maximum_hits)
            fail("Physics query observation exceeds its limit");
        query.hit_positions.clear();
        std::vector<std::string> details;
        details.reserve(hits.size());
        for (const auto& hit : hits) {
            const auto id = take(StableId::parse(hit.at("entity_id").get<std::string>()));
            auto text = id.to_string();
            if (hit.at("is_trigger").get<bool>())
                text += " [trigger]";
            if (query.kind == PhysicsDebugQueryKind::raycast) {
                const auto position = vector_value(hit.at("position"));
                query.hit_positions.push_back(position);
                text += " fraction " + std::to_string(hit.at("fraction").get<double>());
                text += " / position " + vector_text(position);
                const auto& normal = hit.at("normal");
                text += normal.is_null() ? " / normal unavailable"
                                         : " / normal " + vector_text(vector_value(normal));
            }
            details.push_back(std::move(text));
        }
        value.query_details = std::move(details);
        value.debug_query = std::move(query);
    });
}
const std::optional<PhysicsDebugQuery>& PhysicsPanel::debug_query() const noexcept {
    return impl_->debug_query;
}
std::span<const std::string> PhysicsPanel::query_details() const noexcept {
    return impl_->query_details;
}
void PhysicsPanel::clear_query() noexcept {
    impl_->debug_query.reset();
    impl_->query_details.clear();
}
Result<void> PhysicsPanel::start(AuthoredEntityId camera, SceneRevision expected,
                                 std::string_view target, MonotonicTime now) {
    auto& value = *impl_;
    return boundary([&] {
        if (value.state.active || value.pending)
            fail("Stop or reconcile the current preview first");
        const auto status = value.call({{"op", "status"}});
        if (status.at("active").get<bool>())
            fail("A preview is already active in this project");
        // Allocate the schedule before publishing the runtime owner.
        value.scheduler = take(OfflineScheduler::create(now));
        value.send({{"op", "start"},
                    {"owner_scope", status.at("owner_scope")},
                    {"expected_generation", status.at("generation")},
                    {"expected_revision", revision_json(expected)},
                    {"camera", camera.value().to_string()},
                    {"target", {{"id", target}, {"side", "client"}}}},
                   "start");
        value.state.running = true;
    });
}
void PhysicsPanel::pause(bool paused) noexcept {
    impl_->state.running =
        !paused && impl_->state.active && !impl_->state.uncertain && !impl_->state.faulted;
}
Result<void> PhysicsPanel::poll(MonotonicTime now, bool blocked) {
    auto& value = *impl_;
    auto result = boundary([&] {
        if (value.session.state_current() &&
            (!value.session.state().open ||
             (value.state.active && value.state.source_revision.scene_instance !=
                                        value.session.state().revision.scene_instance))) {
            value.state = {};
            value.pending.reset();
            value.scheduler.reset();
            value.debug_query.reset();
            value.query_details.clear();
            value.body_details.clear();
        }
        if (!value.state.active)
            return;
        // A known fault already reached the caller with its original context.
        // Stay quiescent until Stop instead of replacing that diagnostic on
        // every subsequent display frame.
        if (value.state.faulted)
            return;
        if (!value.scheduler || value.state.uncertain)
            fail("Reconcile or stop the physics preview before continuing");
        TickCallback callback = [&](std::uint64_t tick) {
            return boundary([&] {
                if (tick != value.state.tick)
                    fail("Physics schedule and preview tick differ");
                value.step_once();
            });
        };
        const auto report = take(value.scheduler->poll(
            now, callback, OfflineControl{blocked || !value.state.running, false}));
        value.state.alpha = blocked || !value.state.running ? 1 : report.interpolation_alpha();
    });
    if (!result) {
        value.state.running = false;
        if (value.scheduler && value.scheduler->faulted() && !value.state.uncertain)
            value.state.faulted = true;
    }
    return result;
}
Result<void> PhysicsPanel::step(MonotonicTime now) {
    auto& value = *impl_;
    return boundary([&] {
        if (value.state.running)
            fail("Pause before single stepping");
        if (!value.scheduler || value.state.uncertain || value.state.faulted)
            fail("Reconcile or start a healthy preview before stepping");
        TickCallback callback = [&](std::uint64_t tick) {
            return boundary([&] {
                if (tick != value.state.tick)
                    fail("Physics schedule and preview tick differ");
                value.step_once();
            });
        };
        take(value.scheduler->poll(now, callback, OfflineControl{true, true}));
        value.state.alpha = 1;
    });
}
Result<void> PhysicsPanel::remove(AuthoredEntityId entity) {
    auto& value = *impl_;
    return boundary([&] {
        if (!value.state.active || !value.state.session_id || value.state.running)
            fail("Pause the active preview before removing a runtime body");
        value.send({{"op", "remove"},
                    {"session_id", value.state.session_id->to_string()},
                    {"sequence", std::to_string(value.state.next_sequence)},
                    {"expected_tick", std::to_string(value.state.tick)},
                    {"entity_id", entity.value().to_string()}},
                   "remove");
        value.state.alpha = 1;
    });
}
Result<void> PhysicsPanel::reconcile(MonotonicTime now) {
    auto& value = *impl_;
    return boundary([&] {
        if (!value.pending)
            fail("There is no unresolved physics operation");
        const auto result = value.call(Json::parse(*value.pending));
        value.observe(result, value.pending_op);
        value.pending.reset();
        value.state.uncertain = false;
        value.state.running = false;
        if (value.state.active)
            value.reset_schedule(now);
    });
}
Result<void> PhysicsPanel::stop() {
    auto& value = *impl_;
    return boundary([&] {
        if (value.session.state_current() &&
            (!value.session.state().open ||
             (value.state.active && value.state.source_revision.scene_instance !=
                                        value.session.state().revision.scene_instance))) {
            value.state = {};
            value.scheduler.reset();
            value.pending.reset();
            value.debug_query.reset();
            value.query_details.clear();
            value.body_details.clear();
            return;
        }
        if (!value.state.active || !value.state.session_id)
            fail("Reconcile an uncertain start before stopping it");
        // Scoped stop is always available, including after an unresolved step.
        const auto result =
            value.call({{"op", "stop"}, {"session_id", value.state.session_id->to_string()}});
        value.observe(result, "stop");
        value.pending.reset();
    });
}
} // namespace shark::editor
