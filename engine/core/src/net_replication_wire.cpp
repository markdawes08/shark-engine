#include "shark/net/replication.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <string_view>

namespace shark::net {
namespace {
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

Error invalid(const char* operation) {
    return {ErrorCode::invalid_argument,
            "Invalid replication protocol value",
            {.operation = operation}};
}
void append_u64(std::vector<std::byte>& bytes, std::uint64_t value) {
    for (unsigned index = 0; index < 8; ++index)
        bytes.push_back(static_cast<std::byte>((value >> ((7U - index) * 8U)) & 0xffU));
}
std::uint64_t read_u64(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index)
        value = (value << 8U) | std::to_integer<std::uint64_t>(bytes[offset + index]);
    return value;
}
void append_id(std::vector<std::byte>& bytes, const StableId& id) {
    bytes.insert(bytes.end(), id.bytes().begin(), id.bytes().end());
}
Result<StableId> read_id(std::span<const std::byte> bytes, std::size_t offset) {
    constexpr char digits[] = "0123456789abcdef";
    std::array<char, 36> text{};
    std::size_t position = 0;
    for (std::size_t index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            text[position++] = '-';
        const auto byte = std::to_integer<unsigned>(bytes[offset + index]);
        text[position++] = digits[byte >> 4U];
        text[position++] = digits[byte & 0xfU];
    }
    return StableId::parse(std::string_view{text.data(), text.size()});
}
std::vector<std::byte> prefix(std::uint8_t kind, std::size_t capacity) {
    std::vector<std::byte> bytes;
    bytes.reserve(capacity);
    bytes.push_back(static_cast<std::byte>(replication_protocol_version >> 8U));
    bytes.push_back(static_cast<std::byte>(replication_protocol_version & 0xffU));
    bytes.push_back(static_cast<std::byte>(kind));
    bytes.push_back(std::byte{0});
    return bytes;
}
bool header(std::span<const std::byte> bytes) {
    return bytes.size() >= 4 && bytes[0] == std::byte{0} && bytes[1] == std::byte{1} &&
           (bytes[2] == std::byte{1} || bytes[2] == std::byte{2}) && bytes[3] == std::byte{0};
}
bool canonical(const ReplicatedPose& pose) {
    if (!canonicalize_replicated_pose(pose))
        return false;
    for (double value : pose.coefficients)
        if (value == 0 && std::signbit(value))
            return false;
    return true;
}
} // namespace

Result<ReplicatedPose> canonicalize_replicated_pose(ReplicatedPose pose) {
    math::Mat4 matrix{1.0};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            auto& value = pose.coefficients[static_cast<std::size_t>(row * 4 + column)];
            if (value == 0)
                value = 0.0;
            matrix[column][row] = value;
        }
    }
    if (!validate_transform_matrix(matrix))
        return invalid("net.replication.pose");
    return pose;
}

Result<void> validate_replication_update(const ReplicationUpdate& update) {
    constexpr auto operation = "net.replication.validate";
    if (update.ownership_epoch == 0 || update.revision == 0 ||
        update.changes.size() > replication_max_changes ||
        (update.baseline ? update.base_revision != 0
                         : (update.base_revision == 0 || update.base_revision >= update.revision ||
                            update.changes.empty())))
        return invalid(operation);
    std::uint64_t previous = 0;
    std::size_t poses = 0;
    std::size_t despawns = 0;
    for (const auto& record : update.changes) {
        if (record.id.value <= previous)
            return invalid(operation);
        previous = record.id.value;
        switch (record.kind) {
        case ReplicationChange::spawn:
        case ReplicationChange::update:
            if ((update.baseline && record.kind != ReplicationChange::spawn) || !record.pose ||
                !canonical(*record.pose) || ++poses > replication_max_entities)
                return invalid(operation);
            break;
        case ReplicationChange::despawn:
            if (update.baseline || record.pose || ++despawns > replication_max_entities)
                return invalid(operation);
            break;
        default:
            return invalid(operation);
        }
    }
    return {};
}

Result<std::vector<std::byte>> encode_replication_update(const ReplicationUpdate& update) {
    auto valid = validate_replication_update(update);
    if (!valid)
        return valid.error();
    auto bytes = prefix(update.baseline ? 1 : 2, 3742);
    append_id(bytes, update.session.value());
    append_u64(bytes, update.ownership_epoch);
    append_id(bytes, update.stream.value());
    append_u64(bytes, update.revision);
    append_u64(bytes, update.base_revision);
    bytes.push_back(static_cast<std::byte>(update.changes.size() >> 8U));
    bytes.push_back(static_cast<std::byte>(update.changes.size() & 0xffU));
    for (const auto& record : update.changes) {
        bytes.push_back(static_cast<std::byte>(record.kind));
        append_u64(bytes, record.id.value);
        if (record.pose) {
            bytes.push_back(std::byte{1});
            for (double value : record.pose->coefficients)
                append_u64(bytes, std::bit_cast<std::uint64_t>(value));
        }
    }
    return bytes;
}

