#include "graphics_scene.hpp"

#include <cstddef>
#include <cstring>
#include <limits>
#include <type_traits>

namespace shark::client::detail {
namespace {
Error failure(const char* operation, const char* message) {
    return {ErrorCode::execution_failed, message, {.operation = operation}};
}
template <class Handle, class Resources> Handle checked(Handle handle, Resources& resources) {
    if (!handle)
        throw failure("graphics.scene.create", "NVRHI resource creation returned no handle");
    ++resources.created;
    return handle;
}
nvrhi::TextureDesc target_desc(WindowExtent extent, nvrhi::Format format, const char* name) {
    return nvrhi::TextureDesc()
        .setWidth(extent.width)
        .setHeight(extent.height)
        .setFormat(format)
        .setIsRenderTarget(true)
        .setDebugName(name)
        .enableAutomaticStateTracking(format == nvrhi::Format::D32
                                          ? nvrhi::ResourceStates::DepthWrite
                                          : nvrhi::ResourceStates::RenderTarget);
}
std::size_t pipeline_index(const SceneViewportDraw& draw) noexcept {
    return draw.double_sided ? 2U : draw.mirrored ? 1U : 0U;
}
struct Constants {
    std::array<float, 16> transform;
    std::array<float, 4> base_color;
};
static_assert(std::is_standard_layout_v<Constants> && sizeof(Constants) == 80);
static_assert(offsetof(Constants, base_color) == 64);
static_assert(std::is_standard_layout_v<SceneViewportVertex> && sizeof(SceneViewportVertex) == 28);

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
        pitch > std::numeric_limits<std::size_t>::max() / extent.height)
        throw failure("graphics.scene.readback", "Invalid staging mapping or row pitch");
    auto* output = static_cast<std::byte*>(destination);
    for (std::uint32_t row = 0; row < extent.height; ++row)
        std::memcpy(output + row * packed_pitch, mapped + row * pitch, packed_pitch);
}

std::unique_ptr<SceneTarget> create_target(nvrhi::IDevice& device, WindowExtent extent,
                                           bool fail_partial, nvrhi::ITexture* color,
                                           bool capture) {
    auto target = std::make_unique<SceneTarget>();
    target->extent = extent;
    target->color =
        color ? nvrhi::TextureHandle(color)
              : checked(device.createTexture(target_desc(extent, nvrhi::Format::RGBA8_UNORM,
                                                         "Shark scene viewport color")),
                        *target);
    target->depth = checked(
        device.createTexture(target_desc(extent, nvrhi::Format::D32, "Shark scene viewport depth")),
        *target);
    if (fail_partial)
        throw failure("graphics.test.scene_partial_creation",
                      "Injected failure after creating candidate scene attachments");
    if (capture) {
        target->color_readback = checked(
            device.createStagingTexture(target->color->getDesc(), nvrhi::CpuAccessMode::Read),
            *target);
        target->depth_readback = checked(
            device.createStagingTexture(target->depth->getDesc(), nvrhi::CpuAccessMode::Read),
            *target);
    }
    target->framebuffer = checked(device.createFramebuffer(nvrhi::FramebufferDesc()
                                                               .addColorAttachment(target->color)
                                                               .setDepthAttachment(target->depth)),
                                  *target);
    return target;
}
} // namespace

std::unique_ptr<SceneTarget> create_scene_target(nvrhi::IDevice& device, WindowExtent extent,
                                                 bool fail_partial) {
    return create_target(device, extent, fail_partial, nullptr, true);
}
std::unique_ptr<SceneTarget> create_sampled_scene_target(nvrhi::IDevice& device,
                                                         WindowExtent extent, bool fail_partial) {
    return create_target(device, extent, fail_partial, nullptr, false);
}
std::unique_ptr<SceneTarget> create_window_scene_target(nvrhi::IDevice& device,
                                                        nvrhi::ITexture* color, WindowExtent extent,
                                                        bool capture, bool fail_partial) {
    return create_target(device, extent, fail_partial, color, capture);
}

