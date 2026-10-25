#pragma once

#include "shark/scene.hpp"

namespace shark::scene_detail {

// Internal Core/Tools publication port. Never expose a candidate, its handles
// or its cursors to an adapter. Only AuthoredScene publishes authoring results.
struct SceneEditAccess {
    [[nodiscard]] static Result<std::unique_ptr<Scene>> prepare(const Scene& scene,
                                                                SceneRevision expected) {
        return scene.prepare_edit(expected);
    }
    [[nodiscard]] static Result<SceneRevision> commit(Scene& scene, Scene& candidate,
                                                      SceneRevision expected) {
        return scene.commit_edit(candidate, expected);
    }
};

} // namespace shark::scene_detail
