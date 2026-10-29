#pragma once

#include "shark/error.hpp"
#include "shark/scene_render.hpp"
#include "shark/tools/asset_import.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace shark::tools {

class AuthoringCaptureProvider;

struct AuthoringProtocolOptions {
    std::filesystem::path root;
    // Trusted host-startup configuration, never programs selected by an RPC.
    // Both paths are required to enable native Windows standalone export.
    std::filesystem::path export_python;
    std::filesystem::path export_tool;
    // Borrowed application provider; it must outlive the protocol owner.
    AuthoringCaptureProvider* capture{nullptr};
};

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

struct AuthoringViewportRequest {
    AuthoredEntityId camera;
    SceneRevision expected_revision;
    RenderViewport viewport{640, 360};
    math::Vec3 local_origin{0.0};
    std::uint64_t origin_generation{1};
    AssetTargetProfile target;
};

// Creating-thread CPU adapter over ProjectFiles and one AuthoredScene. The root
// is fixed at creation; project.open acquires its existing exclusive file lock
// until project.close or destruction. No native handles or mutable scene borrow
// cross this boundary. There is no listener, player protocol or GUI dependency.
class AuthoringProtocol final {
  public:
    [[nodiscard]] static Result<std::unique_ptr<AuthoringProtocol>>
    create(std::filesystem::path root = {});
    [[nodiscard]] static Result<std::unique_ptr<AuthoringProtocol>>
    create(AuthoringProtocolOptions options);
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

    // CPU-only read of this retained owner, including unsaved edits. Extracts
    // original scene identities with immutable fresh client asset leases; no
    // mutable Scene borrow, source write, implicit catalog scan/import or GPU.
    // The caller explicitly selects a camera, scene revision and content target.
    // Dimensions are 1..2048, local origin is bounded by render_coordinate_limit,
    // and the source world anchor is retained independently of that local offset.
    // Imported content requires an explicitly populated retained asset catalog.
    [[nodiscard]] Result<RenderScene> viewport_snapshot(const AuthoringViewportRequest& request);

  private:
    struct Impl;
    explicit AuthoringProtocol(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace shark::tools
