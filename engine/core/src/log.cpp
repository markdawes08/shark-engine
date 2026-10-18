#include <shark/log.hpp>

#include <array>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>

namespace shark {
namespace {

std::string_view level_name(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::debug:
        return "debug";
    case LogLevel::info:
        return "info";
    case LogLevel::warning:
        return "warning";
    case LogLevel::error:
        return "error";
    }
    return {};
}

bool is_utf8(std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<std::uint8_t>(text[index]);
        if (first < 0x80U) {
            ++index;
            continue;
        }
        std::size_t count = 0;
        std::uint32_t point = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2U && first <= 0xdfU) {
            count = 2;
            point = first & 0x1fU;
            minimum = 0x80U;
        } else if (first >= 0xe0U && first <= 0xefU) {
            count = 3;
            point = first & 0x0fU;
            minimum = 0x800U;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            count = 4;
            point = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (count > text.size() - index) {
            return false;
        }
        for (std::size_t offset = 1; offset < count; ++offset) {
            const auto next = static_cast<std::uint8_t>(text[index + offset]);
            if ((next & 0xc0U) != 0x80U) {
                return false;
            }
            point = (point << 6U) | (next & 0x3fU);
        }
        if (point < minimum || point > 0x10ffffU || (point >= 0xd800U && point <= 0xdfffU)) {
            return false;
        }
        index += count;
    }
    return true;
}

void append_string(std::string& output, std::string_view value) {
    constexpr std::string_view hex = "0123456789abcdef";
    output.push_back('"');
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') {
            output.push_back('\\');
            output.push_back(character);
        } else if (byte < 0x20U) {
            output.append("\\u00");
            output.push_back(hex[byte >> 4U]);
            output.push_back(hex[byte & 0x0fU]);
        } else {
            output.push_back(character);
        }
    }
    output.push_back('"');
}

void append_field(std::string& output, std::string_view name, std::string_view value) {
    if (output.size() > 1) {
        output.push_back(',');
    }
    append_string(output, name);
    output.push_back(':');
    append_string(output, value);
}

Error invalid_field(std::string_view name, std::string_view reason) {
    return {ErrorCode::invalid_argument,
            "Cannot log field '" + std::string(name) + "': " + std::string(reason),
            {.operation = "log.write"}};
}

Error sink_failure() {
    return {ErrorCode::io_failure,
            "Cannot write or flush the diagnostic sink",
            {.operation = "log.write"}};
}

} // namespace

Result<void> Logger::write(LogLevel level, const Error& error) {
    const auto level_text = level_name(level);
    if (level_text.empty()) {
        return invalid_field("level", "unsupported log level");
    }
    struct Field {
        std::string_view name;
        std::string_view value;
    };
    const std::array fields{
        Field{"message", error.message},
        Field{"operation", error.context.operation},
        Field{"request_id", error.context.request_id},
        Field{"path", error.context.path},
        Field{"entity_id", error.context.entity_id},
        Field{"asset_id", error.context.asset_id},
    };
    for (const auto& field : fields) {
        const auto limit = field.name == "path" ? max_path_bytes : max_field_bytes;
        if (field.value.size() > limit) {
            return invalid_field(field.name, field.name == "path" ? "exceeds the 4096-byte limit"
                                                                  : "exceeds the 1024-byte limit");
        }
        if (!is_utf8(field.value)) {
            return invalid_field(field.name, "malformed UTF-8");
        }
    }

    std::string record;
    record.reserve(max_record_bytes);
    record.push_back('{');
    append_field(record, "level", level_text);
    append_field(record, "code", error_code_name(error.code));
    append_field(record, "category", error_category_name(error.code));
    for (const auto& field : fields) {
        if (!field.value.empty() || field.name == "message" || field.name == "operation") {
            append_field(record, field.name, field.value);
        }
    }
    record.append("}\n");
    // Six fields expand to at most six bytes per input byte plus fixed keys.
    static_assert((max_field_bytes * 5 + max_path_bytes) * 6 + 1024 < max_record_bytes);
    try {
        sink_.write(record.data(), static_cast<std::streamsize>(record.size()));
        sink_.flush();
        if (!sink_) {
            return sink_failure();
        }
    } catch (...) {
        // ostream can rethrow an arbitrary exception from its stream buffer.
        return sink_failure();
    }
    return {};
}

} // namespace shark
