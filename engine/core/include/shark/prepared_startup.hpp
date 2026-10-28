#pragma once

#include "shark/content_digest.hpp"
#include "shark/prepared_scene.hpp"

namespace shark {

inline constexpr std::size_t prepared_startup_maximum_bytes = 64 * 1024;
inline constexpr std::size_t prepared_startup_shader_maximum_bytes = 4 * 1024 * 1024;
inline constexpr std::string_view prepared_startup_target = "windows-x86_64";
inline constexpr std::string_view prepared_startup_filename = "startup.sharkstart";
inline constexpr std::string_view prepared_startup_scene_path = "content/startup.sharkscene";
inline constexpr std::string_view prepared_startup_vertex_path = "assets/scene-vs.spv";
inline constexpr std::string_view prepared_startup_pixel_path = "assets/scene-ps.spv";

struct PreparedFileIdentity {
    std::uint32_t bytes{};
    ContentDigest digest{};
    bool operator==(const PreparedFileIdentity&) const = default;
};
struct PreparedStartupMesh {
    AssetId asset;
    PreparedFileIdentity file;
    bool operator==(const PreparedStartupMesh&) const = default;
};

// Engine-owned development startup inventory. Project is the source project's
// stable identity, not a runtime authority or a Tools owner. Paths are fixed by
// this format; source paths, history and import settings are never serialized.
struct PreparedStartup {
    StableId project;
    WorldId world;
    SceneId scene;
    AuthoredEntityId camera;
    Name name;
    std::string target{prepared_startup_target};
    PreparedFileIdentity scene_file{};
    PreparedFileIdentity vertex_shader{};
    PreparedFileIdentity pixel_shader{};
    // Strictly ascending asset IDs, excluding the two engine built-ins.
    std::vector<PreparedStartupMesh> meshes{};
    bool operator==(const PreparedStartup&) const = default;
};

[[nodiscard]] std::string prepared_startup_mesh_path(AssetId asset);
[[nodiscard]] Result<std::vector<std::byte>> encode_prepared_startup(const PreparedStartup& value);
[[nodiscard]] Result<PreparedStartup> decode_prepared_startup(std::span<const std::byte> payload);

} // namespace shark
