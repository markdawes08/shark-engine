#pragma once

#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace shark::probe {

// A writer for this probe's owned report. It is not a public engine JSON API.
class Json final {
  public:
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() : value_(nullptr) {}
    Json(bool value) : value_(value) {}
    Json(std::uint32_t value) : value_(static_cast<std::uint64_t>(value)) {}
    Json(std::uint64_t value) : value_(value) {}
    Json(double value) : value_(value) {
        if (!std::isfinite(value)) {
            throw std::invalid_argument("JSON numbers must be finite");
        }
    }
    Json(const char* value) : value_(std::string(value)) {}
    Json(std::string value) : value_(std::move(value)) {}
    Json(Array value) : value_(std::move(value)) {}
    Json(Object value) : value_(std::move(value)) {}

    void write(std::ostream& stream) const {
        std::visit([&stream](const auto& value) { writeValue(stream, value); }, value_);
    }

  private:
    static void writeValue(std::ostream& stream, std::nullptr_t) {
        stream << "null";
    }
    static void writeValue(std::ostream& stream, bool value) {
        stream << (value ? "true" : "false");
    }
    static void writeValue(std::ostream& stream, std::uint64_t value) {
        stream << value;
    }
    static void writeValue(std::ostream& stream, double value) {
        // to_chars is locale independent and preserves enough precision for
        // independently checking the fixed readback tolerances.
        char buffer[64]{};
        const auto result =
            std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general,
                          std::numeric_limits<double>::max_digits10);
        if (result.ec != std::errc{}) {
            throw std::runtime_error("Cannot encode JSON floating-point value");
        }
        stream.write(buffer, result.ptr - buffer);
    }
    static void writeValue(std::ostream& stream, const std::string& value) {
        constexpr std::string_view hex = "0123456789abcdef";
        stream << '"';
        for (const char character : value) {
            const auto byte = static_cast<unsigned char>(character);
            if (byte == '"' || byte == '\\') {
                stream << '\\' << character;
            } else if (byte < 0x20U || byte >= 0x7fU) {
                // Driver strings need not be valid UTF-8. Preserve their bytes
                // unambiguously as U+00xx, keeping every emitted document valid.
                stream << "\\u00" << hex[byte >> 4U] << hex[byte & 0xfU];
            } else {
                stream << character;
            }
        }
        stream << '"';
    }
    static void writeValue(std::ostream& stream, const Array& values) {
        stream << '[';
        bool first = true;
        for (const auto& value : values) {
            if (!first) {
                stream << ',';
            }
            first = false;
            value.write(stream);
        }
        stream << ']';
    }
    static void writeValue(std::ostream& stream, const Object& values) {
        stream << '{';
        bool first = true;
        for (const auto& [key, value] : values) {
            if (!first) {
                stream << ',';
            }
            first = false;
            writeValue(stream, key);
            stream << ':';
            value.write(stream);
        }
        stream << '}';
    }

    std::variant<std::nullptr_t, bool, std::uint64_t, double, std::string, Array, Object> value_;
};

} // namespace shark::probe
