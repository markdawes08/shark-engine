#include "crypto_runtime.hpp"

#include <psa/crypto.h>
#include <psa/crypto_extra.h>

namespace shark::detail {

CryptoRuntime::~CryptoRuntime() {
    if (initialized)
        mbedtls_psa_crypto_free();
}
CryptoRuntime& crypto_runtime() {
    static CryptoRuntime runtime;
    return runtime;
}
Result<void> initialize_crypto() {
    if (!crypto_runtime().initialized) {
        if (psa_crypto_init() != PSA_SUCCESS) {
            return Error{ErrorCode::entropy_unavailable,
                         "TLS cryptographic initialization failed",
                         {.operation = "net.tls.initialize"}};
        }
        crypto_runtime().initialized = true;
    }
    return {};
}

} // namespace shark::detail
