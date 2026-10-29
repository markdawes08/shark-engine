#pragma once

#include "graphics_scene.hpp"
#include <shark/client/gui_draw.hpp>

namespace shark::client::detail {

// One reusable image slot, owned through submitted completion by the existing
// swapchain. CPU frame replacement cannot retire or mutate these GPU owners.
struct GuiResources final {
    nvrhi::ShaderHandle vertex_shader;
    nvrhi::ShaderHandle pixel_shader;
    nvrhi::InputLayoutHandle input_layout;
    nvrhi::BindingLayoutHandle binding_layout;
    nvrhi::BufferHandle vertices;
    nvrhi::BufferHandle indices;
    nvrhi::BufferHandle constants;
    nvrhi::TextureHandle atlas;
    nvrhi::SamplerHandle sampler;
    nvrhi::BindingSetHandle bindings;
    nvrhi::GraphicsPipelineHandle pipeline;
    std::uint64_t submitted{0};
    std::uint32_t created{0};
};
[[nodiscard]] std::unique_ptr<GuiResources> create_gui_resources(nvrhi::IDevice&, const GuiAtlas&,
                                                                 const SceneTarget&);
void record_gui_frame(GuiResources&, const GuiAtlas&, const GuiFrame&, const SceneTarget&,
                      nvrhi::ICommandList&, bool upload_atlas);

} // namespace shark::client::detail
