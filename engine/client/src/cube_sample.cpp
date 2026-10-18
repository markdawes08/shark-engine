#include <shark/client/cube_sample.hpp>

#include <algorithm>

namespace shark::client {

Result<OffscreenRequest> make_cube_sample(const ShaderBundle& bundle, WindowExtent extent,
                                          bool alternate) {
    OffscreenRequest request;
    request.vertex_shader = bundle.vertex_shader;
    request.pixel_shader = bundle.pixel_shader;
    request.extent = extent;
    request.texture_extent = {2, 2};
    request.texture_rgba8 = {255, 32, 16,  255, 16,  255, 32, 255,
                             32,  16, 255, 255, 255, 224, 16, 255};
    request.transform = {.56F, .252F,  .12F,  0, 0, .56F, -.15F, 0,
                         .42F, -.336F, -.16F, 0, 0, 0,    .5F,   1};
    constexpr float h = .6F;
    constexpr std::array<std::array<float, 3>, 24> positions{
        {{-h, -h, h},  {h, -h, h},  {h, h, h},    {-h, h, h},  {h, -h, -h}, {-h, -h, -h},
         {-h, h, -h},  {h, h, -h},  {h, -h, h},   {h, -h, -h}, {h, h, -h},  {h, h, h},
         {-h, -h, -h}, {-h, -h, h}, {-h, h, h},   {-h, h, -h}, {-h, h, h},  {h, h, h},
         {h, h, -h},   {-h, h, -h}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h},  {-h, -h, h}}};
    constexpr std::array<std::array<float, 2>, 4> uv{{{0, 1}, {1, 1}, {1, 0}, {0, 0}}};
    for (std::size_t index = 0; index < positions.size(); ++index) {
        request.vertices.push_back({positions[index], uv[index % 4]});
    }
    for (const auto& position : positions) {
        request.vertices.push_back(
            {{position[0] * .65F + .384F, position[1] * .65F - .48F, position[2] * .65F - .512F},
             {.25F, .25F}});
    }
    for (std::uint32_t face = 0; face < 12; ++face) {
        for (const auto index : {0U, 1U, 2U, 0U, 2U, 3U}) {
            request.indices.push_back(face * 4 + index);
        }
    }
    if (alternate) {
        for (std::size_t left = 0, right = request.indices.size() - 3; left < right;
             left += 3, right -= 3) {
            for (std::size_t corner = 0; corner < 3; ++corner) {
                std::swap(request.indices[left + corner], request.indices[right + corner]);
            }
        }
        for (std::size_t channel = 0; channel < 4; ++channel) {
            std::swap(request.texture_rgba8[channel], request.texture_rgba8[12 + channel]);
        }
    }
    if (auto valid = validate_offscreen_request(request); !valid) {
        return valid.error();
    }
    return request;
}

} // namespace shark::client
