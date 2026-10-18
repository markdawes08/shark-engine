#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace shark::detail {

// Zero is permanently exhausted. The final valid token is issued exactly once.
inline std::uint64_t claim_scene_instance(std::atomic<std::uint64_t>& next) noexcept {
    auto value = next.load(std::memory_order_relaxed);
    while (value != 0) {
        const auto following = value == std::numeric_limits<std::uint64_t>::max() ? 0 : value + 1;
        if (next.compare_exchange_weak(value, following, std::memory_order_relaxed)) {
            return value;
        }
    }
    return 0;
}

// A false result retires the slot. Never recycle a generation after overflow.
inline bool advance_entity_generation(std::uint64_t& value) noexcept {
    if (value == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    ++value;
    return true;
}

} // namespace shark::detail
