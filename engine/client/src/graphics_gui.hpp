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
    std::unique_ptr<SceneTarget> viewport_target;
    std::unique_ptr<SceneFrameResources> viewport_frame;
    nvrhi::BindingSetHandle viewport_bindings;
    std::uint64_t viewport_revision{0};
    bool viewport_uploaded{false};
    std::uint64_t submitted{0};
    std::uint32_t created{0};
};
// Only a completed image slot may adopt this complete candidate. A same-size
// candidate borrows the retained target while preparing; it submits no work.
struct GuiViewportUpdate final {
    std::unique_ptr<SceneTarget> target;
    std::unique_ptr<SceneFrameResources> frame;
    nvrhi::BindingSetHandle bindings;
    std::uint64_t revision{0};
    bool clear{false};
};
[[nodiscard]] std::unique_ptr<GuiResources> create_gui_resources(nvrhi::IDevice&, const GuiAtlas&,
                                                                 const SceneTarget&);
[[nodiscard]] GuiViewportUpdate prepare_gui_viewport(nvrhi::IDevice&, const GuiResources&,
                                                     const std::optional<SceneViewportRequest>&,
                                                     std::uint64_t revision, bool fail_partial);
void adopt_gui_viewport(GuiResources&, GuiViewportUpdate) noexcept;
[[nodiscard]] std::uint32_t gui_resource_count(const GuiResources&) noexcept;
void record_gui_frame(GuiResources&, const GuiAtlas&, const GuiFrame&, const SceneTarget&,
                      nvrhi::ICommandList&, bool upload_atlas);

} // namespace shark::client::detail
