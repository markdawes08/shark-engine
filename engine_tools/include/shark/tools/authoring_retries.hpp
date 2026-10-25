#pragma once

#include "shark/tools/authoring_history.hpp"

#include <memory>
#include <string_view>

namespace shark::tools {

using AuthoringMutationScope = TypedId<struct AuthoringMutationScopeDomain>;

struct AuthoringMutationId {
    AuthoringMutationScope scope;
    std::uint64_t sequence;
    auto operator<=>(const AuthoringMutationId&) const = default;
};

inline constexpr std::size_t authoring_maximum_mutation_entries = 64;
inline constexpr std::size_t authoring_maximum_mutation_bytes = 16 * 1024 * 1024;
inline constexpr std::size_t authoring_maximum_mutation_payload_bytes = 512 * 1024;
inline constexpr std::size_t authoring_maximum_mutation_name_bytes = 4096;

struct AuthoringMutationLimits {
    std::size_t max_entries{32};
    std::size_t max_retained_bytes{8 * 1024 * 1024};
};

struct UndoAuthoringMutation {
    AuthoringHistoryTicket ticket;
};
struct RedoAuthoringMutation {
    AuthoringHistoryTicket ticket;
};
struct ClearAuthoringHistoryMutation {};

using AuthoringMutation = std::variant<AuthoringCommand, AuthoringBatch, UndoAuthoringMutation,
                                       RedoAuthoringMutation, ClearAuthoringHistoryMutation>;

struct AuthoringMutationRequest {
    AuthoringMutationId id;
    SceneRevision expected_revision;
    AuthoringMutation mutation;
};

enum class AuthoringMutationState {
    recorded,
    retained,
    replayed,
    not_recorded,
    unknown_outcome,
    payload_mismatch,
    out_of_order,
    not_admitted,
};

[[nodiscard]] constexpr std::string_view
authoring_mutation_state_name(AuthoringMutationState state) noexcept {
    switch (state) {
    case AuthoringMutationState::recorded:
        return "recorded";
    case AuthoringMutationState::retained:
        return "retained";
    case AuthoringMutationState::replayed:
        return "replayed";
    case AuthoringMutationState::not_recorded:
        return "not_recorded";
    case AuthoringMutationState::unknown_outcome:
        return "unknown_outcome";
    case AuthoringMutationState::payload_mismatch:
        return "payload_mismatch";
    case AuthoringMutationState::out_of_order:
        return "out_of_order";
    case AuthoringMutationState::not_admitted:
        return "not_admitted";
    }
    return "unknown";
}

// Immutable retained data: the original revision/result, never a new live view.
struct AuthoringMutationOutcome {
    AuthoringMutationId id;
    SceneRevision observed_revision;
    Result<AuthoringCommandEffect> result;
    // Present only for a successful clear-history operation.
    std::optional<AuthoringHistoryStatus> history_after;
};

struct AuthoringMutationReply {
    AuthoringMutationId id;
    AuthoringMutationState state;
    // Current owner revision; a retained original outcome can have an older one.
    SceneRevision observed_revision;
    Result<std::shared_ptr<const AuthoringMutationOutcome>> outcome;
};

struct AuthoringMutationStatus {
    AuthoringMutationScope scope;
    SceneRevision revision;
    std::uint64_t high_watermark;
    std::optional<AuthoringMutationId> next;
    std::optional<AuthoringMutationId> oldest_retained;
    std::size_t retained_entries;
    // Conservative payload charge plus a fixed bounded outcome reservation.
    std::size_t retained_bytes;
    AuthoringMutationLimits limits;
};

} // namespace shark::tools
