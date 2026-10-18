#include "cases.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace shark::probe {
namespace {

using Object = Json::Object;
using Array = Json::Array;
using Bytes = std::vector<std::byte>;
constexpr std::uint32_t imageSize = 64;
constexpr std::uint32_t computeSize = 16;
constexpr std::uint32_t depthSize = 2048;
constexpr std::uint32_t depthLayers = 12;
constexpr double unormTolerance = 1.0 / 255.0;
constexpr double hdrTolerance = 0.002;
constexpr double depthTolerance = 0.00001;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Handle> Handle checked(Handle handle, const char* name) {
    require(static_cast<bool>(handle), std::string("NVRHI creation failed: ") + name);
    return handle;
}

Bytes loadShader(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    require(static_cast<bool>(file), "Cannot open shader: " + path.string());
    const auto length = file.tellg();
    require(length >= 20 && length <= 1024 * 1024 && length % 4 == 0,
            "Shader size is invalid: " + path.string());
    Bytes result(static_cast<std::size_t>(length));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    require(static_cast<bool>(file), "Cannot read complete shader: " + path.string());
    std::uint32_t magic = 0;
    std::memcpy(&magic, result.data(), sizeof(magic));
    require(magic == 0x07230203U, "Shader is not little-endian SPIR-V: " + path.string());
    return result;
}

struct Comparison final {
    std::string name;
    double absoluteTolerance;
    double relativeTolerance;
    std::uint64_t compared = 0;
    std::uint64_t failed = 0;
    double maxError = 0;
    double maxRatio = 0;

    void add(double actual, double expected) {
        ++compared;
        if (!std::isfinite(actual) || !std::isfinite(expected)) {
            ++failed;
            // JSON does not encode nonfinite metrics. The failure is explicit.
            maxError = std::numeric_limits<double>::max();
            maxRatio = std::numeric_limits<double>::max();
            return;
        }
        const auto error = std::abs(actual - expected);
        const auto tolerance = std::max(absoluteTolerance, relativeTolerance * std::abs(expected));
        const auto ratio = tolerance == 0.0 ? (error == 0.0 ? 0.0 : 2.0) : error / tolerance;
        maxError = std::max(maxError, error);
        maxRatio = std::max(maxRatio, ratio);
        if (error > tolerance) {
            ++failed;
        }
    }

