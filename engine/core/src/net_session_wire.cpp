#include "shark/net/session.hpp"

#include <array>
#include <string_view>

namespace shark::net {
namespace {
Error invalid(const char* operation, ErrorCode code = ErrorCode::invalid_argument) {
    return {code, "Invalid session protocol payload", {.operation = operation}};
}

void append_u64(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index) {
        bytes.push_back(static_cast<std::byte>((value >> ((7U - index) * 8U)) & 0xffU));
    }
}

std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) | std::to_integer<std::uint64_t>(bytes[offset + index]);
    }
    return value;
}

void append_id(std::vector<std::byte>& bytes, const StableId& id) {
    const auto& value = id.bytes();
    bytes.insert(bytes.end(), value.begin(), value.end());
}

Result<StableId> read_id(std::span<const std::byte> bytes, std::size_t offset,
                         const char* operation) {
    constexpr char digits[] = "0123456789abcdef";
    std::array<char, 36> text{};
    std::size_t position = 0;
    for (std::size_t index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            text[position++] = '-';
        }
        const auto byte = std::to_integer<unsigned>(bytes[offset + index]);
        text[position++] = digits[byte >> 4U];
        text[position++] = digits[byte & 0xfU];
    }
    auto parsed = StableId::parse(std::string_view{text.data(), text.size()});
    if (!parsed) {
        return invalid(operation, ErrorCode::invalid_id);
    }
    return parsed.value();
}

std::vector<std::byte> prefix(std::uint8_t kind, std::size_t capacity) {
    std::vector<std::byte> bytes;
    bytes.reserve(capacity);
    bytes.push_back(static_cast<std::byte>(session_protocol_version >> 8U));
    bytes.push_back(static_cast<std::byte>(session_protocol_version & 0xffU));
    bytes.push_back(static_cast<std::byte>(kind));
    bytes.push_back(std::byte{0});
    return bytes;
}

Result<std::uint8_t> header(std::span<const std::byte> bytes, std::size_t maximum,
                            const char* operation) {
    if (bytes.size() < 4 || bytes.size() > maximum || bytes[0] != std::byte{0} ||
        bytes[1] != static_cast<std::byte>(session_protocol_version) || bytes[3] != std::byte{0}) {
        return invalid(operation);
    }
    return std::to_integer<std::uint8_t>(bytes[2]);
}

bool valid_envelope(const SessionEnvelope& envelope) {
    return envelope.ownership_epoch != 0 && envelope.sequence != 0;
}

void append_envelope(std::vector<std::byte>& bytes, const SessionEnvelope& envelope) {
    append_id(bytes, envelope.session.value());
    append_u64(bytes, envelope.ownership_epoch);
    append_u64(bytes, envelope.sequence);
    append_id(bytes, envelope.character.value());
}

Result<SessionEnvelope> read_envelope(std::span<const std::byte> bytes, const char* operation) {
    auto session = read_id(bytes, 4, operation);
    auto character = read_id(bytes, 36, operation);
    if (!session) {
        return session.error();
    }
    if (!character) {
        return character.error();
    }
    SessionEnvelope value{SessionId{session.value()}, read_u64(bytes, 20), read_u64(bytes, 28),
                          CharacterId{character.value()}};
    if (!valid_envelope(value)) {
        return invalid(operation);
    }
    return value;
}

bool valid_snapshot(const SessionSnapshot& snapshot) {
    return snapshot.ownership_epoch != 0 && (!snapshot.ready || snapshot.last_sequence != 0) &&
           (snapshot.active || (!snapshot.ready && snapshot.last_sequence != 0));
}
} // namespace

Result<std::vector<std::byte>> encode_session_request(const SessionRequest& request) {
    constexpr auto operation = "net.session-wire.encode-request";
    if (const auto* login = std::get_if<LoginRequest>(&request)) {
        auto bytes = prefix(1, 36);
        append_id(bytes, login->account.value());
        append_id(bytes, login->character.value());
        return bytes;
    }
    const SessionEnvelope* envelope = nullptr;
    std::uint8_t kind = 0;
    if (const auto* ready = std::get_if<SetReadyRequest>(&request)) {
        envelope = &ready->authority;
        kind = 2;
    } else if (const auto* heartbeat = std::get_if<HeartbeatRequest>(&request)) {
        envelope = &heartbeat->authority;
        kind = 3;
    } else if (const auto* logout = std::get_if<LogoutRequest>(&request)) {
        envelope = &logout->authority;
        kind = 4;
    }
    if (!envelope || !valid_envelope(*envelope)) {
        return invalid(operation);
    }
    auto bytes = prefix(kind, kind == 2 ? 53 : 52);
    append_envelope(bytes, *envelope);
    if (const auto* ready = std::get_if<SetReadyRequest>(&request)) {
        bytes.push_back(ready->ready ? std::byte{1} : std::byte{0});
    }
    return bytes;
}

