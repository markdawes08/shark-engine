#pragma once

#include "shark/error.hpp"

#include <cstdint>
#include <mutex>

namespace shark::detail {

struct CryptoRuntime {
    std::mutex mutex;
    bool initialized{false};
    std::uint64_t last_connection_id{0};
    ~CryptoRuntime();
};

[[nodiscard]] CryptoRuntime& crypto_runtime();
// Caller holds crypto_runtime().mutex. All PSA users share this lifetime.
[[nodiscard]] Result<void> initialize_crypto();

} // namespace shark::detail
