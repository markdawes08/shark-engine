#pragma once

#include "shark/server/region_image.hpp"

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace shark::server::rpc_detail {
struct Failure {
    Error error;
};
[[noreturn]] inline void fail(const char* message) {
    throw Failure{{ErrorCode::conflict, message, {"server.region.process"}}};
}
template <class T> T need(Result<T> result) {
    if (!result)
        throw Failure{std::move(result).error()};
    return std::move(result).value();
}
inline void need(Result<void> result) {
    if (!result)
        throw Failure{std::move(result).error()};
}
template <class T, class F> Result<T> boundary(F&& function) {
    try {
        if constexpr (std::is_void_v<T>) {
            function();
            return {};
        } else
            return function();
    } catch (const Failure& failure) {
        return failure.error;
    } catch (const std::exception&) {
        return Error{ErrorCode::execution_failed,
                     "Region process allocation or codec failure",
                     {"server.region.process"}};
    }
}
struct Writer {
    std::vector<std::byte> bytes;
    void u64(std::uint64_t value) {
        for (unsigned shift = 64; shift != 0; shift -= 8)
            bytes.push_back(static_cast<std::byte>((value >> (shift - 8)) & 255));
    }
    void blob(std::span<const std::byte> value) {
        if (value.size() > 16 * 1024 * 1024)
            fail("Oversized control payload");
        u64(value.size());
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    template <class T> void id(const T& value) {
        const auto text = value.value().to_string();
        blob(std::as_bytes(std::span{text.data(), text.size()}));
    }
    void real(double value) {
        if (!std::isfinite(value) || (value == 0 && std::signbit(value)))
            fail("Noncanonical control scalar");
        u64(std::bit_cast<std::uint64_t>(value));
    }
    void key(WorldCellKey value) {
        id(value.world);
        u64(std::bit_cast<std::uint64_t>(value.cell.x));
        u64(std::bit_cast<std::uint64_t>(value.cell.y));
        u64(std::bit_cast<std::uint64_t>(value.cell.z));
    }
    void peers(std::span<const RegionPeer> values) {
        if (values.size() > movement_maximum_characters)
            fail("Too many control peers");
        u64(values.size());
        for (const auto& value : values) {
            u64(value.connection);
            blob(need(net::encode_session_reply(net::SessionReply{value.session})));
            const auto text = value.entity.value().to_string();
            blob(std::as_bytes(std::span{text}));
        }
    }
};
struct Reader {
    std::span<const std::byte> bytes;
    std::uint64_t u64() {
        if (bytes.size() < 8)
            fail("Truncated control payload");
        std::uint64_t result = 0;
        for (const auto byte : bytes.first(8))
            result = (result << 8) | std::to_integer<unsigned>(byte);
        bytes = bytes.subspan(8);
        return result;
    }
    std::span<const std::byte> blob() {
        const auto length = u64();
        if (length > bytes.size())
            fail("Truncated control field");
        const auto result = bytes.first(static_cast<std::size_t>(length));
        bytes = bytes.subspan(static_cast<std::size_t>(length));
        return result;
    }
    template <class T> T id() {
        const auto text = blob();
        if (text.size() != 36)
            fail("Invalid control identity");
        return T{need(StableId::parse({reinterpret_cast<const char*>(text.data()), text.size()}))};
    }
    double real() {
        const auto value = std::bit_cast<double>(u64());
        if (!std::isfinite(value) || (value == 0 && std::signbit(value)))
            fail("Noncanonical control scalar");
        return value;
    }
    bool flag() {
        const auto value = u64();
        if (value > 1)
            fail("Invalid control flag");
        return value == 1;
    }
    WorldCellKey key() {
        WorldCellKey value{id<WorldId>(),
                           {std::bit_cast<std::int64_t>(u64()), std::bit_cast<std::int64_t>(u64()),
                            std::bit_cast<std::int64_t>(u64())}};
        need(validate_world_position({value.world, value.cell, {0, 0, 0}}));
        return value;
    }
    MonotonicTime time() {
        const auto value = u64();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            fail("Invalid receiver clock");
        return MonotonicTime{static_cast<std::int64_t>(value)};
    }
    std::vector<RegionPeer> peers() {
        const auto count = u64();
        if (count > movement_maximum_characters)
            fail("Too many control peers");
        std::vector<RegionPeer> result;
        for (std::uint64_t i = 0; i < count; ++i) {
            const auto connection = u64();
            auto reply = need(net::decode_session_reply(blob()));
            const auto* session = std::get_if<net::SessionSnapshot>(&reply);
            if (!session || !session->active || !session->ready || connection == 0)
                fail("Invalid trusted peer observation");
            const auto text = blob();
            if (text.size() != 36)
                fail("Invalid peer entity");
            const auto entity = WorldEntityId{
                need(StableId::parse({reinterpret_cast<const char*>(text.data()), text.size()}))};
            for (const auto& prior : result)
                if (prior.connection == connection || prior.entity == entity ||
                    prior.session.session == session->session)
                    fail("Duplicate peer observation");
            result.push_back({connection, *session, entity});
        }
        return result;
    }
    void end() const {
        if (!bytes.empty())
            fail("Trailing control payload");
    }
};
enum class Op : std::uint64_t {
    stage = 1,
    activate,
    inspect,
    freeze,
    prepare_move,
    prepare_combat,
    commit_move,
    commit_combat,
    cancel,
    step,
    close,
    cell_prefetch,
    cell_cancel,
    cell_take,
    cell_drain,
    cell_activate,
    cell_deactivate,
    cell_evict,
    cell_usage,
    pursuit_set,
    pursuit_cancel,
    combat_view
};
} // namespace shark::server::rpc_detail
