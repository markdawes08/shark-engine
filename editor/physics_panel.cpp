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
        if (error_code_name(item) == code)
            return {item, value.at("message").get<std::string>(), {.operation = "editor.physics"}};
    fail("Unknown physics error code");
}
Json revision_json(SceneRevision value) {
    return {{"scene_instance", std::to_string(value.scene_instance)},
            {"revision", std::to_string(value.revision)}};
}
SceneRevision revision_value(const Json& value) {
    return {decimal(value.at("scene_instance")), decimal(value.at("revision"))};
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

    explicit Impl(ProjectSession& value) : session(value) {}
    Json call(const Json& parameters) {
        const auto reply = take(session.request("scene.physics", parameters.dump()));
        const auto envelope = Json::parse(reply.json);
        if (envelope.contains("error")) {
            auto error = envelope.at("error").at("data");
            error["message"] = envelope.at("error").at("message");
            auto decoded = decode_error(error);
            const bool certain = decoded.code != ErrorCode::execution_failed &&
                                 decoded.code != ErrorCode::io_failure;
            throw Failure{std::move(decoded), certain};
        }
        return envelope.at("result");
    }
    void observe(const Json& value, std::string_view operation) {
        if (operation == "stop") {
            state = {};
            scheduler.reset();
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
        }
        if (!value.state.active)
            return;
        if (!value.scheduler || value.state.uncertain || value.state.faulted)
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
