#include "shark/tools/scene_runner.hpp"

#include "shark/bootstrap.hpp"
#include "shark/time.hpp"
#include "utf8.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <new>
#include <set>
#include <utility>

namespace shark::tools {
namespace {
using Json = nlohmann::json;

Error run_error(ErrorCode code, const char* message) {
    return {code, message, {.operation = "scene.run"}};
}

bool token(std::string_view text) {
    return !text.empty() && text.size() <= 64 &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-';
           });
}

bool diagnostic(std::string_view text, std::size_t maximum = 1024) {
    return text.size() <= maximum && shark::detail::valid_utf8(text);
}

Error safe_error(const Error& input) {
    const auto& error = input;
    if (error.message.empty() || !diagnostic(error.message) ||
        !diagnostic(error.context.operation) || !diagnostic(error.context.request_id) ||
        !diagnostic(error.context.entity_id) || !diagnostic(error.context.asset_id) ||
        !diagnostic(error.context.path, 4096)) {
        return run_error(ErrorCode::execution_failed, "Scene case returned malformed diagnostics");
    }
    auto result = error;
    if (result.context.operation.empty())
        result.context.operation = "scene.run.callback";
    return result;
}

const char* status_name(SceneRunStatus status) {
    switch (status) {
    case SceneRunStatus::passed:
        return "PASS";
    case SceneRunStatus::failed:
        return "FAIL";
    case SceneRunStatus::cancelled:
        return "CANCELLED";
    case SceneRunStatus::not_run:
        return "NOT_RUN";
    }
    return "INVALID";
}
const char* phase_name(SceneRunPhase phase) {
    switch (phase) {
    case SceneRunPhase::setup:
        return "setup";
    case SceneRunPhase::tick:
        return "tick";
    case SceneRunPhase::finish:
        return "finish";
    }
    return "invalid";
}
const char* failure_name(SceneRunFailureKind kind) {
    switch (kind) {
    case SceneRunFailureKind::assertion:
        return "assertion";
    case SceneRunFailureKind::callback:
        return "callback";
    case SceneRunFailureKind::exception:
        return "exception";
    case SceneRunFailureKind::limit:
        return "limit";
    case SceneRunFailureKind::owner:
        return "owner";
    }
    return "invalid";
}

Json error_json(const Error& input) {
    const auto error = safe_error(input);
    return {{"code", error_code_name(error.code)},  {"message", error.message},
            {"operation", error.context.operation}, {"request_id", error.context.request_id},
            {"path", error.context.path},           {"entity_id", error.context.entity_id},
            {"asset_id", error.context.asset_id}};
}
Json revision_json(const std::optional<SceneRevision>& revision) {
    if (!revision)
        return nullptr;
    return {{"instance", revision->scene_instance}, {"value", revision->revision}};
}

template <typename T, typename Function> Result<T> boundary(Function&& function) {
    try {
        return function();
    } catch (const std::bad_alloc&) {
        return run_error(ErrorCode::execution_failed, "Scene runner allocation failed");
    } catch (...) {
        return run_error(ErrorCode::execution_failed, "Scene runner operation failed");
    }
}
} // namespace

Result<void> validate_scene_run_config(const SceneRunConfig& config) {
    if (!token(config.run_id) || !token(config.revision) || config.ticks == 0 ||
        config.ticks > scene_run_maximum_ticks || config.tick_rate_hz == 0 ||
        config.tick_rate_hz > 1000 || config.inputs.size() > scene_run_maximum_inputs) {
        return run_error(ErrorCode::invalid_configuration,
                         "Scene run configuration exceeds its bounds");
    }
    std::uint64_t previous = 0;
    for (const auto& input : config.inputs) {
        if (!token(input.action) || input.tick >= config.ticks || input.tick < previous) {
            return run_error(ErrorCode::invalid_configuration,
                             "Inputs need ordered in-range ticks and action IDs");
        }
        previous = input.tick;
    }
    return {};
}

SceneRunContext::SceneRunContext(AuthoredScene& owner, SceneRunCaseResult& result,
                                 std::uint64_t seed)
    : owner_(owner), result_(result), random_state_(seed), thread_(std::this_thread::get_id()) {}

AuthoredScene& SceneRunContext::owner() noexcept {
    return owner_;
}

