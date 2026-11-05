#pragma once

#include "shark/error.hpp"
#include "shark/physics.hpp"
#include "shark/runtime_prefab.hpp"
#include "shark/scene_identity.hpp"
#include "shark/scene_query.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace shark {

class Scene;

enum class ScriptProfile { server_gameplay, client_presentation };
enum class ScriptPhase { load, init, update, fixed_update, contact, destroy };

// Immutable trusted source captures. The caller owns file access, source
// freshness, project containment and typed-file overlap checks.
struct ScriptSourceSnapshot {
    std::string path;
    std::string text;
};

inline constexpr std::size_t script_runtime_maximum_sources = 64;
inline constexpr std::size_t script_runtime_maximum_source_bytes = 32 * 1024;
inline constexpr std::size_t script_runtime_maximum_total_source_bytes = 2 * 1024 * 1024;
inline constexpr std::size_t script_runtime_maximum_scripts = 64;
inline constexpr std::size_t script_runtime_maximum_vm_bytes = 64 * 1024 * 1024;
inline constexpr std::size_t script_runtime_maximum_instructions = 10'000'000;
inline constexpr std::size_t script_runtime_maximum_logs = 256;
inline constexpr std::size_t script_runtime_maximum_log_bytes = 64 * 1024;
inline constexpr std::size_t script_runtime_maximum_log_message_bytes = 256;

struct ScriptRuntimeConfig {
    ScriptProfile profile{ScriptProfile::client_presentation};
    std::size_t max_scripts{16};
    std::size_t memory_bytes_per_script{4 * 1024 * 1024};
    std::size_t instruction_limit{100000};
};

struct ScriptLogEntry {
    EntityReference entity;
    ScriptPhase phase;
    std::uint64_t tick;
    std::string message;
};

struct ScriptStopReport {
    std::size_t attempted{0};
    std::size_t discarded{0};
    // Errors are owned in reverse order. Stale/changed instances are discarded
    // with an error; every other destroy callback is attempted after failures.
    std::vector<Error> errors;
    std::size_t removed_spawned_entities{0};
    std::size_t removed_spawned_bodies{0};
    std::size_t remaining_spawned_roots{0};
};

inline constexpr std::size_t script_runtime_maximum_contact_events = 256;
inline constexpr std::size_t script_runtime_maximum_deferred_physics = 64;

// Consumed observations and explicit partial effects are retained by value.
// error means callbacks or deferred work failed; no result claims rollback.
struct ScriptContactReport {
    std::vector<PhysicsContactEvent> events;
    std::size_t attempted_callbacks{0};
    std::size_t skipped_events{0};
    std::size_t undelivered_events{0};
    std::size_t applied_commands{0};
    std::size_t discarded_commands{0};
    std::optional<Error> error;
};

enum class ScriptEntityMutationKind { spawn, destroy };
enum class ScriptEntityMutationState { queued, applied, failed, discarded };

// Owned historical effects, not live authority or a rollback promise. Failed
// operations retain the exact prefix of entity/native/script effects completed.
struct ScriptEntityMutationReceipt {
    EntityReference requester;
    std::uint64_t sequence;
    ScriptEntityMutationKind kind;
    ScriptEntityMutationState state{ScriptEntityMutationState::queued};
    std::optional<WorldEntityId> root{};
    std::vector<EntityReference> created_entities{};
    std::vector<EntityReference> removed_entities{};
    std::size_t created_bodies{0};
    std::size_t removed_bodies{0};
    std::size_t initialized_scripts{0};
    std::size_t destroyed_scripts{0};
    std::size_t attempted_destroy_callbacks{0};
    bool native_progress_uncertain{false};
    std::optional<Error> error{};
};

struct ScriptEntityMutationReport {
    std::vector<std::shared_ptr<const ScriptEntityMutationReceipt>> outcomes;
};

inline constexpr std::size_t script_runtime_maximum_entity_mutations = 64;

struct ScriptRuntimeStats {
    ScriptProfile profile;
    SceneRevision initial_revision;
    std::size_t scripts;
    std::size_t initialized;
    std::size_t queued_logs;
    std::optional<std::uint64_t> last_fixed_tick;
    bool started; // Initialization began; remains true after stop/close.
    bool faulted;
    bool closed;
};

// Single creating-thread owner. No Scene pointer survives a public call.
// Source chunks load into isolated VMs in stable entity-identity order. start,
// update, fixed_update and stop synchronously invoke on_init, on_update,
// on_fixed_update and on_destroy, respectively, when defined. A callback
// failure faults the owner without rolling back prior callbacks. Explicit stop
// attempts initialized destroy callbacks in reverse order; close discards them
// and reports the number skipped. A live owner must die on its creating thread.
class ScriptRuntime final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<ScriptRuntime>>
    create(Scene& scene, std::span<const ScriptSourceSnapshot> sources,
           ScriptRuntimeConfig config = {});
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;
    ScriptRuntime(ScriptRuntime&&) = delete;
    ScriptRuntime& operator=(ScriptRuntime&&) = delete;

    // Trusted host opt-in before start. Owns validated value snapshots; no
    // source parser, file access or gameplay admission is inferred from scope.
    [[nodiscard]] Result<void> enable_spawning(Scene& scene, PhysicsWorld& physics,
                                               RuntimeSpawnAuthority& authority,
                                               std::span<const RuntimePrefab> prefabs);

    [[nodiscard]] Result<void> start(Scene& scene);
    [[nodiscard]] Result<void> start(Scene& scene, PhysicsWorld& physics);
    [[nodiscard]] Result<void> update(Scene& scene, double delta_seconds);
    [[nodiscard]] Result<void> update(Scene& scene, PhysicsWorld& physics, double delta_seconds);
    // Tick may begin at zero, then must be consecutive and at most INT64_MAX.
    // Call from the existing offline/server scheduler; no clock is owned here.
    [[nodiscard]] Result<void> fixed_update(Scene& scene, std::uint64_t tick, double delta_seconds);
    [[nodiscard]] Result<void> fixed_update(Scene& scene, PhysicsWorld& physics, std::uint64_t tick,
                                            double delta_seconds);
    // Drain a bounded native FIFO prefix after a completed update. Any exits
    // produced by deferred removals stay queued for a later dispatch.
    [[nodiscard]] Result<ScriptContactReport> dispatch_contacts(Scene& scene, PhysicsWorld& physics,
                                                                std::size_t maximum = 256);
    [[nodiscard]] Result<ScriptStopReport> stop(Scene& scene);
    [[nodiscard]] Result<ScriptStopReport> stop(Scene& scene, PhysicsWorld& physics);
    [[nodiscard]] Result<std::vector<ScriptLogEntry>> take_logs();
    [[nodiscard]] Result<ScriptEntityMutationReport> take_entity_mutations();
    [[nodiscard]] Result<ScriptRuntimeStats> stats() const;
    // Close without Scene access. Reports initialized callbacks not invoked.
    [[nodiscard]] Result<std::size_t> close();

  private:
    [[nodiscard]] Result<void> start_impl(Scene& scene, PhysicsWorld* physics);
    [[nodiscard]] Result<void> update_impl(Scene& scene, PhysicsWorld* physics,
                                           double delta_seconds);
    [[nodiscard]] Result<void> fixed_update_impl(Scene& scene, PhysicsWorld* physics,
                                                 std::uint64_t tick, double delta_seconds);
    [[nodiscard]] Result<ScriptStopReport> stop_impl(Scene& scene, PhysicsWorld* physics);
    struct Impl;
    explicit ScriptRuntime(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark
