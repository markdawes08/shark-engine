#include "gltf_image_decoder.hpp"
#include "gltf_image_test_port.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace {
struct ImageArena;
struct alignas(std::max_align_t) ImageAllocation {
    std::size_t bytes;
    ImageAllocation* previous;
    ImageAllocation* next;
    ImageArena* owner;
};
thread_local ImageArena* image_arena{};
thread_local std::optional<std::size_t> image_failed_allocation;
thread_local std::size_t image_allocation_attempts{};
thread_local std::size_t image_live_allocations{};

struct ImageArena {
    shark::tools::detail::GltfImageDecodeBudget& budget;
    std::stop_token stop;
    ImageAllocation* head{};
    std::size_t live_bytes{};
    bool failed{};
    ImageArena* previous{image_arena};

    ImageArena(shark::tools::detail::GltfImageDecodeBudget& value, std::stop_token token)
        : budget(value), stop(token) {
        image_arena = this;
    }
    ~ImageArena() {
        while (head) {
            auto* allocation = head;
            head = allocation->next;
            --image_live_allocations;
            std::free(allocation);
        }
        image_arena = previous;
    }
};
void* image_allocate(std::size_t bytes) noexcept {
    using namespace shark::tools;
    if (!image_arena)
        return nullptr;
    auto& arena = *image_arena;
    const auto attempt = image_allocation_attempts++;
    const auto requested = std::max<std::size_t>(bytes, 1);
    if ((image_failed_allocation && attempt == *image_failed_allocation) ||
        arena.stop.stop_requested() ||
        requested > gltf_texture_maximum_decoder_live_bytes - sizeof(ImageAllocation)) {
        arena.failed = true;
        return nullptr;
    }
    const auto total = requested + sizeof(ImageAllocation);
    if (arena.live_bytes > gltf_texture_maximum_decoder_live_bytes - total ||
        total > gltf_texture_maximum_decoder_cumulative_bytes ||
        arena.budget.cumulative_bytes > gltf_texture_maximum_decoder_cumulative_bytes - total) {
        arena.failed = true;
        return nullptr;
    }
    auto* allocation = static_cast<ImageAllocation*>(std::malloc(total));
    if (!allocation) {
        arena.failed = true;
        return nullptr;
    }
    *allocation = {requested, nullptr, arena.head, &arena};
    // Some recoverable upstream JPEG exits leave padding or incomplete plane
    // regions untouched. Every decoder-owned byte starts with a deterministic
    // value, including the new tail of a successful realloc; retained prefixes
    // are copied over this zeroed payload only after allocation succeeds.
    std::memset(allocation + 1, 0, requested);
    if (arena.head)
        arena.head->previous = allocation;
    arena.head = allocation;
    arena.live_bytes += total;
    arena.budget.cumulative_bytes += total;
    ++image_live_allocations;
    return allocation + 1;
}
void image_free(void* pointer) noexcept {
    if (!pointer)
        return;
    auto* allocation = static_cast<ImageAllocation*>(pointer) - 1;
    auto& arena = *allocation->owner;
    if (allocation->previous)
        allocation->previous->next = allocation->next;
    else
        arena.head = allocation->next;
    if (allocation->next)
        allocation->next->previous = allocation->previous;
    arena.live_bytes -= allocation->bytes + sizeof(ImageAllocation);
    --image_live_allocations;
    std::free(allocation);
}
void* image_reallocate(void* pointer, std::size_t, std::size_t bytes) noexcept {
    if (!pointer)
        return image_allocate(bytes);
    if (bytes == 0) {
        image_free(pointer);
        return nullptr;
    }
    auto* allocation = static_cast<ImageAllocation*>(pointer) - 1;
    auto* replacement = image_allocate(bytes);
    if (!replacement)
        return nullptr; // The original allocation remains live and unchanged.
    std::memcpy(replacement, pointer, std::min(allocation->bytes, bytes));
    image_free(pointer);
    return replacement;
}
} // namespace

#define STBI_MALLOC(bytes) image_allocate(bytes)
#define STBI_REALLOC_SIZED(pointer, old_bytes, bytes) image_reallocate(pointer, old_bytes, bytes)
#define STBI_FREE(pointer) image_free(pointer)
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS 4096
#include <stb_image.h>

