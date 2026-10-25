#pragma once

#include "shark/tools/authored_commands.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace shark::tools {

inline constexpr std::size_t authoring_maximum_history_entries = 32;
inline constexpr std::size_t authoring_maximum_history_bytes = 64 * 1024 * 1024;

struct AuthoringHistoryLimits {
    std::size_t max_entries{32};
    // Conservative retained snapshot charge, not allocator overhead or process RSS.
    std::size_t max_retained_bytes{32 * 1024 * 1024};
};

struct AuthoringHistoryTicket {
    std::uint64_t value;
    auto operator<=>(const AuthoringHistoryTicket&) const = default;
};

struct AuthoringHistoryGroup {
    AuthoringHistoryTicket ticket;
    std::size_t command_count;
};

struct AuthoringHistoryStatus {
    SceneRevision revision;
    bool synchronized;
    std::size_t undo_entries;
    std::size_t redo_entries;
    std::size_t retained_bytes;
    AuthoringHistoryLimits limits;
    std::optional<AuthoringHistoryGroup> next_undo;
    std::optional<AuthoringHistoryGroup> next_redo;
};

struct AuthoringHistoryReply {
    AuthoringHistoryTicket ticket;
    SceneRevision observed_revision;
    Result<AuthoringCommandEffect> outcome;
};

} // namespace shark::tools
