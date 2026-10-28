#pragma once

#include "shark/scene_query.hpp"

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

// Returns the catalog array; the service owns the method-result envelope.
[[nodiscard]] Result<nlohmann::json> component_schemas_json();
[[nodiscard]] nlohmann::json revision_json(SceneRevision revision);

// The service retains field selection alongside its opaque continuation token.
// This distinguishes an unselected optional component from a selected null.
[[nodiscard]] Result<nlohmann::json>
page_json(const Scene& scene, const SceneQueryPage& page, SceneQueryFields fields,
          std::optional<std::string_view> next_token = std::nullopt);

} // namespace shark::tools::authoring_wire
