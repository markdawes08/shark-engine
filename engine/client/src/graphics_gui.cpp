#include "graphics_gui.hpp"

#include <cstddef>
#include <type_traits>

namespace shark::client::detail {
namespace {
template <class Handle> Handle checked(Handle value, GuiResources& resources) {
    if (!value)
        throw Error{ErrorCode::execution_failed,
                    "NVRHI GUI resource creation returned no handle",
                    {.operation = "graphics.gui.create"}};
    ++resources.created;
    return value;
}
} // namespace

std::unique_ptr<GuiResources> create_gui_resources(nvrhi::IDevice& device, const GuiAtlas& atlas,
                                                   const SceneTarget& target) {
    static_assert(std::is_standard_layout_v<GuiVertex> && sizeof(GuiVertex) == 32);
    auto owner = std::make_unique<GuiResources>();
    auto& resources = *owner;
    const auto shader = [&device, &resources](const ShaderArtifact& artifact,
                                              nvrhi::ShaderType type) {
        nvrhi::ShaderDesc desc;
        desc.shaderType = type;
        desc.entryName = artifact.entry_point;
        desc.debugName = "Shark offline GUI shader";
        return checked(device.createShader(desc, artifact.words.data(),
                                           artifact.words.size() * sizeof(std::uint32_t)),
                       resources);
    };
    resources.vertex_shader = shader(atlas.vertex_shader, nvrhi::ShaderType::Vertex);
    resources.pixel_shader = shader(atlas.pixel_shader, nvrhi::ShaderType::Pixel);
    const std::array attributes{
        nvrhi::VertexAttributeDesc()
            .setName("POSITION")
            .setFormat(nvrhi::Format::RG32_FLOAT)
            .setOffset(static_cast<std::uint32_t>(offsetof(GuiVertex, position)))
            .setElementStride(sizeof(GuiVertex)),
        nvrhi::VertexAttributeDesc()
            .setName("TEXCOORD")
            .setFormat(nvrhi::Format::RG32_FLOAT)
            .setOffset(static_cast<std::uint32_t>(offsetof(GuiVertex, uv)))
            .setElementStride(sizeof(GuiVertex)),
        nvrhi::VertexAttributeDesc()
            .setName("COLOR")
            .setFormat(nvrhi::Format::RGBA32_FLOAT)
            .setOffset(static_cast<std::uint32_t>(offsetof(GuiVertex, color)))
            .setElementStride(sizeof(GuiVertex))};
    resources.input_layout = checked(
        device.createInputLayout(attributes.data(), static_cast<std::uint32_t>(attributes.size()),
                                 resources.vertex_shader),
        resources);
    nvrhi::BindingLayoutDesc layout;
    layout.setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
        .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::Sampler(0));
    resources.binding_layout = checked(device.createBindingLayout(layout), resources);
    resources.vertices = checked(
        device.createBuffer(nvrhi::BufferDesc()
                                .setByteSize(gui_max_vertices * sizeof(GuiVertex))
                                .setIsVertexBuffer(true)
                                .setDebugName("Shark GUI vertices")
                                .enableAutomaticStateTracking(nvrhi::ResourceStates::VertexBuffer)),
        resources);
    resources.indices = checked(
        device.createBuffer(nvrhi::BufferDesc()
                                .setByteSize(gui_max_indices * sizeof(std::uint32_t))
                                .setIsIndexBuffer(true)
                                .setDebugName("Shark GUI indices")
                                .enableAutomaticStateTracking(nvrhi::ResourceStates::IndexBuffer)),
        resources);
    resources.constants =
        checked(device.createBuffer(
                    nvrhi::BufferDesc()
                        .setByteSize(4 * sizeof(float))
                        .setIsConstantBuffer(true)
                        .setDebugName("Shark GUI transform")
                        .enableAutomaticStateTracking(nvrhi::ResourceStates::ConstantBuffer)),
                resources);
    resources.atlas = checked(device.createTexture(nvrhi::TextureDesc()
                                                       .setWidth(atlas.extent.width)
                                                       .setHeight(atlas.extent.height)
                                                       .setFormat(nvrhi::Format::RGBA8_UNORM)
                                                       .setDebugName("Shark GUI atlas")
                                                       .enableAutomaticStateTracking(
                                                           nvrhi::ResourceStates::ShaderResource)),
                              resources);
    resources.sampler =
        checked(device.createSampler(nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(
                    nvrhi::SamplerAddressMode::Clamp)),
                resources);
    nvrhi::BindingSetDesc bindings;
    bindings.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, resources.constants))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, resources.atlas))
        .addItem(nvrhi::BindingSetItem::Sampler(0, resources.sampler));
    resources.bindings =
        checked(device.createBindingSet(bindings, resources.binding_layout), resources);
    nvrhi::GraphicsPipelineDesc pipeline;
    pipeline.setVertexShader(resources.vertex_shader)
        .setPixelShader(resources.pixel_shader)
        .setInputLayout(resources.input_layout)
        .addBindingLayout(resources.binding_layout);
    pipeline.renderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false);
    pipeline.renderState.rasterState.setCullNone().enableScissor();
    pipeline.renderState.blendState.targets[0]
        .setBlendEnable(true)
        .setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
        .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
        .setSrcBlendAlpha(nvrhi::BlendFactor::One)
        .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
    resources.pipeline =
        checked(device.createGraphicsPipeline(pipeline, target.framebuffer->getFramebufferInfo()),
                resources);
    return owner;
}

