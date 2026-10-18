#include "graphics_offscreen.hpp"

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <type_traits>

namespace shark::client::detail {
namespace {
Error failure(const char* operation, const char* text) {
    return {ErrorCode::execution_failed, text, {.operation = operation}};
}
template <class Handle> Handle checked(Handle value, OffscreenResources& resources) {
    if (!value) {
        throw failure("graphics.offscreen.create", "NVRHI resource creation returned no handle");
    }
    ++resources.created;
    return value;
}
nvrhi::TextureDesc texture_desc(WindowExtent extent, nvrhi::Format format, const char* name,
                                bool target) {
    return nvrhi::TextureDesc()
        .setWidth(extent.width)
        .setHeight(extent.height)
        .setFormat(format)
        .setIsRenderTarget(target)
        .setDebugName(name)
        .enableAutomaticStateTracking(format == nvrhi::Format::D32
                                          ? nvrhi::ResourceStates::DepthWrite
                                          : nvrhi::ResourceStates::ShaderResource);
}
void read_rows(nvrhi::IDevice& device, nvrhi::IStagingTexture* staging, WindowExtent extent,
               void* destination) {
    std::size_t pitch = 0;
    const auto* mapped = static_cast<const std::byte*>(device.mapStagingTexture(
        staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &pitch));
    const auto unmap = [&device, staging](const std::byte*) {
        device.unmapStagingTexture(staging);
    };
    const std::unique_ptr<const std::byte, decltype(unmap)> guard(mapped, unmap);
    const auto packed_pitch = static_cast<std::size_t>(extent.width) * 4;
    if (!mapped || pitch < packed_pitch ||
        pitch > std::numeric_limits<std::size_t>::max() / extent.height) {
        throw failure("graphics.offscreen.readback", "Invalid staging mapping or row pitch");
    }
    auto* output = static_cast<std::byte*>(destination);
    for (std::uint32_t row = 0; row < extent.height; ++row) {
        std::memcpy(output + row * packed_pitch, mapped + row * pitch, packed_pitch);
    }
}
std::unique_ptr<OffscreenResources> create_resources(nvrhi::IDevice& device,
                                                     const OffscreenRequest& request,
                                                     bool fail_partial, nvrhi::ITexture* target,
                                                     WindowExtent extent, bool capture) {
    static_assert(std::is_standard_layout_v<ResourceVertex> && sizeof(ResourceVertex) == 20);
    static_assert(sizeof(float) == 4);
    auto owner = std::make_unique<OffscreenResources>();
    auto& resources = *owner;
    resources.extent = extent;
    resources.color =
        target ? nvrhi::TextureHandle(target)
               : checked(device.createTexture(texture_desc(extent, nvrhi::Format::RGBA8_UNORM,
                                                           "Shark offscreen color", true)),
                         resources);
    resources.depth = checked(device.createTexture(texture_desc(extent, nvrhi::Format::D32,
                                                                "Shark offscreen depth", true)),
                              resources);
    if (fail_partial) {
        throw failure(target ? "graphics.test.window_draw_partial_creation"
                             : "graphics.test.offscreen_partial_creation",
                      "Injected failure after creating owned render targets");
    }
    const auto shader = [&device, &resources](const ShaderArtifact& artifact,
                                              nvrhi::ShaderType type) {
        nvrhi::ShaderDesc desc;
        desc.shaderType = type;
        desc.entryName = artifact.entry_point;
        desc.debugName = "Shark offline shader";
        return checked(device.createShader(desc, artifact.words.data(),
                                           artifact.words.size() * sizeof(std::uint32_t)),
                       resources);
    };
    resources.vertex_shader = shader(request.vertex_shader, nvrhi::ShaderType::Vertex);
    resources.pixel_shader = shader(request.pixel_shader, nvrhi::ShaderType::Pixel);
    const std::array attributes{
        nvrhi::VertexAttributeDesc()
            .setName("POSITION")
            .setFormat(nvrhi::Format::RGB32_FLOAT)
            .setOffset(static_cast<std::uint32_t>(offsetof(ResourceVertex, position)))
            .setElementStride(sizeof(ResourceVertex)),
        nvrhi::VertexAttributeDesc()
            .setName("TEXCOORD")
            .setFormat(nvrhi::Format::RG32_FLOAT)
            .setOffset(static_cast<std::uint32_t>(offsetof(ResourceVertex, uv)))
            .setElementStride(sizeof(ResourceVertex))};
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
                                .setByteSize(request.vertices.size() * sizeof(ResourceVertex))
                                .setIsVertexBuffer(true)
                                .setDebugName("Shark offscreen vertices")
                                .enableAutomaticStateTracking(nvrhi::ResourceStates::VertexBuffer)),
        resources);
    resources.indices = checked(
        device.createBuffer(nvrhi::BufferDesc()
                                .setByteSize(request.indices.size() * sizeof(std::uint32_t))
                                .setIsIndexBuffer(true)
                                .setDebugName("Shark offscreen indices")
                                .enableAutomaticStateTracking(nvrhi::ResourceStates::IndexBuffer)),
        resources);
    resources.constants =
        checked(device.createBuffer(
                    nvrhi::BufferDesc()
                        .setByteSize(sizeof(request.transform))
                        .setIsConstantBuffer(true)
                        .setDebugName("Shark offscreen transform")
                        .enableAutomaticStateTracking(nvrhi::ResourceStates::ConstantBuffer)),
                resources);
    resources.texture = checked(
        device.createTexture(texture_desc(request.texture_extent, nvrhi::Format::RGBA8_UNORM,
                                          "Shark offscreen texture", false)),
        resources);
    if (capture) {
        resources.color_readback = checked(
            device.createStagingTexture(resources.color->getDesc(), nvrhi::CpuAccessMode::Read),
            resources);
        resources.depth_readback = checked(
            device.createStagingTexture(resources.depth->getDesc(), nvrhi::CpuAccessMode::Read),
            resources);
    }
    resources.sampler =
        checked(device.createSampler(nvrhi::SamplerDesc().setAllFilters(false).setAllAddressModes(
                    nvrhi::SamplerAddressMode::Clamp)),
                resources);
    nvrhi::BindingSetDesc bindings;
    bindings.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, resources.constants))
        .addItem(nvrhi::BindingSetItem::Texture_SRV(0, resources.texture))
        .addItem(nvrhi::BindingSetItem::Sampler(0, resources.sampler));
    resources.bindings =
        checked(device.createBindingSet(bindings, resources.binding_layout), resources);
    resources.framebuffer =
        checked(device.createFramebuffer(nvrhi::FramebufferDesc()
                                             .addColorAttachment(resources.color)
                                             .setDepthAttachment(resources.depth)),
                resources);
    nvrhi::GraphicsPipelineDesc pipeline;
    pipeline.setVertexShader(resources.vertex_shader)
        .setPixelShader(resources.pixel_shader)
        .setInputLayout(resources.input_layout)
        .addBindingLayout(resources.binding_layout);
    pipeline.renderState.depthStencilState.setDepthTestEnable(true)
        .setDepthWriteEnable(true)
        .setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
    pipeline.renderState.rasterState.setFrontCounterClockwise(true).setCullBack().enableDepthClip();
    resources.pipeline = checked(
        device.createGraphicsPipeline(pipeline, resources.framebuffer->getFramebufferInfo()),
        resources);
    if (!target) {
        resources.commands = checked(device.createCommandList(), resources);
    }
    return owner;
}

