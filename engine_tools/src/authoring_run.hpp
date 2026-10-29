#pragma once

#include "shark/tools/scene_runner.hpp"

namespace shark::tools {

struct AuthoringRunRequest {
    SceneRevision expected_revision;
    SceneRunConfig config;
    std::optional<std::size_t> expected_entities{};
};

struct AuthoringRunResult {
    SceneRevision observed_revision;
    bool dirty;
    std::size_t entity_count;
    // SHA-256 of the canonical captured source scene, including unsaved values.
    std::string source_digest;
    SceneRunStatus status;
    std::string report_json;
};

// A native static scene-integrity case over the current captured source values.
// Uses the existing deterministic runner and its isolated case owner, never a
// second project writer. This does not execute gameplay, Lua or device input.
// Empty config.inputs is required because this case has no input actions.
[[nodiscard]] Result<AuthoringRunResult> run_authoring_scene(const AuthoredScene& owner,
                                                             const AuthoringRunRequest& request,
                                                             std::stop_token stop = {});

} // namespace shark::tools
