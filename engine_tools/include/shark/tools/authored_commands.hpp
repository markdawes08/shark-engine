#pragma once

#include "shark/scene.hpp"

#include <optional>
#include <variant>
#include <vector>

namespace shark::tools {

struct CreateEntityCommand {
    AuthoredEntityId id;
    Name name{};
    Transform transform{};
    std::optional<AuthoredEntityId> parent{};
};

struct DeleteEntityCommand {
    AuthoredEntityId id;
};

struct SetNameCommand {
    AuthoredEntityId id;
    Name name;
};

struct SetTransformCommand {
    AuthoredEntityId id;
    Transform transform;
};

struct ReparentEntityCommand {
    AuthoredEntityId id;
    std::optional<AuthoredEntityId> parent{};
    ReparentMode mode{ReparentMode::keep_local};
};

// Typed local authoring values, not a player or serialized authoring protocol.
// A command is one immediate edit; no batch, history or retry identity is implied.
using AuthoringCommand = std::variant<CreateEntityCommand, DeleteEntityCommand, SetNameCommand,
                                      SetTransformCommand, ReparentEntityCommand>;

struct AuthoringCommandEffect {
    bool changed;
    // Sorted unique stable IDs to refresh, including deleted descendants and
    // parents whose child links changed. Empty for a Core no-op. At most the
    // authored scene entity bound for one command. Batch unions add at most the
    // command bound in new identities. History conservatively includes the union
    // of both complete snapshots (at most twice the source entity bound).
    // This is not a list of new/current entities.
    std::vector<AuthoredEntityId> affected_entities;
};

struct AuthoringCommandReply {
    // Current committed revision after the decision, including a rejection.
    SceneRevision observed_revision;
    Result<AuthoringCommandEffect> outcome;
};

struct AuthoringBatchDomain;
using AuthoringBatchId = TypedId<AuthoringBatchDomain>;
inline constexpr std::size_t authoring_maximum_batch_commands = 256;

// Caller-supplied correlation identity, not retained retry/deduplication identity.
// Commands execute in order against the private candidate, then publish together.
struct AuthoringBatch {
    AuthoringBatchId id;
    std::vector<AuthoringCommand> commands;
};

struct AuthoringBatchReply {
    AuthoringBatchId id;
    // Preview always reports the unchanged live revision. Commit reports the
    // committed revision; no-op and rejected batches leave it unchanged.
    SceneRevision observed_revision;
    Result<AuthoringCommandEffect> outcome;
};

} // namespace shark::tools
