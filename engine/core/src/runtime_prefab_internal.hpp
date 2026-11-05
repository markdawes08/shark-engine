#pragma once

#include "shark/runtime_prefab.hpp"

#include <thread>

namespace shark::detail {

struct RuntimeSpawnAuthorityState {
    std::uint64_t scene_instance;
    RegionSceneScope scope;
    std::thread::id owner;
    bool active{true};
};

[[nodiscard]] Result<void>
check_runtime_spawn_authority(const std::weak_ptr<RuntimeSpawnAuthorityState>& authority,
                              const Scene& scene, const RegionSceneScope& expected,
                              const char* operation);

} // namespace shark::detail