Result<SessionRequest> decode_session_request(std::span<const std::byte> bytes) {
    constexpr auto operation = "net.session-wire.decode-request";
    auto kind = header(bytes, session_max_request_bytes, operation);
    if (!kind) {
        return kind.error();
    }
    if (kind.value() == 1) {
        if (bytes.size() != 36) {
            return invalid(operation);
        }
        auto account = read_id(bytes, 4, operation);
        auto character = read_id(bytes, 20, operation);
        if (!account) {
            return account.error();
        }
        if (!character) {
            return character.error();
        }
        return SessionRequest{
            LoginRequest{AccountId{account.value()}, CharacterId{character.value()}}};
    }
    if (kind.value() < 2 || kind.value() > 4 || bytes.size() != (kind.value() == 2 ? 53U : 52U)) {
        return invalid(operation);
    }
    auto envelope = read_envelope(bytes, operation);
    if (!envelope) {
        return envelope.error();
    }
    if (kind.value() == 2) {
        if (bytes[52] != std::byte{0} && bytes[52] != std::byte{1}) {
            return invalid(operation);
        }
        return SessionRequest{SetReadyRequest{envelope.value(), bytes[52] == std::byte{1}}};
    }
    if (kind.value() == 3) {
        return SessionRequest{HeartbeatRequest{envelope.value()}};
    }
    return SessionRequest{LogoutRequest{envelope.value()}};
}

Result<std::vector<std::byte>> encode_session_reply(const SessionReply& reply) {
    constexpr auto operation = "net.session-wire.encode-reply";
    if (std::holds_alternative<SessionRejected>(reply)) {
        return prefix(2, 4);
    }
    const auto* snapshot = std::get_if<SessionSnapshot>(&reply);
    if (!snapshot || !valid_snapshot(*snapshot)) {
        return invalid(operation);
    }
    auto bytes = prefix(1, session_max_reply_bytes);
    append_id(bytes, snapshot->session.value());
    append_id(bytes, snapshot->account.value());
    append_id(bytes, snapshot->character.value());
    append_u64(bytes, snapshot->ownership_epoch);
    append_u64(bytes, snapshot->last_sequence);
    bytes.push_back(snapshot->ready ? std::byte{1} : std::byte{0});
    bytes.push_back(snapshot->active ? std::byte{1} : std::byte{0});
    return bytes;
}

Result<SessionReply> decode_session_reply(std::span<const std::byte> bytes) {
    constexpr auto operation = "net.session-wire.decode-reply";
    auto kind = header(bytes, session_max_reply_bytes, operation);
    if (!kind) {
        return kind.error();
    }
    if (kind.value() == 2 && bytes.size() == 4) {
        return SessionReply{SessionRejected{}};
    }
    if (kind.value() != 1 || bytes.size() != session_max_reply_bytes ||
        (bytes[68] != std::byte{0} && bytes[68] != std::byte{1}) ||
        (bytes[69] != std::byte{0} && bytes[69] != std::byte{1})) {
        return invalid(operation);
    }
    auto session = read_id(bytes, 4, operation);
    auto account = read_id(bytes, 20, operation);
    auto character = read_id(bytes, 36, operation);
    if (!session) {
        return session.error();
    }
    if (!account) {
        return account.error();
    }
    if (!character) {
        return character.error();
    }
    SessionSnapshot snapshot{
        SessionId{session.value()}, AccountId{account.value()}, CharacterId{character.value()},
        read_u64(bytes, 52),        read_u64(bytes, 60),        bytes[68] == std::byte{1},
        bytes[69] == std::byte{1}};
    if (!valid_snapshot(snapshot)) {
        return invalid(operation);
    }
    return SessionReply{snapshot};
}

} // namespace shark::net
