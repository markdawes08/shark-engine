#pragma once

#include "shark/scene_query.hpp"
#include "shark/tools/authoring_retries.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string_view>

namespace shark::tools::authoring_wire {

// Private adapter values. Resolve durable IDs through the active owner only
// after parameter validation; native handles never enter the wire contract.
struct Query {
    SceneQueryOptions options{};
    std::optional<AuthoredEntityId> parent_id{};
};

struct Inspect {
    AuthoredEntityId entity_id;
    SceneQueryFields fields{};
    std::optional<SceneRevision> expected_revision{};
};

[[nodiscard]] Result<Query> parse_query(const nlohmann::json& params);
[[nodiscard]] Result<Inspect> parse_inspect(const nlohmann::json& params);

struct Preview {
    AuthoringBatch batch;
    SceneRevision expected_revision;
};

// Decode structure without canonicalizing command values: the shared owner
// validates them and retains the exact typed input and its terminal decision.
[[nodiscard]] Result<SceneRevision> parse_revision(const nlohmann::json& value);
[[nodiscard]] Result<std::optional<SceneRevision>>
parse_optional_revision(const nlohmann::json& params);
[[nodiscard]] Result<AuthoringMutationId> parse_mutation_id(const nlohmann::json& value);
[[nodiscard]] Result<AuthoringMutationRequest> parse_mutation(const nlohmann::json& params);
[[nodiscard]] Result<Preview> parse_preview(const nlohmann::json& params);

[[nodiscard]] nlohmann::json history_json(const AuthoringHistoryStatus& value);
[[nodiscard]] nlohmann::json mutation_status_json(const AuthoringMutationStatus& value);
[[nodiscard]] nlohmann::json mutation_reply_json(const AuthoringMutationReply& value);
[[nodiscard]] nlohmann::json batch_reply_json(const AuthoringBatchReply& value);

[[nodiscard]] nlohmann::json mutation_params_schema();
[[nodiscard]] nlohmann::json preview_params_schema();
[[nodiscard]] nlohmann::json mutation_reply_schema();
[[nodiscard]] nlohmann::json history_schema();
[[nodiscard]] nlohmann::json mutation_status_schema();
[[nodiscard]] nlohmann::json batch_reply_schema();

// Returns the catalog array; the service owns the method-result envelope.
[[nodiscard]] Result<nlohmann::json> component_schemas_json();
[[nodiscard]] nlohmann::json revision_json(SceneRevision revision);

// The service retains field selection alongside its opaque continuation token.
// This distinguishes an unselected optional component from a selected null.
[[nodiscard]] Result<nlohmann::json>
page_json(const Scene& scene, const SceneQueryPage& page, SceneQueryFields fields,
          std::optional<std::string_view> next_token = std::nullopt);

} // namespace shark::tools::authoring_wire
