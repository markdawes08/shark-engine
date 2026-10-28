#pragma once

#include "shark/name.hpp"
#include "shark/render_components.hpp"
#include "shark/scene_identity.hpp"
#include "shark/transform.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace shark {

inline constexpr std::size_t scene_query_maximum_page_size = 256;
inline constexpr std::size_t scene_query_maximum_scan_budget = 4096;

struct SceneRevision {
    std::uint64_t scene_instance{0};
    std::uint64_t revision{0};
    bool operator==(const SceneRevision&) const = default;
};

enum class SceneQuerySelection { all, roots, children };

// Identity/scope/authority are always returned. All false is identity-only.
struct SceneQueryFields {
    bool name{true};
    bool transform{true};
    bool hierarchy{true};
    // Opt in explicitly; older {false,false,false} callers remain identity-only.
    bool camera{false};
    bool mesh_renderer{false};
    bool operator==(const SceneQueryFields&) const = default;
};

struct SceneQueryOptions {
    SceneQuerySelection selection{SceneQuerySelection::all};
    // Required only for the direct-children selection.
    std::optional<EntityHandle> parent{};
    // Exact byte prefix, valid UTF-8 without controls, at most 256 bytes.
    // Empty matches every Name. No case folding or normalization.
    std::string name_prefix{};
    SceneQueryFields fields{};
    std::size_t page_size{64};
    std::size_t scan_budget{256};
    std::optional<SceneRevision> expected_revision{};
};

struct SceneInspectedTransform {
    Transform local;
    math::Mat4 world_matrix;
};

struct SceneInspectedHierarchy {
    std::optional<EntityHandle> parent;
    std::size_t child_count;
    std::size_t depth;
};

struct SceneInspectedEntity {
    EntityReference reference;
    std::optional<Name> name;
    std::optional<SceneInspectedTransform> transform;
    std::optional<SceneInspectedHierarchy> hierarchy;
    std::optional<Camera> camera{};
    std::optional<MeshRenderer> mesh_renderer{};
};

class Scene;

// Opaque process-local value. No default construction, serialized representation,
// Scene pointer, registry handle or retained full-scene snapshot.
class SceneQueryCursor final {
  public:
    // Copy-only, including rvalue use: moving must not leave a reusable token
    // with an emptied filter but the original revision/position.
    SceneQueryCursor(const SceneQueryCursor&) = default;
    SceneQueryCursor& operator=(const SceneQueryCursor&) = default;
    [[nodiscard]] SceneRevision revision() const noexcept {
        return revision_;
    }

  private:
    friend class Scene;
    SceneQueryCursor(SceneRevision revision, SceneQueryOptions options, std::size_t next_slot)
        : revision_(revision), options_(std::move(options)), next_slot_(next_slot) {}
    SceneRevision revision_;
    SceneQueryOptions options_;
    std::size_t next_slot_;
};

// Owns its values independently of the Scene. Const views last only as long as
// this page; copy the page/value for longer or synchronized cross-thread use.
class SceneQueryPage final {
  public:
    [[nodiscard]] SceneRevision revision() const noexcept {
        return revision_;
    }
    [[nodiscard]] const SceneScope& scope() const& noexcept {
        return scope_;
    }
    const SceneScope& scope() const&& = delete;
    [[nodiscard]] std::span<const SceneInspectedEntity> entities() const& noexcept {
        return entities_;
    }
    std::span<const SceneInspectedEntity> entities() const&& = delete;
    [[nodiscard]] std::size_t scanned_slots() const noexcept {
        return scanned_slots_;
    }
    [[nodiscard]] const std::optional<SceneQueryCursor>& next_cursor() const& noexcept {
        return next_cursor_;
    }
    const std::optional<SceneQueryCursor>& next_cursor() const&& = delete;

  private:
    friend class Scene;
    SceneQueryPage(SceneRevision revision, SceneScope scope,
                   std::vector<SceneInspectedEntity> entities, std::size_t scanned_slots,
                   std::optional<SceneQueryCursor> next_cursor)
        : revision_(revision), scope_(std::move(scope)), entities_(std::move(entities)),
          scanned_slots_(scanned_slots), next_cursor_(std::move(next_cursor)) {}
    SceneRevision revision_;
    SceneScope scope_;
    std::vector<SceneInspectedEntity> entities_;
    std::size_t scanned_slots_;
    std::optional<SceneQueryCursor> next_cursor_;
};

} // namespace shark
