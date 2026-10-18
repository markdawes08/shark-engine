#include "shark/net/wire.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace shark::net {
namespace {

constexpr std::array magic{std::byte{0x53}, std::byte{0x48}, std::byte{0x52}, std::byte{0x4b}};

Error wire_error(const char* operation, const char* message) {
    return Error{ErrorCode::invalid_argument, message, ErrorContext{operation}};
}

void write_integer(const std::span<std::byte> bytes, const std::uint64_t value) {
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto shift = (bytes.size() - 1 - index) * 8;
        bytes[index] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

std::uint64_t read_integer(const std::span<const std::byte> bytes) {
    std::uint64_t result = 0;
    for (const auto byte : bytes) {
        result = (result << 8U) | std::to_integer<std::uint8_t>(byte);
    }
    return result;
}

Result<void> validate_fields(const MessageKind kind, const std::uint64_t sequence,
                             const std::size_t payload_size, const char* operation) {
    switch (kind) {
    case MessageKind::hello:
    case MessageKind::hello_ack:
        if (payload_size != 0) {
            return wire_error(operation, "Hello messages require an empty payload");
        }
        break;
    case MessageKind::probe:
    case MessageKind::probe_reply:
    case MessageKind::session_request:
    case MessageKind::session_reply:
        break;
    default:
        return wire_error(operation, "Unknown wire message kind");
    }
    if (sequence == 0) {
        return wire_error(operation, "Wire sequence must be nonzero");
    }
    if (payload_size > wire_max_payload) {
        return wire_error(operation, "Wire payload exceeds the 4096-byte limit");
    }
    return {};
}

struct Header {
    MessageKind kind;
    std::uint64_t sequence;
    std::size_t payload_size;
};

Result<Header> parse_header(const std::span<const std::byte> bytes) {
    constexpr auto operation = "net.wire.decode";
    if (bytes.size() < wire_header_bytes) {
        return wire_error(operation, "Truncated wire header");
    }
    if (!std::equal(magic.begin(), magic.end(), bytes.begin())) {
        return wire_error(operation, "Invalid wire magic");
    }
    if (read_integer(bytes.subspan(4, 2)) != wire_version) {
        return wire_error(operation, "Unsupported gameplay-wire version; supported version is 1");
    }
    if (bytes[7] != std::byte{0}) {
        return wire_error(operation, "Reserved wire flags must be zero");
    }
    const auto kind = static_cast<MessageKind>(std::to_integer<std::uint8_t>(bytes[6]));
    const auto sequence = read_integer(bytes.subspan(8, 8));
    const auto payload_size = read_integer(bytes.subspan(16, 4));
    // Check the untrusted width before conversion, addition or body allocation.
    if (payload_size > wire_max_payload) {
        return wire_error(operation, "Wire payload exceeds the 4096-byte limit");
    }
    const auto size = static_cast<std::size_t>(payload_size);
    if (auto valid = validate_fields(kind, sequence, size, operation); !valid) {
        return valid.error();
    }
    return Header{kind, sequence, size};
}

} // namespace

Result<std::vector<std::byte>> encode_frame(const Frame& frame) {
    if (auto valid =
            validate_fields(frame.kind, frame.sequence, frame.payload.size(), "net.wire.encode");
        !valid) {
        return valid.error();
    }
    std::vector<std::byte> bytes(wire_header_bytes + frame.payload.size());
    const std::span header{bytes.data(), wire_header_bytes};
    std::copy(magic.begin(), magic.end(), header.begin());
    write_integer(header.subspan(4, 2), wire_version);
    header[6] = static_cast<std::byte>(frame.kind);
    header[7] = std::byte{0};
    write_integer(header.subspan(8, 8), frame.sequence);
    write_integer(header.subspan(16, 4), frame.payload.size());
    std::copy(frame.payload.begin(), frame.payload.end(), bytes.begin() + wire_header_bytes);
    return bytes;
}

Result<Frame> decode_frame(const std::span<const std::byte> bytes) {
    auto header = parse_header(bytes);
    if (!header) {
        return header.error();
    }
    const auto expected = wire_header_bytes + header.value().payload_size;
    if (bytes.size() != expected) {
        return wire_error("net.wire.decode", bytes.size() < expected
                                                 ? "Truncated wire payload"
                                                 : "Trailing bytes after wire frame");
    }
    const auto payload = bytes.subspan(wire_header_bytes);
    return Frame{header.value().kind, header.value().sequence, {payload.begin(), payload.end()}};
}

Result<DecodeStep> FrameDecoder::consume(const std::span<const std::byte> bytes) {
    if (failed_) {
        return wire_error("net.wire.decode", "Wire decoder is failed; close this transport");
    }
    if (bytes.empty()) {
        return DecodeStep{};
    }
    std::size_t consumed = 0;
    for (;;) {
        const auto count = std::min(bytes.size() - consumed, target_ - buffered_);
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(consumed), count,
                    buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_));
        consumed += count;
        buffered_ += count;
        if (buffered_ < target_) {
            return DecodeStep{consumed, std::nullopt};
        }
        if (!header_checked_) {
            auto header = parse_header(std::span{buffer_.data(), wire_header_bytes});
            if (!header) {
                failed_ = true;
                return header.error();
            }
            target_ = wire_header_bytes + header.value().payload_size;
            header_checked_ = true;
            if (buffered_ < target_) {
                continue;
            }
        }
        auto frame = decode_frame(std::span{buffer_.data(), target_});
        if (!frame) {
            failed_ = true;
            return frame.error();
        }
        buffered_ = 0;
        target_ = wire_header_bytes;
        header_checked_ = false;
        return DecodeStep{consumed, std::move(frame).value()};
    }
}

Result<void> FrameDecoder::finish() {
    if (failed_) {
        return wire_error("net.wire.finish", "Wire decoder failed before end of stream");
    }
    if (buffered_ != 0) {
        failed_ = true;
        return wire_error("net.wire.finish", "Truncated wire frame at end of stream");
    }
    return {};
}

Result<std::uint64_t> next_sequence(const std::uint64_t last) {
    if (last == std::numeric_limits<std::uint64_t>::max()) {
        return Error{ErrorCode::conflict, "Wire sequence exhausted; a new transport is required",
                     ErrorContext{"net.sequence"}};
    }
    return last + 1;
}

Result<std::uint64_t> check_sequence(const std::uint64_t last, const std::uint64_t candidate) {
    if (candidate == 0) {
        return wire_error("net.sequence", "Wire sequence must be nonzero");
    }
    const auto expected = next_sequence(last);
    if (!expected) {
        return expected.error();
    }
    if (candidate != expected.value()) {
        return Error{ErrorCode::conflict, "Wire sequence must advance by exactly one",
                     ErrorContext{"net.sequence"}};
    }
    return candidate;
}

} // namespace shark::net