Result<void> SceneRunContext::fail(SceneRunFailureKind kind, Error error) {
    if (!result_.failure) {
        result_.failure = SceneRunFailure{kind, phase_, tick_, safe_error(std::move(error))};
        result_.status = SceneRunStatus::failed;
    }
    return result_.failure->error;
}

Result<void> SceneRunContext::require(bool condition, std::string_view message,
                                      std::optional<AuthoredEntityId> entity) {
    if (std::this_thread::get_id() != thread_)
        return run_error(ErrorCode::conflict, "Scene run context belongs to another thread");
    if (result_.failure)
        return result_.failure->error;
    if (result_.assertions == scene_run_maximum_assertions)
        return fail(SceneRunFailureKind::limit,
                    run_error(ErrorCode::execution_failed, "Scene assertion limit reached"));
    ++result_.assertions;
    if (message.empty() || !diagnostic(message))
        return fail(
            SceneRunFailureKind::callback,
            run_error(ErrorCode::invalid_argument, "Assertion message must be bounded UTF-8"));
    if (condition)
        return {};
    Error error{
        ErrorCode::execution_failed, std::string{message}, {.operation = "scene.run.assertion"}};
    if (entity)
        error.context.entity_id = entity->value().to_string();
    return fail(SceneRunFailureKind::assertion, std::move(error));
}

Result<std::uint64_t> SceneRunContext::random_u64() {
    if (std::this_thread::get_id() != thread_)
        return run_error(ErrorCode::conflict, "Scene run context belongs to another thread");
    if (result_.failure)
        return result_.failure->error;
    if (result_.random_draws == scene_run_maximum_random_draws) {
        auto failed =
            fail(SceneRunFailureKind::limit,
                 run_error(ErrorCode::execution_failed, "Scene random draw limit reached"));
        return failed.error();
    }
    ++result_.random_draws;
    auto value = (random_state_ += 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

Result<SceneRunReport> run_scene_cases(const ProjectDocument& project, const SceneDocument& scene,
                                       const SceneRunConfig& config,
                                       std::span<const SceneRunCase> cases, std::stop_token stop) {
    return boundary<SceneRunReport>([&]() -> Result<SceneRunReport> {
        if (auto valid = validate_scene_run_config(config); !valid)
            return valid.error();
        if (cases.empty() || cases.size() > scene_run_maximum_cases)
            return run_error(ErrorCode::invalid_argument, "Scene runner needs 1..32 cases");
        std::set<std::string> names;
        for (const auto& test : cases) {
            if (!token(test.id) || !test.tick || !names.insert(test.id).second)
                return run_error(ErrorCode::invalid_argument,
                                 "Cases need unique IDs and tick callbacks");
        }
        // Complete document/cross-reference/world validation precedes all callbacks.
        auto initial = AuthoredScene::create(project, scene);
        if (!initial)
            return initial.error();
        SceneRunReport report{config, project.project, scene.scene, SceneRunStatus::passed, {}};
        report.cases.reserve(cases.size());
        for (const auto& test : cases) {
            report.cases.push_back(SceneRunCaseResult{.id = test.id});
            auto& result = report.cases.back();
            if (stop.stop_requested()) {
                if (report.status != SceneRunStatus::failed)
                    report.status = SceneRunStatus::cancelled;
                continue;
            }
            auto candidate = report.cases.size() == 1 ? std::move(initial)
                                                      : AuthoredScene::create(project, scene);
            if (!candidate)
                return candidate.error();
            auto& owner = *candidate.value();
            SceneRunContext context{owner, result, config.seed};
            auto before = owner.status();
            if (!before)
                return before.error();
            result.initial_revision = before.value().revision;
            const auto invoke = [&](auto&& callback) -> Result<void> {
                try {
                    auto outcome = callback();
                    if (!outcome)
                        return context.fail(SceneRunFailureKind::callback,
                                            std::move(outcome).error());
                    if (result.failure)
                        return result.failure->error;
                    return {};
                } catch (...) {
                    return context.fail(SceneRunFailureKind::exception,
                                        run_error(ErrorCode::execution_failed,
                                                  "Scene case callback threw an exception"));
                }
            };
            Result<void> outcome;
            if (test.setup)
                outcome = invoke([&] { return test.setup(context); });
            auto schedule =
                ServerScheduler::create({config.tick_rate_hz, 1, 1}, MonotonicTime::zero());
            if (!schedule)
                return schedule.error();
            std::size_t input_cursor = 0;
            const TickCallback tick = [&](std::uint64_t index) -> Result<void> {
                context.phase_ = SceneRunPhase::tick;
                context.tick_ = index;
                const auto begin = input_cursor;
                while (input_cursor < config.inputs.size() &&
                       config.inputs[input_cursor].tick == index)
                    ++input_cursor;
                const SceneRunTick step{index, config.tick_rate_hz,
                                        std::span<const SceneRunInput>{config.inputs}.subspan(
                                            begin, input_cursor - begin)};
                auto completed = invoke([&] { return test.tick(context, step); });
                if (completed)
                    ++result.ticks_completed;
                return completed;
            };
            for (std::uint64_t index = 0; outcome && index < config.ticks; ++index) {
                if (stop.stop_requested())
                    break;
                // ceil((index+1)*1e9/rate) admits exactly one rational tick.
                const auto nanoseconds =
                    ((index + 1) * 1'000'000'000ULL + config.tick_rate_hz - 1) /
                    config.tick_rate_hz;
                auto pass = schedule.value()->poll(
                    MonotonicTime{static_cast<std::int64_t>(nanoseconds)}, tick, stop);
                if (pass && pass.value().cancelled && pass.value().executed == 0)
                    break;
                if (!pass) {
                    if (!result.failure)
                        static_cast<void>(
                            context.fail(SceneRunFailureKind::callback, pass.error()));
                    outcome = pass.error();
                } else if (pass.value().executed != 1 || pass.value().discarded != 0 ||
                           pass.value().pending != 0) {
                    outcome =
                        context.fail(SceneRunFailureKind::callback,
                                     run_error(ErrorCode::execution_failed,
                                               "Scene schedule did not complete exactly one tick"));
                }
            }
            if (outcome && !stop.stop_requested()) {
                context.phase_ = SceneRunPhase::finish;
                context.tick_.reset();
                if (test.finish)
                    outcome = invoke([&] { return test.finish(context); });
                if (outcome && result.assertions == 0)
                    outcome = context.fail(SceneRunFailureKind::assertion,
                                           run_error(ErrorCode::execution_failed,
                                                     "Scene case completed without assertions"));
            }
            auto after = owner.status();
            if (!after) {
                static_cast<void>(context.fail(SceneRunFailureKind::owner, after.error()));
            } else {
                result.final_revision = after.value().revision;
            }
            if (result.failure) {
                result.status = SceneRunStatus::failed;
                report.status = SceneRunStatus::failed;
            } else if (stop.stop_requested()) {
                result.status = SceneRunStatus::cancelled;
                if (report.status != SceneRunStatus::failed)
                    report.status = SceneRunStatus::cancelled;
            } else {
                result.status = SceneRunStatus::passed;
            }
        }
        return report;
    });
}

Result<std::string> encode_scene_run_report(const SceneRunReport& report,
                                            std::string_view result_path) {
    return boundary<std::string>([&]() -> Result<std::string> {
        if (auto valid = validate_scene_run_config(report.config); !valid)
            return valid.error();
        if (report.cases.empty() || report.cases.size() > scene_run_maximum_cases ||
            !diagnostic(result_path, 4096))
            return run_error(ErrorCode::invalid_argument, "Invalid scene result bounds");
        Json cases = Json::array();
        std::set<std::string> ids;
        bool failed = false, cancelled = false;
        for (const auto& result : report.cases) {
            if (!token(result.id) || !ids.insert(result.id).second ||
                std::string_view{status_name(result.status)} == "INVALID" ||
                result.ticks_completed > report.config.ticks ||
                result.assertions > scene_run_maximum_assertions ||
                result.random_draws > scene_run_maximum_random_draws ||
                (result.status == SceneRunStatus::failed) != result.failure.has_value() ||
                (result.status == SceneRunStatus::passed &&
                 (result.ticks_completed != report.config.ticks || result.assertions == 0 ||
                  !result.final_revision)))
                return run_error(ErrorCode::invalid_argument, "Inconsistent scene case result");
            if (result.status == SceneRunStatus::not_run &&
                (result.ticks_completed != 0 || result.assertions != 0 ||
                 result.random_draws != 0 || result.initial_revision || result.final_revision))
                return run_error(ErrorCode::invalid_argument,
                                 "Unexecuted case has execution state");
            if (result.status != SceneRunStatus::not_run && !result.initial_revision)
                return run_error(ErrorCode::invalid_argument,
                                 "Executed case lacks its initial revision");
            if (result.initial_revision && result.final_revision &&
                (result.initial_revision->scene_instance == 0 ||
                 result.final_revision->scene_instance == 0 ||
                 (result.initial_revision->scene_instance ==
                      result.final_revision->scene_instance &&
                  result.initial_revision->revision > result.final_revision->revision)))
                return run_error(ErrorCode::invalid_argument,
                                 "Case revisions contradict scene ownership");
            failed = failed || result.status == SceneRunStatus::failed;
            cancelled = cancelled || result.status == SceneRunStatus::cancelled ||
                        result.status == SceneRunStatus::not_run;
            Json entry{{"id", result.id},
                       {"status", status_name(result.status)},
                       {"ticks_completed", result.ticks_completed},
                       {"assertions", result.assertions},
                       {"random_draws", result.random_draws},
                       {"initial_revision", revision_json(result.initial_revision)},
                       {"final_revision", revision_json(result.final_revision)},
                       {"failure", nullptr}};
            if (result.failure) {
                const auto& failure = *result.failure;
                if (std::string_view{failure_name(failure.kind)} == "invalid" ||
                    std::string_view{phase_name(failure.phase)} == "invalid" ||
                    ((failure.phase == SceneRunPhase::tick) != failure.tick.has_value()) ||
                    (failure.tick && *failure.tick >= report.config.ticks))
                    return run_error(ErrorCode::invalid_argument, "Invalid scene failure context");
                entry["failure"] = {{"kind", failure_name(failure.kind)},
                                    {"phase", phase_name(failure.phase)},
                                    {"tick", failure.tick ? Json(*failure.tick) : Json(nullptr)},
                                    {"error", error_json(failure.error)}};
            }
            cases.push_back(std::move(entry));
        }
        const auto expected =
            failed ? SceneRunStatus::failed
                   : (cancelled ? SceneRunStatus::cancelled : SceneRunStatus::passed);
        if (report.status != expected)
            return run_error(ErrorCode::invalid_argument, "Scene run status contradicts cases");
        Json inputs = Json::array();
        for (const auto& input : report.config.inputs)
            inputs.push_back(
                {{"tick", input.tick}, {"action", input.action}, {"value", input.value}});
        Json artifacts = Json::array();
        if (!result_path.empty())
            artifacts.push_back(
                {{"role", "result"}, {"path", result_path}, {"format", "shark.scene-run"}});
        Json output{{"format", "shark.scene-run"},
                    {"schema_version", scene_run_version},
                    {"engine_version", version_string()},
                    {"status", status_name(report.status)},
                    {"mode", "cpu"},
                    {"renderer_initialized", false},
                    {"run_id", report.config.run_id},
                    {"revision", report.config.revision},
                    {"seed", report.config.seed},
                    {"tick_rate_hz", report.config.tick_rate_hz},
                    {"ticks", report.config.ticks},
                    {"random_algorithm", "splitmix64-v1"},
                    {"source_schema_version", source_document_version},
                    {"project_id", report.project.value().to_string()},
                    {"scene_id", report.scene.value().to_string()},
                    {"inputs", std::move(inputs)},
                    {"cases", std::move(cases)},
                    {"artifacts", std::move(artifacts)}};
        auto text = output.dump(2) + '\n';
        if (text.size() > scene_run_maximum_result_bytes)
            return run_error(ErrorCode::execution_failed, "Scene run result byte limit exceeded");
        return text;
    });
}

Result<std::string> encode_scene_run_error(const Error& error) {
    return boundary<std::string>([&]() -> Result<std::string> {
        return Json{{"format", "shark.scene-run"},
                    {"schema_version", scene_run_version},
                    {"status", "ERROR"},
                    {"mode", "none"},
                    {"renderer_initialized", false},
                    {"error", error_json(error)}}
                   .dump(2) +
               '\n';
    });
}
} // namespace shark::tools