    [[nodiscard]] Json report() const {
        return Object{{"name", name},
                      {"status", failed == 0 && compared > 0 ? "PASS" : "FAIL"},
                      {"compared_values", compared},
                      {"failed_values", failed},
                      {"max_absolute_error", maxError},
                      {"max_tolerance_ratio", maxRatio},
                      {"absolute_tolerance", absoluteTolerance},
                      {"relative_tolerance", relativeTolerance}};
    }
};

float halfToFloat(std::uint16_t bits) {
    const auto sign = (static_cast<std::uint32_t>(bits) & 0x8000U) << 16U;
    const auto exponent = (static_cast<std::uint32_t>(bits) >> 10U) & 31U;
    const auto fraction = static_cast<std::uint32_t>(bits) & 1023U;
    if (exponent == 0) {
        const auto magnitude = std::ldexp(static_cast<float>(fraction), -24);
        return sign != 0 ? -magnitude : magnitude;
    }
    const auto exponent32 = exponent == 31U ? 255U : exponent + 112U;
    return std::bit_cast<float>(sign | (exponent32 << 23U) | (fraction << 13U));
}

// Cube fixture values are exact nonnegative integers 1..15, so no rounding
// policy is involved in producing the half-float upload reference.
std::uint16_t integerHalf(std::uint32_t value) {
    require(value > 0 && value <= 15, "Invalid analytic cube fixture value");
    const auto bits = std::bit_cast<std::uint32_t>(static_cast<float>(value));
    return static_cast<std::uint16_t>(((bits >> 13U) & 0x3ffU) |
                                      ((((bits >> 23U) & 0xffU) - 112U) << 10U));
}

float readFloat(const Bytes& bytes, std::size_t index) {
    float value = 0;
    std::memcpy(&value, bytes.data() + index * sizeof(value), sizeof(value));
    return value;
}

float readHalf(const Bytes& bytes, std::size_t index) {
    std::uint16_t value = 0;
    std::memcpy(&value, bytes.data() + index * sizeof(value), sizeof(value));
    return halfToFloat(value);
}

double srgbDecode(std::uint8_t value) {
    const double encoded = static_cast<double>(value) / 255.0;
    return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
}

struct Globals final {
    std::array<float, 4> transform{1.0F, 1.0F, 0.5F, 0.0F};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<std::uint32_t, 4> control{};
};
static_assert(sizeof(Globals) == 48);

class Fixture final {
  public:
    Fixture(nvrhi::IDevice* device, const std::filesystem::path& shaders,
            std::filesystem::path artifacts)
        : device_(device), artifacts_(std::move(artifacts)) {
        require(device_ != nullptr, "GPU cases require an NVRHI device");
        require(std::endian::native == std::endian::little,
                "Probe artifacts require little endian");
        require(!std::filesystem::exists(artifacts_) ||
                    (std::filesystem::is_directory(artifacts_) &&
                     !std::filesystem::is_symlink(artifacts_) &&
                     std::filesystem::is_empty(artifacts_)),
                "GPU artifact directory must be new or empty: " + artifacts_.string());
        std::filesystem::create_directories(artifacts_);
        auto shader = [&](const char* file, nvrhi::ShaderType type, const char* entry) {
            const auto bytes = loadShader(shaders / file);
            nvrhi::ShaderDesc desc;
            desc.shaderType = type;
            desc.entryName = entry;
            desc.debugName = file;
            return checked(device_->createShader(desc, bytes.data(), bytes.size()), file);
        };
        vertex_ = shader("fixture_vs.spv", nvrhi::ShaderType::Vertex, "vsMain");
        fragment_ = shader("fixture_ps.spv", nvrhi::ShaderType::Pixel, "psMain");
        sample_ = shader("sample_ps.spv", nvrhi::ShaderType::Pixel, "sampleMain");
        compute_ = shader("compute_cs.spv", nvrhi::ShaderType::Compute, "csMain");
        commands_ = checked(device_->createCommandList(), "command list");
        constants_ = checked(device_->createBuffer(nvrhi::BufferDesc()
                                                       .setByteSize(sizeof(Globals))
                                                       .setIsConstantBuffer(true)
                                                       .setIsVolatile(true)
                                                       .setMaxVersions(128)
                                                       .setDebugName("02b.constants")),
                             "volatile constant buffer");
        sampler_ = checked(device_->createSampler(nvrhi::SamplerDesc().setAllFilters(true)),
                           "linear sampler");
        nearest_ = checked(device_->createSampler(nvrhi::SamplerDesc().setAllFilters(false)),
                           "nearest sampler");
        nvrhi::BindingLayoutDesc layout;
        layout.setVisibility(nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel)
            .addItem(nvrhi::BindingLayoutItem::VolatileConstantBuffer(0))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(0))
            .addItem(nvrhi::BindingLayoutItem::Sampler(0));
        drawLayout_ = checked(device_->createBindingLayout(layout), "draw binding layout");
        layout.addItem(nvrhi::BindingLayoutItem::Texture_SRV(1))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(2))
            .addItem(nvrhi::BindingLayoutItem::Texture_SRV(3));
        sampleLayout_ = checked(device_->createBindingLayout(layout), "sampling binding layout");
        indices16_ = indexBuffer(6U * sizeof(std::uint16_t), "02b.indices16");
        indices32_ = indexBuffer(6U * sizeof(std::uint32_t), "02b.indices32");
        reverse32_ = indexBuffer(6U * sizeof(std::uint32_t), "02b.reverse32");
        constexpr std::array<std::uint16_t, 6> values16{0, 2, 1, 1, 2, 3};
        constexpr std::array<std::uint32_t, 6> values32{0, 2, 1, 1, 2, 3};
        constexpr std::array<std::uint32_t, 6> reverse{0, 1, 2, 1, 3, 2};
        commands_->open();
        commands_->writeBuffer(indices16_, values16.data(), sizeof(values16));
        commands_->writeBuffer(indices32_, values32.data(), sizeof(values32));
        commands_->writeBuffer(reverse32_, reverse.data(), sizeof(reverse));
        submit();
    }

    ~Fixture() {
        // Every successful submission is waited before local resources leave
        // scope. On exceptions the owning process still bounds this teardown.
        device_->waitForIdle();
        device_->runGarbageCollection();
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    nvrhi::TextureHandle
    texture(nvrhi::Format format, std::uint32_t width, std::uint32_t height, const char* name,
            bool render = false, bool storage = false, std::uint32_t layers = 1,
            std::uint32_t mips = 1,
            nvrhi::TextureDimension dimension = nvrhi::TextureDimension::Texture2D) {
        auto desc = nvrhi::TextureDesc()
                        .setFormat(format)
                        .setWidth(width)
                        .setHeight(height)
                        .setArraySize(layers)
                        .setMipLevels(mips)
                        .setDimension(dimension)
                        .setIsRenderTarget(render)
                        .setIsUAV(storage)
                        .setDebugName(name)
                        .enableAutomaticStateTracking(format == nvrhi::Format::D32
                                                          ? nvrhi::ResourceStates::ShaderResource |
                                                                nvrhi::ResourceStates::DepthRead
                                                          : nvrhi::ResourceStates::ShaderResource);
        return checked(device_->createTexture(desc), name);
    }

    nvrhi::FramebufferHandle framebuffer(nvrhi::ITexture* color, nvrhi::ITexture* depth = nullptr,
                                         std::uint32_t layer = 0) {
        nvrhi::FramebufferDesc desc;
        if (color != nullptr) {
            desc.addColorAttachment(color);
        }
        if (depth != nullptr) {
            desc.setDepthAttachment(
                nvrhi::FramebufferAttachment().setTexture(depth).setArraySlice(layer));
        }
        return checked(device_->createFramebuffer(desc), "framebuffer");
    }

    nvrhi::GraphicsPipelineHandle pipeline(nvrhi::IFramebuffer* framebuffer, bool depth,
                                           bool blending = false, bool sampling = false) {
        nvrhi::GraphicsPipelineDesc desc;
        desc.setVertexShader(vertex_)
            .setPixelShader(framebuffer->getDesc().colorAttachments.empty()
                                ? nullptr
                                : (sampling ? sample_.Get() : fragment_.Get()))
            .addBindingLayout(sampling ? sampleLayout_.Get() : drawLayout_.Get());
        desc.renderState.depthStencilState.setDepthTestEnable(depth)
            .setDepthWriteEnable(depth)
            .setDepthFunc(nvrhi::ComparisonFunc::LessOrEqual);
        desc.renderState.rasterState.setFrontCounterClockwise(true).setCullBack().enableDepthClip();
        if (blending) {
            desc.renderState.blendState.targets[0]
                .enableBlend()
                .setSrcBlend(nvrhi::BlendFactor::One)
                .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
                .setSrcBlendAlpha(nvrhi::BlendFactor::One)
                .setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
        }
        return checked(device_->createGraphicsPipeline(desc, framebuffer->getFramebufferInfo()),
                       "graphics pipeline");
    }

    nvrhi::BindingSetHandle bindings(nvrhi::ITexture* input, bool linear = true,
                                     nvrhi::ITexture* srgb = nullptr,
                                     nvrhi::ITexture* cube = nullptr,
                                     nvrhi::ITexture* depth = nullptr) {
        const bool sampling = srgb != nullptr;
        nvrhi::BindingSetDesc desc;
        desc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, constants_))
            .addItem(nvrhi::BindingSetItem::Texture_SRV(0, input))
            .addItem(nvrhi::BindingSetItem::Sampler(0, linear ? sampler_.Get() : nearest_.Get()));
        if (sampling) {
            desc.addItem(nvrhi::BindingSetItem::Texture_SRV(1, srgb))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(2, cube))
                .addItem(nvrhi::BindingSetItem::Texture_SRV(3, depth));
        }
        return checked(
            device_->createBindingSet(desc, sampling ? sampleLayout_.Get() : drawLayout_.Get()),
            "graphics binding set");
    }

    void draw(nvrhi::IFramebuffer* framebuffer, nvrhi::IGraphicsPipeline* pipeline,
              nvrhi::IBindingSet* bindings, const Globals& globals, bool index32 = false,
              bool reverse = false) {
        const auto& info = framebuffer->getFramebufferInfo();
        commands_->writeBuffer(constants_, &globals, sizeof(globals));
        nvrhi::GraphicsState state;
        state.setPipeline(pipeline).setFramebuffer(framebuffer).addBindingSet(bindings);
        // NVRHI's Vulkan adapter normally negates viewport height for D3D
        // coordinates. These explicit bounds cancel that backend inversion,
        // giving VkViewport{x=0,y=0,width=width,height=height}. The shader applies
        // the ADR's sole projection Y negate; scissor remains ordinary top-left.
        state.viewport.addViewport(nvrhi::Viewport(0.0F, static_cast<float>(info.width),
                                                   static_cast<float>(info.height), 0.0F, 0.0F,
                                                   1.0F));
        state.viewport.addScissorRect(
            nvrhi::Rect(0, static_cast<int>(info.width), 0, static_cast<int>(info.height)));
        state.setIndexBuffer(
            nvrhi::IndexBufferBinding()
                .setBuffer(reverse ? reverse32_.Get()
                                   : (index32 ? indices32_.Get() : indices16_.Get()))
                .setFormat(index32 || reverse ? nvrhi::Format::R32_UINT : nvrhi::Format::R16_UINT)
                .setOffset(0));
        commands_->setGraphicsState(state);
        commands_->drawIndexed(nvrhi::DrawArguments().setVertexCount(6));
    }

    void submit() {
        commands_->close();
        device_->executeCommandList(commands_);
        require(device_->waitForIdle(), "GPU submission did not retire successfully");
        device_->runGarbageCollection();
    }

    Bytes readTexture(nvrhi::ITexture* source, std::uint32_t pixelBytes, std::uint32_t layer = 0,
                      std::uint32_t extent = 0, std::uint32_t sourceOrigin = 0) {
        auto desc = source->getDesc();
        desc.width = extent == 0 ? desc.width : extent;
        desc.height = extent == 0 ? desc.height : extent;
        desc.arraySize = 1;
        desc.mipLevels = 1;
        desc.dimension = nvrhi::TextureDimension::Texture2D;
        auto staging = checked(device_->createStagingTexture(desc, nvrhi::CpuAccessMode::Read),
                               "readback staging texture");
        commands_->open();
        commands_->copyTexture(staging, nvrhi::TextureSlice(), source,
                               nvrhi::TextureSlice()
                                   .setArraySlice(layer)
                                   .setOrigin(sourceOrigin, sourceOrigin)
                                   .setSize(desc.width, desc.height));
        submit();
        std::size_t rowPitch = 0;
        const auto* mapped = static_cast<const std::byte*>(device_->mapStagingTexture(
            staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
        const auto unmap = [this, &staging](const std::byte*) {
            device_->unmapStagingTexture(staging);
        };
        const std::unique_ptr<const std::byte, decltype(unmap)> mappedGuard(mapped, unmap);
        require(mapped != nullptr && rowPitch >= static_cast<std::size_t>(desc.width) * pixelBytes,
                "GPU readback mapping failed or returned an invalid row pitch");
        Bytes bytes(static_cast<std::size_t>(desc.width) * desc.height * pixelBytes);
        for (std::uint32_t row = 0; row < desc.height; ++row) {
            const auto packedPitch = static_cast<std::size_t>(desc.width) * pixelBytes;
            std::memcpy(bytes.data() + row * packedPitch, mapped + row * rowPitch, packedPitch);
        }
        return bytes;
    }

    Json save(const char* filename, const Bytes& bytes, const char* format, std::uint32_t width,
              std::uint32_t height, std::uint32_t layers = 1) const {
        const auto path = artifacts_ / filename;
        require(!std::filesystem::exists(path),
                "Refusing to replace GPU artifact: " + path.string());
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();
        require(static_cast<bool>(output), "Cannot save GPU artifact: " + path.string());
        return Object{{"path", filename}, {"format", format},
                      {"width", width},   {"height", height},
                      {"layers", layers}, {"bytes", static_cast<std::uint64_t>(bytes.size())}};
    }

    nvrhi::IDevice* device_;
    nvrhi::CommandListHandle commands_;
    nvrhi::ShaderHandle compute_;

  private:
    nvrhi::BufferHandle indexBuffer(std::uint64_t size, const char* name) const {
        return checked(device_->createBuffer(
                           nvrhi::BufferDesc()
                               .setByteSize(size)
                               .setIsIndexBuffer(true)
                               .setDebugName(name)
                               .enableAutomaticStateTracking(nvrhi::ResourceStates::IndexBuffer)),
                       name);
    }

    std::filesystem::path artifacts_;
    nvrhi::ShaderHandle vertex_;
    nvrhi::ShaderHandle fragment_;
    nvrhi::ShaderHandle sample_;
    nvrhi::BufferHandle constants_;
    nvrhi::SamplerHandle sampler_;
    nvrhi::SamplerHandle nearest_;
    nvrhi::BindingLayoutHandle drawLayout_;
    nvrhi::BindingLayoutHandle sampleLayout_;
    nvrhi::BufferHandle indices16_;
    nvrhi::BufferHandle indices32_;
    nvrhi::BufferHandle reverse32_;
};

Object result(const char* id, const std::vector<Comparison>& comparisons, Array artifacts,
              Object details, bool& allPassed) {
    Array checks;
    bool passed = true;
    for (const auto& comparison : comparisons) {
        checks.emplace_back(comparison.report());
        passed = passed && comparison.compared > 0 && comparison.failed == 0;
    }
    allPassed = allPassed && passed;
    return Object{{"id", id},
                  {"status", passed ? "PASS" : "FAIL"},
                  {"checks", std::move(checks)},
                  {"artifacts", std::move(artifacts)},
                  {"details", std::move(details)}};
}

constexpr std::array<std::uint8_t, 16> texturePixels{128, 0, 0,   255, 0,   128, 0,   255,
                                                     0,   0, 128, 255, 128, 128, 128, 255};

Object graphicsCase(Fixture& fixture, nvrhi::ITexture* srgb, bool injectMismatch, bool& passed) {
    auto color =
        fixture.texture(nvrhi::Format::RGBA8_UNORM, imageSize, imageSize, "02b.color", true);
    auto depth = fixture.texture(nvrhi::Format::D32, imageSize, imageSize, "02b.depth", true);
    auto framebuffer = fixture.framebuffer(color, depth);
    auto pipeline = fixture.pipeline(framebuffer, true);
    auto bindings = fixture.bindings(srgb, false);
    fixture.commands_->open();
    fixture.commands_->clearTextureFloat(color, nvrhi::AllSubresources, nvrhi::Color(0.0F));
    fixture.commands_->clearDepthStencilTexture(depth, nvrhi::AllSubresources, true, 1.0F, false,
                                                0);
    Globals globals;
    globals.transform[2] = 0.75F;
    fixture.draw(framebuffer, pipeline, bindings, globals);
    globals.transform = {0.5F, 0.5F, 0.25F, 1.0F};
    globals.color = {0.0F, 1.0F, 0.0F, 1.0F};
    fixture.draw(framebuffer, pipeline, bindings, globals, true);
    globals.transform[2] = 0.9F;
    globals.color = {1.0F, 0.0F, 0.0F, 1.0F};
    fixture.draw(framebuffer, pipeline, bindings, globals);
    globals.transform = {1.0F, 1.0F, 0.1F, 1.0F};
    globals.color = {0.0F, 0.0F, 1.0F, 1.0F};
    fixture.draw(framebuffer, pipeline, bindings, globals, true, true);
    fixture.submit();
    const auto colorBytes = fixture.readTexture(color, 4);
    const auto depthBytes = fixture.readTexture(depth, 4);
    Comparison colorCheck{"textured_depth_winding_uv", unormTolerance, 0.0};
    Comparison depthCheck{"conventional_depth_readback", depthTolerance, 0.0};
    // Exclude exactly one pixel at primitive and quadrant boundaries. Every
    // other pixel has an analytic reference independent of raster edge rules.
    for (std::uint32_t y = 1; y < imageSize - 1; ++y) {
        for (std::uint32_t x = 1; x < imageSize - 1; ++x) {
            if (x == 15 || x == 16 || x == 31 || x == 32 || x == 47 || x == 48 || y == 15 ||
                y == 16 || y == 31 || y == 32 || y == 47 || y == 48) {
                continue;
            }
            const bool foreground = x > 16 && x < 47 && y > 16 && y < 47;
            const auto quadrant = (y >= 32 ? 2U : 0U) + (x >= 32 ? 1U : 0U);
            const auto pixel = static_cast<std::size_t>(y) * imageSize + x;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                double expected =
                    foreground
                        ? (channel == 1 || channel == 3 ? 1.0 : 0.0)
                        : (channel == 3 ? 1.0 : srgbDecode(texturePixels[quadrant * 4U + channel]));
                if (injectMismatch && x == 1 && y == 1 && channel == 0) {
                    expected += 0.5;
                }
                colorCheck.add(static_cast<double>(
                                   std::to_integer<std::uint8_t>(colorBytes[pixel * 4 + channel])) /
                                   255.0,
                               expected);
            }
            depthCheck.add(readFloat(depthBytes, pixel), foreground ? 0.25 : 0.75);
        }
    }
    return result("P02-02", {colorCheck, depthCheck},
                  {fixture.save("graphics.rgba8", colorBytes, "rgba8", imageSize, imageSize),
                   fixture.save("graphics.d32", depthBytes, "d32f-le", imageSize, imageSize)},
                  {{"reference", "sRGB 2x2 quadrant texture; center green z=.25 over z=.75; z=.9 "
                                 "red rejected; reversed z=.1 blue culled"},
                   {"edge_exclusions", "outer border and x/y15,16,31,32,47,48"},
                   {"native_viewport_height", "positive"},
                   {"projection_y_negations", 1U},
                   {"framebuffer_front_face", "COUNTER_CLOCKWISE"},
                   {"depth_compare", "LESS_OR_EQUAL"},
                   {"index_widths", Array{16U, 32U}},
                   {"texture_origin", "top-left"},
                   {"injected_readback_mismatch", injectMismatch}},
                  passed);
}

