#include <shark/scene.hpp>
#include <shark/schema.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using shark::ComponentSchemaId;
using shark::PropertyId;
using Triple = std::array<double, 3>;
using QuaternionArray = std::array<double, 4>;

shark::PropertySchema property(const ComponentSchemaId component, const PropertyId id) {
    auto result = shark::property_schema(component, id);
    REQUIRE(result);
    return std::move(result).value();
}

template <typename T>
void schema_failure(const shark::Result<T>& result, const std::string& operation) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == shark::ErrorCode::invalid_argument);
    CHECK(result.error().context.operation == operation);
}

void name_agrees(const shark::Name& name) {
    const auto direct = shark::validate_name(name);
    const auto schema = shark::validate_component(ComponentSchemaId::name, 1, name);
    REQUIRE(schema.has_value() == direct.has_value());
    if (direct) {
        REQUIRE(std::holds_alternative<shark::Name>(schema.value()));
        CHECK(std::get<shark::Name>(schema.value()) == name);
    } else {
        CHECK(schema.error().code == direct.error().code);
        CHECK(schema.error().message == direct.error().message);
        CHECK(schema.error().context.operation == direct.error().context.operation);
    }
}

void transform_agrees(const shark::Transform& transform) {
    const auto direct = shark::canonicalize_transform(transform);
    const auto schema = shark::validate_component(ComponentSchemaId::transform, 1, transform);
    REQUIRE(schema.has_value() == direct.has_value());
    if (direct) {
        REQUIRE(std::holds_alternative<shark::Transform>(schema.value()));
        CHECK(std::get<shark::Transform>(schema.value()) == direct.value());
    } else {
        CHECK(schema.error().code == direct.error().code);
        CHECK(schema.error().message == direct.error().message);
        CHECK(schema.error().context.operation == direct.error().context.operation);
    }
}

shark::StableId fixed_id(const std::string& text) {
    auto result = shark::StableId::parse(text);
    REQUIRE(result);
    return result.value();
}

std::unique_ptr<shark::Scene> scene() {
    auto result = shark::Scene::create(shark::AuthoredSceneScope{
        shark::SceneId{fixed_id("00000000-0000-4000-8000-000000000001")}});
    REQUIRE(result);
    return std::move(result).value();
}

shark::EntitySeed seed() {
    return {shark::AuthoredEntityId{fixed_id("00000000-0000-4000-8000-000000000002")},
            std::nullopt,
            {},
            {},
            std::nullopt};
}

} // namespace

TEST_CASE("schema catalog publishes stable required component and typed property identities",
          "[schema]") {
    const auto catalog = shark::component_schemas();
    REQUIRE(catalog.size() == 4);
    CHECK(catalog[0].id == ComponentSchemaId::name);
    CHECK(catalog[0].stable_id == "shark.name");
    CHECK(catalog[0].display_name == "Name");
    CHECK(catalog[1].id == ComponentSchemaId::transform);
    CHECK(catalog[1].stable_id == "shark.transform");
    CHECK(catalog[1].display_name == "Transform");
    CHECK(static_cast<std::uint32_t>(catalog[0].id) == 1);
    CHECK(static_cast<std::uint32_t>(catalog[1].id) == 2);
    CHECK(shark::component_schema_version == 1);
    for (const auto& component : catalog) {
        CHECK(component.version == 1);
        const bool mandatory =
            component.id == ComponentSchemaId::name || component.id == ComponentSchemaId::transform;
        CHECK(component.required == mandatory);
        CHECK(component.removable == !mandatory);
        const auto lookup = shark::component_schema(component.id, component.version);
        REQUIRE(lookup);
        CHECK(lookup.value() == component);
        for (const auto& field : component.properties) {
            CHECK(field.mutability == shark::PropertyMutability::scene_owner_write);
            CHECK_FALSE(field.display_name.empty());
            CHECK(property(component.id, field.id) == field);
        }
    }
    REQUIRE(catalog[0].properties.size() == 1);
    REQUIRE(catalog[1].properties.size() == 3);
    const auto& name = catalog[0].properties[0];
    const auto& translation = catalog[1].properties[0];
    const auto& rotation = catalog[1].properties[1];
    const auto& scale = catalog[1].properties[2];
    CHECK(name.id == PropertyId::name_value);
    CHECK(translation.id == PropertyId::transform_translation);
    CHECK(rotation.id == PropertyId::transform_rotation);
    CHECK(scale.id == PropertyId::transform_scale);
    CHECK(static_cast<std::uint32_t>(name.id) == 1);
    CHECK(static_cast<std::uint32_t>(translation.id) == 2);
    CHECK(static_cast<std::uint32_t>(rotation.id) == 3);
    CHECK(static_cast<std::uint32_t>(scale.id) == 4);
    CHECK(name.key == "value");
    CHECK(translation.key == "translation");
    CHECK(rotation.key == "rotation");
    CHECK(scale.key == "scale");
    CHECK(name.type == shark::PropertyType::utf8_text);
    CHECK(translation.type == shark::PropertyType::float64_vector3);
    CHECK(rotation.type == shark::PropertyType::float64_quaternion_xyzw);
    CHECK(scale.type == shark::PropertyType::float64_vector3);
    CHECK(name.unit == shark::PropertyUnit::none);
    CHECK(translation.unit == shark::PropertyUnit::meters);
    CHECK(rotation.unit == shark::PropertyUnit::dimensionless);
    CHECK(scale.unit == shark::PropertyUnit::dimensionless);
}