void record_resources(OffscreenResources& resources, const OffscreenRequest& request,
                      nvrhi::ICommandList* commands, bool upload, bool clear_color) {
    // The backend already opened this command list with ownership retained.
    if (upload) {
        commands->writeBuffer(resources.vertices, request.vertices.data(),
                              request.vertices.size() * sizeof(ResourceVertex));
        commands->writeBuffer(resources.indices, request.indices.data(),
                              request.indices.size() * sizeof(std::uint32_t));
        commands->writeBuffer(resources.constants, request.transform.data(),
                              sizeof(request.transform));
        commands->writeTexture(resources.texture, 0, 0, request.texture_rgba8.data(),
                               static_cast<std::size_t>(request.texture_extent.width) * 4);
    }
    if (clear_color) {
        commands->clearTextureFloat(resources.color, nvrhi::AllSubresources,
                                    nvrhi::Color(0, 0, 0, 1));
    }
    commands->clearDepthStencilTexture(resources.depth, nvrhi::AllSubresources, true, 1.0F, false,
                                       0);
    nvrhi::GraphicsState state;
    state.setPipeline(resources.pipeline)
        .setFramebuffer(resources.framebuffer)
        .addBindingSet(resources.bindings)
        .addVertexBuffer(
            nvrhi::VertexBufferBinding().setBuffer(resources.vertices).setSlot(0).setOffset(0))
        .setIndexBuffer(nvrhi::IndexBufferBinding()
                            .setBuffer(resources.indices)
                            .setFormat(nvrhi::Format::R32_UINT)
                            .setOffset(0));
    // Cancel NVRHI's Vulkan viewport inversion. The shader provides the sole
    // projection Y inversion and native Vulkan receives a positive height.
    state.viewport.addViewport(nvrhi::Viewport(0.0F, static_cast<float>(resources.extent.width),
                                               static_cast<float>(resources.extent.height), 0.0F,
                                               0.0F, 1.0F));
    state.viewport.addScissorRect(nvrhi::Rect(0, static_cast<int>(resources.extent.width), 0,
                                              static_cast<int>(resources.extent.height)));
    commands->setGraphicsState(state);
    commands->drawIndexed(
        nvrhi::DrawArguments().setVertexCount(static_cast<std::uint32_t>(request.indices.size())));
    if (resources.color_readback) {
        commands->copyTexture(resources.color_readback, nvrhi::TextureSlice(), resources.color,
                              nvrhi::TextureSlice());
        commands->copyTexture(resources.depth_readback, nvrhi::TextureSlice(), resources.depth,
                              nvrhi::TextureSlice());
    }
}
} // namespace

