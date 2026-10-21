#pragma once

#include "shark/scene.hpp"
#include "shark/tools/source_document.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace shark::tools {

enum class UnsavedScenePolicy { reject, discard };

struct AuthoredSceneStatus {
    SceneRevision revision;
    // Relative to the last successful in-memory activation, not a disk save.
    bool dirty;
    std::size_t pending_mutations;
};

struct CapturedScene {
    SceneRevision revision;
    ProjectDocument project;
    SceneDocument scene;
    bool dirty;
};

// Owns one authored Scene on its creating thread. Project settings and source-only
// metadata are immutable between activations. This is not a filesystem/project
// lock, undo history, live server region or terminal protocol.
class AuthoredScene final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<AuthoredScene>>
    create(const ProjectDocument& project, const SceneDocument& scene, SceneLimits limits = {});
    ~AuthoredScene();
    AuthoredScene(const AuthoredScene&) = delete;
    AuthoredScene& operator=(const AuthoredScene&) = delete;
    AuthoredScene(AuthoredScene&&) = delete;
    AuthoredScene& operator=(AuthoredScene&&) = delete;

    [[nodiscard]] Result<AuthoredSceneStatus> status() const;
    // A local borrow, valid only until successful activation or owner destruction.
    // Do not retain across either boundary; retain checked handles/revisions instead.
    [[nodiscard]] Result<std::reference_wrapper<Scene>> scene();
    [[nodiscard]] Result<std::reference_wrapper<const Scene>> scene() const;
    // Complete owned authored data at one committed revision. Pending deferred
    // work rejects; capture does not clear dirty state or acknowledge a file save.
    [[nodiscard]] Result<CapturedScene>
    capture(std::optional<SceneRevision> expected_revision = std::nullopt) const;
    // Resolve within the immutable project catalog and prepare a complete private
    // candidate before closing/replacing the old Scene. Pending work, visits,
    // stale revisions and (by default) unsaved edits reject without replacement.
    // Success always has a fresh instance, including an identical-source reload.
    [[nodiscard]] Result<SceneRevision>
    activate(const SceneDocument& candidate, SceneRevision expected_revision,
             UnsavedScenePolicy unsaved = UnsavedScenePolicy::reject);

  private:
    struct Impl;
    explicit AuthoredScene(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::tools