TEST_CASE("schema defaults reconstruct existing components with explicit xyzw quaternion order",
          "[schema]") {
    const auto name = property(ComponentSchemaId::name, PropertyId::name_value);
    const auto translation =
        property(ComponentSchemaId::transform, PropertyId::transform_translation);
    const auto rotation = property(ComponentSchemaId::transform, PropertyId::transform_rotation);
    const auto scale = property(ComponentSchemaId::transform, PropertyId::transform_scale);
    const auto& text = std::get<std::string>(name.default_value);
    const auto& position = std::get<Triple>(translation.default_value);
    const auto& xyzw = std::get<QuaternionArray>(rotation.default_value);
    const auto& dimensions = std::get<Triple>(scale.default_value);
    CHECK(text == "Entity");
    CHECK(position == Triple{0, 0, 0});
    CHECK(xyzw == QuaternionArray{0, 0, 0, 1});
    CHECK(dimensions == Triple{1, 1, 1});
    const shark::Name default_name{text};
    const shark::Transform default_transform{{position[0], position[1], position[2]},
                                             {xyzw[3], xyzw[0], xyzw[1], xyzw[2]},
                                             {dimensions[0], dimensions[1], dimensions[2]}};
    CHECK(default_name == shark::Name{});
    CHECK(default_transform == shark::Transform{});
    name_agrees(default_name);
    transform_agrees(default_transform);
    auto owner = scene();
    const auto entity = owner->create_entity(seed());
    REQUIRE(entity);
    const auto state = owner->entity_state(entity.value());
    REQUIRE(state);
    CHECK(state.value().name == default_name);
    CHECK(state.value().local_transform == default_transform);
}

TEST_CASE("schema metadata and defaults are owned copies isolated from later discovery",
          "[schema]") {
    const auto original = shark::component_schemas();
    auto changed = shark::component_schemas();
    changed[0].stable_id = "caller-edited";
    changed[0].version = 9;
    changed[0].required = false;
    changed[0].removable = true;
    changed[0].properties[0].key = "other";
    std::get<std::string>(changed[0].properties[0].default_value) = "Caller";
    std::get<shark::TextConstraints>(changed[0].properties[0].constraints).maximum_bytes = 1;
    std::get<Triple>(changed[1].properties[0].default_value)[0] = 99;
    changed[1].properties.clear();
    auto single = shark::component_schema(ComponentSchemaId::transform);
    REQUIRE(single);
    single.value().properties[1].default_value = QuaternionArray{1, 2, 3, 4};
    auto field = property(ComponentSchemaId::transform, PropertyId::transform_scale);
    std::get<shark::Vector3Constraints>(field.constraints).minimum_absolute_component = 99;
    CHECK(shark::component_schemas() == original);
    const auto reloaded = shark::component_schema(ComponentSchemaId::transform);
    REQUIRE(reloaded);
    CHECK(reloaded.value() == original[1]);
    CHECK(property(ComponentSchemaId::transform, PropertyId::transform_scale) ==
          original[1].properties[2]);
}

