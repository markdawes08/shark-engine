#pragma once

#include "shark/content_digest.hpp"
#include "shark/stable_id.hpp"

#include "utf8.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

// Small private wire primitives shared by the two prepared development formats.
// These do not parse authoring source or define the later cooked format.
namespace shark::prepared_detail {

using Bytes = std::span<const std::byte>;
inline constexpr std::size_t envelope_bytes = 44;
struct Failure {
    Error error;
};
[[noreturn]] inline void fail(const char* message,
                              ErrorCode code = ErrorCode::invalid_configuration) {
    throw Failure{Error{code, message, {}}};
}
inline void require(bool condition, const char* message) {
    if (!condition)
        fail(message);
}
template <typename T> T take(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
inline void take(Result<void> result) {
    if (!result)
        throw Failure{std::move(result).error()};
}
template <typename T, typename F> Result<T> boundary(const char* operation, F&& function) {
    try {
        if constexpr (std::is_void_v<T>) {
            function();
            return {};
        } else
            return function();
    } catch (Failure& failure) {
        failure.error.context.operation = operation;
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::execution_failed, "Prepared content operation failed", {operation}};
    }
}
inline Bytes bytes(std::string_view text) {
    return std::as_bytes(std::span{text.data(), text.size()});
}

struct Writer {
    std::size_t limit;
    std::vector<std::byte> output;
    explicit Writer(std::size_t maximum) : limit(maximum) {}
    void raw(Bytes value) {
        require(value.size() <= limit - output.size(), "Prepared payload exceeds its byte bound");
        output.insert(output.end(), value.begin(), value.end());
    }
    void integer(std::uint64_t value, unsigned width) {
        std::array<std::byte, 8> data{};
        for (unsigned i = 0; i < width; ++i)
            data[i] = static_cast<std::byte>((value >> (i * 8U)) & 255U);
        raw(Bytes{data}.first(width));
    }
    void u8(std::uint8_t value) {
        integer(value, 1);
    }
    void u32(std::uint32_t value) {
        integer(value, 4);
    }
    void i32(std::int32_t value) {
        u32(std::bit_cast<std::uint32_t>(value));
    }
    void f32(float value) {
        require(std::isfinite(value), "Prepared floating value must be finite");
        u32(std::bit_cast<std::uint32_t>(value));
    }
    void f64(double value) {
        require(std::isfinite(value), "Prepared floating value must be finite");
        integer(std::bit_cast<std::uint64_t>(value), 8);
    }
    void id(const StableId& value) {
        raw(value.bytes());
    }
    void text(std::string_view value, std::size_t maximum = 4096) {
        require(value.size() <= maximum &&
                    value.size() <= std::numeric_limits<std::uint32_t>::max() &&
                    detail::valid_utf8(value),
                "Invalid prepared UTF-8 string");
        u32(static_cast<std::uint32_t>(value.size()));
        raw(bytes(value));
    }
};
struct Reader {
    Bytes input;
    std::size_t allocation_limit;
    std::size_t position{};
    std::size_t allocated{};
    Reader(Bytes bytes, std::size_t maximum) : input(bytes), allocation_limit(maximum) {}
    [[nodiscard]] std::size_t remaining() const noexcept {
        return input.size() - position;
    }
    void charge(std::size_t count, std::size_t size = 1) {
        require(size == 0 || count <= (allocation_limit - allocated) / size,
                "Prepared payload exceeds its decoded allocation bound");
        allocated += count * size;
    }
    Bytes raw(std::size_t count) {
        require(count <= remaining(), "Truncated prepared payload");
        const auto value = input.subspan(position, count);
        position += count;
        return value;
    }
    std::uint64_t integer(unsigned width) {
        const auto data = raw(width);
        std::uint64_t value{};
        for (unsigned i = 0; i < width; ++i)
            value |= static_cast<std::uint64_t>(std::to_integer<unsigned>(data[i])) << (i * 8U);
        return value;
    }
    std::uint8_t u8() {
        return static_cast<std::uint8_t>(integer(1));
    }
    std::uint32_t u32() {
        return static_cast<std::uint32_t>(integer(4));
    }
    std::int32_t i32() {
        return std::bit_cast<std::int32_t>(u32());
    }
    float f32() {
        const auto value = std::bit_cast<float>(u32());
        require(std::isfinite(value), "Prepared floating value must be finite");
        return value;
    }
    double f64() {
        const auto value = std::bit_cast<double>(integer(8));
        require(std::isfinite(value), "Prepared floating value must be finite");
        return value;
    }
    StableId id() {
        const auto data = raw(16);
        constexpr std::string_view hex = "0123456789abcdef";
        std::array<char, 36> text{};
        std::size_t offset{};
        for (std::size_t i = 0; i < data.size(); ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10)
                text[offset++] = '-';
            const auto value = std::to_integer<unsigned>(data[i]);
            text[offset++] = hex[value >> 4U];
            text[offset++] = hex[value & 15U];
        }
        return take(StableId::parse(std::string_view{text.data(), text.size()}));
    }
    std::string text(std::size_t maximum = 4096) {
        const auto length = static_cast<std::size_t>(u32());
        require(length <= maximum && length <= remaining(), "Invalid prepared string length");
        charge(length + 1);
        const auto data = raw(length);
        std::string result(reinterpret_cast<const char*>(data.data()), data.size());
        require(detail::valid_utf8(result), "Invalid prepared UTF-8 string");
        return result;
    }
    std::size_t count(std::size_t maximum, std::size_t minimum_encoded_bytes,
                      std::size_t decoded_record_bytes) {
        const auto value = static_cast<std::size_t>(u32());
        require(value <= maximum &&
                    (minimum_encoded_bytes == 0 || value <= remaining() / minimum_encoded_bytes),
                "Invalid prepared record count");
        charge(value, decoded_record_bytes);
        return value;
    }
    void finish() const {
        require(remaining() == 0, "Trailing bytes in prepared payload");
    }
};
inline std::vector<std::byte> wrap(std::string_view magic, Bytes body, std::size_t maximum) {
    require(magic.size() == 8 && maximum >= envelope_bytes &&
                body.size() <= maximum - envelope_bytes,
            "Invalid prepared envelope size");
    Writer writer{maximum};
    writer.raw(bytes(magic));
    writer.u32(1);
    writer.raw(take(sha256(body)).bytes);
    writer.raw(body);
    return std::move(writer.output);
}
inline Bytes unwrap(Bytes payload, std::string_view magic, std::size_t maximum) {
    require(magic.size() == 8 && payload.size() >= envelope_bytes && payload.size() <= maximum,
            "Invalid prepared envelope size");
    Reader reader{payload, 0};
    const auto actual_magic = reader.raw(8);
    require(std::equal(actual_magic.begin(), actual_magic.end(), bytes(magic).begin()),
            "Unknown prepared payload kind");
    require(reader.u32() == 1, "Unsupported prepared payload version; expected 1");
    const auto digest = reader.raw(32);
    const auto body = payload.subspan(envelope_bytes);
    const auto expected = take(sha256(body));
    require(std::equal(digest.begin(), digest.end(), expected.bytes.begin()),
            "Prepared payload digest mismatch");
    return body;
}

} // namespace shark::prepared_detail