struct ComputeResult final {
    Object report;
    nvrhi::TextureHandle texture;
};

ComputeResult computeCase(Fixture& fixture, bool& passed) {
    constexpr std::size_t count = computeSize * computeSize;
    auto input =
        checked(fixture.device_->createBuffer(
                    nvrhi::BufferDesc()
                        .setByteSize(count * sizeof(std::uint32_t))
                        .setStructStride(4)
                        .enableAutomaticStateTracking(nvrhi::ResourceStates::ShaderResource)),
                "compute input");
    auto output =
        checked(fixture.device_->createBuffer(
                    nvrhi::BufferDesc()
                        .setByteSize(count * sizeof(std::uint32_t))
                        .setStructStride(4)
                        .setCanHaveUAVs(true)
                        .enableAutomaticStateTracking(nvrhi::ResourceStates::UnorderedAccess)),
                "compute output");
    auto readback = checked(fixture.device_->createBuffer(
                                nvrhi::BufferDesc()
                                    .setByteSize(count * sizeof(std::uint32_t))
                                    .setCpuAccess(nvrhi::CpuAccessMode::Read)
                                    .enableAutomaticStateTracking(nvrhi::ResourceStates::CopyDest)),
                            "buffer readback");
    auto image = fixture.texture(nvrhi::Format::RGBA16_FLOAT, computeSize, computeSize,
                                 "02b.compute.storage", false, true);
    auto consumed = fixture.texture(nvrhi::Format::RGBA16_FLOAT, computeSize, computeSize,
                                    "02b.compute.consumed", true);
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.setVisibility(nvrhi::ShaderType::Compute)
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0))
        .addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0))
        .addItem(nvrhi::BindingLayoutItem::Texture_UAV(1));
    auto layout =
        checked(fixture.device_->createBindingLayout(layoutDesc), "compute binding layout");
    auto bindings = checked(fixture.device_->createBindingSet(
                                nvrhi::BindingSetDesc()
                                    .addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, input))
                                    .addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(0, output))
                                    .addItem(nvrhi::BindingSetItem::Texture_UAV(1, image)),
                                layout),
                            "compute bindings");
    auto pipeline =
        checked(fixture.device_->createComputePipeline(nvrhi::ComputePipelineDesc()
                                                           .setComputeShader(fixture.compute_)
                                                           .addBindingLayout(layout)),
                "compute pipeline");
    auto framebuffer = fixture.framebuffer(consumed);
    auto graphicsPipeline = fixture.pipeline(framebuffer, false);
    auto graphicsBindings = fixture.bindings(image);
    std::array<std::uint32_t, count> inputs{};
    for (std::size_t index = 0; index < count; ++index) {
        inputs[index] = static_cast<std::uint32_t>(index) * 7U + 3U;
    }
    fixture.commands_->open();
    fixture.commands_->writeBuffer(input, inputs.data(), sizeof(inputs));
    fixture.commands_->setComputeState(
        nvrhi::ComputeState().setPipeline(pipeline).addBindingSet(bindings));
    fixture.commands_->dispatch(1, 2, 1);
    fixture.draw(framebuffer, graphicsPipeline, graphicsBindings, Globals{});
    fixture.commands_->copyBuffer(readback, 0, output, 0, sizeof(inputs));
    fixture.submit();
    Bytes integers(sizeof(inputs));
    const auto* mapped = fixture.device_->mapBuffer(readback, nvrhi::CpuAccessMode::Read);
    require(mapped != nullptr, "Cannot map transformed integer buffer");
    {
        const auto unmap = [&fixture, &readback](const void*) {
            fixture.device_->unmapBuffer(readback);
        };
        const std::unique_ptr<const void, decltype(unmap)> mappedGuard(mapped, unmap);
        std::memcpy(integers.data(), mapped, integers.size());
    }
    const auto storageBytes = fixture.readTexture(image, 8);
    const auto consumedBytes = fixture.readTexture(consumed, 8);
    Comparison integerCheck{"integer_buffer_transform", 0.0, 0.0};
    Comparison storageCheck{"rgba16f_storage_write", hdrTolerance, hdrTolerance};
    Comparison consumptionCheck{"compute_to_graphics_sample", hdrTolerance, hdrTolerance};
    for (std::size_t index = 0; index < count; ++index) {
        std::uint32_t actual = 0;
        std::memcpy(&actual, integers.data() + index * sizeof(actual), sizeof(actual));
        integerCheck.add(actual, inputs[index] * 3U + 11U);
        const std::array<double, 4> expected{2.0 + static_cast<double>(index % computeSize) / 8.0,
                                             4.0 + static_cast<double>(index / computeSize) / 8.0,
                                             8.0, 0.5};
        for (std::size_t channel = 0; channel < 4; ++channel) {
            storageCheck.add(readHalf(storageBytes, index * 4 + channel), expected[channel]);
            consumptionCheck.add(readHalf(consumedBytes, index * 4 + channel), expected[channel]);
        }
    }
    return {
        result("P02-03", {integerCheck, storageCheck, consumptionCheck},
               {fixture.save("compute.u32", integers, "u32-le", computeSize, computeSize),
                fixture.save("compute-storage.rgba16f", storageBytes, "rgba16f-le", computeSize,
                             computeSize),
                fixture.save("compute-consumed.rgba16f", consumedBytes, "rgba16f-le", computeSize,
                             computeSize)},
               {{"reference", "input[i]=7*i+3; output[i]=3*input[i]+11; image=(2+x/8,4+y/8,8,.5)"},
                {"workgroup", Array{16U, 8U, 1U}},
                {"dispatch", Array{1U, 2U, 1U}},
                {"integer_comparison", "exact"},
                {"edge_exclusions", "none"},
                {"storage_image_format", "rgba16f explicitly declared in SPIR-V"}},
               passed),
        image};
}

