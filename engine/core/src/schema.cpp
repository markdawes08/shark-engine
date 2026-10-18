#include "shark/schema.hpp"

#include <limits>
#include <utility>

namespace shark {
namespace {

const std::array<ComponentSchema, 2>& catalog() {
    constexpr auto minimum = std::numeric_limits<double>::lowest();
    constexpr auto maximum = std::numeric_limits<double>::max();
    constexpr auto writable = PropertyMutability::scene_owner_write;
    const Transform defaults;
    static const std::array<ComponentSchema, 2> schemas{
        ComponentSchema{ComponentSchemaId::name,
                        "shark.name",
                        "Name",
                        component_schema_version,
                        true,
                        false,
                        {{PropertyId::name_value, "value", "Value", PropertyType::utf8_text,
                          PropertyUnit::none, writable, Name{}.value,
                          TextConstraints{1, maximum_name_bytes, true, true, false, false}}}},
        ComponentSchema{
            ComponentSchemaId::transform,
            "shark.transform",
            "Transform",
            component_schema_version,
            true,
            false,
            {{PropertyId::transform_translation, "translation", "Translation",
              PropertyType::float64_vector3, PropertyUnit::meters, writable,
              std::array{defaults.translation.x, defaults.translation.y, defaults.translation.z},
              Vector3Constraints{minimum, maximum, std::nullopt, true}},
             {PropertyId::transform_rotation, "rotation", "Rotation",
              PropertyType::float64_quaternion_xyzw, PropertyUnit::dimensionless, writable,
              std::array{defaults.rotation.x, defaults.rotation.y, defaults.rotation.z,
                         defaults.rotation.w},
              QuaternionConstraints{minimum, maximum, true, true, true,
                                    QuaternionSignPolicy::positive_w_then_xyz}},
             {PropertyId::transform_scale, "scale", "Scale", PropertyType::float64_vector3,
              PropertyUnit::dimensionless, writable,
              std::array{defaults.scale.x, defaults.scale.y, defaults.scale.z},
              Vector3Constraints{minimum, maximum, transform_minimum_scale, true}}}}};
    return schemas;
}

Error schema_error(const char* operation, const char* message) {
    return Error{ErrorCode::invalid_argument, message, ErrorContext{operation}};
}

Result<std::size_t> schema_index(const ComponentSchemaId id, const std::uint32_t version,
                                 const char* operation) {
    std::size_t index = 0;
    switch (id) {
    case ComponentSchemaId::name:
        break;
    case ComponentSchemaId::transform:
        index = 1;
        break;
    default:
        return schema_error(operation, "Unknown component schema ID");
    }
    if (version != component_schema_version) {
        return schema_error(operation,
                            "Unsupported component schema version; supported version is 1");
    }
    return index;
}

} // namespace

std::array<ComponentSchema, 2> component_schemas() {
    return catalog();
}

Result<ComponentSchema> component_schema(const ComponentSchemaId id, const std::uint32_t version) {
    auto index = schema_index(id, version, "schema.lookup");
    if (!index) {
        return index.error();
    }
    return catalog()[index.value()];
}

Result<PropertySchema> property_schema(const ComponentSchemaId component, const PropertyId property,
                                       const std::uint32_t version) {
    auto index = schema_index(component, version, "schema.property");
    if (!index) {
        return index.error();
    }
    for (const auto& candidate : catalog()[index.value()].properties) {
        if (candidate.id == property) {
            return candidate;
        }
    }
    return schema_error("schema.property", "Property ID is not declared by this component schema");
}

Result<ComponentValue> validate_component(const ComponentSchemaId id, const std::uint32_t version,
                                          ComponentValue value) {
    auto index = schema_index(id, version, "schema.validate");
    if (!index) {
        return index.error();
    }
    if (id == ComponentSchemaId::name) {
        const auto* name = std::get_if<Name>(&value);
        if (!name) {
            return schema_error("schema.validate", "Name schema requires a Name value");
        }
        auto valid = validate_name(*name);
        if (!valid) {
            return valid.error();
        }
        return value;
    }
    const auto* transform = std::get_if<Transform>(&value);
    if (!transform) {
        return schema_error("schema.validate", "Transform schema requires a Transform value");
    }
    auto canonical = canonicalize_transform(*transform);
    if (!canonical) {
        return canonical.error();
    }
    return ComponentValue{std::move(canonical).value()};
}

} // namespace shark
