#include "shark/schema.hpp"

#include <limits>
#include <numbers>
#include <utility>

namespace shark {
namespace {

const std::array<ComponentSchema, 6>& catalog() {
    constexpr auto minimum = std::numeric_limits<double>::lowest();
    constexpr auto maximum = std::numeric_limits<double>::max();
    constexpr auto writable = PropertyMutability::scene_owner_write;
    const Transform defaults;
    const Camera camera;
    const RigidBody body;
    static const std::array<ComponentSchema, 6> schemas{
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
              Vector3Constraints{minimum, maximum, transform_minimum_scale, true}}}},
        ComponentSchema{
            ComponentSchemaId::camera,
            "shark.camera",
            "Camera",
            component_schema_version,
            false,
            true,
            {{PropertyId::camera_projection, "projection", "Projection", PropertyType::enumeration,
              PropertyUnit::none, writable, static_cast<std::uint32_t>(camera.projection),
              EnumerationConstraints{{{0, "perspective"}, {1, "orthographic"}}}},
             {PropertyId::camera_vertical_fov_radians, "vertical_fov_radians",
              "Vertical field of view", PropertyType::float64, PropertyUnit::radians, writable,
              camera.vertical_fov_radians,
              ScalarConstraints{0.0, std::numbers::pi, false, false, true}},
             {PropertyId::camera_orthographic_height, "orthographic_height", "Orthographic height",
              PropertyType::float64, PropertyUnit::meters, writable, camera.orthographic_height,
              ScalarConstraints{0.0, maximum, false, true, true}},
             {PropertyId::camera_near_clip, "near_clip", "Near clip", PropertyType::float64,
              PropertyUnit::meters, writable, camera.near_clip,
              ScalarConstraints{0.0, maximum, true, true, true}},
             {PropertyId::camera_far_clip, "far_clip", "Far clip", PropertyType::float64,
              PropertyUnit::meters, writable, camera.far_clip,
              ScalarConstraints{0.0, maximum, false, true, true}}}},
        ComponentSchema{
            ComponentSchemaId::mesh_renderer,
            "shark.mesh_renderer",
            "MeshRenderer",
            component_schema_version,
            false,
            true,
            {{PropertyId::mesh_renderer_asset, "asset_id", "Asset", PropertyType::asset_id,
              PropertyUnit::none, writable, std::monostate{}, AssetIdConstraints{true, true}},
             {PropertyId::mesh_renderer_mesh_index, "mesh_index", "Mesh index",
              PropertyType::uint32, PropertyUnit::none, writable, std::uint32_t{0},
              Uint32Constraints{0, std::numeric_limits<std::uint32_t>::max()}},
             {PropertyId::mesh_renderer_visible, "visible", "Visible", PropertyType::boolean,
              PropertyUnit::none, writable, true, BooleanConstraints{}}}},
        ComponentSchema{
            ComponentSchemaId::rigid_body,
            "shark.rigid_body",
            "RigidBody",
            component_schema_version,
            false,
            true,
            {{PropertyId::rigid_body_motion, "motion", "Motion", PropertyType::enumeration,
              PropertyUnit::none, writable, std::uint32_t{0},
              EnumerationConstraints{{{0, "static"}, {1, "dynamic"}, {2, "kinematic"}}}},
             {PropertyId::rigid_body_shape, "shape", "Shape", PropertyType::enumeration,
              PropertyUnit::none, writable, std::uint32_t{0},
              EnumerationConstraints{{{0, "box"}, {1, "sphere"}, {2, "capsule"}}}},
             {PropertyId::rigid_body_half_extents, "half_extents", "Box half extents",
              PropertyType::float64_vector3, PropertyUnit::meters, writable,
              std::array{0.5, 0.5, 0.5},
              Vector3Constraints{physics_minimum_dimension, physics_maximum_dimension, std::nullopt,
                                 true}},
             {PropertyId::rigid_body_radius, "radius", "Radius", PropertyType::float64,
              PropertyUnit::meters, writable, body.radius,
              ScalarConstraints{physics_minimum_dimension, physics_maximum_dimension, true, true,
                                true}},
             {PropertyId::rigid_body_half_height, "half_height", "Capsule half height",
              PropertyType::float64, PropertyUnit::meters, writable, body.half_height,
              ScalarConstraints{physics_minimum_dimension, physics_maximum_dimension, true, true,
                                true}},
             {PropertyId::rigid_body_mass, "mass", "Mass", PropertyType::float64,
              PropertyUnit::kilograms, writable, body.mass,
              ScalarConstraints{0.01, physics_maximum_mass, true, true, true}},
             {PropertyId::rigid_body_friction, "friction", "Friction", PropertyType::float64,
              PropertyUnit::dimensionless, writable, body.friction,
              ScalarConstraints{0, 1, true, true, true}},
             {PropertyId::rigid_body_restitution, "restitution", "Restitution",
              PropertyType::float64, PropertyUnit::dimensionless, writable, body.restitution,
              ScalarConstraints{0, 1, true, true, true}},
             {PropertyId::rigid_body_collision_layer, "collision_layer", "Collision layer",
              PropertyType::uint32, PropertyUnit::none, writable, body.collision_layer,
              Uint32Constraints{0, 15}},
             {PropertyId::rigid_body_collision_mask, "collision_mask", "Collision mask",
              PropertyType::uint32, PropertyUnit::none, writable, body.collision_mask,
              Uint32Constraints{0, 0xffff}},
             {PropertyId::rigid_body_is_trigger, "is_trigger", "Trigger", PropertyType::boolean,
              PropertyUnit::none, writable, body.is_trigger, BooleanConstraints{}}}},
        ComponentSchema{
            ComponentSchemaId::script,
            "shark.script",
            "Script",
            component_schema_version,
            false,
            true,
            {{PropertyId::script_source, "source", "Source", PropertyType::utf8_text,
              PropertyUnit::none, writable, Script{}.source,
              TextConstraints{script_minimum_source_path_bytes, script_maximum_source_path_bytes,
                              true, true, false, false}},
             {PropertyId::script_enabled, "enabled", "Enabled", PropertyType::boolean,
              PropertyUnit::none, writable, true, BooleanConstraints{}}}}};
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
    case ComponentSchemaId::camera:
        index = 2;
        break;
    case ComponentSchemaId::mesh_renderer:
        index = 3;
        break;
    case ComponentSchemaId::rigid_body:
        index = 4;
        break;
    case ComponentSchemaId::script:
        index = 5;
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

std::array<ComponentSchema, 6> component_schemas() {
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
    if (id == ComponentSchemaId::camera) {
        const auto* camera = std::get_if<Camera>(&value);
        if (!camera) {
            return schema_error("schema.validate", "Camera schema requires a Camera value");
        }
        auto valid = validate_camera(*camera);
        if (!valid) {
            return valid.error();
        }
        return value;
    }
    if (id == ComponentSchemaId::mesh_renderer) {
        const auto* renderer = std::get_if<MeshRenderer>(&value);
        if (!renderer) {
            return schema_error("schema.validate",
                                "MeshRenderer schema requires a MeshRenderer value");
        }
        auto valid = validate_mesh_renderer(*renderer);
        if (!valid) {
            return valid.error();
        }
        return value;
    }
    if (id == ComponentSchemaId::rigid_body) {
        const auto* body = std::get_if<RigidBody>(&value);
        if (!body)
            return schema_error("schema.validate", "RigidBody schema requires a RigidBody value");
        auto valid = validate_rigid_body(*body);
        if (!valid)
            return valid.error();
        return value;
    }
    if (id == ComponentSchemaId::script) {
        const auto* script = std::get_if<Script>(&value);
        if (!script)
            return schema_error("schema.validate", "Script schema requires a Script value");
        auto valid = validate_script(*script);
        if (!valid)
            return valid.error();
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
