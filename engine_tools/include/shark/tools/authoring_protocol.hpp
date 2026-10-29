#pragma once

#include "shark/error.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace shark::tools {

inline constexpr std::uint32_t authoring_protocol_version = 1;
inline constexpr std::size_t authoring_protocol_maximum_request_bytes = 64 * 1024;
inline constexpr std::size_t authoring_protocol_maximum_response_bytes = 1024 * 1024;
inline constexpr std::size_t authoring_protocol_maximum_cursors = 32;

struct AuthoringProtocolReply {
    // One JSON-RPC object without transport framing. Valid notifications return
    // an empty string, including notifications whose operation fails.
    std::string json;
    bool failed;
};

// Creating-thread CPU adapter over ProjectFiles and one AuthoredScene. The root
// is fixed at creation; project.open acquires its existing exclusive file lock
// until project.close or destruction. No native handles or mutable scene borrow
// cross this boundary. There is no listener, player protocol or GUI dependency.
class AuthoringProtocol final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<AuthoringProtocol>>
    create(std::filesystem::path root = {});
    ~AuthoringProtocol();
    AuthoringProtocol(const AuthoringProtocol&) = delete;
    AuthoringProtocol& operator=(const AuthoringProtocol&) = delete;
    AuthoringProtocol(AuthoringProtocol&&) = delete;
    AuthoringProtocol& operator=(AuthoringProtocol&&) = delete;

    // The outer Result reports service-access/output failures. Protocol and
    // operation failures are serialized replies with failed=true, including
    // nested retained decisions carried by a JSON-RPC result. Unsupported RPC
    // batches never dispatch. Scene mutations use the shared retained ledger;
    // uncertain/lost output requires outcome lookup or file reconciliation,
    // never an assumption that a failed reply rolled back a dispatched write.
    [[nodiscard]] Result<AuthoringProtocolReply> handle(std::string_view request);

  private:
    struct Impl;
    explicit AuthoringProtocol(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::tools
