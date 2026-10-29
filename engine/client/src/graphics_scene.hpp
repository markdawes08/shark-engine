#pragma once

#include <shark/client/scene_viewport.hpp>

#include <nvrhi/nvrhi.h>

#include <memory>

namespace shark::client::detail {

// Persistent viewport attachments. They are replaced only while no scene frame
// is pending, and destroyed before the existing device owner releases NVRHI.
struct SceneTarget final {
    nvrhi::TextureHandle color;
    nvrhi::TextureHandle depth;
    nvrhi::StagingTextureHandle color_readback;
    nvrhi::StagingTextureHandle depth_readback;
    nvrhi::FramebufferHandle framebuffer;
    WindowExtent extent{};
    std::uint32_t created{0};
};

struct SceneDrawResources final {
    nvrhi::BufferHandle constants;
    nvrhi::BindingSetHandle bindings;
};

// One bounded submission, retained together with its target through any failed
// completion observation. No device, queue, scheduler or mutable Scene is owned.
struct SceneFrameResources final {
    nvrhi::ShaderHandle vertex_shader;
    nvrhi::ShaderHandle pixel_shader;
    nvrhi::InputLayoutHandle input_layout;
    nvrhi::BindingLayoutHandle binding_layout;
    nvrhi::BufferHandle vertices;
    nvrhi::BufferHandle indices;
    std::array<nvrhi::GraphicsPipelineHandle, 3> pipelines;
    std::vector<SceneDrawResources> draws;
    nvrhi::CommandListHandle commands;
    std::uint64_t submitted{0};
    std::uint32_t created{0};
    bool unretired_recording{false};
};

// Creation submits nothing. Install ownership and mark unretired_recording
// before opening the command list; only the backend may prove completion.
[[nodiscard]] std::unique_ptr<SceneTarget> create_scene_target(nvrhi::IDevice&, WindowExtent,
                                                               bool fail_partial);
[[nodiscard]] std::unique_ptr<SceneTarget> create_window_scene_target(nvrhi::IDevice&,
                                                                      nvrhi::ITexture*,
                                                                      WindowExtent, bool capture,
                                                                      bool fail_partial);
[[nodiscard]] std::unique_ptr<SceneFrameResources>
create_scene_frame(nvrhi::IDevice&, const SceneViewportRequest&, const SceneTarget&,
                   bool fail_partial, bool own_commands = true);
void record_scene_frame(SceneFrameResources&, const SceneViewportRequest&, const SceneTarget&);
void record_window_scene_frame(SceneFrameResources&, const SceneViewportRequest&,
                               const SceneTarget&, nvrhi::ICommandList&, bool upload);
[[nodiscard]] OffscreenReadback read_scene_frame(nvrhi::IDevice&, const SceneFrameResources&,
                                                 const SceneTarget&);
[[nodiscard]] OffscreenReadback read_scene_target(nvrhi::IDevice&, const SceneTarget&,
                                                  std::uint64_t submitted, std::uint32_t created);

} // namespace shark::client::detail
