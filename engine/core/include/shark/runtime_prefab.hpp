#pragma once

#include "shark/asset_id.hpp"
#include "shark/prepared_scene.hpp"

#include <memory>

namespace shark {

class ScriptRuntime;
namespace detail {
struct RuntimeSpawnAuthorityState;
}

inline constexpr std::size_t runtime_prefab_maximum_prefabs = 16;
inline constexpr std::size_t runtime_prefab_maximum_entities = 4096;
inline constexpr std::size_t runtime_prefab_maximum_decoded_bytes = 16 * 1024 * 1024;

// Owned immutable-catalog input. Authored IDs and parent links are local to this
// asset and must be remapped before runtime creation. No source parser or
// authoring metadata is required by this Core value.
struct RuntimePrefab {
    AssetId asset;
    AuthoredEntityId root;
    std::vector<PreparedEntity> entities;
    bool operator==(const RuntimePrefab&) const = default;
};

// Validates a complete single-root hierarchy and all current component values,
// including composed transforms, before returning an owned canonical copy.
[[nodiscard]] Result<RuntimePrefab> validate_runtime_prefab(const RuntimePrefab& prefab);

// Explicit trusted-host grant, not proof of authentication. The host must first
// establish actual ownership before creating or supplying this capability.
// Revocation/destruction invalidates all weak copies held by a runtime. The
// creating thread owns access and destruction; no Scene pointer is retained.
class RuntimeSpawnAuthority final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<RuntimeSpawnAuthority>>
    create(const Scene& scene, RegionSceneScope expected);
    ~RuntimeSpawnAuthority();
    RuntimeSpawnAuthority(const RuntimeSpawnAuthority&) = delete;
    RuntimeSpawnAuthority& operator=(const RuntimeSpawnAuthority&) = delete;
    RuntimeSpawnAuthority(RuntimeSpawnAuthority&&) = delete;
    RuntimeSpawnAuthority& operator=(RuntimeSpawnAuthority&&) = delete;

    [[nodiscard]] Result<void> revoke();

  private:
    friend class ScriptRuntime;
    explicit RuntimeSpawnAuthority(std::shared_ptr<detail::RuntimeSpawnAuthorityState> state);
    std::shared_ptr<detail::RuntimeSpawnAuthorityState> state_;
};

} // namespace shark