std::unique_ptr<SceneFrameResources> create_scene_frame(nvrhi::IDevice& device,
                                                        const SceneViewportRequest& request,
                                                        const SceneTarget& target,
                                                        bool fail_partial, bool own_commands) {
    auto frame = std::make_unique<SceneFrameResources>();
    auto& resources = *frame;
    if (own_commands)
        resources.commands = checked(device.createCommandList(), resources);
    if (fail_partial)
        throw failure("graphics.test.scene_partial_creation",
                      "Injected failure while creating a candidate scene frame");
    if (request.draws.empty())
        return frame;

    const auto shader = [&device, &resources](const ShaderArtifact& artifact,
                                              nvrhi::ShaderType type) {
        nvrhi::ShaderDesc desc;
        desc.shaderType = type;
        desc.entryName = artifact.entry_point;
        desc.debugName = "Shark offline scene shader";
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
            .setOffset(static_cast<std::uint32_t>(offsetof(SceneViewportVertex, position)))
            .setElementStride(sizeof(SceneViewportVertex)),
        nvrhi::VertexAttributeDesc()
            .setName("COLOR")
            .setFormat(nvrhi::Format::RGBA32_FLOAT)
            .setOffset(static_cast<std::uint32_t>(offsetof(SceneViewportVertex, color)))
            .setElementStride(sizeof(SceneViewportVertex))};
    resources.input_layout = checked(
        device.createInputLayout(attributes.data(), static_cast<std::uint32_t>(attributes.size()),
                                 resources.vertex_shader),
        resources);
    nvrhi::BindingLayoutDesc layout;
    layout.setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
        .addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    resources.binding_layout = checked(device.createBindingLayout(layout), resources);
    resources.vertices = checked(
        device.createBuffer(nvrhi::BufferDesc()
                                .setByteSize(request.vertices.size() * sizeof(SceneViewportVertex))
                                .setIsVertexBuffer(true)
                                .setDebugName("Shark scene vertices")
                                .enableAutomaticStateTracking(nvrhi::ResourceStates::VertexBuffer)),
        resources);
    resources.indices = checked(
        device.createBuffer(nvrhi::BufferDesc()
                                .setByteSize(request.indices.size() * sizeof(std::uint32_t))
                                .setIsIndexBuffer(true)
                                .setDebugName("Shark scene indices")
                                .enableAutomaticStateTracking(nvrhi::ResourceStates::IndexBuffer)),
        resources);

    resources.draws.reserve(request.draws.size());
    for (const auto& draw : request.draws) {
        const auto index = pipeline_index(draw);
        if (!resources.pipelines[index]) {
            nvrhi::GraphicsPipelineDesc pipeline;
            pipeline.setVertexShader(resources.vertex_shader)
                .setPixelShader(resources.pixel_shader)
                .setInputLayout(resources.input_layout)
                .addBindingLayout(resources.binding_layout);
            pipeline.renderState.depthStencilState.setDepthTestEnable(true)
                .setDepthWriteEnable(true)
                .setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
            auto& raster = pipeline.renderState.rasterState;
            raster.setFrontCounterClockwise(!draw.mirrored).setCullBack().enableDepthClip();
            if (draw.double_sided)
                raster.setCullNone();
            resources.pipelines[index] = checked(
                device.createGraphicsPipeline(pipeline, target.framebuffer->getFramebufferInfo()),
                resources);
        }
        SceneDrawResources value;
        value.constants =
            checked(device.createBuffer(
                        nvrhi::BufferDesc()
                            .setByteSize(sizeof(Constants))
                            .setIsConstantBuffer(true)
                            .setDebugName("Shark scene draw constants")
                            .enableAutomaticStateTracking(nvrhi::ResourceStates::ConstantBuffer)),
                    resources);
        value.bindings = checked(
            device.createBindingSet(nvrhi::BindingSetDesc().addItem(
                                        nvrhi::BindingSetItem::ConstantBuffer(0, value.constants)),
                                    resources.binding_layout),
            resources);
        resources.draws.push_back(std::move(value));
    }
    return frame;
}