TEST_CASE("schema lookups and validation reject unknown identities versions and value types",
          "[schema]") {
    for (const auto unknown : std::array{0U, 5U, std::numeric_limits<std::uint32_t>::max()}) {
        const auto id = static_cast<ComponentSchemaId>(unknown);
        schema_failure(shark::component_schema(id), "schema.lookup");
        schema_failure(shark::property_schema(id, PropertyId::name_value), "schema.property");
        schema_failure(shark::validate_component(id, 1, shark::Name{}), "schema.validate");
    }
    for (const auto version : std::array{0U, 2U, std::numeric_limits<std::uint32_t>::max()}) {
        for (const auto id :
             std::array{ComponentSchemaId::name, ComponentSchemaId::transform,
                        ComponentSchemaId::camera, ComponentSchemaId::mesh_renderer}) {
            const auto lookup = shark::component_schema(id, version);
            schema_failure(lookup, "schema.lookup");
            CHECK(lookup.error().message.find("supported version is 1") != std::string::npos);
            schema_failure(shark::property_schema(id, PropertyId::name_value, version),
                           "schema.property");
            schema_failure(shark::validate_component(id, version, shark::Name{}),
                           "schema.validate");
        }
    }
    for (const auto unknown : std::array{0U, 5U, std::numeric_limits<std::uint32_t>::max()}) {
        for (const auto id : std::array{ComponentSchemaId::name, ComponentSchemaId::transform}) {
            schema_failure(shark::property_schema(id, static_cast<PropertyId>(unknown)),
                           "schema.property");
        }
    }
    for (const auto field :
         std::array{PropertyId::transform_translation, PropertyId::transform_rotation,
                    PropertyId::transform_scale}) {
        schema_failure(shark::property_schema(ComponentSchemaId::name, field), "schema.property");
    }
    schema_failure(shark::property_schema(ComponentSchemaId::transform, PropertyId::name_value),
                   "schema.property");
    schema_failure(shark::validate_component(ComponentSchemaId::name, 1, shark::Transform{}),
                   "schema.validate");
    schema_failure(shark::validate_component(ComponentSchemaId::transform, 1, shark::Name{}),
                   "schema.validate");
}

TEST_CASE("Name schema constraints agree with UTF8 byte and control validation", "[schema]") {
    const auto field = property(ComponentSchemaId::name, PropertyId::name_value);
    const auto rules = std::get<shark::TextConstraints>(field.constraints);
    CHECK(rules.minimum_bytes == 1);
    CHECK(rules.maximum_bytes == 256);
    CHECK(rules.maximum_bytes == shark::maximum_name_bytes);
    CHECK(rules.require_utf8);
    CHECK(rules.forbid_c0_c1_del);
    CHECK_FALSE(rules.unique);
    CHECK_FALSE(rules.normalizes_unicode);
    const std::vector<shark::Name> names{
        {"a"},
        {std::string(rules.maximum_bytes, 'x')},
        {std::string(rules.maximum_bytes + 1, 'x')},
        {""},
        {" "},
        {"A\nB"},
        {std::string("A\0B", 3)},
        {std::string(1, '\x7f')},
        {"\xc2\x80"},
        {"\xc0\xaf"},
        {"\xed\xa0\x80"},
        {"\xe2\x82"},
        {"\xc3\xa9"},
        {"e\xcc\x81"},
        {"\xf0\x9f\x8c\x8a"},
    };
    for (const auto& name : names) {
        name_agrees(name);
    }
    std::string multibyte;
    for (std::size_t index = 0; index < rules.maximum_bytes / 2; ++index) {
        multibyte += "\xc3\xa9";
    }
    REQUIRE(multibyte.size() == rules.maximum_bytes);
    CHECK(shark::validate_component(ComponentSchemaId::name, 1, shark::Name{multibyte}));
    multibyte += "\xc3\xa9";
    CHECK_FALSE(shark::validate_component(ComponentSchemaId::name, 1, shark::Name{multibyte}));
}

