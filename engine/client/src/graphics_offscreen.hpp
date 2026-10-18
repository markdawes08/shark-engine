#pragma once

#include <shark/client/gpu_resources.hpp>

#include <nvrhi/nvrhi.h>

#include <memory>

namespace shark::client::detail {

// All handles survive recording and submission until the owning Vulkan backend
// proves completion. No device or queue ownership is introduced here.
struct OffscreenResources final {
    nvrhi::ShaderHandle vertex_shader;
    nvrhi::ShaderHandle pixel_shader;
    nvrhi::InputLayoutHandle input_layout;
    nvrhi::BindingLayoutHandle binding_layout;
    nvrhi::BufferHandle vertices;
    nvrhi::BufferHandle indices;
    nvrhi::BufferHandle constants;
    nvrhi::TextureHandle color;
    nvrhi::TextureHandle depth;
    nvrhi::TextureHandle texture;
    nvrhi::StagingTextureHandle color_readback;
    nvrhi::StagingTextureHandle depth_readback;
    nvrhi::SamplerHandle sampler;
    nvrhi::BindingSetHandle bindings;
    nvrhi::FramebufferHandle framebuffer;
    nvrhi::GraphicsPipelineHandle pipeline;
    nvrhi::CommandListHandle commands;
    WindowExtent extent{};
    std::uint64_t submitted{0};
    std::uint32_t created{0};
    bool unretired_recording{false};
};

// Helpers throw owned Error or allocation/native exceptions to the backend's
// guarded synchronous phase. Creation has no GPU submission. Recording requires
// the backend to install ownership and mark unretired_recording first.
[[nodiscard]] std::unique_ptr<OffscreenResources>
create_offscreen_resources(nvrhi::IDevice&, const OffscreenRequest&, bool fail_partial);
void record_offscreen_resources(OffscreenResources&, const OffscreenRequest&);
// Caller must first prove the submission timeline completed. Staging map's
// pinned NVRHI internal wait then observes an already-complete submission.
[[nodiscard]] OffscreenReadback read_offscreen_resources(nvrhi::IDevice&,
                                                         const OffscreenResources&);

} // namespace shark::client::detail
