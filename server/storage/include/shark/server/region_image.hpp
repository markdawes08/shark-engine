#pragma once

#include "shark/server/regions.hpp"
#include "shark/world_cell.hpp"

#include <filesystem>

namespace shark::server {

inline constexpr std::uint32_t region_process_image_version = 2;
inline constexpr std::uint32_t region_process_image_legacy_version = 1;
// Reserve control-envelope space inside the sixteen-MiB child frame.
inline constexpr std::size_t region_process_image_maximum_bytes = 16 * 1024 * 1024 - 4096;

struct RegionImageEntity {
    EntityReference source;
    Name name;
    Transform transform;
    std::optional<WorldEntityId> parent;
    std::optional<Camera> camera;
    std::optional<MeshRenderer> mesh_renderer;
    std::optional<RigidBody> rigid_body;
};
enum class RegionCellState : std::uint8_t { unloaded, resident, active };
struct RegionCellFileSource {
    std::filesystem::path path;
    ContentDigest digest;
    std::uint64_t bytes;
    bool operator==(const RegionCellFileSource&) const = default;
};
struct RegionImageCell {
    WorldCellKey key;
    std::uint64_t content_revision;
    WorldEntityId proxy;
    std::vector<std::byte> payload;
    RegionCellState state{RegionCellState::active};
    std::optional<RegionCellFileSource> source{};
    PhysicsStaticMeshSettings settings{};
};
struct RegionImageAsset {
    AssetId asset;
    std::vector<std::byte> payload;
};
enum class RegionPursuitStatus : std::uint8_t {
    active,
    stopped,
    blocked,
    awaiting_readiness,
    target_unavailable,
    cancelled
};
struct RegionNpcPursuit {
    std::uint64_t request;
    WorldEntityId target;
    net::CombatTargetId life;
    double speed;
    double stop_distance;
    std::uint64_t committed_steps;
    double committed_distance;
    RegionPursuitStatus status{RegionPursuitStatus::active};
    std::optional<WorldPosition> steering_anchor{};
    bool operator==(const RegionNpcPursuit&) const = default;
};
struct RegionImageNpc {
    EntityReference source;
    PhysicsBodyHandle body;
    std::optional<math::Vec3> pending;
    std::optional<RegionNpcPursuit> pursuit{};
};
struct RegionPeer {
    std::uint64_t connection;
    net::SessionSnapshot session;
    WorldEntityId entity;
};
// Closed process-channel payload. Source refs/handles are relocation keys;
// gateway receipts and persisted writer checks supply execution authority.
struct RegionProcessImage {
    WorldLocalFrame frame;
    std::vector<RegionDefinition> regions;
    std::vector<RegionImageEntity> entities;
    PhysicsTransferState physics;
    MovementCut movement;
    CombatCut combat;
    CombatRulesPortableState rules;
    std::vector<RegionImageCell> cells;
    std::vector<RegionImageAsset> assets;
    std::vector<RegionImageNpc> npcs;
    std::vector<RegionPeer> peers;
    std::vector<storage::RegionWriter> writers;
    std::uint64_t route_watermark;
    RegionInstanceId executor_instance;
    WorldCellResidencyLimits residency_limits{};
};

[[nodiscard]] Result<std::vector<std::byte>>
encode_region_process_image(const RegionProcessImage& image);
[[nodiscard]] Result<RegionProcessImage>
decode_region_process_image(std::span<const std::byte> bytes);

// Pure staged server-frame change. No authority, native write or client rebase
// occurs. Native positions/mesh vertices must retain their exact float values.
[[nodiscard]] Result<RegionProcessImage>
reframe_region_process_image(const RegionProcessImage& image, WorldLocalFrame target);

} // namespace shark::server
