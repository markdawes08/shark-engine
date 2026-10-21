#pragma once

#include "shark/name.hpp"
#include "shark/project_path.hpp"
#include "shark/scene_identity.hpp"
#include "shark/transform.hpp"
#include "shark/world_position.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace shark::tools {

using ProjectId = TypedId<struct ProjectIdDomain>;

inline constexpr std::uint32_t source_document_version = 1;
inline constexpr std::size_t source_maximum_bytes = 4 * 1024 * 1024;
inline constexpr std::size_t source_maximum_entities = 4096;
inline constexpr std::size_t source_maximum_scenes = 256;
inline constexpr std::size_t source_maximum_json_depth = 32;
inline constexpr std::size_t source_maximum_json_nodes = 200000;
inline constexpr std::size_t source_maximum_string_bytes = 4096;
inline constexpr std::size_t source_maximum_extras_bytes = 4096;
inline constexpr std::size_t source_maximum_total_extras_bytes = 65536;
inline constexpr std::size_t source_maximum_hierarchy_depth = 256;

// Value documents only: these neither own an active Scene nor perform file I/O.
// IDs are durable authored identity; process-local handles and authority/session
// metadata are never accepted or emitted by this format.
struct ProjectSceneEntry {
    SceneId scene;
    ProjectPath path;
    bool operator==(const ProjectSceneEntry&) const = default;
};

struct ProjectDocument {
    ProjectId project;
    WorldId world;
    Name name;
    SceneId startup_scene;
    std::vector<ProjectSceneEntry> scenes;
    // Opaque JSON object, validated and emitted canonically within fixed bounds.
    std::string extras_json{"{}"};
    bool operator==(const ProjectDocument&) const = default;
};

struct SourceEntity {
    AuthoredEntityId id;
    std::optional<AuthoredEntityId> parent;
    Name name{};
    Transform transform{};
    std::string extras_json{"{}"};
    bool operator==(const SourceEntity&) const = default;
};

struct SceneDocument {
    ProjectId project;
    SceneId scene;
    // Optional stable world/cell anchor for this authored scene's local frame.
    // It does not confer live region authority or implement origin shifting.
    std::optional<WorldPosition> world_origin;
    std::vector<SourceEntity> entities;
    std::string extras_json{"{}"};
    bool operator==(const SceneDocument&) const = default;
};

template <typename Document> struct DecodedSource {
    Document document;
    // 0 means an explicit in-memory 0-to-1 migration occurred. Input is untouched.
    std::uint32_t source_schema_version;
};

// Accept only documented schemas 0 and 1. Validate the entire bounded candidate,
// including references/hierarchy and existing pure component constraints. Local
// world-matrix feasibility and active-scene publication belong to Scene loading.
// Error context.path is a JSON pointer (empty at the document root).
[[nodiscard]] Result<DecodedSource<ProjectDocument>> decode_project_document(std::string_view text);
[[nodiscard]] Result<DecodedSource<SceneDocument>> decode_scene_document(std::string_view text);
[[nodiscard]] Result<ProjectDocument> validate_project_document(const ProjectDocument& document);
[[nodiscard]] Result<SceneDocument> validate_scene_document(const SceneDocument& document);

// Revalidate caller-owned candidates, then write schema 1 with deterministic
// two-space formatting, canonical extras and trailing newline. No file is opened.
[[nodiscard]] Result<std::string> encode_project_document(const ProjectDocument& document);
[[nodiscard]] Result<std::string> encode_scene_document(const SceneDocument& document);

} // namespace shark::tools