namespace shark::tools::detail {
namespace {
static_assert(gltf_texture_maximum_dimension == STBI_MAX_DIMENSIONS);
using Bytes = std::span<const std::byte>;
struct ImageFailure {
    Error error;
};
[[noreturn]] void fail(std::string message, ErrorCode code = ErrorCode::invalid_configuration) {
    throw ImageFailure{Error{code, std::move(message), {}}};
}
void poll(std::stop_token stop) {
    if (stop.stop_requested())
        fail("glTF image conversion was cancelled", ErrorCode::execution_failed);
}
std::size_t add(std::size_t a, std::size_t b) {
    if (b > std::numeric_limits<std::size_t>::max() - a)
        fail("glTF image size addition overflow");
    return a + b;
}
std::size_t multiply(std::size_t a, std::size_t b) {
    if (a && b > std::numeric_limits<std::size_t>::max() / a)
        fail("glTF image size multiplication overflow");
    return a * b;
}
void range(Bytes bytes, std::size_t offset, std::size_t count) {
    if (offset > bytes.size() || count > bytes.size() - offset)
        fail("Truncated glTF image structure");
}
unsigned byte(Bytes bytes, std::size_t offset) {
    range(bytes, offset, 1);
    return std::to_integer<unsigned>(bytes[offset]);
}
std::uint32_t big(Bytes bytes, std::size_t offset, unsigned count = 4) {
    range(bytes, offset, count);
    std::uint32_t result{};
    for (unsigned i = 0; i < count; ++i)
        result = (result << 8U) | byte(bytes, offset + i);
    return result;
}
constexpr auto crc_table() {
    std::array<std::uint32_t, 256> result{};
    for (std::uint32_t i = 0; i < result.size(); ++i) {
        auto value = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value >> 1U) ^ ((value & 1U) ? 0xedb88320U : 0U);
        result[i] = value;
    }
    return result;
}
std::uint32_t crc32(Bytes bytes, std::stop_token stop) {
    static constexpr auto table = crc_table();
    std::uint32_t value = 0xffffffffU;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if ((i & 4095U) == 0)
            poll(stop);
        value = table[(value ^ std::to_integer<unsigned>(bytes[i])) & 255U] ^ (value >> 8U);
    }
    return value ^ 0xffffffffU;
}
void dimensions(GltfImageInfo& info) {
    if (!info.width || !info.height || info.width > gltf_texture_maximum_dimension ||
        info.height > gltf_texture_maximum_dimension)
        fail("glTF image dimensions exceed their bound");
    const auto pixels = multiply(info.width, info.height);
    if (pixels > gltf_texture_maximum_pixels)
        fail("glTF image pixel count exceeds its bound");
    info.decoded_bytes = multiply(pixels, info.component_bits == 16 ? 8U : 4U);
    if (info.decoded_bytes > gltf_texture_maximum_decoded_bytes)
        fail("glTF image decoded bytes exceed their bound");
}
struct PngHeader {
    GltfImageInfo info;
    unsigned depth{};
    unsigned color{};
    unsigned channels{};
    unsigned interlace{};
    std::size_t palette_entries{};
    std::size_t idat_bytes{};
};
std::size_t pass_extent(std::size_t extent, unsigned start, unsigned step) {
    return extent > start ? (extent - start + step - 1) / step : 0;
}
std::size_t png_pass_bytes(const PngHeader& header, std::size_t width, std::size_t height) {
    if (!width || !height)
        return 0;
    return multiply(add(multiply(multiply(width, header.channels), header.depth), 7) / 8 + 1,
                    height);
}
constexpr std::array<unsigned, 7> pass_x{0, 4, 0, 2, 0, 1, 0};
constexpr std::array<unsigned, 7> pass_y{0, 0, 4, 0, 2, 0, 1};
constexpr std::array<unsigned, 7> pass_dx{8, 8, 4, 4, 2, 2, 1};
constexpr std::array<unsigned, 7> pass_dy{8, 8, 8, 4, 4, 2, 2};

