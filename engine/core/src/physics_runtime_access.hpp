#pragma once

#include "scene_physics_access.hpp"
#include "shark/physics.hpp"

namespace shark::scene_detail {

// Private coordination between the existing script and physics owners. A lease
// is returned only for this world's current checked simulation Scene.
struct PhysicsRuntimeAccess {
    [[nodiscard]] static Result<PhysicsLease> lease(PhysicsWorld& world, const Scene& scene);
};

} // namespace shark::scene_detail
