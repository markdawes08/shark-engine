#pragma once

#include "shark/stable_id.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <variant>

namespace shark {

using SceneId = TypedId<struct SceneIdDomain>;
using AuthoredEntityId = TypedId<struct AuthoredEntityIdDomain>;
using WorldId = TypedId<struct WorldIdDomain>;
using RegionId = TypedId<struct RegionIdDomain>;
using RegionInstanceId = TypedId<struct RegionInstanceIdDomain>;
using WorldEntityId = TypedId<struct WorldEntityIdDomain>;
using ReplicaSessionId = TypedId<struct ReplicaSessionIdDomain>;

// Scope metadata, never proof of authentication or distributed ownership.
struct AuthorityStamp {
    RegionId region;
    RegionInstanceId instance;
    std::uint64_t epoch;
    auto operator<=>(const AuthorityStamp&) const = default;
};
struct AuthoredSceneScope {
    SceneId scene;
    auto operator<=>(const AuthoredSceneScope&) const = default;
};
struct RegionSceneScope {
    WorldId world;
    AuthorityStamp authority;
    auto operator<=>(const RegionSceneScope&) const = default;
};
struct ReplicaSceneScope {
    WorldId world;
    ReplicaSessionId session;
    auto operator<=>(const ReplicaSceneScope&) const = default;
};
using SceneScope = std::variant<AuthoredSceneScope, RegionSceneScope, ReplicaSceneScope>;
using EntityIdentity = std::variant<AuthoredEntityId, WorldEntityId>;

// Process-local checked value. Never serialize this as a durable or wire ID.
struct EntityHandle {
    std::uint64_t scene_instance{0};
    std::uint32_t slot{std::numeric_limits<std::uint32_t>::max()};
    std::uint64_t generation{0};
    auto operator<=>(const EntityHandle&) const = default;
};
struct EntitySeed {
    EntityIdentity identity;
    // Required only for replicas. Authoritative entities derive this from scope.
    std::optional<AuthorityStamp> replica_authority{};
};
struct EntityReference {
    EntityHandle handle;
    EntityIdentity identity;
    SceneScope scope;
    std::optional<AuthorityStamp> authority;
    auto operator<=>(const EntityReference&) const = default;
};

} // namespace shark
