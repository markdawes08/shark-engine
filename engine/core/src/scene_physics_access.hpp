#pragma once

#include "shark/scene.hpp"

#include <span>

namespace shark::scene_detail {

// Private owner port, never an authoring/adapter capability. The Scene only
// borrows a weak lease, so native teardown never needs a borrowed Scene pointer.
using PhysicsLease = std::shared_ptr<const std::uint64_t>;
struct PhysicsPoseUpdate {
    EntityHandle entity;
    Transform local;
};
struct PhysicsWorldUpdate {
    EntityHandle entity;
    math::Mat4 world;
};
struct PhysicsPoseCandidate {
    SceneRevision revision;
    std::vector<PhysicsPoseUpdate> poses;
    std::vector<PhysicsWorldUpdate> worlds;
    bool changed{false};
};
struct ScenePhysicsAccess {
    static Result<void> bind(Scene& scene, const PhysicsLease& lease);
    static Result<void> check(const Scene& scene, const PhysicsLease& lease);
    static Result<void> unbind(Scene& scene, const PhysicsLease& lease);
    static Result<void> check_detach(const Scene& scene, const PhysicsLease& lease,
                                     const EntityReference& entity);
    static void detach(Scene& scene, EntityHandle entity) noexcept;
    static Result<PhysicsPoseCandidate> prepare(const Scene& scene, const PhysicsLease& lease,
                                                std::span<const PhysicsPoseUpdate> poses);
    static Result<void> commit(Scene& scene, const PhysicsLease& lease,
                               const PhysicsPoseCandidate& candidate);
};

} // namespace shark::scene_detail
