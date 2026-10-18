#include <shark/log.hpp>
#include <shark/name.hpp>
#include <shark/project_path.hpp>
#include <shark/scene.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

template <typename Id> Id id(std::uint32_t value) {
    std::string text = "00000000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 0xfU];
        value >>= 4U;
    }
    auto parsed = shark::StableId::parse(text);
    REQUIRE(parsed);
    return Id{parsed.value()};
}

shark::EntitySeed seed(const std::uint32_t value,
                       const std::optional<shark::EntityHandle> parent = std::nullopt,
                       const shark::Transform transform = {}) {
    return {id<shark::AuthoredEntityId>(value), std::nullopt, {}, transform, parent};
}

std::unique_ptr<shark::Scene> scene(const shark::SceneLimits limits = {}) {
    auto result = shark::Scene::create(shark::AuthoredSceneScope{id<shark::SceneId>(1)}, limits);
    REQUIRE(result);
    return std::move(result).value();
}

shark::EntityHandle create(shark::Scene& owner, shark::EntitySeed value) {
    auto result = owner.create_entity(std::move(value));
    REQUIRE(result);
    return result.value();
}

shark::SceneEntityState state(const shark::Scene& owner, const shark::EntityHandle handle) {
    auto result = owner.entity_state(handle);
    REQUIRE(result);
    return std::move(result).value();
}

shark::SceneStats stats(const shark::Scene& owner) {
    const auto result = owner.stats();
    REQUIRE(result);
    return result.value();
}

shark::Transform translated(const double x, const double y = 0, const double z = 0) {
    shark::Transform result;
    result.translation = {x, y, z};
    return result;
}

shark::Transform scaled(const double x, const double y, const double z) {
    shark::Transform result;
    result.scale = {x, y, z};
    return result;
}

shark::Transform rotated_z(const double radians) {
    shark::Transform result;
    result.rotation = {std::cos(radians / 2), 0, 0, std::sin(radians / 2)};
    return result;
}

void same_matrix(const shark::math::Mat4& actual, const shark::math::Mat4& expected) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            CHECK(std::abs(actual[column][row] - expected[column][row]) <= 1e-9);
        }
    }
}

void world_translation(const shark::SceneEntityState& value, const double x, const double y,
                       const double z) {
    CHECK(std::abs(value.world_matrix[3][0] - x) <= 1e-9);
    CHECK(std::abs(value.world_matrix[3][1] - y) <= 1e-9);
    CHECK(std::abs(value.world_matrix[3][2] - z) <= 1e-9);
    CHECK(value.world_matrix[3][3] == 1);
}

void unchanged(const shark::SceneEntityState& actual, const shark::SceneEntityState& previous) {
    CHECK(actual.reference == previous.reference);
    CHECK(actual.name == previous.name);
    CHECK(actual.local_transform.translation == previous.local_transform.translation);
    CHECK(actual.local_transform.rotation == previous.local_transform.rotation);
    CHECK(actual.local_transform.scale == previous.local_transform.scale);
    CHECK(actual.parent == previous.parent);
    CHECK(actual.children == previous.children);
    CHECK(actual.depth == previous.depth);
    same_matrix(actual.world_matrix, previous.world_matrix);
}

template <typename T> void rejected(const shark::Result<T>& result) {
    REQUIRE_FALSE(result);
    CHECK_FALSE(result.error().context.operation.empty());
}

} // namespace

TEST_CASE("names validate bounded Unicode scalars and reject all C0 C1 and DEL controls",
          "[hierarchy]") {
    CHECK(shark::Name{}.value == "Entity");
    CHECK(shark::maximum_name_bytes == 256);
    CHECK(shark::validate_name({" "}));
    CHECK(shark::validate_name({std::string(256, 'n')}));
    CHECK(shark::validate_name({"\xc3\xa9"}));
    CHECK(shark::validate_name({"e\xcc\x81"}));
    CHECK(shark::validate_name({"\xe9\xb2\xa8\xf0\x9f\xa6\x88"}));
    std::string multibyte;
    for (int count = 0; count < 128; ++count) {
        multibyte += "\xc3\xa9";
    }
    CHECK(shark::validate_name({multibyte}));
    rejected(shark::validate_name({multibyte + "a"}));
    rejected(shark::validate_name({""}));
    rejected(shark::validate_name({std::string(257, 'n')}));
    for (unsigned int code = 0; code < 32; ++code) {
        const auto invalid =
            shark::validate_name({std::string{"x"} + static_cast<char>(code) + "y"});
        rejected(invalid);
        CHECK(invalid.error().code == shark::ErrorCode::invalid_argument);
        CHECK(invalid.error().context.operation == "name.validate");
    }
    rejected(shark::validate_name({std::string{"x\x7f"}}));
    for (unsigned int code = 0x80; code <= 0x9f; ++code) {
        rejected(shark::validate_name({std::string{"\xc2"} + static_cast<char>(code)}));
    }
    const std::array malformed{
        std::string{"\x80"},
        std::string{"\xbf"},
        std::string{"\xc0\xaf"},
        std::string{"\xc1\xbf"},
        std::string{"\xc2"},
        std::string{"\xe0\x80\x80"},
        std::string{"\xe2\x82"},
        std::string{"\xe2\x28\xa1"},
        std::string{"\xed\xa0\x80"},
        std::string{"\xed\xbf\xbf"},
        std::string{"\xf0\x80\x80\x80"},
        std::string{"\xf0\x9f\xa6"},
        std::string{"\xf4\x90\x80\x80"},
        std::string{"\xff"},
    };
    for (const auto& invalid : malformed) {
        rejected(shark::validate_name({invalid}));
    }

    // Sharing scalar decoding does not impose Name's control policy on other APIs.
    REQUIRE(shark::ProjectPath::parse("existing-\xc2\x85-name"));
    std::ostringstream output;
    shark::Logger logger(output);
    REQUIRE(logger.write(shark::LogLevel::info, {shark::ErrorCode::invalid_argument,
                                                 std::string(1, '\0') + "\xc2\x85",
                                                 {.operation = "name.test"}}));
    CHECK(output.str().find("\\u0000") != std::string::npos);
    CHECK(output.str().find("\xc2\x85") != std::string::npos);
}

TEST_CASE("entities own mandatory default Name and Transform with copied hierarchy state",
          "[hierarchy]") {
    auto owner = scene();
    const auto root = create(*owner, seed(10));
    const auto initial = state(*owner, root);
    CHECK(initial.name.value == "Entity");
    CHECK(initial.local_transform.translation == shark::math::Vec3(0));
    CHECK(initial.local_transform.scale == shark::math::Vec3(1));
    CHECK(initial.local_transform.rotation == shark::math::Quat(1, 0, 0, 0));
    CHECK_FALSE(initial.parent);
    CHECK(initial.children.empty());
    CHECK(initial.depth == 1);
    same_matrix(initial.world_matrix, shark::math::Mat4(1));

    auto child_seed = seed(11, root, translated(2, 3, 4));
    child_seed.name = {"\xc3\xa9"};
    const auto child = create(*owner, child_seed);
    child_seed.name.value = "changed caller input";
    const auto child_state = state(*owner, child);
    CHECK(child_state.name.value == "\xc3\xa9");
    CHECK(child_state.parent == root);
    CHECK(child_state.depth == 2);
    CHECK(state(*owner, root).children == std::vector{child});
    world_translation(child_state, 2, 3, 4);
    CHECK(initial.children.empty());
    auto modified_copy = child_state;
    modified_copy.name.value = "copy only";
    modified_copy.local_transform.translation = {99, 99, 99};
    modified_copy.parent.reset();
    unchanged(state(*owner, child), child_state);
    auto invalid_seed = seed(12, root);
    invalid_seed.name.value.clear();
    rejected(owner->create_entity(invalid_seed));
    rejected(owner->defer_create(invalid_seed));
    CHECK(stats(*owner).live_entities == 2);
    CHECK(stats(*owner).pending_mutations == 0);
}

TEST_CASE("display names remain nonunique byte preserving and revision aware", "[hierarchy]") {
    auto owner = scene();
    const auto first = create(*owner, seed(10));
    const auto second = create(*owner, seed(11));
    const auto identity = state(*owner, first).reference;
    REQUIRE(owner->set_name(first, {"\xc3\xa9"}));
    REQUIRE(owner->set_name(second, {"\xc3\xa9"}));
    CHECK(state(*owner, first).name == state(*owner, second).name);
    CHECK(state(*owner, first).reference == identity);
    const auto before = stats(*owner).revision;
    REQUIRE(owner->set_name(first, {"\xc3\xa9"}));
    CHECK(stats(*owner).revision == before);
    REQUIRE(owner->set_name(first, {"e\xcc\x81"}));
    CHECK(state(*owner, first).name.value == "e\xcc\x81");
    CHECK(stats(*owner).revision == before + 1);
    const auto saved = state(*owner, first);
    rejected(owner->set_name(first, {""}));
    rejected(owner->set_name(first, {"\xc2\x85"}));
    rejected(owner->defer_set_name(first, {"\xff"}));
    unchanged(state(*owner, first), saved);
    CHECK(stats(*owner).revision == before + 1);
    CHECK(stats(*owner).pending_mutations == 0);
}

TEST_CASE("hierarchy caches propagate translation and negative nonuniform scale immediately",
          "[hierarchy]") {
    auto owner = scene();
    const auto root = create(*owner, seed(10, {}, translated(10, 20, 30)));
    const auto child = create(*owner, seed(11, root, translated(1, 2, 3)));
    const auto leaf = create(*owner, seed(12, child, translated(2, 1, -1)));
    const auto saved_leaf = state(*owner, leaf);
    world_translation(state(*owner, child), 11, 22, 33);
    world_translation(saved_leaf, 13, 23, 32);
    const auto before = stats(*owner).revision;
    auto new_root = translated(10, 20, 30);
    new_root.scale = {2, -3, 4};
    REQUIRE(owner->set_transform(root, new_root));
    CHECK(stats(*owner).revision == before + 1);
    world_translation(state(*owner, child), 12, 14, 42);
    world_translation(state(*owner, leaf), 16, 11, 38);
    CHECK(state(*owner, leaf).world_matrix[0][0] == 2);
    CHECK(state(*owner, leaf).world_matrix[1][1] == -3);
    CHECK(state(*owner, leaf).world_matrix[2][2] == 4);
    CHECK(state(*owner, leaf).depth == 3);
    world_translation(saved_leaf, 13, 23, 32);
    REQUIRE(owner->set_transform(root, new_root));
    CHECK(stats(*owner).revision == before + 1);
    REQUIRE(owner->set_transform(child, translated(0, 0, 0)));
    world_translation(state(*owner, leaf), 14, 17, 26);
    CHECK(stats(*owner).revision == before + 2);
}

TEST_CASE("keep local reparent preserves local TRS and updates both ancestry paths",
          "[hierarchy]") {
    auto owner = scene();
    const auto first_root = create(*owner, seed(10, {}, translated(10)));
    const auto second_root = create(*owner, seed(11, {}, translated(-20)));
    const auto child = create(*owner, seed(12, first_root, translated(2, 3, 4)));
    const auto leaf = create(*owner, seed(13, child, translated(5)));
    const auto saved_child = state(*owner, child);
    const auto before = stats(*owner).revision;
    REQUIRE(owner->reparent(child, second_root, shark::ReparentMode::keep_local));
    CHECK(stats(*owner).revision == before + 1);
    CHECK(state(*owner, first_root).children.empty());
    CHECK(state(*owner, second_root).children == std::vector{child});
    CHECK(state(*owner, child).parent == second_root);
    CHECK(state(*owner, child).local_transform.translation ==
          saved_child.local_transform.translation);
    world_translation(state(*owner, child), -18, 3, 4);
    world_translation(state(*owner, leaf), -13, 3, 4);
    REQUIRE(owner->reparent(child, {}, shark::ReparentMode::keep_local));
    CHECK(state(*owner, second_root).children.empty());
    CHECK_FALSE(state(*owner, child).parent);
    CHECK(state(*owner, child).depth == 1);
    CHECK(state(*owner, leaf).depth == 2);
    world_translation(state(*owner, child), 2, 3, 4);
    world_translation(state(*owner, leaf), 7, 3, 4);
}

TEST_CASE("keep world reparent preserves descendant matrices and derives representable local TRS",
          "[hierarchy]") {
    auto owner = scene();
    const auto old_parent = create(*owner, seed(10, {}, translated(10)));
    auto new_parent_transform = translated(100, -20, 0);
    new_parent_transform.scale = {2, 2, 2};
    const auto new_parent = create(*owner, seed(11, {}, new_parent_transform));
    const auto child = create(*owner, seed(12, old_parent, translated(1, 2, 3)));
    const auto leaf = create(*owner, seed(13, child, translated(4, 5, 6)));
    const auto old_child = state(*owner, child);
    const auto old_leaf = state(*owner, leaf);
    const auto before = stats(*owner).revision;
    REQUIRE(owner->reparent(child, new_parent, shark::ReparentMode::keep_world));
    CHECK(stats(*owner).revision == before + 1);
    CHECK(state(*owner, child).parent == new_parent);
    same_matrix(state(*owner, child).world_matrix, old_child.world_matrix);
    same_matrix(state(*owner, leaf).world_matrix, old_leaf.world_matrix);
    CHECK(state(*owner, child).local_transform.translation == shark::math::Vec3(-44.5, 11, 1.5));
    CHECK(state(*owner, child).local_transform.scale == shark::math::Vec3(0.5));
    REQUIRE(owner->reparent(child, {}, shark::ReparentMode::keep_world));
    CHECK_FALSE(state(*owner, child).parent);
    CHECK(state(*owner, child).depth == 1);
    CHECK(state(*owner, leaf).depth == 2);
    same_matrix(state(*owner, child).world_matrix, old_child.world_matrix);
    same_matrix(state(*owner, leaf).world_matrix, old_leaf.world_matrix);
    CHECK(state(*owner, child).local_transform.translation == shark::math::Vec3(11, 2, 3));
    REQUIRE(owner->reparent(child, {}, shark::ReparentMode::keep_world));
    REQUIRE(owner->reparent(child, {}, shark::ReparentMode::keep_local));
    CHECK(stats(*owner).revision == before + 2);
}

TEST_CASE("world shear is allowed but keep world reparent rejects nonlocal TRS atomically",
          "[hierarchy]") {
    constexpr double quarter_turn = 0.78539816339744830962;
    auto owner = scene();
    const auto parent = create(*owner, seed(10, {}, scaled(2, 1, 1)));
    const auto child = create(*owner, seed(11, parent, rotated_z(quarter_turn)));
    const auto leaf = create(*owner, seed(12, child, translated(3)));
    const auto parent_before = state(*owner, parent);
    const auto child_before = state(*owner, child);
    const auto leaf_before = state(*owner, leaf);
    const auto revision = stats(*owner).revision;
    REQUIRE(owner->reparent(child, parent, shark::ReparentMode::keep_world));
    CHECK(stats(*owner).revision == revision);
    rejected(owner->reparent(child, {}, shark::ReparentMode::keep_world));
    CHECK(stats(*owner).revision == revision);
    unchanged(state(*owner, parent), parent_before);
    unchanged(state(*owner, child), child_before);
    unchanged(state(*owner, leaf), leaf_before);
    REQUIRE(owner->reparent(child, {}, shark::ReparentMode::keep_local));
    CHECK(state(*owner, parent).children.empty());
    CHECK_FALSE(state(*owner, child).parent);
    CHECK(state(*owner, child).local_transform.rotation == child_before.local_transform.rotation);
    CHECK(state(*owner, leaf).depth == 2);
    // A nonuniform destination can also require a sheared local matrix.
    const auto root_rotated = create(*owner, seed(13, {}, rotated_z(quarter_turn)));
    const auto rotated_before = state(*owner, root_rotated);
    const auto next_revision = stats(*owner).revision;
    rejected(owner->reparent(root_rotated, parent, shark::ReparentMode::keep_world));
    unchanged(state(*owner, root_rotated), rotated_before);
    CHECK(stats(*owner).revision == next_revision);
}

TEST_CASE(
    "keep world reparent composes a rotated scaled destination ancestry and preserves descendants",
    "[hierarchy]") {
    constexpr double half_turn = 1.57079632679489661923;
    constexpr double quarter_turn = 0.78539816339744830962;
    auto owner = scene();
    auto ancestor_transform = rotated_z(half_turn);
    ancestor_transform.translation = {10, 20, 30};
    ancestor_transform.scale = {2, 2, 2};
    const auto ancestor = create(*owner, seed(10, {}, ancestor_transform));
    auto destination_transform = rotated_z(quarter_turn);
    destination_transform.translation = {1, 2, 3};
    destination_transform.scale = {3, 3, 3};
    const auto destination = create(*owner, seed(11, ancestor, destination_transform));
    auto entity_transform = rotated_z(half_turn);
    entity_transform.translation = {7, -4, 9};
    entity_transform.scale = {3, 4, -5};
    const auto entity = create(*owner, seed(12, {}, entity_transform));
    auto leaf_transform = translated(1, 2, 3);
    leaf_transform.scale = {2, 1, 1};
    const auto leaf = create(*owner, seed(13, entity, leaf_transform));

    // Analytic column-major matrices, independent of the production compose/inverse helpers.
    shark::math::Mat4 expected_entity{1};
    expected_entity[0] = {0, 3, 0, 0};
    expected_entity[1] = {-4, 0, 0, 0};
    expected_entity[2] = {0, 0, -5, 0};
    expected_entity[3] = {7, -4, 9, 1};
    auto expected_leaf = expected_entity;
    expected_leaf[0] = {0, 6, 0, 0};
    expected_leaf[3] = {-1, -1, -6, 1};
    const auto diagonal = 6 * std::sqrt(0.5);
    shark::math::Mat4 expected_destination{1};
    expected_destination[0] = {-diagonal, diagonal, 0, 0};
    expected_destination[1] = {-diagonal, -diagonal, 0, 0};
    expected_destination[2] = {0, 0, 6, 0};
    expected_destination[3] = {6, 22, 36, 1};
    same_matrix(state(*owner, destination).world_matrix, expected_destination);
    same_matrix(state(*owner, entity).world_matrix, expected_entity);
    same_matrix(state(*owner, leaf).world_matrix, expected_leaf);
    const auto old_identity = state(*owner, entity).reference;
    const auto revision = stats(*owner).revision;

    REQUIRE(owner->reparent(entity, destination, shark::ReparentMode::keep_world));
    CHECK(stats(*owner).revision == revision + 1);
    CHECK(state(*owner, entity).reference == old_identity);
    CHECK(state(*owner, entity).parent == destination);
    CHECK(state(*owner, destination).children == std::vector{entity});
    CHECK(state(*owner, entity).depth == 3);
    CHECK(state(*owner, leaf).depth == 4);
    CHECK(state(*owner, leaf).local_transform == leaf_transform);
    same_matrix(state(*owner, entity).world_matrix, expected_entity);
    same_matrix(state(*owner, leaf).world_matrix, expected_leaf);
    same_matrix(state(*owner, destination).world_matrix, expected_destination);

    // A later ancestor edit follows the new links and updates both cached worlds.
    ancestor_transform.translation.x += 100;
    REQUIRE(owner->set_transform(ancestor, ancestor_transform));
    expected_entity[3][0] += 100;
    expected_leaf[3][0] += 100;
    same_matrix(state(*owner, entity).world_matrix, expected_entity);
    same_matrix(state(*owner, leaf).world_matrix, expected_leaf);
    CHECK(stats(*owner).revision == revision + 2);
}

TEST_CASE("negative scale survives representable keep world reparent without changing orientation",
          "[hierarchy]") {
    auto owner = scene();
    const auto negative_parent = create(*owner, seed(10, {}, scaled(-2, 3, 4)));
    const auto child = create(*owner, seed(11, negative_parent, translated(1, 2, 3)));
    const auto saved = state(*owner, child);
    world_translation(saved, -2, 6, 12);
    REQUIRE(owner->reparent(child, {}, shark::ReparentMode::keep_world));
    same_matrix(state(*owner, child).world_matrix, saved.world_matrix);
    CHECK(state(*owner, child).depth == 1);
    CHECK_FALSE(state(*owner, child).parent);
    REQUIRE(owner->reparent(child, negative_parent, shark::ReparentMode::keep_world));
    same_matrix(state(*owner, child).world_matrix, saved.world_matrix);
}

TEST_CASE("invalid transforms and descendant overflow preserve every affected entity",
          "[hierarchy]") {
    auto owner = scene();
    const auto parent = create(*owner, seed(10));
    const auto child = create(*owner, seed(11, parent, scaled(1e200, 1e200, 1e200)));
    const auto parent_before = state(*owner, parent);
    const auto child_before = state(*owner, child);
    const auto revision = stats(*owner).revision;
    rejected(owner->set_transform(parent, scaled(1e200, 1e200, 1e200)));
    unchanged(state(*owner, parent), parent_before);
    unchanged(state(*owner, child), child_before);
    CHECK(stats(*owner).revision == revision);
    rejected(owner->create_entity(seed(12, child, scaled(1e200, 1e200, 1e200))));
    CHECK(stats(*owner).live_entities == 2);
    CHECK(stats(*owner).revision == revision);
    CHECK(state(*owner, child).children.empty());
    const auto other = create(*owner, seed(13, {}, scaled(1e200, 1e200, 1e200)));
    const auto other_before = state(*owner, other);
    rejected(owner->reparent(parent, other, shark::ReparentMode::keep_local));
    unchanged(state(*owner, parent), parent_before);
    unchanged(state(*owner, child), child_before);
    unchanged(state(*owner, other), other_before);
    CHECK(stats(*owner).revision == revision + 1);

    std::array invalid{scaled(0, 1, 1), scaled(1e-7, 1, 1),
                       translated(std::numeric_limits<double>::infinity()),
                       translated(std::numeric_limits<double>::quiet_NaN())};
    for (const auto& transform : invalid) {
        rejected(owner->set_transform(parent, transform));
        rejected(owner->defer_set_transform(parent, transform));
        rejected(owner->create_entity(seed(14, {}, transform)));
    }
    auto zero_quaternion = shark::Transform{};
    zero_quaternion.rotation = {0, 0, 0, 0};
    rejected(owner->set_transform(parent, zero_quaternion));
    unchanged(state(*owner, parent), parent_before);
    CHECK(stats(*owner).pending_mutations == 0);
}

TEST_CASE("scene transforms store normalized quaternion equivalents as revision noops",
          "[hierarchy]") {
    auto owner = scene();
    auto transform = translated(-0.0, 2, -0.0);
    transform.rotation = {2, 0, 0, 0};
    const auto entity = create(*owner, seed(10, {}, transform));
    const auto initial = state(*owner, entity);
    CHECK(initial.local_transform.rotation == shark::math::Quat(1, 0, 0, 0));
    CHECK_FALSE(std::signbit(initial.local_transform.translation.x));
    CHECK_FALSE(std::signbit(initial.local_transform.translation.z));
    const auto before = stats(*owner).revision;
    transform.rotation = {-4, -0.0, -0.0, -0.0};
    REQUIRE(owner->set_transform(entity, transform));
    CHECK(stats(*owner).revision == before);
    unchanged(state(*owner, entity), initial);
    transform.rotation = {0, 0, 0, -2};
    REQUIRE(owner->set_transform(entity, transform));
    CHECK(state(*owner, entity).local_transform.rotation == shark::math::Quat(0, 0, 0, 1));
    CHECK(stats(*owner).revision == before + 1);
    transform.rotation = {0, 0, 0, 1};
    REQUIRE(owner->set_transform(entity, transform));
    CHECK(stats(*owner).revision == before + 1);
}

TEST_CASE(
    "hierarchy rejects self cycles malformed modes stale and foreign parents without mutation",
    "[hierarchy]") {
    auto owner = scene();
    auto other = scene();
    const auto root = create(*owner, seed(10));
    const auto child = create(*owner, seed(11, root));
    const auto leaf = create(*owner, seed(12, child));
    const auto foreign = create(*other, seed(10));
    const auto stale = create(*owner, seed(13));
    REQUIRE(owner->destroy_entity(stale));
    const auto reused = create(*owner, seed(13));
    CHECK(reused.slot == stale.slot);
    CHECK(reused.generation != stale.generation);
    const auto before = stats(*owner).revision;
    const std::array prior{state(*owner, root), state(*owner, child), state(*owner, leaf)};
    rejected(owner->reparent(root, root, shark::ReparentMode::keep_local));
    rejected(owner->reparent(root, leaf, shark::ReparentMode::keep_local));
    rejected(owner->reparent(child, leaf, shark::ReparentMode::keep_world));
    rejected(owner->reparent(child, foreign, shark::ReparentMode::keep_local));
    rejected(owner->reparent(child, stale, shark::ReparentMode::keep_local));
    rejected(owner->reparent(child, {}, static_cast<shark::ReparentMode>(255)));
    rejected(owner->create_entity(seed(14, foreign)));
    rejected(owner->create_entity(seed(14, stale)));
    rejected(owner->set_name(stale, {"old"}));
    rejected(owner->set_transform(stale, {}));
    rejected(owner->entity_state(stale));
    rejected(owner->entity_state(foreign));
    rejected(owner->reparent(stale, root, shark::ReparentMode::keep_local));
    rejected(owner->defer_reparent(child, foreign, shark::ReparentMode::keep_local));
    rejected(owner->defer_reparent(child, stale, shark::ReparentMode::keep_local));
    for (const auto& saved : prior) {
        unchanged(state(*owner, saved.reference.handle), saved);
    }
    CHECK(stats(*owner).revision == before);
    CHECK(stats(*owner).pending_mutations == 0);
}

TEST_CASE("sibling snapshots are slot ordered across reparent and slot reuse", "[hierarchy]") {
    auto owner = scene();
    const auto root = create(*owner, seed(10));
    const auto first = create(*owner, seed(30));
    const auto second = create(*owner, seed(20));
    const auto third = create(*owner, seed(11));
    REQUIRE(owner->reparent(third, root, shark::ReparentMode::keep_local));
    REQUIRE(owner->reparent(first, root, shark::ReparentMode::keep_local));
    REQUIRE(owner->reparent(second, root, shark::ReparentMode::keep_local));
    CHECK(state(*owner, root).children == std::vector{first, second, third});
    const auto old_copy = state(*owner, root);
    REQUIRE(owner->destroy_entity(second));
    const auto replacement = create(*owner, seed(25, root));
    CHECK(replacement.slot == second.slot);
    CHECK(state(*owner, root).children == std::vector{first, replacement, third});
    CHECK(old_copy.children == std::vector{first, second, third});
    REQUIRE(owner->reparent(first, {}, shark::ReparentMode::keep_world));
    CHECK(state(*owner, root).children == std::vector{replacement, third});
}

TEST_CASE("subtree destruction is one operation and invalidates every descendant before reuse",
          "[hierarchy]") {
    auto owner = scene();
    const auto root = create(*owner, seed(10));
    const auto branch = create(*owner, seed(11, root));
    const auto leaf = create(*owner, seed(12, branch));
    const auto sibling = create(*owner, seed(13, root));
    const auto branch_reference = state(*owner, branch).reference;
    const auto leaf_reference = state(*owner, leaf).reference;
    const auto before = stats(*owner).revision;
    REQUIRE(owner->destroy_entity(branch));
    CHECK(stats(*owner).revision == before + 1);
    CHECK(stats(*owner).live_entities == 2);
    CHECK(state(*owner, root).children == std::vector{sibling});
    rejected(owner->resolve(branch_reference));
    rejected(owner->resolve(leaf_reference));
    rejected(owner->find(leaf_reference.identity));
    const auto replacement = create(*owner, seed(11, root));
    CHECK(replacement.slot == branch.slot);
    CHECK(replacement.generation != branch.generation);
    rejected(owner->resolve(branch_reference));
    rejected(owner->entity_state(leaf));
    const auto before_root_delete = stats(*owner).revision;
    REQUIRE(owner->destroy_entity(root));
    CHECK(stats(*owner).revision == before_root_delete + 1);
    CHECK(stats(*owner).live_entities == 0);
    rejected(owner->entity_state(sibling));
    rejected(owner->entity_state(replacement));
}

TEST_CASE("depth bounds validate the entire moved subtree and deep destruction is iterative",
          "[hierarchy]") {
    const auto scope = shark::AuthoredSceneScope{id<shark::SceneId>(1)};
    rejected(shark::Scene::create(scope, {10, 10, 0}));
    rejected(shark::Scene::create(scope, {10, 10, 1025}));
    auto owner = scene({10, 10, 3});
    const auto root = create(*owner, seed(10));
    const auto child = create(*owner, seed(11, root));
    const auto leaf = create(*owner, seed(12, child));
    const auto detached = create(*owner, seed(13));
    const auto detached_leaf = create(*owner, seed(14, detached));
    const auto revision = stats(*owner).revision;
    rejected(owner->create_entity(seed(15, leaf)));
    rejected(owner->reparent(detached, child, shark::ReparentMode::keep_local));
    CHECK(stats(*owner).revision == revision);
    CHECK_FALSE(state(*owner, detached).parent);
    CHECK(state(*owner, detached_leaf).depth == 2);
    REQUIRE(owner->reparent(detached, root, shark::ReparentMode::keep_local));
    CHECK(state(*owner, detached_leaf).depth == 3);

    auto deep = scene({1025, 8, 1024});
    std::optional<shark::EntityHandle> parent;
    std::vector<shark::EntityHandle> handles;
    for (std::uint32_t index = 0; index < 1024; ++index) {
        const auto entity = create(*deep, seed(100 + index, parent));
        handles.push_back(entity);
        parent = entity;
    }
    CHECK(state(*deep, handles.back()).depth == 1024);
    rejected(deep->create_entity(seed(2000, handles.back())));
    const auto before = stats(*deep).revision;
    REQUIRE(deep->destroy_entity(handles.front()));
    CHECK(stats(*deep).live_entities == 0);
    CHECK(stats(*deep).revision == before + 1);
    rejected(deep->entity_state(handles.back()));
    const auto replacement = create(*deep, seed(100));
    CHECK(replacement.slot == handles.front().slot);
    CHECK(replacement.generation > handles.front().generation);
}

TEST_CASE("replica parenting requires the same authority stamp within its session", "[hierarchy]") {
    auto made = shark::Scene::create(
        shark::ReplicaSceneScope{id<shark::WorldId>(1), id<shark::ReplicaSessionId>(2)});
    REQUIRE(made);
    auto owner = std::move(made).value();
    const shark::AuthorityStamp authority{id<shark::RegionId>(3), id<shark::RegionInstanceId>(4),
                                          5};
    shark::EntitySeed first_seed{id<shark::WorldEntityId>(10), authority};
    const auto root = create(*owner, first_seed);
    auto child_seed = first_seed;
    child_seed.identity = id<shark::WorldEntityId>(11);
    child_seed.parent = root;
    const auto child = create(*owner, child_seed);
    for (int mismatch = 0; mismatch < 3; ++mismatch) {
        auto foreign_authority = authority;
        if (mismatch == 0) {
            ++foreign_authority.epoch;
        } else if (mismatch == 1) {
            foreign_authority.instance = id<shark::RegionInstanceId>(8);
        } else {
            foreign_authority.region = id<shark::RegionId>(8);
        }
        shark::EntitySeed foreign_seed{id<shark::WorldEntityId>(20), foreign_authority};
        foreign_seed.parent = root;
        rejected(owner->create_entity(foreign_seed));
        foreign_seed.parent.reset();
        const auto foreign = create(*owner, foreign_seed);
        const auto before = stats(*owner).revision;
        rejected(owner->reparent(child, foreign, shark::ReparentMode::keep_local));
        CHECK(stats(*owner).revision == before);
        CHECK(state(*owner, child).parent == root);
        REQUIRE(owner->destroy_entity(foreign));
    }
}

TEST_CASE("deferred hierarchy operations own their input and commit FIFO with individual failures",
          "[hierarchy]") {
    auto owner = scene();
    const auto first_root = create(*owner, seed(10, {}, translated(10)));
    const auto next_root = create(*owner, seed(11, {}, translated(20)));
    const auto child = create(*owner, seed(12, first_root, translated(1)));
    shark::Name input{"Queued name"};
    auto transform = translated(3);
    const auto rename = owner->defer_set_name(child, input);
    const auto move = owner->defer_set_transform(child, transform);
    const auto reparent = owner->defer_reparent(child, next_root, shark::ReparentMode::keep_local);
    REQUIRE(rename);
    REQUIRE(move);
    REQUIRE(reparent);
    input.value = "mutated caller storage";
    transform.translation = {900, 900, 900};
    rejected(owner->set_name(child, {"immediate"}));
    rejected(owner->set_transform(child, {}));
    rejected(owner->reparent(child, {}, shark::ReparentMode::keep_local));
    const auto before = stats(*owner).revision;
    const auto report = owner->apply_deferred();
    REQUIRE(report);
    REQUIRE(report.value().outcomes.size() == 3);
    CHECK(report.value().outcomes[0].kind == shark::SceneMutationKind::set_name);
    CHECK(report.value().outcomes[1].kind == shark::SceneMutationKind::set_transform);
    CHECK(report.value().outcomes[2].kind == shark::SceneMutationKind::reparent);
    CHECK(report.value().outcomes[0].ticket == rename.value());
    CHECK(report.value().outcomes[1].ticket == move.value());
    CHECK(report.value().outcomes[2].ticket == reparent.value());
    for (const auto& outcome : report.value().outcomes) {
        REQUIRE(outcome.result);
        CHECK(outcome.result.value() == child);
    }
    CHECK(report.value().revision_before == before);
    CHECK(report.value().revision_after == before + 3);
    CHECK(state(*owner, child).name.value == "Queued name");
    CHECK(state(*owner, child).parent == next_root);
    world_translation(state(*owner, child), 23, 0, 0);

    REQUIRE(owner->defer_destroy(next_root));
    REQUIRE(owner->defer_set_name(child, {"stale child"}));
    REQUIRE(owner->defer_set_transform(child, translated(4)));
    REQUIRE(owner->defer_reparent(child, first_root, shark::ReparentMode::keep_local));
    REQUIRE(owner->defer_set_name(first_root, {"surviving root"}));
    const auto failed = owner->apply_deferred();
    REQUIRE(failed);
    REQUIRE(failed.value().outcomes.size() == 5);
    REQUIRE(failed.value().outcomes[0].result);
    rejected(failed.value().outcomes[1].result);
    rejected(failed.value().outcomes[2].result);
    rejected(failed.value().outcomes[3].result);
    REQUIRE(failed.value().outcomes[4].result);
    CHECK(failed.value().revision_after == failed.value().revision_before + 2);
    CHECK(stats(*owner).live_entities == 1);
    CHECK(state(*owner, first_root).name.value == "surviving root");
}

TEST_CASE("deferred reparent reevaluates ancestry and transformed parents at its actual boundary",
          "[hierarchy]") {
    auto owner = scene();
    const auto first = create(*owner, seed(10, {}, translated(10)));
    const auto second = create(*owner, seed(11, {}, translated(20)));
    REQUIRE(owner->defer_reparent(first, second, shark::ReparentMode::keep_local));
    REQUIRE(owner->defer_reparent(second, first, shark::ReparentMode::keep_local));
    const auto cycles = owner->apply_deferred();
    REQUIRE(cycles);
    REQUIRE(cycles.value().outcomes.size() == 2);
    REQUIRE(cycles.value().outcomes[0].result);
    rejected(cycles.value().outcomes[1].result);
    CHECK(state(*owner, first).parent == second);
    CHECK_FALSE(state(*owner, second).parent);
    CHECK(cycles.value().revision_after == cycles.value().revision_before + 1);
    REQUIRE(owner->reparent(first, {}, shark::ReparentMode::keep_world));
    const auto old_world = state(*owner, first).world_matrix;
    REQUIRE(owner->defer_set_transform(second, translated(50)));
    REQUIRE(owner->defer_reparent(first, second, shark::ReparentMode::keep_world));
    const auto moved = owner->apply_deferred();
    REQUIRE(moved);
    REQUIRE(moved.value().outcomes.size() == 2);
    REQUIRE(moved.value().outcomes[0].result);
    REQUIRE(moved.value().outcomes[1].result);
    same_matrix(state(*owner, first).world_matrix, old_world);
    CHECK(state(*owner, first).local_transform.translation.x == -20);
    REQUIRE(owner->defer_set_name(first, state(*owner, first).name));
    REQUIRE(owner->defer_set_transform(first, state(*owner, first).local_transform));
    REQUIRE(owner->defer_reparent(first, second, shark::ReparentMode::keep_world));
    const auto noops = owner->apply_deferred();
    REQUIRE(noops);
    CHECK(noops.value().revision_before == noops.value().revision_after);
    for (const auto& outcome : noops.value().outcomes) {
        REQUIRE(outcome.result);
    }
}

TEST_CASE("deferred creation rechecks parent lifetime and parent world matrices before publication",
          "[hierarchy]") {
    auto owner = scene();
    const auto parent = create(*owner, seed(10));
    REQUIRE(owner->defer_destroy(parent));
    REQUIRE(owner->defer_create(seed(11, parent)));
    REQUIRE(owner->defer_create(seed(12)));
    const auto stale_parent = owner->apply_deferred();
    REQUIRE(stale_parent);
    REQUIRE(stale_parent.value().outcomes.size() == 3);
    REQUIRE(stale_parent.value().outcomes[0].result);
    rejected(stale_parent.value().outcomes[1].result);
    REQUIRE(stale_parent.value().outcomes[2].result);
    CHECK(stale_parent.value().revision_after == stale_parent.value().revision_before + 2);
    const auto replacement = stale_parent.value().outcomes[2].result.value();
    CHECK(replacement.slot == parent.slot);
    CHECK(replacement.generation != parent.generation);
    CHECK(state(*owner, replacement).children.empty());
    REQUIRE(owner->defer_set_transform(replacement, scaled(1e200, 1e200, 1e200)));
    REQUIRE(owner->defer_create(seed(13, replacement, scaled(1e200, 1e200, 1e200))));
    const auto overflow = owner->apply_deferred();
    REQUIRE(overflow);
    REQUIRE(overflow.value().outcomes.size() == 2);
    REQUIRE(overflow.value().outcomes[0].result);
    rejected(overflow.value().outcomes[1].result);
    CHECK(overflow.value().revision_after == overflow.value().revision_before + 1);
    CHECK(stats(*owner).live_entities == 1);
    CHECK(state(*owner, replacement).children.empty());
    rejected(owner->find(seed(13).identity));
}

TEST_CASE("deferred transform failure is atomic and new mutations respect shared queue capacity",
          "[hierarchy]") {
    auto owner = scene({10, 2, 10});
    const auto root = create(*owner, seed(10));
    const auto child = create(*owner, seed(11, root, scaled(1e200, 1e200, 1e200)));
    const auto root_before = state(*owner, root);
    const auto child_before = state(*owner, child);
    REQUIRE(owner->defer_set_transform(root, scaled(1e200, 1e200, 1e200)));
    REQUIRE(owner->defer_set_name(child, {"valid later operation"}));
    rejected(owner->defer_set_name(root, {"full queue"}));
    rejected(owner->defer_set_transform(root, {}));
    rejected(owner->defer_reparent(child, {}, shark::ReparentMode::keep_local));
    const auto report = owner->apply_deferred();
    REQUIRE(report);
    REQUIRE(report.value().outcomes.size() == 2);
    rejected(report.value().outcomes[0].result);
    REQUIRE(report.value().outcomes[1].result);
    CHECK(report.value().revision_after == report.value().revision_before + 1);
    unchanged(state(*owner, root), root_before);
    same_matrix(state(*owner, child).world_matrix, child_before.world_matrix);
    CHECK(state(*owner, child).name.value == "valid later operation");
    CHECK(stats(*owner).pending_mutations == 0);
}

TEST_CASE(
    "visitors can inspect copied hierarchy and defer edits while failed visits cancel their batch",
    "[hierarchy]") {
    auto owner = scene();
    const auto root = create(*owner, seed(10));
    const auto child = create(*owner, seed(11, root));
    const auto before = state(*owner, child);
    const auto prior = owner->defer_set_name(root, {"existing"});
    REQUIRE(prior);
    std::vector<shark::SceneEntityState> copied;
    const auto failed =
        owner->visit_entities([&](const shark::EntityReference& reference) -> shark::Result<void> {
            copied.push_back(state(*owner, reference.handle));
            rejected(owner->set_name(reference.handle, {"immediate"}));
            rejected(owner->set_transform(reference.handle, translated(1)));
            rejected(owner->reparent(reference.handle, {}, shark::ReparentMode::keep_local));
            REQUIRE(owner->defer_set_name(reference.handle, {"canceled"}));
            REQUIRE(owner->defer_set_transform(reference.handle, translated(4)));
            REQUIRE(owner->defer_reparent(reference.handle, {}, shark::ReparentMode::keep_local));
            if (reference.handle == child) {
                return shark::Error{shark::ErrorCode::execution_failed,
                                    "fixture callback failure",
                                    {.operation = "hierarchy.test.visitor"}};
            }
            return {};
        });
    rejected(failed);
    CHECK(copied.size() == 2);
    CHECK(stats(*owner).pending_mutations == 1);
    unchanged(state(*owner, child), before);
    const auto applied = owner->apply_deferred();
    REQUIRE(applied);
    REQUIRE(applied.value().outcomes.size() == 1);
    CHECK(applied.value().outcomes.front().ticket == prior.value());
    REQUIRE(applied.value().outcomes.front().result);
    CHECK(state(*owner, root).name.value == "existing");
    REQUIRE(owner->visit_entities([&](const shark::EntityReference& item) -> shark::Result<void> {
        REQUIRE(owner->defer_set_name(item.handle, {"visited"}));
        return {};
    }));
    REQUIRE(owner->apply_deferred());
    CHECK(state(*owner, child).name.value == "visited");
    CHECK(copied.back().name.value == "Entity");
}

TEST_CASE("new hierarchy methods enforce owner thread and closed scene lifetime", "[hierarchy]") {
    auto owner = scene();
    const auto root = create(*owner, seed(10));
    const auto child = create(*owner, seed(11, root));
    const auto before = state(*owner, child);
    std::array<bool, 7> rejected_calls{};
    std::thread worker([&] {
        rejected_calls[0] = !owner->entity_state(child);
        rejected_calls[1] = !owner->set_name(child, {"foreign thread"});
        rejected_calls[2] = !owner->set_transform(child, translated(8));
        rejected_calls[3] = !owner->reparent(child, {}, shark::ReparentMode::keep_local);
        rejected_calls[4] = !owner->defer_set_name(child, {"foreign thread"});
        rejected_calls[5] = !owner->defer_set_transform(child, translated(8));
        rejected_calls[6] = !owner->defer_reparent(child, {}, shark::ReparentMode::keep_local);
    });
    worker.join();
    for (const auto rejected_call : rejected_calls) {
        CHECK(rejected_call);
    }
    unchanged(state(*owner, child), before);
    CHECK(stats(*owner).pending_mutations == 0);
    REQUIRE(owner->close());
    rejected(owner->entity_state(child));
    rejected(owner->set_name(child, {"closed"}));
    rejected(owner->set_transform(child, {}));
    rejected(owner->reparent(child, {}, shark::ReparentMode::keep_local));
    rejected(owner->defer_set_name(child, {"closed"}));
    rejected(owner->defer_set_transform(child, {}));
    rejected(owner->defer_reparent(child, {}, shark::ReparentMode::keep_local));
}
