#pragma once

#include "scene_physics_access.hpp"
#include "shark/scene.hpp"

#include <cstdint>
#include <memory>

namespace shark::scene_detail {

// Runtime owns the shared token, Scene retains only a weak reference. This
// fences duplicate Script owners without retaining or borrowing Scene lifetime.
using ScriptLease = std::shared_ptr<const std::uint64_t>;
struct SceneScriptAccess {
    static Result<void> bind(Scene& scene, const ScriptLease& lease);
    static Result<void> check(const Scene& scene, const ScriptLease& lease);
    static Result<EntityHandle> create_entity(Scene& scene, const ScriptLease& script,
                                              const PhysicsLease& physics, EntitySeed seed);
    static Result<void> destroy_entity(Scene& scene, const ScriptLease& script,
                                       const PhysicsLease& physics, const EntityReference& entity);
};

} // namespace shark::scene_detail