GuiViewportUpdate prepare_gui_viewport(nvrhi::IDevice& device, const GuiResources& resources,
                                       const std::optional<SceneViewportRequest>& request,
                                       std::uint64_t revision, bool fail_partial) {
    GuiViewportUpdate candidate;
    candidate.revision = revision;
    candidate.clear = !request;
    if (!request)
        return candidate;
    if (!resources.viewport_target || resources.viewport_target->extent != request->extent)
        candidate.target = create_sampled_scene_target(device, request->extent, fail_partial);
    const auto& target = candidate.target ? *candidate.target : *resources.viewport_target;
    candidate.frame =
        create_scene_frame(device, *request, target, fail_partial && !candidate.target, false);
    if (candidate.target) {
        nvrhi::BindingSetDesc bindings;
        bindings
            .addItem(nvrhi::BindingSetItem::ConstantBuffer(0, resources.constants))
            // The temporary scene profile is opaque even when source color
            // carries alpha. GUI coverage must not reinterpret scene alpha.
            .addItem(nvrhi::BindingSetItem::Texture_SRV(0, target.color)
                         .setOverrideComponentMapping(
                             nvrhi::ComponentMapping().setA(nvrhi::ComponentSwizzle::One)))
            .addItem(nvrhi::BindingSetItem::Sampler(0, resources.sampler));
        candidate.bindings = device.createBindingSet(bindings, resources.binding_layout);
        if (!candidate.bindings)
            throw Error{ErrorCode::execution_failed,
                        "NVRHI scene viewport binding creation returned no handle",
                        {.operation = "graphics.gui.viewport.create"}};
    } else {
        candidate.bindings = resources.viewport_bindings;
    }
    return candidate;
}

void adopt_gui_viewport(GuiResources& resources, GuiViewportUpdate candidate) noexcept {
    // The caller proved prior image-slot completion before preparation. Move
    // complete owners only after native validation accepted the whole candidate.
    resources.viewport_bindings = std::move(candidate.bindings);
    resources.viewport_frame = std::move(candidate.frame);
    if (candidate.clear)
        resources.viewport_target.reset();
    else if (candidate.target)
        resources.viewport_target = std::move(candidate.target);
    resources.viewport_revision = candidate.revision;
    resources.viewport_uploaded = false;
}

std::uint32_t gui_resource_count(const GuiResources& resources) noexcept {
    return resources.created +
           (resources.viewport_target ? resources.viewport_target->created : 0) +
           (resources.viewport_frame ? resources.viewport_frame->created : 0) +
           (resources.viewport_bindings ? 1U : 0U);
}

void record_gui_frame(GuiResources& resources, const GuiAtlas& atlas, const GuiFrame& frame,
                      const SceneTarget& target, nvrhi::ICommandList& commands, bool upload_atlas) {
    if (frame.viewport)
        record_sampled_scene_frame(*resources.viewport_frame, *frame.viewport,
                                   *resources.viewport_target, commands,
                                   !resources.viewport_uploaded);
    if (upload_atlas)
        commands.writeTexture(resources.atlas, 0, 0, atlas.rgba8.data(),
                              static_cast<std::size_t>(atlas.extent.width) * 4);
    if (!frame.vertices.empty())
        commands.writeBuffer(resources.vertices, frame.vertices.data(),
                             frame.vertices.size() * sizeof(GuiVertex));
    if (!frame.indices.empty())
        commands.writeBuffer(resources.indices, frame.indices.data(),
                             frame.indices.size() * sizeof(std::uint32_t));
    const std::array transform{2.0F / static_cast<float>(frame.extent.width),
                               2.0F / static_cast<float>(frame.extent.height), -1.0F, -1.0F};
    commands.writeBuffer(resources.constants, transform.data(), sizeof(transform));
    commands.clearDepthStencilTexture(target.depth, nvrhi::AllSubresources, true, 1.0F, false, 0);
    nvrhi::GraphicsState state;
    state.setPipeline(resources.pipeline)
        .setFramebuffer(target.framebuffer)
        .addVertexBuffer(
            nvrhi::VertexBufferBinding().setBuffer(resources.vertices).setSlot(0).setOffset(0))
        .setIndexBuffer(nvrhi::IndexBufferBinding()
                            .setBuffer(resources.indices)
                            .setFormat(nvrhi::Format::R32_UINT)
                            .setOffset(0));
    // Positive native Vulkan viewport: GUI positions already use top-left pixels.
    state.viewport.addViewport(nvrhi::Viewport(0.0F, static_cast<float>(frame.extent.width),
                                               static_cast<float>(frame.extent.height), 0.0F, 0.0F,
                                               1.0F));
    for (const auto& draw : frame.draws) {
        state.bindings.resize(0);
        state.addBindingSet(draw.texture == GuiTexture::scene_viewport ? resources.viewport_bindings
                                                                       : resources.bindings);
        state.viewport.scissorRects.resize(0);
        state.viewport.addScissorRect(
            nvrhi::Rect(draw.clip[0], draw.clip[2], draw.clip[1], draw.clip[3]));
        commands.setGraphicsState(state);
        commands.drawIndexed(nvrhi::DrawArguments()
                                 .setVertexCount(draw.index_count)
                                 .setStartIndexLocation(draw.first_index));
    }
    if (target.color_readback) {
        commands.copyTexture(target.color_readback, nvrhi::TextureSlice(), target.color,
                             nvrhi::TextureSlice());
        commands.copyTexture(target.depth_readback, nvrhi::TextureSlice(), target.depth,
                             nvrhi::TextureSlice());
    }
}
} // namespace shark::client::detail