PngHeader inspect_png(Bytes bytes, std::stop_token stop) {
    constexpr std::array<unsigned char, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    range(bytes, 0, signature.size());
    for (std::size_t i = 0; i < signature.size(); ++i)
        if (byte(bytes, i) != signature[i])
            fail("glTF PNG signature is invalid");
    PngHeader header;
    bool ihdr{}, plte{}, trns{}, idat{}, ended_idat{};
    std::size_t offset = 8;
    while (offset < bytes.size()) {
        poll(stop);
        range(bytes, offset, 12);
        const auto length = static_cast<std::size_t>(big(bytes, offset));
        if (length > 0x7fffffffU)
            fail("glTF PNG chunk length is invalid");
        range(bytes, offset + 8, add(length, 4));
        const auto type = big(bytes, offset + 4);
        for (unsigned i = 0; i < 4; ++i) {
            const auto value = byte(bytes, offset + 4 + i);
            if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z')))
                fail("glTF PNG chunk name is invalid");
        }
        if ((byte(bytes, offset + 6) & 32U) != 0)
            fail("glTF PNG chunk reserved bit is invalid");
        const auto payload = bytes.subspan(offset + 8, length);
        if (crc32(bytes.subspan(offset + 4, length + 4), stop) != big(bytes, offset + 8 + length))
            fail("glTF PNG chunk CRC does not match");
        if (!ihdr && type != 0x49484452U)
            fail("glTF PNG must begin with IHDR");
        if (idat && type != 0x49444154U)
            ended_idat = true;
        switch (type) {
        case 0x49484452U: { // IHDR
            if (ihdr || length != 13)
                fail("glTF PNG IHDR is invalid");
            ihdr = true;
            header.info.width = big(payload, 0);
            header.info.height = big(payload, 4);
            header.depth = byte(payload, 8);
            header.color = byte(payload, 9);
            header.interlace = byte(payload, 12);
            if (byte(payload, 10) || byte(payload, 11) || header.interlace > 1)
                fail("glTF PNG compression/filter/interlace mode is invalid");
            const auto depth = header.depth;
            bool legal{};
            switch (header.color) {
            case 0:
                header.channels = 1;
                legal = depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
                break;
            case 2:
                header.channels = 3;
                legal = depth == 8 || depth == 16;
                break;
            case 3:
                header.channels = 1;
                legal = depth == 1 || depth == 2 || depth == 4 || depth == 8;
                break;
            case 4:
                header.channels = 2;
                legal = depth == 8 || depth == 16;
                break;
            case 6:
                header.channels = 4;
                legal = depth == 8 || depth == 16;
                break;
            default:
                break;
            }
            if (!legal)
                fail("glTF PNG color type and bit depth are incompatible");
            header.info.component_bits = depth == 16 ? 16U : 8U;
            dimensions(header.info);
            header.info.work_items = multiply(multiply(header.info.width, header.info.height), 2);
            if (!header.interlace)
                header.info.png_raw_bytes =
                    png_pass_bytes(header, header.info.width, header.info.height);
            else
                for (std::size_t i = 0; i < pass_x.size(); ++i)
                    header.info.png_raw_bytes =
                        add(header.info.png_raw_bytes,
                            png_pass_bytes(header,
                                           pass_extent(header.info.width, pass_x[i], pass_dx[i]),
                                           pass_extent(header.info.height, pass_y[i], pass_dy[i])));
            break;
        }
        case 0x504c5445U: // PLTE
            if (plte || trns || idat || !length || length > 768 || length % 3 ||
                header.color == 0 || header.color == 4)
                fail("glTF PNG palette is invalid");
            plte = true;
            header.palette_entries = length / 3;
            if (header.color == 3 && header.palette_entries > (std::size_t{1} << header.depth))
                fail("glTF PNG palette exceeds its sample range");
            break;
        case 0x74524e53U: // tRNS
            if (trns || idat)
                fail("glTF PNG transparency chunk order is invalid");
            trns = true;
            if (header.color == 3) {
                if (!plte || !length || length > header.palette_entries)
                    fail("glTF PNG palette transparency is invalid");
            } else if (header.color == 0 || header.color == 2) {
                const unsigned samples = header.color == 0 ? 1U : 3U;
                if (length != samples * 2U)
                    fail("glTF PNG transparency sample count is invalid");
                const auto maximum = (1U << header.depth) - 1U;
                for (unsigned i = 0; i < samples; ++i)
                    if (big(payload, i * 2U, 2) > maximum)
                        fail("glTF PNG transparency sample is out of range");
            } else
                fail("glTF PNG transparency is incompatible with its color type");
            break;
        case 0x49444154U: // IDAT
            if (ended_idat || (header.color == 3 && !plte))
                fail("glTF PNG image data chunk order is invalid");
            idat = true;
            header.idat_bytes = add(header.idat_bytes, length);
            break;
        case 0x49454e44U: // IEND
            if (length || !idat || header.idat_bytes < 6 || offset + 12 != bytes.size())
                fail("glTF PNG terminal image data is invalid");
            return header;
        default:
            if ((byte(bytes, offset + 4) & 32U) == 0)
                fail("glTF PNG has an unsupported critical chunk");
            // Embedded color profiles, animation and orientation metadata do
            // not select a texture usage's transfer function or orientation.
            break;
        }
        offset += length + 12;
    }
    fail("glTF PNG is missing IEND");
}

unsigned jpeg_quantization(Bytes payload) {
    std::size_t offset{};
    unsigned tables{};
    while (offset < payload.size()) {
        const auto selector = byte(payload, offset++);
        if ((selector >> 4U) > 1 || (selector & 15U) > 3)
            fail("glTF JPEG quantization selector is invalid");
        tables |= 1U << (selector & 15U);
        const auto count = (selector >> 4U) ? 128U : 64U;
        range(payload, offset, count);
        const auto sample_bytes = (selector >> 4U) ? 2U : 1U;
        for (unsigned i = 0; i < 64; ++i)
            if (!big(payload, offset + i * sample_bytes, sample_bytes))
                fail("glTF JPEG quantization sample is zero");
        offset += count;
    }
    if (!offset)
        fail("glTF JPEG quantization table is empty");
    return tables;
}
unsigned jpeg_huffman(Bytes payload) {
    std::size_t offset{};
    unsigned tables{};
    while (offset < payload.size()) {
        const auto selector = byte(payload, offset++);
        if ((selector >> 4U) > 1 || (selector & 15U) > 3)
            fail("glTF JPEG Huffman selector is invalid");
        tables |= 1U << ((selector >> 4U) * 4 + (selector & 15U));
        range(payload, offset, 16);
        std::size_t count{};
        for (unsigned i = 0; i < 16; ++i)
            count += byte(payload, offset++);
        if (!count || count > 256)
            fail("glTF JPEG Huffman symbol count is invalid");
        range(payload, offset, count);
        offset += count;
    }
    if (!offset)
        fail("glTF JPEG Huffman table is empty");
    return tables;
}
GltfImageInfo inspect_jpeg(Bytes bytes, std::stop_token stop) {
    // Validate complete marker/segment framing and bound repeated scan work.
    // This is not a second entropy decoder: some framing-complete damaged
    // entropy streams may be tolerated by the pinned upstream JPEG decoder.
    if (big(bytes, 0, 2) != 0xffd8U)
        fail("glTF JPEG is missing SOI");
    GltfImageInfo info;
    std::array<unsigned, 4> component_ids{};
    std::array<unsigned, 4> quantization_ids{};
    std::array<unsigned, 4> horizontal_sampling{};
    std::array<unsigned, 4> vertical_sampling{};
    std::array<bool, 4> initial_dc{};
    unsigned component_count{};
    unsigned maximum_horizontal_sampling{1};
    unsigned maximum_vertical_sampling{1};
    unsigned frame_marker{};
    unsigned restart_interval{};
    unsigned quantization_tables{};
    unsigned huffman_tables{};
    std::size_t scans{};
    std::size_t offset = 2;
    while (offset < bytes.size()) {
        poll(stop);
        if (byte(bytes, offset++) != 0xffU)
            fail("glTF JPEG requires a marker outside scan data");
        while (byte(bytes, offset) == 0xffU)
            ++offset;
        const auto marker = byte(bytes, offset++);
        if (marker == 0xd9U) {
            if (!frame_marker || !scans || offset != bytes.size())
                fail("glTF JPEG terminal marker is invalid");
            for (unsigned i = 0; i < component_count; ++i)
                if (!initial_dc[i])
                    fail("glTF JPEG component has no initial scan");
            info.work_items = multiply(multiply(info.width, info.height), scans);
            if (info.work_items > gltf_texture_maximum_work)
                fail("glTF JPEG scan work exceeds its bound");
            return info;
        }
        if (!marker || marker == 0xd8U || marker == 1U || (marker >= 0xd0U && marker <= 0xd7U))
            fail("glTF JPEG marker is misplaced");
        const auto length = static_cast<std::size_t>(big(bytes, offset, 2));
        if (length < 2)
            fail("glTF JPEG segment length is invalid");
        range(bytes, offset, length);
        const auto payload = bytes.subspan(offset + 2, length - 2);
        offset += length;
        if (marker == 0xc0U || marker == 0xc1U || marker == 0xc2U) {
            if (frame_marker || scans || payload.size() < 6 || byte(payload, 0) != 8)
                fail("glTF JPEG frame is invalid or uses unsupported precision");
            frame_marker = marker;
            info.height = big(payload, 1, 2);
            info.width = big(payload, 3, 2);
            component_count = byte(payload, 5);
            if ((component_count != 1 && component_count != 3 && component_count != 4) ||
                payload.size() != 6 + 3 * component_count)
                fail("glTF JPEG component layout is invalid");
            for (unsigned i = 0; i < component_count; ++i) {
                component_ids[i] = byte(payload, 6 + 3 * i);
                if (std::find(component_ids.begin(), component_ids.begin() + i, component_ids[i]) !=
                    component_ids.begin() + i)
                    fail("glTF JPEG component identifier is duplicated");
                const auto sampling = byte(payload, 7 + 3 * i);
                quantization_ids[i] = byte(payload, 8 + 3 * i);
                if (!(sampling >> 4U) || (sampling >> 4U) > 4 || !(sampling & 15U) ||
                    (sampling & 15U) > 4 || quantization_ids[i] > 3)
                    fail("glTF JPEG component sampling is invalid");
                horizontal_sampling[i] = sampling >> 4U;
                vertical_sampling[i] = sampling & 15U;
                maximum_horizontal_sampling =
                    std::max(maximum_horizontal_sampling, horizontal_sampling[i]);
                maximum_vertical_sampling =
                    std::max(maximum_vertical_sampling, vertical_sampling[i]);
            }
            dimensions(info);
        } else if (marker == 0xdaU) {
            if (!frame_marker || payload.empty())
                fail("glTF JPEG scan precedes its frame");
            const auto count = byte(payload, 0);
            if (!count || count > component_count || payload.size() != 4 + count * 2)
                fail("glTF JPEG scan component layout is invalid");
            std::array<unsigned, 4> seen{};
            for (unsigned i = 0; i < count; ++i) {
                const auto id = byte(payload, 1 + i * 2);
                if (std::find(component_ids.begin(), component_ids.begin() + component_count, id) ==
                        component_ids.begin() + component_count ||
                    std::find(seen.begin(), seen.begin() + i, id) != seen.begin() + i)
                    fail("glTF JPEG scan component identifier is invalid");
                seen[i] = id;
                const auto tables = byte(payload, 2 + i * 2);
                if ((tables >> 4U) > 3 || (tables & 15U) > 3)
                    fail("glTF JPEG scan Huffman selector is invalid");
            }
            const auto first = byte(payload, 1 + count * 2);
            const auto last = byte(payload, 2 + count * 2);
            const auto approximation = byte(payload, 3 + count * 2);
            if (frame_marker != 0xc2U) {
                if (first || last != 63 || approximation)
                    fail("glTF JPEG sequential scan parameters are invalid");
            } else if (first > 63 || last > 63 || last < first || (!first && last) ||
                       (first && count != 1) || (approximation >> 4U) > 13 ||
                       (approximation & 15U) > 13 ||
                       ((approximation >> 4U) &&
                        (approximation >> 4U) != (approximation & 15U) + 1))
                fail("glTF JPEG progressive scan parameters are invalid");
            for (unsigned i = 0; i < count; ++i) {
                const auto id = byte(payload, 1 + i * 2);
                const auto component = static_cast<std::size_t>(
                    std::find(component_ids.begin(), component_ids.begin() + component_count, id) -
                    component_ids.begin());
                const auto tables = byte(payload, 2 + i * 2);
                const bool initial = !first && !(approximation >> 4U);
                if ((quantization_tables & (1U << quantization_ids[component])) == 0 ||
                    ((frame_marker != 0xc2U || initial) &&
                     (huffman_tables & (1U << (tables >> 4U))) == 0) ||
                    ((frame_marker != 0xc2U || first) &&
                     (huffman_tables & (1U << (4 + (tables & 15U)))) == 0))
                    fail("glTF JPEG scan references an undeclared coding table");
                if (initial) {
                    if (initial_dc[component])
                        fail("glTF JPEG component initial scan is duplicated");
                    initial_dc[component] = true;
                } else if (!initial_dc[component]) {
                    fail("glTF JPEG component refinement precedes its initial scan");
                }
            }
            ++scans;
            if (multiply(multiply(info.width, info.height), scans) > gltf_texture_maximum_work)
                fail("glTF JPEG scan work exceeds its bound");
            // A restart interval counts scan MCUs. An interleaved MCU spans
            // the frame's maximum sampling grid; a noninterleaved scan counts
            // the selected component's actual 8x8 blocks, including progressive
            // AC/DC scans and components subsampled relative to the frame.
            const auto mcu_width = multiply(maximum_horizontal_sampling, 8);
            const auto mcu_height = multiply(maximum_vertical_sampling, 8);
            std::size_t scanned_mcus{};
            if (count > 1) {
                scanned_mcus = multiply(add(info.width, mcu_width - 1) / mcu_width,
                                        add(info.height, mcu_height - 1) / mcu_height);
            } else {
                const auto component = static_cast<std::size_t>(
                    std::find(component_ids.begin(), component_ids.begin() + component_count,
                              byte(payload, 1)) -
                    component_ids.begin());
                scanned_mcus = multiply(
                    add(multiply(info.width, horizontal_sampling[component]), mcu_width - 1) /
                        mcu_width,
                    add(multiply(info.height, vertical_sampling[component]), mcu_height - 1) /
                        mcu_height);
            }
            const auto restart_markers =
                restart_interval ? (scanned_mcus - 1) / restart_interval : 0;
            std::size_t seen_restart_markers{};
            unsigned expected_restart{};
            bool entropy{};
            bool next_marker{};
            while (offset < bytes.size()) {
                if ((offset & 4095U) == 0)
                    poll(stop);
                if (byte(bytes, offset) != 0xffU) {
                    entropy = true;
                    ++offset;
                    continue;
                }
                const auto marker_start = offset++;
                while (byte(bytes, offset) == 0xffU)
                    ++offset;
                const auto escaped = byte(bytes, offset++);
                if (!escaped) {
                    entropy = true;
                    continue;
                }
                if (escaped >= 0xd0U && escaped <= 0xd7U) {
                    if (!restart_interval || escaped != 0xd0U + expected_restart ||
                        seen_restart_markers >= restart_markers)
                        fail("glTF JPEG restart marker sequence is invalid");
                    ++seen_restart_markers;
                    expected_restart = (expected_restart + 1) & 7U;
                    continue;
                }
                offset = marker_start;
                next_marker = true;
                break;
            }
            if (!entropy || !next_marker)
                fail("Truncated glTF JPEG entropy segment");
            if (seen_restart_markers != restart_markers)
                fail("glTF JPEG restart marker inventory is incomplete");
        } else if (marker == 0xdbU) {
            quantization_tables |= jpeg_quantization(payload);
        } else if (marker == 0xc4U) {
            huffman_tables |= jpeg_huffman(payload);
        } else if (marker == 0xddU) {
            if (payload.size() != 2)
                fail("glTF JPEG restart interval is invalid");
            restart_interval = big(payload, 0, 2);
        } else if (marker == 0xdcU) {
            if (!frame_marker || payload.size() != 2 || big(payload, 0, 2) != info.height)
                fail("glTF JPEG line count is invalid");
        } else if (!((marker >= 0xe0U && marker <= 0xefU) || marker == 0xfeU)) {
            fail("glTF JPEG coding marker is unsupported");
        }
    }
    fail("glTF JPEG is missing EOI");
}

GltfImageInfo inspect(Bytes bytes, std::string_view mime, std::stop_token stop) {
    poll(stop);
    if (bytes.empty() || bytes.size() > gltf_source_maximum_file_bytes)
        fail("glTF encoded image exceeds its byte bound");
    if (mime == "image/png")
        return inspect_png(bytes, stop).info;
    if (mime == "image/jpeg")
        return inspect_jpeg(bytes, stop);
    fail("glTF image media type is unsupported");
}

struct ImageFree {
    void operator()(void* pointer) const noexcept {
        image_free(pointer);
    }
};
using ImagePointer = std::unique_ptr<void, ImageFree>;
ImagePointer scratch(std::size_t bytes) {
    ImagePointer result{image_allocate(bytes)};
    if (!result)
        fail("glTF image decoder allocation failed", ErrorCode::execution_failed);
    return result;
}
std::uint32_t adler32(Bytes bytes, std::stop_token stop) {
    std::uint32_t a = 1, b{};
    for (std::size_t offset = 0; offset < bytes.size();) {
        poll(stop);
        const auto end = std::min(bytes.size(), offset + 4096);
        for (; offset < end; ++offset) {
            a += std::to_integer<unsigned>(bytes[offset]);
            b += a;
        }
        a %= 65521U;
        b %= 65521U;
    }
    return (b << 16U) | a;
}
void validate_png_rows(std::span<std::byte> raw, const PngHeader& header, std::stop_token stop) {
    std::size_t offset{};
    const auto passes = header.interlace ? 7U : 1U;
    for (unsigned i = 0; i < passes; ++i) {
        const auto width = header.interlace ? pass_extent(header.info.width, pass_x[i], pass_dx[i])
                                            : header.info.width;
        const auto height = header.interlace
                                ? pass_extent(header.info.height, pass_y[i], pass_dy[i])
                                : header.info.height;
        if (!width || !height)
            continue;
        const auto row_bytes = (width * header.channels * header.depth + 7) / 8;
        std::optional<std::size_t> previous_row;
        for (std::size_t row = 0; row < height; ++row) {
            poll(stop);
            const auto filter = byte(raw, offset);
            if (filter > 4)
                fail("glTF PNG row filter is invalid");
            if (header.color == 3) {
                // stb expands palette indices without checking the declared
                // palette length. Reverse the one-byte packed-row filters here
                // before accepting any index, so unused vendor palette slots
                // can never become uninitialized output pixels. This modifies
                // only disposable, already checksummed preflight scratch.
                for (std::size_t column = 0; column < row_bytes; ++column) {
                    const auto left = column ? byte(raw, offset + column) : 0U;
                    const auto above = previous_row ? byte(raw, *previous_row + column) : 0U;
                    const auto upper_left =
                        previous_row && column ? byte(raw, *previous_row + column - 1) : 0U;
                    unsigned prediction{};
                    if (filter == 1)
                        prediction = left;
                    else if (filter == 2)
                        prediction = above;
                    else if (filter == 3)
                        prediction = (left + above) / 2;
                    else if (filter == 4) {
                        const auto estimate =
                            static_cast<int>(left + above) - static_cast<int>(upper_left);
                        const auto dl = std::abs(estimate - static_cast<int>(left));
                        const auto da = std::abs(estimate - static_cast<int>(above));
                        const auto du = std::abs(estimate - static_cast<int>(upper_left));
                        prediction = dl <= da && dl <= du ? left : da <= du ? above : upper_left;
                    }
                    raw[offset + 1 + column] = static_cast<std::byte>(
                        (byte(raw, offset + 1 + column) + prediction) & 255U);
                }
                for (std::size_t column = 0; column < width; ++column) {
                    const auto bit = column * header.depth;
                    const auto sample =
                        (byte(raw, offset + 1 + bit / 8) >> (8 - header.depth - bit % 8)) &
                        ((1U << header.depth) - 1U);
                    if (sample >= header.palette_entries)
                        fail("glTF PNG sample references a missing palette entry");
                }
                previous_row = offset + 1;
            }
            offset += row_bytes + 1;
        }
    }
    if (offset != raw.size())
        fail("glTF PNG reconstructed row size is invalid");
}
void validate_png_stream(Bytes bytes, const PngHeader& header, std::stop_token stop) {
    auto compressed = scratch(header.idat_bytes);
    auto* data = static_cast<stbi_uc*>(compressed.get());
    std::size_t target{};
    for (std::size_t offset = 8; offset < bytes.size();) {
        poll(stop);
        const auto length = static_cast<std::size_t>(big(bytes, offset));
        if (big(bytes, offset + 4) == 0x49444154U) {
            std::memcpy(data + target, bytes.data() + offset + 8, length);
            target += length;
        }
        offset += length + 12;
    }
    if ((data[0] & 15U) != 8 || (data[0] >> 4U) > 7 || (data[1] & 32U) ||
        ((static_cast<unsigned>(data[0]) * 256 + data[1]) % 31U))
        fail("glTF PNG zlib header is invalid");
    auto raw = scratch(header.info.png_raw_bytes);
    // Use the pinned decoder's unmodified fixed-output zlib primitive. The
    // public helper has no consumed-input report and skips Adler-32. Its private
    // state lets this boundary reject extra/truncated DEFLATE bytes as well as
    // expansion beyond the exact bounded scanline inventory. This is not a
    // second DEFLATE implementation or a vendor patch.
    stbi__zbuf state{};
    state.zbuffer = data;
    state.zbuffer_end = data + header.idat_bytes;
    if (!stbi__do_zlib(&state, static_cast<char*>(raw.get()),
                       static_cast<int>(header.info.png_raw_bytes), 0, 1) ||
        static_cast<std::size_t>(state.zout - state.zout_start) != header.info.png_raw_bytes)
        fail("glTF PNG compressed row stream is invalid");
    const auto remaining_bits = state.num_bits - (state.hit_zeof_once ? 16 : 0);
    const auto read_bytes = static_cast<std::size_t>(state.zbuffer - data);
    if (remaining_bits < 0 || static_cast<std::size_t>(remaining_bits / 8) > read_bytes ||
        read_bytes - static_cast<std::size_t>(remaining_bits / 8) != header.idat_bytes - 4)
        fail("glTF PNG DEFLATE stream has trailing or truncated bytes");
    const Bytes rows{static_cast<const std::byte*>(raw.get()), header.info.png_raw_bytes};
    const Bytes encoded{reinterpret_cast<const std::byte*>(data), header.idat_bytes};
    if (adler32(rows, stop) != big(encoded, header.idat_bytes - 4))
        fail("glTF PNG compressed row checksum does not match");
    validate_png_rows({static_cast<std::byte*>(raw.get()), header.info.png_raw_bytes}, header,
                      stop);
}
Error allocation_error(std::string_view operation) {
    return Error{ErrorCode::execution_failed,
                 "glTF image decoder allocation failed",
                 {.operation = std::string{operation}}};
}
} // namespace

void gltf_image_fail_allocation(std::optional<std::size_t> position) noexcept {
    image_failed_allocation = position;
    image_allocation_attempts = 0;
}
std::size_t gltf_image_allocation_attempts() noexcept {
    return image_allocation_attempts;
}
std::size_t gltf_image_live_allocations() noexcept {
    return image_live_allocations;
}

Result<GltfImageInfo> inspect_gltf_image(Bytes bytes, std::string_view mime, std::stop_token stop) {
    try {
        return inspect(bytes, mime, stop);
    } catch (ImageFailure& failure) {
        failure.error.context.operation = "asset.gltf.image.inspect";
        return std::move(failure.error);
    } catch (const std::bad_alloc&) {
        return allocation_error("asset.gltf.image.inspect");
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "glTF image inspection failed",
                     {.operation = "asset.gltf.image.inspect"}};
    }
}
Result<GltfDecodedImage> decode_gltf_image(Bytes bytes, std::string_view mime,
                                           const GltfImageInfo& expected,
                                           GltfImageDecodeBudget& budget, std::stop_token stop) {
    try {
        const auto info = inspect(bytes, mime, stop);
        if (info != expected)
            fail("glTF image header differs from its precharged inventory", ErrorCode::conflict);
        ImageArena arena{budget, stop};
        if (mime == "image/png")
            validate_png_stream(bytes, inspect_png(bytes, stop), stop);
        poll(stop);
        int width{}, height{}, components{};
        ImagePointer decoded{
            info.component_bits == 16
                ? static_cast<void*>(stbi_load_16_from_memory(
                      reinterpret_cast<const stbi_uc*>(bytes.data()),
                      static_cast<int>(bytes.size()), &width, &height, &components, 4))
                : static_cast<void*>(stbi_load_from_memory(
                      reinterpret_cast<const stbi_uc*>(bytes.data()),
                      static_cast<int>(bytes.size()), &width, &height, &components, 4))};
        poll(stop);
        if (arena.failed)
            fail("glTF image decoder allocation failed", ErrorCode::execution_failed);
        if (!decoded || width != static_cast<int>(info.width) ||
            height != static_cast<int>(info.height))
            fail("glTF image decoder rejected the encoded image");
        GltfDecodedImage result{info, std::vector<std::byte>(info.decoded_bytes)};
        if (info.component_bits == 16) {
            const auto* samples = static_cast<const stbi_us*>(decoded.get());
            for (std::size_t i = 0; i < info.decoded_bytes / 2; ++i) {
                if ((i & 4095U) == 0)
                    poll(stop);
                result.rgba[i * 2] = static_cast<std::byte>(samples[i] & 255U);
                result.rgba[i * 2 + 1] = static_cast<std::byte>(samples[i] >> 8U);
            }
        } else {
            std::memcpy(result.rgba.data(), decoded.get(), result.rgba.size());
        }
        poll(stop);
        return result;
    } catch (ImageFailure& failure) {
        failure.error.context.operation = "asset.gltf.image.decode";
        return std::move(failure.error);
    } catch (const std::bad_alloc&) {
        return allocation_error("asset.gltf.image.decode");
    } catch (...) {
        return Error{ErrorCode::execution_failed,
                     "glTF image conversion failed",
                     {.operation = "asset.gltf.image.decode"}};
    }
}
} // namespace shark::tools::detail
