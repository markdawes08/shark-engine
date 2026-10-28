#pragma once

#include "shark/name.hpp"
#include "shark/render_components.hpp"
#include "shark/scene_identity.hpp"
#include "shark/scene_query.hpp"
#include "shark/transform.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace shark {

namespace scene_detail {
struct SceneEditAccess;
}

struct EntitySeed {
    EntityIdentity identity;
    // Required only for replicas. Authoritative entities derive this from scope.
    std::optional<AuthorityStamp> replica_authority{};
    Name name{};
    Transform transform{};
    std::optional<EntityHandle> parent{};
    std::optional<Camera> camera{};
    std::optional<MeshRenderer> mesh_renderer{};
};

struct SceneLimits {
    std::size_t max_entities{4096};
    std::size_t max_pending_mutations{1024};
    // Root depth is one. The hard maximum is 1024; traversal is iterative.
    std::size_t max_hierarchy_depth{256};
};
struct SceneStats {
    std::uint64_t instance;
    std::uint64_t revision;
    std::size_t live_entities;
    std::size_t pending_mutations;
    std::size_t retired_slots;
    bool closed;
};
enum class ReparentMode { keep_local, keep_world };
enum class SceneMutationKind {
    create,
    destroy,
    set_name,
    set_transform,
    reparent,
    set_camera,
    set_mesh_renderer
};

// Owned per-entity copy. World matrix is scene-local, not a global MMO location.
// Name and Transform are mandatory; Camera and MeshRenderer are optional.
struct SceneEntityState {
    EntityReference reference;
    Name name;
    Transform local_transform;
    std::optional<EntityHandle> parent;
    std::vector<EntityHandle> children;
    math::Mat4 world_matrix;
    std::size_t depth;
    std::optional<Camera> camera{};
    std::optional<MeshRenderer> mesh_renderer{};
};
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
    [[nodiscard]] Result<void> set_name(EntityHandle handle, Name name);
    [[nodiscard]] Result<void> set_transform(EntityHandle handle, Transform transform);
    // Null removes the optional component. Exact no-ops preserve the revision.
    [[nodiscard]] Result<void> set_camera(EntityHandle handle, std::optional<Camera> camera);
    [[nodiscard]] Result<void> set_mesh_renderer(EntityHandle handle,
                                                 std::optional<MeshRenderer> renderer);
    [[nodiscard]] Result<void> reparent(EntityHandle handle, std::optional<EntityHandle> parent,
                                        ReparentMode mode);
    [[nodiscard]] Result<SceneEntityState> entity_state(EntityHandle handle) const;
    // Committed-state inspection. Pages are owned, immutable and bounded; reads
    // are allowed during visits and while deferred work is pending.
    [[nodiscard]] Result<SceneQueryPage>
    query_entities(const SceneQueryOptions& options = {}) const;
    [[nodiscard]] Result<SceneQueryPage> continue_query(const SceneQueryCursor& cursor) const;
    [[nodiscard]] Result<SceneQueryPage>
    inspect_entity(EntityHandle handle, SceneQueryFields fields = {},
                   std::optional<SceneRevision> expected_revision = std::nullopt) const;
    [[nodiscard]] Result<EntityReference> reference(EntityHandle handle) const;
    [[nodiscard]] Result<EntityHandle> resolve(const EntityReference& reference) const;
    [[nodiscard]] Result<EntityHandle> find(const EntityIdentity& identity) const;
    [[nodiscard]] Result<SceneStats> stats() const;

    [[nodiscard]] Result<SceneMutationTicket> defer_create(EntitySeed seed);
    [[nodiscard]] Result<SceneMutationTicket> defer_destroy(EntityHandle handle);
    [[nodiscard]] Result<SceneMutationTicket> defer_set_name(EntityHandle handle, Name name);
    [[nodiscard]] Result<SceneMutationTicket> defer_set_transform(EntityHandle handle,
                                                                  Transform transform);
    [[nodiscard]] Result<SceneMutationTicket> defer_set_camera(EntityHandle handle,
                                                               std::optional<Camera> camera);
    [[nodiscard]] Result<SceneMutationTicket>
    defer_set_mesh_renderer(EntityHandle handle, std::optional<MeshRenderer> renderer);
    [[nodiscard]] Result<SceneMutationTicket>
    defer_reparent(EntityHandle handle, std::optional<EntityHandle> parent, ReparentMode mode);
    // FIFO, per-operation outcomes; this is not an atomic authoring transaction.
    [[nodiscard]] Result<SceneMutationReport> apply_deferred();
    // Stable slot order, owned values. Only deferred writes are allowed during a
    // visit. A callback failure discards writes queued by that visit alone.
    [[nodiscard]] Result<void>
    visit_entities(const std::function<Result<void>(const EntityReference&)>& visitor);
    // Idempotent; cancels pending work and permanently invalidates all handles.
    [[nodiscard]] Result<void> close();

  private:
    friend struct scene_detail::SceneEditAccess;
    [[nodiscard]] Result<void> check_edit(SceneRevision expected) const;
    // Authored-scene staging only. The private candidate retains checked handle
    // identity but is not live state. Both operations require an immediate owner
    // boundary. Publication rechecks the base revision and advances it once if
    // any candidate operation changed state; it consumes/closes the candidate.
    [[nodiscard]] Result<std::unique_ptr<Scene>> prepare_edit(SceneRevision expected) const;
    [[nodiscard]] Result<SceneRevision> commit_edit(Scene& candidate, SceneRevision expected,
                                                    bool force_revision);

    struct Impl;
    explicit Scene(std::unique_ptr<Impl> impl) noexcept;
    [[nodiscard]] Result<SceneQueryPage> query_page(const SceneQueryOptions& options,
                                                    std::size_t next_slot) const;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark
