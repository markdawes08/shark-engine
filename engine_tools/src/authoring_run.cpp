#include "authoring_run.hpp"

#include "shark/content_digest.hpp"

#include <array>
#include <new>

namespace shark::tools {
namespace {
Error run_error(ErrorCode code, const char* message) {
    return {code, message, {.operation = "authoring.scene.run"}};
}
} // namespace

Result<AuthoringRunResult> run_authoring_scene(const AuthoredScene& owner,
                                               const AuthoringRunRequest& request,
                                               std::stop_token stop) {
    try {
        if (auto valid = validate_scene_run_config(request.config); !valid)
            return valid.error();
        if (!request.config.inputs.empty())
            return run_error(ErrorCode::invalid_argument,
                             "The scene-integrity case does not support input actions");
        if (request.expected_entities && *request.expected_entities > source_maximum_entities)
            return run_error(ErrorCode::invalid_argument,
                             "Expected entity count exceeds the source scene limit");

        auto captured = owner.capture(request.expected_revision);
        if (!captured)
            return captured.error();
        const auto& snapshot = captured.value();
        auto encoded = encode_scene_document(snapshot.scene);
        if (!encoded)
            return encoded.error();
        auto digest = sha256(std::as_bytes(std::span{encoded.value()}));
        if (!digest)
            return digest.error();

        std::optional<SceneRevision> case_revision;
        const auto inspect_values = [&](SceneRunContext& context) -> Result<void> {
            auto current = context.owner().capture();
            if (!current)
                return current.error();
            return context.require(current.value().project == snapshot.project &&
                                       current.value().scene == snapshot.scene,
                                   "Isolated scene preserves the captured authored values");
        };
        SceneRunCase test{.id = "scene-integrity"};
        test.setup = [&](SceneRunContext& context) -> Result<void> {
            auto state = context.owner().scene();
            if (!state)
                return state.error();
            auto stats = state.value().get().stats();
            if (!stats)
                return stats.error();
            const auto expected =
                request.expected_entities.value_or(snapshot.scene.entities.size());
            auto count = context.require(stats.value().live_entities == expected,
                                         "Scene entity count differs from expected_entities");
            if (!count)
                return count.error();
            auto status = context.owner().status();
            if (!status)
                return status.error();
            case_revision = status.value().revision;
            return inspect_values(context);
        };
        test.tick = [&](SceneRunContext& context, const SceneRunTick&) -> Result<void> {
            auto status = context.owner().status();
            if (!status)
                return status.error();
            return context.require(case_revision && status.value().revision == *case_revision &&
                                       !status.value().dirty &&
                                       status.value().pending_mutations == 0,
                                   "Static scene remains unchanged at the deterministic tick");
        };
        test.finish = inspect_values;
        const std::array cases{std::move(test)};
        auto run = run_scene_cases(snapshot.project, snapshot.scene, request.config, cases, stop);
        if (!run)
            return run.error();
        auto report = encode_scene_run_report(run.value());
        if (!report)
            return report.error();

        auto observed = owner.status();
        if (!observed)
            return observed.error();
        if (observed.value().revision != snapshot.revision ||
            observed.value().dirty != snapshot.dirty || observed.value().pending_mutations != 0)
            return run_error(ErrorCode::conflict, "Authored scene changed during the captured run");
        return AuthoringRunResult{
            snapshot.revision,       snapshot.dirty,     snapshot.scene.entities.size(),
            digest.value().to_hex(), run.value().status, std::move(report).value()};
    } catch (const std::bad_alloc&) {
        return run_error(ErrorCode::execution_failed,
                         "Cannot allocate bounded authored run values");
    } catch (...) {
        return run_error(ErrorCode::execution_failed, "Cannot complete the authored scene run");
    }
}

} // namespace shark::tools
