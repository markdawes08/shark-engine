#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace shark::detail {

// Shared scalar validation for Core text boundaries. Existing path/diagnostic
// callers accept controls; display names additionally reject C0, DEL and C1.
inline bool valid_utf8(std::string_view text, bool reject_controls = false) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = static_cast<std::uint8_t>(text[index]);
        if (first < 0x80U) {
            if (reject_controls && (first < 0x20U || first == 0x7fU)) {
                return false;
            }
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
        if (point < minimum || point > 0x10ffffU || (point >= 0xd800U && point <= 0xdfffU) ||
            (reject_controls && point >= 0x80U && point <= 0x9fU)) {
            return false;
        }
        index += count;
    }
    return true;
}

} // namespace shark::detail
