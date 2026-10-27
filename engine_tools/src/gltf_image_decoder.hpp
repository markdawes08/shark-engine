#pragma once

#include "shark/tools/gltf_textures.hpp"

namespace shark::tools::detail {

struct GltfImageInfo {
    std::uint32_t width{};
    std::uint32_t height{};
    unsigned component_bits{8};
    std::size_t decoded_bytes{};
    std::size_t work_items{};
    std::size_t png_raw_bytes{};
    bool operator==(const GltfImageInfo&) const = default;
};
struct GltfDecodedImage {
    GltfImageInfo info;
    std::vector<std::byte> rgba;
};
struct GltfImageDecodeBudget {
    std::size_t cumulative_bytes{};
};

[[nodiscard]] Result<GltfImageInfo> inspect_gltf_image(std::span<const std::byte> bytes,
                                                       std::string_view mime, std::stop_token stop);
[[nodiscard]] Result<GltfDecodedImage> decode_gltf_image(std::span<const std::byte> bytes,
                                                         std::string_view mime,
                                                         const GltfImageInfo& expected,
                                                         GltfImageDecodeBudget& budget,
                                                         std::stop_token stop);

} // namespace shark::tools::detail
