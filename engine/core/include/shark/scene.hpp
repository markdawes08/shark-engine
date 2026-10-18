#pragma once

#include "shark/scene_identity.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace shark {

struct SceneLimits {
    std::size_t max_entities{4096};
    std::size_t max_pending_mutations{1024};
};
struct SceneStats {
    std::uint64_t instance;
    std::uint64_t revision;
    std::size_t live_entities;
    std::size_t pending_mutations;
    std::size_t retired_slots;
    bool closed;
};
enum class SceneMutationKind { create, destroy };
using SceneMutationTicket = std::uint64_t;
struct SceneMutationOutcome {
    SceneMutationTicket ticket;
    SceneMutationKind kind;
    Result<EntityHandle> result;
};
struct SceneMutationReport {
    std::uint64_t revision_before;
    std::uint64_t revision_after;
    std::vector<SceneMutationOutcome> outcomes;
};

// A single writer owns the scene on its creating thread. All methods, including
// reads, check that owner. Values may cross threads; registry access may not.
class Scene final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<Scene>> create(SceneScope scope,
                                                               SceneLimits limits = {});
    ~Scene();
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;
    Scene(Scene&&) = delete;
    Scene& operator=(Scene&&) = delete;

    // Immediate writes require no active visit and no pending deferred writes.
    [[nodiscard]] Result<EntityHandle> create_entity(EntitySeed seed);
    [[nodiscard]] Result<void> destroy_entity(EntityHandle handle);
    [[nodiscard]] Result<EntityReference> reference(EntityHandle handle) const;
    [[nodiscard]] Result<EntityHandle> resolve(const EntityReference& reference) const;
    [[nodiscard]] Result<EntityHandle> find(const EntityIdentity& identity) const;
    [[nodiscard]] Result<SceneStats> stats() const;

    [[nodiscard]] Result<SceneMutationTicket> defer_create(EntitySeed seed);
    [[nodiscard]] Result<SceneMutationTicket> defer_destroy(EntityHandle handle);
    // FIFO, per-operation outcomes; this is not an atomic authoring transaction.
    [[nodiscard]] Result<SceneMutationReport> apply_deferred();
    // Stable slot order, owned values. Only deferred writes are allowed during a
    // visit. A callback failure discards writes queued by that visit alone.
    [[nodiscard]] Result<void>
    visit_entities(const std::function<Result<void>(const EntityReference&)>& visitor);
    // Idempotent; cancels pending work and permanently invalidates all handles.
    [[nodiscard]] Result<void> close();

  private:
    struct Impl;
    explicit Scene(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark
