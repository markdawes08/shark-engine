#pragma once

#include "shark/tools/authored_scene.hpp"

namespace shark::tools::detail {

// Private invariant-fixture access for pending work, visits and closed owners.
// Never expose this header or mutable borrow through public authoring adapters.
struct AuthoredSceneTestAccess {
    [[nodiscard]] static Result<std::reference_wrapper<Scene>> scene(AuthoredScene& owner);
};

} // namespace shark::tools::detail