Result<ReplicationUpdate> decode_replication_update(std::span<const std::byte> bytes) {
    constexpr auto operation = "net.replication.decode-update";
    if (bytes.size() < replication_update_header_bytes || bytes.size() > 3742 || !header(bytes))
        return invalid(operation);
    const auto count =
        (std::to_integer<std::size_t>(bytes[60]) << 8U) | std::to_integer<std::size_t>(bytes[61]);
    if (count > replication_max_changes ||
        count > (bytes.size() - replication_update_header_bytes) / 9)
        return invalid(operation);
    auto session = read_id(bytes, 4);
    auto stream = read_id(bytes, 28);
    if (!session || !stream)
        return invalid(operation);
    ReplicationUpdate update{SessionId{session.value()},
                             read_u64(bytes, 20),
                             ReplicaSessionId{stream.value()},
                             read_u64(bytes, 44),
                             read_u64(bytes, 52),
                             bytes[2] == std::byte{1},
                             {}};
    update.changes.reserve(count);
    std::size_t offset = replication_update_header_bytes;
    std::size_t poses = 0;
    std::size_t despawns = 0;
    for (std::size_t index = 0; index < count; ++index) {
        if (bytes.size() - offset < 9)
            return invalid(operation);
        const auto kind = static_cast<ReplicationChange>(bytes[offset]);
        ReplicationRecord record{kind, {read_u64(bytes, offset + 1)}, {}};
        offset += 9;
        if (kind == ReplicationChange::spawn || kind == ReplicationChange::update) {
            if (++poses > replication_max_entities || bytes.size() - offset < 97 ||
                bytes[offset] != std::byte{1})
                return invalid(operation);
            ++offset;
            ReplicatedPose pose;
            for (auto& value : pose.coefficients) {
                value = std::bit_cast<double>(read_u64(bytes, offset));
                offset += 8;
            }
            record.pose = pose;
        } else if (kind != ReplicationChange::despawn || ++despawns > replication_max_entities) {
            return invalid(operation);
        }
        update.changes.push_back(record);
    }
    if (offset != bytes.size())
        return invalid(operation);
    auto valid = validate_replication_update(update);
    if (!valid)
        return valid.error();
    return update;
}

Result<std::vector<std::byte>> encode_replication_control(const ReplicationControl& control) {
    constexpr auto operation = "net.replication.encode-control";
    const auto* ack = std::get_if<ReplicationAck>(&control);
    const auto* resync = std::get_if<ReplicationResync>(&control);
    if (!ack && !resync)
        return invalid(operation);
    const auto& authority = ack ? ack->authority : resync->authority;
    const auto revision = ack ? ack->revision : resync->known_revision;
    if (authority.ownership_epoch == 0 || authority.sequence == 0 || (ack && revision == 0))
        return invalid(operation);
    auto bytes = prefix(ack ? 1 : 2, replication_control_bytes);
    append_id(bytes, authority.session.value());
    append_u64(bytes, authority.ownership_epoch);
    append_u64(bytes, authority.sequence);
    append_id(bytes, authority.character.value());
    append_id(bytes, (ack ? ack->stream : resync->stream).value());
    append_u64(bytes, revision);
    return bytes;
}

Result<ReplicationControl> decode_replication_control(std::span<const std::byte> bytes) {
    constexpr auto operation = "net.replication.decode-control";
    if (bytes.size() != replication_control_bytes || !header(bytes))
        return invalid(operation);
    auto session = read_id(bytes, 4);
    auto character = read_id(bytes, 36);
    auto stream = read_id(bytes, 52);
    if (!session || !character || !stream)
        return invalid(operation);
    SessionEnvelope envelope{SessionId{session.value()}, read_u64(bytes, 20), read_u64(bytes, 28),
                             CharacterId{character.value()}};
    const auto revision = read_u64(bytes, 68);
    if (envelope.ownership_epoch == 0 || envelope.sequence == 0 ||
        (bytes[2] == std::byte{1} && revision == 0))
        return invalid(operation);
    if (bytes[2] == std::byte{1})
        return ReplicationControl{
            ReplicationAck{envelope, ReplicaSessionId{stream.value()}, revision}};
    return ReplicationControl{
        ReplicationResync{envelope, ReplicaSessionId{stream.value()}, revision}};
}
} // namespace shark::net