namespace {
void record_frame(SceneFrameResources& resources, const SceneViewportRequest& request,
                  const SceneTarget& target, nvrhi::ICommandList& commands, bool upload,
                  bool clear_color) {
    if (upload && !request.draws.empty()) {
        commands.writeBuffer(resources.vertices, request.vertices.data(),
                             request.vertices.size() * sizeof(SceneViewportVertex));
        commands.writeBuffer(resources.indices, request.indices.data(),
                             request.indices.size() * sizeof(std::uint32_t));
        for (std::size_t index = 0; index < request.draws.size(); ++index) {
            const Constants constants{request.draws[index].transform,
                                      request.draws[index].base_color};
            commands.writeBuffer(resources.draws[index].constants, &constants, sizeof(constants));
        }
    }
    if (clear_color)
        commands.clearTextureFloat(target.color, nvrhi::AllSubresources, nvrhi::Color(0, 0, 0, 1));
    commands.clearDepthStencilTexture(target.depth, nvrhi::AllSubresources, true, 1.0F, false, 0);
    for (std::size_t index = 0; index < request.draws.size(); ++index) {
        const auto& draw = request.draws[index];
        nvrhi::GraphicsState state;
        state.setPipeline(resources.pipelines[pipeline_index(draw)])
            .setFramebuffer(target.framebuffer)
            .addBindingSet(resources.draws[index].bindings)
            .addVertexBuffer(
                nvrhi::VertexBufferBinding().setBuffer(resources.vertices).setSlot(0).setOffset(0))
            .setIndexBuffer(nvrhi::IndexBufferBinding()
                                .setBuffer(resources.indices)
                                .setFormat(nvrhi::Format::R32_UINT)
                                .setOffset(0));
        // Cancel NVRHI's native Vulkan Y inversion; the supplied projection
        // already contains the sole Y flip. Native viewport height is positive.
        state.viewport.addViewport(nvrhi::Viewport(0.0F, static_cast<float>(target.extent.width),
                                                   static_cast<float>(target.extent.height), 0.0F,
                                                   0.0F, 1.0F));
        state.viewport.addScissorRect(nvrhi::Rect(0, static_cast<int>(target.extent.width), 0,
                                                  static_cast<int>(target.extent.height)));
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
} // namespace
void record_scene_frame(SceneFrameResources& resources, const SceneViewportRequest& request,
                        const SceneTarget& target) {
    record_frame(resources, request, target, *resources.commands, true, true);
    resources.commands->close();
}
void record_window_scene_frame(SceneFrameResources& resources, const SceneViewportRequest& request,
                               const SceneTarget& target, nvrhi::ICommandList& commands,
                               bool upload) {
    record_frame(resources, request, target, commands, upload, false);
}
void record_sampled_scene_frame(SceneFrameResources& resources, const SceneViewportRequest& request,
                                const SceneTarget& target, nvrhi::ICommandList& commands,
                                bool upload) {
    record_frame(resources, request, target, commands, upload, true);
    // The following GUI draw samples the completed linear color attachment in
    // this same command list. Automatic tracking records the render-to-read edge.
    commands.setTextureState(target.color, nvrhi::AllSubresources,
                             nvrhi::ResourceStates::ShaderResource);
    commands.commitBarriers();
}

OffscreenReadback read_scene_frame(nvrhi::IDevice& device, const SceneFrameResources& frame,
                                   const SceneTarget& target) {
    return read_scene_target(device, target, frame.submitted, frame.created);
}
OffscreenReadback read_scene_target(nvrhi::IDevice& device, const SceneTarget& target,
                                    std::uint64_t submitted, std::uint32_t created) {
    OffscreenReadback result;
    result.extent = target.extent;
    const auto pixels = static_cast<std::size_t>(target.extent.width) * target.extent.height;
    result.rgba8.resize(pixels * 4);
    result.depth.resize(pixels);
    read_rows(device, target.color_readback, target.extent, result.rgba8.data());
    read_rows(device, target.depth_readback, target.extent, result.depth.data());
    result.submitted = submitted;
    result.completed = submitted;
    result.created_resources = created;
    return result;
}
} // namespace shark::client::detail