TEST_CASE("Transform schema ranges allow signed scale at the absolute floor and finite doubles",
          "[schema]") {
    const auto translation =
        property(ComponentSchemaId::transform, PropertyId::transform_translation);
    const auto scale = property(ComponentSchemaId::transform, PropertyId::transform_scale);
    const auto position_rules = std::get<shark::Vector3Constraints>(translation.constraints);
    const auto scale_rules = std::get<shark::Vector3Constraints>(scale.constraints);
    for (const auto& rules : std::array{position_rules, scale_rules}) {
        CHECK(rules.minimum_component == std::numeric_limits<double>::lowest());
        CHECK(rules.maximum_component == std::numeric_limits<double>::max());
        CHECK(rules.finite_only);
    }
    CHECK_FALSE(position_rules.minimum_absolute_component);
    REQUIRE(scale_rules.minimum_absolute_component);
    CHECK(*scale_rules.minimum_absolute_component == 1e-6);
    CHECK(*scale_rules.minimum_absolute_component == shark::transform_minimum_scale);
    const auto floor = *scale_rules.minimum_absolute_component;
    for (const auto magnitude :
         std::array{floor, std::nextafter(floor, 1.0), 1.0, std::numeric_limits<double>::max()}) {
        for (const auto sign : std::array{-1.0, 1.0}) {
            shark::Transform value;
            value.scale = {sign * magnitude, magnitude, -magnitude};
            CHECK(shark::validate_component(ComponentSchemaId::transform, 1, value));
            transform_agrees(value);
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        for (const auto value :
             std::array{0.0, -0.0, std::nextafter(floor, 0.0), -std::nextafter(floor, 0.0)}) {
            shark::Transform candidate;
            candidate.scale[axis] = value;
            CHECK_FALSE(shark::validate_component(ComponentSchemaId::transform, 1, candidate));
            transform_agrees(candidate);
        }
    }
    shark::Transform extreme;
    extreme.translation = {position_rules.minimum_component, position_rules.maximum_component,
                           std::numeric_limits<double>::denorm_min()};
    CHECK(shark::validate_component(ComponentSchemaId::transform, 1, extreme));
    transform_agrees(extreme);
}

TEST_CASE("Transform schema delegates finite and quaternion canonicalization rules", "[schema]") {
    const auto rotation = property(ComponentSchemaId::transform, PropertyId::transform_rotation);
    const auto rules = std::get<shark::QuaternionConstraints>(rotation.constraints);
    CHECK(rules.minimum_component == std::numeric_limits<double>::lowest());
    CHECK(rules.maximum_component == std::numeric_limits<double>::max());
    CHECK(rules.finite_only);
    CHECK(rules.require_nonzero);
    CHECK(rules.normalizes_input);
    CHECK(rules.canonical_sign == shark::QuaternionSignPolicy::positive_w_then_xyz);
    for (const auto magnitude : std::array{std::numeric_limits<double>::denorm_min(), 2.0,
                                           std::numeric_limits<double>::max()}) {
        shark::Transform candidate;
        candidate.translation = {-0.0, 0.0, -0.0};
        candidate.rotation = {-magnitude, magnitude, -magnitude, magnitude};
        const auto result = shark::validate_component(ComponentSchemaId::transform, 1, candidate);
        REQUIRE(result);
        const auto& canonical = std::get<shark::Transform>(result.value());
        CHECK(canonical.rotation == shark::math::Quat{0.5, -0.5, 0.5, -0.5});
        CHECK_FALSE(std::signbit(canonical.translation.x));
        CHECK_FALSE(std::signbit(canonical.translation.z));
        transform_agrees(candidate);
    }
    for (int nonzero = 0; nonzero < 3; ++nonzero) {
        shark::Transform candidate;
        candidate.rotation = {0, 0, 0, 0};
        candidate.rotation[nonzero] = -2;
        const auto result = shark::validate_component(ComponentSchemaId::transform, 1, candidate);
        REQUIRE(result);
        const auto& canonical = std::get<shark::Transform>(result.value());
        CHECK(canonical.rotation[nonzero] == 1);
        CHECK(canonical.rotation.w == 0);
        CHECK_FALSE(std::signbit(canonical.rotation.w));
        transform_agrees(candidate);
    }
    shark::Transform zero;
    zero.rotation = {0, 0, 0, 0};
    CHECK_FALSE(shark::validate_component(ComponentSchemaId::transform, 1, zero));
    transform_agrees(zero);
    for (const auto invalid : std::array{std::numeric_limits<double>::infinity(),
                                         -std::numeric_limits<double>::infinity(),
                                         std::numeric_limits<double>::quiet_NaN()}) {
        for (int axis = 0; axis < 3; ++axis) {
            shark::Transform candidate;
            candidate.translation[axis] = invalid;
            CHECK_FALSE(shark::validate_component(ComponentSchemaId::transform, 1, candidate));
            transform_agrees(candidate);
            candidate = {};
            candidate.scale[axis] = invalid;
            CHECK_FALSE(shark::validate_component(ComponentSchemaId::transform, 1, candidate));
            transform_agrees(candidate);
        }
        for (int axis = 0; axis < 4; ++axis) {
            shark::Transform candidate;
            candidate.rotation[axis] = invalid;
            CHECK_FALSE(shark::validate_component(ComponentSchemaId::transform, 1, candidate));
            transform_agrees(candidate);
        }
    }
}

TEST_CASE("pure schema validation agrees with Scene setters and preserves owned results",
          "[schema]") {
    auto owner = scene();
    const auto entity = owner->create_entity(seed());
    REQUIRE(entity);
    for (const auto& name : std::vector<shark::Name>{
             {"Player"}, {"\xc3\xa9"}, {""}, {"line\n"}, {std::string(257, 'a')}}) {
        const auto before = owner->stats();
        REQUIRE(before);
        auto validated = shark::validate_component(ComponentSchemaId::name, 1, name);
        const auto after_validation = owner->stats();
        REQUIRE(after_validation);
        CHECK(after_validation.value().revision == before.value().revision);
        const auto written = owner->set_name(entity.value(), name);
        REQUIRE(written.has_value() == validated.has_value());
        if (validated) {
            const auto state = owner->entity_state(entity.value());
            REQUIRE(state);
            CHECK(state.value().name == std::get<shark::Name>(validated.value()));
            std::get<shark::Name>(validated.value()).value = "Only the result changes";
            const auto reloaded = owner->entity_state(entity.value());
            REQUIRE(reloaded);
            CHECK(reloaded.value().name == name);
        }
    }
    std::vector<shark::Transform> values(4);
    values[0].translation = {2, -3, 4};
    values[0].rotation = {-2, 0, 0, 0};
    values[0].scale = {-2, 3, 1};
    values[1].scale = {1e-6, -1e-6, 1};
    values[2].scale.x = 0;
    values[3].rotation = {0, 0, 0, 0};
    for (const auto& value : values) {
        const auto before = owner->stats();
        REQUIRE(before);
        const auto validated = shark::validate_component(ComponentSchemaId::transform, 1, value);
        const auto after_validation = owner->stats();
        REQUIRE(after_validation);
        CHECK(after_validation.value().revision == before.value().revision);
        const auto written = owner->set_transform(entity.value(), value);
        REQUIRE(written.has_value() == validated.has_value());
        if (validated) {
            const auto state = owner->entity_state(entity.value());
            REQUIRE(state);
            CHECK(state.value().local_transform == std::get<shark::Transform>(validated.value()));
        }
    }
}

TEST_CASE("local schema validity does not promise hierarchy world feasibility", "[schema]") {
    auto owner = scene();
    auto parent_seed = seed();
    parent_seed.transform.scale = {1e200, 1e200, 1e200};
    const auto parent = owner->create_entity(parent_seed);
    REQUIRE(parent);
    auto child_seed = seed();
    child_seed.identity = shark::AuthoredEntityId{fixed_id("00000000-0000-4000-8000-000000000003")};
    child_seed.parent = parent.value();
    const auto child = owner->create_entity(child_seed);
    REQUIRE(child);
    const auto before = owner->stats();
    REQUIRE(before);
    shark::Transform local;
    local.scale = {1e200, 1e200, 1e200};
    const auto valid = shark::validate_component(ComponentSchemaId::transform, 1, local);
    REQUIRE(valid);
    CHECK_FALSE(owner->set_transform(child.value(), std::get<shark::Transform>(valid.value())));
    const auto after = owner->stats();
    REQUIRE(after);
    CHECK(after.value().revision == before.value().revision);
    const auto state = owner->entity_state(child.value());
    REQUIRE(state);
    CHECK(state.value().local_transform == shark::Transform{});
}
