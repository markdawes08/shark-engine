#pragma once

#include "shark/error.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace shark::net {

// Independent gameplay-wire version, not an authoring/schema/file version.
inline constexpr std::uint16_t wire_version = 1;
inline constexpr std::size_t wire_header_bytes = 20;
inline constexpr std::size_t wire_max_payload = 4096;
inline constexpr std::size_t wire_max_frame_bytes = wire_header_bytes + wire_max_payload;

enum class MessageKind : std::uint8_t { hello = 1, hello_ack = 2, probe = 3, probe_reply = 4 };

// M02a diagnostic messages only; no authentication or gameplay authorization.
// Hello/hello_ack have empty payloads. Probe/probe_reply hold 0..4096 opaque bytes.
struct Frame {
    MessageKind kind{MessageKind::hello};
    std::uint64_t sequence{0};
    std::vector<std::byte> payload{};
    bool operator==(const Frame&) const = default;
};

// Explicit network byte order: SHRK[4], version u16, kind u8, reserved flags u8
// (zero), nonzero sequence u64, payload length u32, then exactly that many bytes.
// Never copies a native struct representation. decode_frame requires one complete
// frame and rejects both truncation and trailing bytes.
[[nodiscard]] Result<std::vector<std::byte>> encode_frame(const Frame& frame);
[[nodiscard]] Result<Frame> decode_frame(std::span<const std::byte> bytes);

struct DecodeStep {
    std::size_t consumed{0};
    std::optional<Frame> frame{};
};

// At most one owned frame per call. Continue with input.subspan(consumed) when
// bytes remain; incomplete input is retained within a fixed 4116-byte buffer.
// Validate the header before reading its body or allocating a payload. Invalid
// input poisons the decoder: close the transport rather than scan for new magic.
// Construct a fresh decoder only for a new transport; finish checks clean EOF.
class FrameDecoder final {
  public:
    [[nodiscard]] Result<DecodeStep> consume(std::span<const std::byte> bytes);
    [[nodiscard]] Result<void> finish();
    [[nodiscard]] std::size_t buffered_bytes() const noexcept {
        return buffered_;
    }
    [[nodiscard]] bool failed() const noexcept {
        return failed_;
    }

  private:
    std::array<std::byte, wire_max_frame_bytes> buffer_{};
    std::size_t buffered_{0};
    std::size_t target_{wire_header_bytes};
    bool header_checked_{false};
    bool failed_{false};
};

// Each reliable ordered TLS/TCP direction starts at sequence 1 and advances by
// exactly one. The returned value replaces last only on success; zero, gaps,
// duplicates/reordering and exhaustion reject without wrap. Keep independent
// send/receive values and reset them only for a new transport. This is neither
// authentication nor an application-commit ACK; hello_ack/probe_reply are
// diagnostic responses, and no unreliable channel is implied.
[[nodiscard]] Result<std::uint64_t> next_sequence(std::uint64_t last);
[[nodiscard]] Result<std::uint64_t> check_sequence(std::uint64_t last, std::uint64_t candidate);

} // namespace shark::net