std::unique_ptr<OffscreenResources> create_offscreen_resources(nvrhi::IDevice& device,
                                                               const OffscreenRequest& request,
                                                               bool fail_partial) {
    return create_resources(device, request, fail_partial, nullptr, request.extent, true);
}
std::unique_ptr<OffscreenResources> create_window_draw_resources(nvrhi::IDevice& device,
                                                                 const OffscreenRequest& request,
                                                                 nvrhi::ITexture* target,
                                                                 WindowExtent extent, bool capture,
                                                                 bool fail_partial) {
    return create_resources(device, request, fail_partial, target, extent, capture);
}
void record_offscreen_resources(OffscreenResources& resources, const OffscreenRequest& request) {
    record_resources(resources, request, resources.commands, true, true);
    resources.commands->close();
}
void record_window_draw_resources(OffscreenResources& resources, const OffscreenRequest& request,
                                  nvrhi::ICommandList& commands, bool upload) {
    record_resources(resources, request, &commands, upload, false);
}

OffscreenReadback read_offscreen_resources(nvrhi::IDevice& device,
                                           const OffscreenResources& resources) {
    OffscreenReadback result;
    result.extent = resources.extent;
    const auto pixels = static_cast<std::size_t>(resources.extent.width) * resources.extent.height;
    result.rgba8.resize(pixels * 4);
    result.depth.resize(pixels);
    read_rows(device, resources.color_readback, resources.extent, result.rgba8.data());
    read_rows(device, resources.depth_readback, resources.extent, result.depth.data());
    result.submitted = resources.submitted;
    result.completed = resources.submitted;
    result.created_resources = resources.created;
    return result;
}
} // namespace shark::client::detail
