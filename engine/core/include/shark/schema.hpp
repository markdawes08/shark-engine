#pragma once

#include "shark/name.hpp"
#include "shark/render_components.hpp"
#include "shark/transform.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace shark {

// Stable schema discriminators, not serialized C++ layouts or protocol versions.
enum class ComponentSchemaId : std::uint32_t {
    name = 1,
    transform = 2,
    camera = 3,
    mesh_renderer = 4
};
enum class PropertyId : std::uint32_t {
    name_value = 1,
    transform_translation = 2,
    transform_rotation = 3,
    transform_scale = 4,
    camera_projection = 5,
    camera_vertical_fov_radians = 6,
    camera_orthographic_height = 7,
    camera_near_clip = 8,
    camera_far_clip = 9,
    mesh_renderer_asset = 10,
    mesh_renderer_mesh_index = 11,
    mesh_renderer_visible = 12,
};
inline constexpr std::uint32_t component_schema_version = 1;

enum class PropertyType : std::uint32_t {
    utf8_text = 1,
    float64_vector3 = 2,
    float64_quaternion_xyzw = 3,
    enumeration = 4,
    float64 = 5,
    asset_id = 6,
    uint32 = 7,
    boolean = 8,
};
enum class PropertyUnit : std::uint32_t { none = 0, meters = 1, dimensionless = 2, radians = 3 };
// Describes existing Scene setters, not gameplay authority or a public adapter.
enum class PropertyMutability : std::uint32_t { scene_owner_write = 1 };
enum class QuaternionSignPolicy : std::uint32_t { positive_w_then_xyz = 1 };

struct TextConstraints {
    std::size_t minimum_bytes;
    std::size_t maximum_bytes;
    bool require_utf8;
    bool forbid_c0_c1_del;
    bool unique;
    bool normalizes_unicode;
    bool operator==(const TextConstraints&) const = default;
};

// Inclusive per-component input range. The absolute floor, when present, also
// permits negative values; it is not a positive lower bound on signed scale.
struct Vector3Constraints {
    double minimum_component;
    double maximum_component;
    std::optional<double> minimum_absolute_component;
    bool finite_only;
    bool operator==(const Vector3Constraints&) const = default;
};

// Range applies to input, before normalization. Values/defaults are x,y,z,w;
// construction of math::Quat must explicitly reorder them to w,x,y,z.
struct QuaternionConstraints {
    double minimum_component;
    double maximum_component;
    bool finite_only;
    bool require_nonzero;
    bool normalizes_input;
    QuaternionSignPolicy canonical_sign;
    bool operator==(const QuaternionConstraints&) const = default;
};

struct EnumerationValue {
    std::uint32_t value;
    std::string key;
    bool operator==(const EnumerationValue&) const = default;
};
struct EnumerationConstraints {
    std::vector<EnumerationValue> values;
    bool operator==(const EnumerationConstraints&) const = default;
};
// Component validation also enforces camera's projection-dependent near bound
// and far > near relationship. These per-field ranges are not that policy.
struct ScalarConstraints {
    double minimum;
    double maximum;
    bool minimum_inclusive;
    bool maximum_inclusive;
    bool finite_only;
    bool operator==(const ScalarConstraints&) const = default;
};
struct AssetIdConstraints {
    bool required;
    bool uuid_v4;
    bool operator==(const AssetIdConstraints&) const = default;
};
struct Uint32Constraints {
    std::uint32_t minimum;
    std::uint32_t maximum;
    bool operator==(const Uint32Constraints&) const = default;
};
struct BooleanConstraints {
    bool operator==(const BooleanConstraints&) const = default;
};

// monostate means the caller must supply a value (MeshRenderer's asset ID).
using PropertyDefault = std::variant<std::string, std::array<double, 3>, std::array<double, 4>,
                                     std::uint32_t, double, AssetId, bool, std::monostate>;
using PropertyConstraints =
    std::variant<TextConstraints, Vector3Constraints, QuaternionConstraints, EnumerationConstraints,
                 ScalarConstraints, AssetIdConstraints, Uint32Constraints, BooleanConstraints>;

struct PropertySchema {
    PropertyId id;
    std::string key;
    std::string display_name;
    PropertyType type;
    PropertyUnit unit;
    PropertyMutability mutability;
    PropertyDefault default_value;
    PropertyConstraints constraints;
    bool operator==(const PropertySchema&) const = default;
};

struct ComponentSchema {
    ComponentSchemaId id;
    std::string stable_id;
    std::string display_name;
    std::uint32_t version;
    bool required;
    bool removable;
    std::vector<PropertySchema> properties;
    bool operator==(const ComponentSchema&) const = default;
};

// Fixed engine-owned catalog in stable ID/property order. Every result owns its
// strings/defaults/metadata; editing a returned copy cannot alter the catalog.
[[nodiscard]] std::array<ComponentSchema, 4> component_schemas();
[[nodiscard]] Result<ComponentSchema>
component_schema(ComponentSchemaId id, std::uint32_t version = component_schema_version);
[[nodiscard]] Result<PropertySchema>
property_schema(ComponentSchemaId component, PropertyId property,
                std::uint32_t version = component_schema_version);

using ComponentValue = std::variant<Name, Transform, Camera, MeshRenderer>;

// Pure local validation returns an owned, canonical value using the same Name
// and Transform validators as Scene. It does not validate world-matrix/hierarchy
// feasibility, reserve a revision, mutate a scene or authorize a player request.
// Unknown IDs, versions and mismatched value types reject explicitly.
[[nodiscard]] Result<ComponentValue> validate_component(ComponentSchemaId id, std::uint32_t version,
                                                        ComponentValue value);

} // namespace shark
