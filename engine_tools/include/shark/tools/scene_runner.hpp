#pragma once

#include "shark/tools/authored_scene.hpp"

#include <functional>
#include <span>
#include <stop_token>
#include <string_view>
#include <thread>

namespace shark::tools {

inline constexpr std::uint32_t scene_run_version = 1;
inline constexpr std::size_t scene_run_maximum_cases = 32;
inline constexpr std::uint32_t scene_run_maximum_ticks = 10000;
inline constexpr std::size_t scene_run_maximum_inputs = 4096;
inline constexpr std::uint32_t scene_run_maximum_assertions = 16384;
inline constexpr std::uint32_t scene_run_maximum_random_draws = 65536;
inline constexpr std::size_t scene_run_maximum_result_bytes = 1024 * 1024;

struct SceneRunInput {
    std::uint64_t tick;
    std::string action;
    std::int32_t value;
    bool operator==(const SceneRunInput&) const = default;
};

struct SceneRunConfig {
    std::uint64_t seed{1337};
    std::uint32_t ticks{60};
    // Exact rational timestep 1 / tick_rate_hz, using Core's existing scheduler.
    std::uint32_t tick_rate_hz{60};
    std::string run_id{"local"};
    // Caller-supplied content/build revision; never inferred from a local roadmap.
    std::string revision{"unspecified"};
    // Nondecreasing ticks, preserving caller order within a tick. No held state
    // or OS input is inferred. Each event is delivered on exactly one tick.
    std::vector<SceneRunInput> inputs;
};

enum class SceneRunStatus { passed, failed, cancelled, not_run };
enum class SceneRunPhase { setup, tick, finish };
enum class SceneRunFailureKind { assertion, callback, exception, limit, owner };

struct SceneRunFailure {
    SceneRunFailureKind kind;
    SceneRunPhase phase;
    std::optional<std::uint64_t> tick;
    Error error;
};

struct SceneRunCaseResult {
    std::string id;
    SceneRunStatus status{SceneRunStatus::not_run};
    std::uint32_t ticks_completed{0};
    std::uint32_t assertions{0};
    std::uint32_t random_draws{0};
    std::optional<SceneRevision> initial_revision{};
    std::optional<SceneRevision> final_revision{};
    std::optional<SceneRunFailure> failure{};
};

struct SceneRunReport {
    SceneRunConfig config;
    ProjectId project;
    SceneId scene;
    SceneRunStatus status;
    std::vector<SceneRunCaseResult> cases;
};

struct SceneRunTick {
    std::uint64_t index;
    // Tick start is index / tick_rate_hz seconds, not a wall-clock observation.
    std::uint32_t tick_rate_hz;
    std::span<const SceneRunInput> inputs;
};

class SceneRunContext;
struct SceneRunCase {
    std::string id;
    std::function<Result<void>(SceneRunContext&)> setup{};
    std::function<Result<void>(SceneRunContext&, const SceneRunTick&)> tick{};
    std::function<Result<void>(SceneRunContext&)> finish{};
};

// Trusted native cases, executed synchronously on this thread. Each case gets a
// fresh owner from the same validated source values. All steps inside a case
// share that owner and its revision/history/retry semantics. Stop is cooperative
// between callbacks; callers must bound callbacks and supervise the process.
[[nodiscard]] Result<SceneRunReport> run_scene_cases(const ProjectDocument& project,
                                                     const SceneDocument& scene,
                                                     const SceneRunConfig& config,
                                                     std::span<const SceneRunCase> cases,
                                                     std::stop_token stop = {});

class SceneRunContext final {
  public:
    SceneRunContext(const SceneRunContext&) = delete;
    SceneRunContext& operator=(const SceneRunContext&) = delete;
    // Callback-scoped borrow. AuthoredScene still checks its owner thread and
    // exposes only shared validated commands and const Core inspection.
    [[nodiscard]] AuthoredScene& owner() noexcept;
    // Sticky failure: ignoring a failed assertion cannot make the case pass.
    [[nodiscard]] Result<void> require(bool condition, std::string_view message,
                                       std::optional<AuthoredEntityId> entity = std::nullopt);
    // SplitMix64-v1, unsigned wraparound, reset to the run seed for every case.
    // No distribution or OS entropy; this is test data, never credentials/IDs.
    [[nodiscard]] Result<std::uint64_t> random_u64();

  private:
    friend Result<SceneRunReport> run_scene_cases(const ProjectDocument&, const SceneDocument&,
                                                  const SceneRunConfig&,
                                                  std::span<const SceneRunCase>, std::stop_token);
    SceneRunContext(AuthoredScene& owner, SceneRunCaseResult& result, std::uint64_t seed);
    [[nodiscard]] Result<void> fail(SceneRunFailureKind kind, Error error);
    AuthoredScene& owner_;
    SceneRunCaseResult& result_;
    std::uint64_t random_state_;
    const std::thread::id thread_;
    SceneRunPhase phase_{SceneRunPhase::setup};
    std::optional<std::uint64_t> tick_;
};

[[nodiscard]] Result<void> validate_scene_run_config(const SceneRunConfig& config);
// Bounded UTF-8 JSON, newline terminated. Local incarnations are diagnostic
// context; compare semantic results without assuming identical incarnation IDs.
[[nodiscard]] Result<std::string> encode_scene_run_report(const SceneRunReport& report,
                                                          std::string_view result_path = {});
[[nodiscard]] Result<std::string> encode_scene_run_error(const Error& error);

} // namespace shark::tools