Object formatsCase(Fixture& fixture, nvrhi::ITexture* srgb, nvrhi::ITexture* linear, bool& passed) {
    auto hdr = fixture.texture(nvrhi::Format::RGBA16_FLOAT, 4, 4, "02b.hdr.blending", true);
    auto ldr = fixture.texture(nvrhi::Format::RGBA8_UNORM, 4, 4, "02b.ldr.blending", true);
    auto hdrGradient =
        fixture.texture(nvrhi::Format::RGBA16_FLOAT, 2, 2, "02b.hdr.linear.gradient");
    auto cube = fixture.texture(nvrhi::Format::RGBA16_FLOAT, 4, 4, "02b.cube", false, false, 6, 3,
                                nvrhi::TextureDimension::TextureCube);
    auto depth = fixture.texture(nvrhi::Format::D32, depthSize, depthSize, "02b.shadow.array", true,
                                 false, depthLayers, 1, nvrhi::TextureDimension::Texture2DArray);
    auto sampled = fixture.texture(nvrhi::Format::RGBA16_FLOAT, 4, 4, "02b.sampled", true);
    auto hdrFramebuffer = fixture.framebuffer(hdr);
    auto ldrFramebuffer = fixture.framebuffer(ldr);
    auto sampleFramebuffer = fixture.framebuffer(sampled);
    auto hdrBlend = fixture.pipeline(hdrFramebuffer, false, true);
    auto ldrBlend = fixture.pipeline(ldrFramebuffer, false, true);
    auto samplePipeline = fixture.pipeline(sampleFramebuffer, false, false, true);
    auto drawBindings = fixture.bindings(linear);
    auto sampleBindings = fixture.bindings(linear, true, srgb, cube, depth);
    std::vector<nvrhi::FramebufferHandle> depthFramebuffers;
    depthFramebuffers.reserve(depthLayers);
    for (std::uint32_t layer = 0; layer < depthLayers; ++layer) {
        depthFramebuffers.emplace_back(fixture.framebuffer(nullptr, depth, layer));
    }
    auto depthPipeline = fixture.pipeline(depthFramebuffers.front(), true);
    fixture.commands_->open();
    std::array<std::uint16_t, 16> hdrGradientPixels{};
    for (std::uint32_t pixel = 0; pixel < 4; ++pixel) {
        hdrGradientPixels[pixel * 4] = integerHalf(2 + pixel * 2);
        hdrGradientPixels[pixel * 4 + 1] = integerHalf(4 + pixel * 2);
        hdrGradientPixels[pixel * 4 + 2] = integerHalf(8 + pixel * 2);
        hdrGradientPixels[pixel * 4 + 3] = integerHalf(1);
    }
    fixture.commands_->writeTexture(hdrGradient, 0, 0, hdrGradientPixels.data(), 16);
    fixture.commands_->clearTextureFloat(hdr, nvrhi::AllSubresources,
                                         nvrhi::Color(2.0F, 4.0F, 8.0F, 1.0F));
    fixture.commands_->clearTextureFloat(ldr, nvrhi::AllSubresources,
                                         nvrhi::Color(0.25F, 0.5F, 0.75F, 1.0F));
    Globals globals;
    globals.transform[3] = 1.0F;
    globals.color = {1.0F, 0.5F, 0.25F, 0.5F};
    fixture.draw(hdrFramebuffer, hdrBlend, drawBindings, globals);
    globals.color = {0.25F, 0.125F, 0.0F, 0.5F};
    fixture.draw(ldrFramebuffer, ldrBlend, drawBindings, globals);
    for (std::uint32_t face = 0; face < 6; ++face) {
        for (std::uint32_t mip = 0; mip < 3; ++mip) {
            const std::uint32_t size = 4U >> mip;
            std::vector<std::uint16_t> pixels(static_cast<std::size_t>(size) * size * 4);
            for (std::size_t pixel = 0; pixel < pixels.size() / 4; ++pixel) {
                pixels[pixel * 4] = integerHalf(2 + face);
                pixels[pixel * 4 + 1] = integerHalf(4 + mip);
                pixels[pixel * 4 + 2] = integerHalf(8 + face + mip);
                pixels[pixel * 4 + 3] = integerHalf(1);
            }
            fixture.commands_->writeTexture(cube, face, mip, pixels.data(), size * 8U);
        }
    }
    fixture.commands_->clearDepthStencilTexture(depth, nvrhi::AllSubresources, true, 1.0F, false,
                                                0);
    for (std::uint32_t layer = 0; layer < depthLayers; ++layer) {
        globals.transform[2] = static_cast<float>(layer + 1) / 16.0F;
        fixture.draw(depthFramebuffers[layer], depthPipeline, drawBindings, globals);
    }
    fixture.submit();
    const auto hdrBytes = fixture.readTexture(hdr, 8);
    const auto ldrBytes = fixture.readTexture(ldr, 4);
    const auto srgbBytes = fixture.readTexture(srgb, 4);
    const auto linearBytes = fixture.readTexture(linear, 4);
    Comparison hdrBlendCheck{"hdr_attachment_blending", hdrTolerance, hdrTolerance};
    Comparison ldrBlendCheck{"unorm_attachment_blending", unormTolerance, 0.0};
    Comparison uploadCheck{"srgb_unorm_transfer_roundtrip", 0.0, 0.0};
    const std::array<double, 4> hdrExpected{2.0, 2.5, 4.25, 1.0};
    const std::array<double, 4> ldrExpected{0.375, 0.375, 0.375, 1.0};
    for (std::size_t index = 0; index < 64; ++index) {
        hdrBlendCheck.add(readHalf(hdrBytes, index), hdrExpected[index % 4]);
        ldrBlendCheck.add(static_cast<double>(std::to_integer<std::uint8_t>(ldrBytes[index])) /
                              255.0,
                          ldrExpected[index % 4]);
    }
    for (std::size_t index = 0; index < texturePixels.size(); ++index) {
        uploadCheck.add(std::to_integer<std::uint8_t>(srgbBytes[index]), texturePixels[index]);
        uploadCheck.add(std::to_integer<std::uint8_t>(linearBytes[index]), texturePixels[index]);
    }
    Comparison srgbCheck{"srgb_decode_linear_filter", hdrTolerance, hdrTolerance};
    Comparison linearCheck{"unorm_linear_filter", hdrTolerance, hdrTolerance};
    Comparison cubeCheck{"cube_six_faces_three_mips", hdrTolerance, hdrTolerance};
    Comparison depthCheck{"depth_array_twelve_layers_readback", depthTolerance, 0.0};
    Comparison depthSampleCheck{"depth_array_twelve_layers_sample", depthTolerance, 0.0};
    Comparison hdrSampleCheck{"hdr_linear_filter", hdrTolerance, hdrTolerance};
    Bytes samples;
    auto sampleDraw = [&](std::uint32_t mode, std::uint32_t layer, std::uint32_t mip,
                          const std::array<double, 4>& expected, Comparison& check,
                          nvrhi::IBindingSet* bindings) {
        Globals values;
        values.control = {mode, layer, mip, 0};
        fixture.commands_->open();
        fixture.draw(sampleFramebuffer, samplePipeline, bindings, values);
        fixture.submit();
        const auto bytes = fixture.readTexture(sampled, 8);
        for (std::size_t index = 0; index < bytes.size() / 2; ++index) {
            check.add(readHalf(bytes, index), expected[index % 4]);
        }
        samples.insert(samples.end(), bytes.begin(), bytes.end());
    };
    const double decodedMean = srgbDecode(128) * 0.5;
    const double linearMean = (128.0 / 255.0) * 0.5;
    sampleDraw(1, 0, 0, {decodedMean, decodedMean, decodedMean, 1.0}, srgbCheck, sampleBindings);
    sampleDraw(4, 0, 0, {linearMean, linearMean, linearMean, 1.0}, linearCheck, sampleBindings);
    for (std::uint32_t face = 0; face < 6; ++face) {
        for (std::uint32_t mip = 0; mip < 3; ++mip) {
            sampleDraw(2, face, mip, {2.0 + face, 4.0 + mip, 8.0 + face + mip, 1.0}, cubeCheck,
                       sampleBindings);
        }
    }
    Bytes depthCrops;
    for (std::uint32_t layer = 0; layer < depthLayers; ++layer) {
        const auto expected = static_cast<double>(layer + 1) / 16.0;
        const auto crop = fixture.readTexture(depth, 4, layer, 8, 1020);
        for (std::size_t index = 0; index < crop.size() / 4; ++index) {
            depthCheck.add(readFloat(crop, index), expected);
        }
        depthCrops.insert(depthCrops.end(), crop.begin(), crop.end());
        sampleDraw(3, layer, 0, {expected, expected, expected, 1.0}, depthSampleCheck,
                   sampleBindings);
    }
    auto hdrBindings = fixture.bindings(hdr, true, srgb, cube, depth);
    sampleDraw(4, 0, 0, hdrExpected, hdrSampleCheck, hdrBindings);
    auto hdrGradientBindings = fixture.bindings(hdrGradient, true, srgb, cube, depth);
    sampleDraw(4, 0, 0, {5.0, 7.0, 11.0, 1.0}, hdrSampleCheck, hdrGradientBindings);
    return result(
        "P02-04",
        {hdrBlendCheck, ldrBlendCheck, uploadCheck, srgbCheck, linearCheck, cubeCheck, depthCheck,
         depthSampleCheck, hdrSampleCheck},
        {fixture.save("formats-hdr.rgba16f", hdrBytes, "rgba16f-le", 4, 4),
         fixture.save("formats-unorm.rgba8", ldrBytes, "rgba8", 4, 4),
         fixture.save("formats-srgb-upload.rgba8", srgbBytes, "rgba8", 2, 2),
         fixture.save("formats-linear-upload.rgba8", linearBytes, "rgba8", 2, 2),
         fixture.save("formats-samples.rgba16f", samples, "rgba16f-le", 4, 4, 34),
         fixture.save("formats-depth-crops.d32", depthCrops, "d32f-le", 8, 8, depthLayers)},
        {{"reference", "sRGB IEC transfer before bilinear average; premultiplied One/InvSrcAlpha "
                       "blend; cube=(2+face,4+mip,8+face+mip,1); depth=(layer+1)/16"},
         {"sample_order", "sRGB center, linear center, face-major cube mip0..2, depth layers0..11, "
                          "blended HDR center, varying 2x2 HDR center (5,7,11,1)"},
         {"depth_array_extent", Array{depthSize, depthSize, depthLayers}},
         {"depth_readback_crop", "8x8 at (1020,1020) in every rendered layer"},
         {"depth_view", "one sampled array binding; twelve per-layer attachment views"},
         {"hdr_storage_coverage", "P02-03 rgba16f_storage_write and compute_to_graphics_sample"},
         {"edge_exclusions", "none for sampled4x4 outputs; depth readback central crop"}},
        passed);
}

} // namespace

ReadbackResults runReadbackCases(nvrhi::IDevice* device,
                                 const std::filesystem::path& shaderDirectory,
                                 const std::filesystem::path& outputDirectory,
                                 bool injectReadbackMismatch) {
    Fixture fixture(device, shaderDirectory, outputDirectory);
    auto srgb = fixture.texture(nvrhi::Format::SRGBA8_UNORM, 2, 2, "02b.srgb.input");
    auto linear = fixture.texture(nvrhi::Format::RGBA8_UNORM, 2, 2, "02b.linear.input");
    fixture.commands_->open();
    fixture.commands_->writeTexture(srgb, 0, 0, texturePixels.data(), 8);
    fixture.commands_->writeTexture(linear, 0, 0, texturePixels.data(), 8);
    fixture.submit();
    ReadbackResults results{true, {}};
    results.cases.emplace_back(graphicsCase(fixture, srgb, injectReadbackMismatch, results.passed));
    auto compute = computeCase(fixture, results.passed);
    results.cases.emplace_back(std::move(compute.report));
    results.cases.emplace_back(formatsCase(fixture, srgb, linear, results.passed));
    return results;
}

} // namespace shark::probe
