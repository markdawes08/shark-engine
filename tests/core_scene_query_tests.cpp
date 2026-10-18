#include <shark/scene.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
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

std::unique_ptr<shark::Scene> scene(shark::SceneScope scope = shark::AuthoredSceneScope{
                                        id<shark::SceneId>(1)}) {
    auto result = shark::Scene::create(std::move(scope));
    REQUIRE(result);
    return std::move(result).value();
}

shark::EntitySeed seed(const std::uint32_t value, std::string name = "Entity",
                       const std::optional<shark::EntityHandle> parent = std::nullopt) {
    return {id<shark::AuthoredEntityId>(value), std::nullopt, {std::move(name)}, {}, parent};
}

shark::EntityHandle create(shark::Scene& owner, shark::EntitySeed value) {
    auto result = owner.create_entity(std::move(value));
    REQUIRE(result);
    return result.value();
}

shark::SceneQueryPage query(const shark::Scene& owner,
                            const shark::SceneQueryOptions& options = {}) {
    auto result = owner.query_entities(options);
    REQUIRE(result);
    return std::move(result).value();
}

shark::SceneQueryPage next(const shark::Scene& owner, const shark::SceneQueryCursor& cursor) {
    auto result = owner.continue_query(cursor);
    REQUIRE(result);
    return std::move(result).value();
}

shark::SceneQueryPage inspect(const shark::Scene& owner, const shark::EntityHandle handle,
                              const shark::SceneQueryFields fields = {}) {
    auto result = owner.inspect_entity(handle, fields);
    REQUIRE(result);
    return std::move(result).value();
}

template <typename T> void rejected(const shark::Result<T>& result, const shark::ErrorCode code) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == code);
    CHECK_FALSE(result.error().context.operation.empty());
}

std::vector<shark::EntityHandle> handles(const shark::SceneQueryPage& page) {
    std::vector<shark::EntityHandle> values;
    for (const auto& entity : page.entities()) {
        values.push_back(entity.reference.handle);
    }
    return values;
}

template <typename T>
concept HasTemporaryEntities = requires(T value) { std::move(value).entities(); };
template <typename T>
concept HasTemporaryScope = requires(T value) { std::move(value).scope(); };
template <typename T>
concept HasTemporaryCursor = requires(T value) { std::move(value).next_cursor(); };
template <typename T>
concept HasChildrenVector = requires(T value) { value.children; };

static_assert(!std::is_default_constructible_v<shark::SceneQueryCursor>);
static_assert(!std::is_aggregate_v<shark::SceneQueryCursor>);
static_assert(std::is_copy_constructible_v<shark::SceneQueryCursor>);
static_assert(std::is_copy_constructible_v<shark::SceneQueryPage>);
static_assert(!std::is_aggregate_v<shark::SceneQueryPage>);
static_assert(std::is_same_v<decltype(std::declval<shark::SceneQueryPage&>().entities()),
                             std::span<const shark::SceneInspectedEntity>>);
static_assert(!HasTemporaryEntities<shark::SceneQueryPage>);
static_assert(!HasTemporaryScope<shark::SceneQueryPage>);
static_assert(!HasTemporaryCursor<shark::SceneQueryPage>);
static_assert(!HasChildrenVector<shark::SceneInspectedEntity>);
static_assert(!HasChildrenVector<shark::SceneInspectedHierarchy>);

} // namespace

TEST_CASE("scene query empty pages retain scope and accept revision zero", "[scene-query]") {
    auto owner = scene();
    const auto page = query(*owner);
    CHECK(page.entities().empty());
    CHECK(page.scanned_slots() == 0);
    CHECK_FALSE(page.next_cursor());
    CHECK(page.revision().scene_instance != 0);
    CHECK(page.revision().revision == 0);
    CHECK(page.scope() == shark::SceneScope{shark::AuthoredSceneScope{id<shark::SceneId>(1)}});

    shark::SceneQueryOptions options;
    options.expected_revision = page.revision();
    CHECK(query(*owner, options).revision() == page.revision());
    options.expected_revision = shark::SceneRevision{};
    rejected(owner->query_entities(options), shark::ErrorCode::conflict);
    options.expected_revision = shark::SceneRevision{page.revision().scene_instance, 1};
    rejected(owner->query_entities(options), shark::ErrorCode::conflict);
}

TEST_CASE("scene query orders all roots and direct children by live slot", "[scene-query]") {
    auto owner = scene();
    const auto first = create(*owner, seed(90, "first"));
    const auto child = create(*owner, seed(80, "child", first));
    const auto grandchild = create(*owner, seed(70, "grandchild", child));
    const auto other = create(*owner, seed(60, "other"));
    const auto sibling = create(*owner, seed(50, "sibling", first));
    const auto last = create(*owner, seed(40, "last", other));
    const std::vector all{first, child, grandchild, other, sibling, last};
    CHECK(handles(query(*owner)) == all);

    shark::SceneQueryOptions options;
    options.selection = shark::SceneQuerySelection::roots;
    const std::vector roots{first, other};
    CHECK(handles(query(*owner, options)) == roots);
    options.selection = shark::SceneQuerySelection::children;
    options.parent = first;
    const std::vector children{child, sibling};
    CHECK(handles(query(*owner, options)) == children);
    options.parent = child;
    CHECK(handles(query(*owner, options)) == std::vector{grandchild});
    options.parent = grandchild;
    CHECK(handles(query(*owner, options)).empty());

    REQUIRE(owner->destroy_entity(other));
    const auto reused = create(*owner, seed(100, "reused"));
    CHECK(reused.slot == other.slot);
    CHECK(handles(query(*owner)) == std::vector{first, child, grandchild, reused, sibling});
}

TEST_CASE("scene query field selection omits payload without changing identity or filters",
          "[scene-query]") {
    auto owner = scene();
    auto root_seed = seed(1, "parent");
    root_seed.transform.translation = {10.0, 20.0, 30.0};
    const auto root = create(*owner, root_seed);
    auto child_seed = seed(2, "selected", root);
    child_seed.transform.translation = {1.0, 2.0, 3.0};
    const auto child = create(*owner, child_seed);
    const auto reference = owner->reference(child);
    REQUIRE(reference);
    for (unsigned int mask = 0; mask < 8; ++mask) {
        CAPTURE(mask);
        const shark::SceneQueryFields fields{(mask & 1U) != 0, (mask & 2U) != 0, (mask & 4U) != 0};
        shark::SceneQueryOptions options;
        options.name_prefix = "sel";
        options.fields = fields;
        const auto page = query(*owner, options);
        const auto single = inspect(*owner, child, fields);
        for (const auto* value : {&page, &single}) {
            REQUIRE(value->entities().size() == 1);
            const auto& entity = value->entities().front();
            CHECK(entity.reference == reference.value());
            CHECK(entity.name.has_value() == fields.name);
            CHECK(entity.transform.has_value() == fields.transform);
            CHECK(entity.hierarchy.has_value() == fields.hierarchy);
            if (entity.name) {
                CHECK(entity.name->value == "selected");
            }
            if (entity.transform) {
                CHECK(entity.transform->local == child_seed.transform);
                CHECK(entity.transform->world_matrix[3][0] == 11.0);
                CHECK(entity.transform->world_matrix[3][1] == 22.0);
                CHECK(entity.transform->world_matrix[3][2] == 33.0);
            }
            if (entity.hierarchy) {
                CHECK(entity.hierarchy->parent == root);
                CHECK(entity.hierarchy->child_count == 0);
                CHECK(entity.hierarchy->depth == 2);
            }
        }
        CHECK(single.scanned_slots() == 1);
        CHECK_FALSE(single.next_cursor());
    }
    const auto root_page = inspect(*owner, root);
    REQUIRE(root_page.entities().front().hierarchy);
    CHECK_FALSE(root_page.entities().front().hierarchy->parent);
    CHECK(root_page.entities().front().hierarchy->child_count == 1);
    CHECK(root_page.entities().front().hierarchy->depth == 1);
}

TEST_CASE("scene query prefixes are exact valid UTF8 bytes without case or normalization folding",
          "[scene-query]") {
    auto owner = scene();
    const auto capital = create(*owner, seed(1, "Alpha"));
    const auto lower = create(*owner, seed(2, "alpha"));
    const std::string composed = "\xC3\xA9";
    const std::string decomposed = "e\xCC\x81";
    const auto accented = create(*owner, seed(3, composed + "clair"));
    const auto separate = create(*owner, seed(4, decomposed + "clair"));
    const auto bounded = create(*owner, seed(5, std::string(256, 'x')));
    shark::SceneQueryOptions options;
    options.name_prefix = "Al";
    CHECK(handles(query(*owner, options)) == std::vector{capital});
    options.name_prefix = "al";
    CHECK(handles(query(*owner, options)) == std::vector{lower});
    options.name_prefix = composed;
    CHECK(handles(query(*owner, options)) == std::vector{accented});
    options.name_prefix = decomposed;
    CHECK(handles(query(*owner, options)) == std::vector{separate});
    options.name_prefix = std::string(256, 'x');
    CHECK(handles(query(*owner, options)) == std::vector{bounded});
    for (const auto& invalid : {std::string(257, 'x'), std::string("\xC3"), std::string("\xC0\xAF"),
                                std::string("\xC2\x85"), std::string("\x7F"), std::string("bad\n"),
                                std::string("a\0b", 3)}) {
        options.name_prefix = invalid;
        rejected(owner->query_entities(options), shark::ErrorCode::invalid_argument);
    }
}

TEST_CASE("scene query rejects unsupported bounds and inconsistent selections", "[scene-query]") {
    auto owner = scene();
    const auto root = create(*owner, seed(1));
    for (const auto size : {std::size_t{0}, shark::scene_query_maximum_page_size + 1}) {
        shark::SceneQueryOptions options;
        options.page_size = size;
        rejected(owner->query_entities(options), shark::ErrorCode::invalid_argument);
    }
    for (const auto budget : {std::size_t{0}, shark::scene_query_maximum_scan_budget + 1}) {
        shark::SceneQueryOptions options;
        options.scan_budget = budget;
        rejected(owner->query_entities(options), shark::ErrorCode::invalid_argument);
    }
    shark::SceneQueryOptions options;
    options.selection = static_cast<shark::SceneQuerySelection>(99);
    rejected(owner->query_entities(options), shark::ErrorCode::invalid_argument);
    options.selection = shark::SceneQuerySelection::children;
    rejected(owner->query_entities(options), shark::ErrorCode::invalid_argument);
    for (const auto selection :
         {shark::SceneQuerySelection::all, shark::SceneQuerySelection::roots}) {
        options.selection = selection;
        options.parent = root;
        rejected(owner->query_entities(options), shark::ErrorCode::invalid_argument);
    }
}

TEST_CASE("scene query and inspection reject stale foreign and malformed handles",
          "[scene-query]") {
    auto owner = scene();
    auto foreign = scene();
    const auto old = create(*owner, seed(1));
    REQUIRE(owner->destroy_entity(old));
    const auto current = create(*owner, seed(2));
    const auto elsewhere = create(*foreign, seed(3));
    auto wrong_generation = current;
    ++wrong_generation.generation;
    for (const auto handle : {old, elsewhere, wrong_generation, shark::EntityHandle{}}) {
        shark::SceneQueryOptions options;
        options.selection = shark::SceneQuerySelection::children;
        options.parent = handle;
        rejected(owner->query_entities(options), shark::ErrorCode::invalid_id);
        rejected(owner->inspect_entity(handle), shark::ErrorCode::invalid_id);
    }
    CHECK(handles(inspect(*owner, current)).size() == 1);
}

TEST_CASE("scene query sparse pages advance through holes and expose empty continuation",
          "[scene-query]") {
    auto owner = scene();
    std::array<shark::EntityHandle, 6> entities;
    for (std::size_t index = 0; index < entities.size(); ++index) {
        entities[index] = create(*owner, seed(static_cast<std::uint32_t>(index + 1)));
    }
    for (const auto index : {0U, 1U, 3U, 4U}) {
        REQUIRE(owner->destroy_entity(entities[index]));
    }
    shark::SceneQueryOptions options;
    options.page_size = 1;
    options.scan_budget = 2;
    auto page = query(*owner, options);
    CHECK(page.entities().empty());
    CHECK(page.scanned_slots() == 2);
    REQUIRE(page.next_cursor());
    page = next(*owner, *page.next_cursor());
    CHECK(handles(page) == std::vector{entities[2]});
    CHECK(page.scanned_slots() == 1);
    REQUIRE(page.next_cursor());
    page = next(*owner, *page.next_cursor());
    CHECK(page.entities().empty());
    CHECK(page.scanned_slots() == 2);
    REQUIRE(page.next_cursor());
    page = next(*owner, *page.next_cursor());
    CHECK(handles(page) == std::vector{entities[5]});
    CHECK(page.scanned_slots() == 1);
    CHECK_FALSE(page.next_cursor());
}

TEST_CASE("scene query default and maximum payload limits are independent of scanning",
          "[scene-query]") {
    auto owner = scene();
    std::vector<shark::EntityHandle> expected;
    for (std::uint32_t value = 1; value <= 257; ++value) {
        expected.push_back(create(*owner, seed(value, std::string(256, 'x'))));
    }
    const auto defaults = query(*owner);
    CHECK(defaults.entities().size() == 64);
    CHECK(defaults.scanned_slots() == 64);
    REQUIRE(defaults.next_cursor());
    shark::SceneQueryOptions options;
    options.page_size = shark::scene_query_maximum_page_size;
    options.scan_budget = shark::scene_query_maximum_scan_budget;
    const auto maximum = query(*owner, options);
    CHECK(maximum.entities().size() == 256);
    CHECK(maximum.scanned_slots() == 256);
    std::size_t name_bytes = 0;
    for (const auto& entity : maximum.entities()) {
        REQUIRE(entity.name);
        name_bytes += entity.name->value.size();
    }
    CHECK(name_bytes == 65536);
    REQUIRE(maximum.next_cursor());
    const auto tail = next(*owner, *maximum.next_cursor());
    CHECK(handles(tail) == std::vector{expected.back()});
    CHECK(tail.scanned_slots() == 1);
    CHECK_FALSE(tail.next_cursor());

    options = {};
    options.name_prefix = "absent";
    const auto filtered = query(*owner, options);
    CHECK(filtered.entities().empty());
    CHECK(filtered.scanned_slots() == 256);
    REQUIRE(filtered.next_cursor());
    const auto filtered_tail = next(*owner, *filtered.next_cursor());
    CHECK(filtered_tail.entities().empty());
    CHECK(filtered_tail.scanned_slots() == 1);
    CHECK_FALSE(filtered_tail.next_cursor());

    options.name_prefix.clear();
    options.scan_budget = 1;
    CHECK(handles(query(*owner, options)).size() == 1);
}

TEST_CASE("scene query cursors own their selection and replay deterministic pages",
          "[scene-query]") {
    auto owner = scene();
    const auto root = create(*owner, seed(1, "parent"));
    const auto first = create(*owner, seed(2, "match-one", root));
    create(*owner, seed(3, "different", root));
    create(*owner, seed(4, "match-grandchild", first));
    const auto second = create(*owner, seed(5, "match-two", root));
    create(*owner, seed(6, "match-root"));
    shark::SceneQueryOptions options;
    options.selection = shark::SceneQuerySelection::children;
    options.parent = root;
    options.name_prefix = "match-";
    options.fields = {false, false, true};
    options.page_size = 1;
    options.scan_budget = 2;
    const auto page = query(*owner, options);
    CHECK(handles(page) == std::vector{first});
    REQUIRE(page.next_cursor());
    const auto cursor = *page.next_cursor();
    CHECK(cursor.revision() == page.revision());
    options = {};
    options.name_prefix = "replacement";
    options.page_size = 0;
    const auto empty = next(*owner, cursor);
    const auto replay = next(*owner, cursor);
    CHECK(empty.entities().empty());
    CHECK(replay.entities().empty());
    CHECK(empty.scanned_slots() == 2);
    CHECK(replay.scanned_slots() == empty.scanned_slots());
    CHECK(replay.revision() == empty.revision());
    REQUIRE(empty.next_cursor());
    auto selected = next(*owner, *empty.next_cursor());
    CHECK(handles(selected) == std::vector{second});
    REQUIRE(selected.entities().size() == 1);
    CHECK_FALSE(selected.entities().front().name);
    CHECK_FALSE(selected.entities().front().transform);
    REQUIRE(selected.entities().front().hierarchy);
    CHECK(selected.entities().front().hierarchy->parent == root);
    REQUIRE(selected.next_cursor());
    selected = next(*owner, *selected.next_cursor());
    CHECK(selected.entities().empty());
    CHECK(selected.scanned_slots() == 1);
    CHECK_FALSE(selected.next_cursor());
}

TEST_CASE("scene query reports high fanout as a bounded child count", "[scene-query]") {
    auto owner = scene();
    const auto root = create(*owner, seed(1));
    for (std::uint32_t value = 2; value <= 301; ++value) {
        create(*owner, seed(value, "child", root));
    }
    const auto page = inspect(*owner, root, {false, false, true});
    REQUIRE(page.entities().size() == 1);
    REQUIRE(page.entities().front().hierarchy);
    CHECK(page.entities().front().hierarchy->child_count == 300);
    CHECK(page.scanned_slots() == 1);
    CHECK_FALSE(page.next_cursor());
}

TEST_CASE("scene query cursor rvalue construction and assignment preserve both filter sources",
          "[scene-query]") {
    auto owner = scene();
    const std::string alpha = "alpha-prefix-with-owned-storage-";
    const std::string beta = "beta-prefix-with-owned-storage-";
    create(*owner, seed(1, alpha + "one"));
    create(*owner, seed(2, beta + "one"));
    const auto alpha_second = create(*owner, seed(3, alpha + "two"));
    const auto beta_second = create(*owner, seed(4, beta + "two"));
    const auto alpha_third = create(*owner, seed(5, alpha + "three"));
    const auto beta_third = create(*owner, seed(6, beta + "three"));
    shark::SceneQueryOptions options;
    options.name_prefix = alpha;
    options.page_size = 1;
    const auto alpha_page = query(*owner, options);
    options.name_prefix = beta;
    const auto beta_page = query(*owner, options);
    REQUIRE(alpha_page.next_cursor());
    REQUIRE(beta_page.next_cursor());
    auto alpha_source = *alpha_page.next_cursor();
    auto beta_source = *beta_page.next_cursor();
    const auto remaining = [&](const shark::SceneQueryCursor& cursor) {
        std::vector<shark::EntityHandle> result;
        auto page = next(*owner, cursor);
        std::size_t pages = 0;
        while (true) {
            REQUIRE(++pages <= 6);
            const auto selected = handles(page);
            result.insert(result.end(), selected.begin(), selected.end());
            if (!page.next_cursor()) {
                break;
            }
            page = next(*owner, *page.next_cursor());
        }
        return result;
    };
    auto destination = std::move(alpha_source);
    const std::vector alpha_expected{alpha_second, alpha_third};
    const std::vector beta_expected{beta_second, beta_third};
    CHECK(remaining(alpha_source) == alpha_expected);
    CHECK(remaining(destination) == alpha_expected);
    destination = std::move(beta_source);
    CHECK(remaining(beta_source) == beta_expected);
    CHECK(remaining(destination) == beta_expected);
    CHECK(remaining(alpha_source) == alpha_expected);
}

TEST_CASE("scene query pagination is complete and duplicate free across selections and budgets",
          "[scene-query]") {
    auto owner = scene();
    const auto root = create(*owner, seed(1, "keep-root"));
    const auto other = create(*owner, seed(2, "other-root"));
    const auto child = create(*owner, seed(3, "keep-child", root));
    const auto hole = create(*owner, seed(4, "removed", root));
    const auto sibling = create(*owner, seed(5, "other-child", root));
    const auto cousin = create(*owner, seed(6, "keep-cousin", other));
    const auto grandchild = create(*owner, seed(7, "keep-grandchild", child));
    const auto last = create(*owner, seed(8, "keep-last", root));
    REQUIRE(owner->destroy_entity(hole));
    struct Selection {
        shark::SceneQuerySelection kind;
        std::optional<shark::EntityHandle> parent;
        std::vector<shark::EntityHandle> all;
        std::vector<shark::EntityHandle> prefixed;
    };
    const std::array cases{
        Selection{shark::SceneQuerySelection::all,
                  std::nullopt,
                  {root, other, child, sibling, cousin, grandchild, last},
                  {root, child, cousin, grandchild, last}},
        Selection{shark::SceneQuerySelection::roots, std::nullopt, {root, other}, {root}},
        Selection{
            shark::SceneQuerySelection::children, root, {child, sibling, last}, {child, last}}};
    for (const auto& selection : cases) {
        for (const bool prefix : {false, true}) {
            for (const std::size_t page_size : {1U, 2U, 7U}) {
                for (const std::size_t scan_budget : {1U, 3U, 8U}) {
                    CAPTURE(static_cast<int>(selection.kind), prefix, page_size, scan_budget);
                    shark::SceneQueryOptions options;
                    options.selection = selection.kind;
                    options.parent = selection.parent;
                    options.name_prefix = prefix ? "keep-" : "";
                    options.page_size = page_size;
                    options.scan_budget = scan_budget;
                    auto page = query(*owner, options);
                    const auto revision = page.revision();
                    std::vector<shark::EntityHandle> collected;
                    std::size_t scanned = 0;
                    std::size_t pages = 0;
                    while (true) {
                        REQUIRE(++pages <= 8);
                        CHECK(page.revision() == revision);
                        CHECK(page.entities().size() <= page_size);
                        REQUIRE(page.scanned_slots() >= 1);
                        CHECK(page.scanned_slots() <= scan_budget);
                        scanned += page.scanned_slots();
                        const auto selected = handles(page);
                        collected.insert(collected.end(), selected.begin(), selected.end());
                        if (!page.next_cursor()) {
                            break;
                        }
                        page = next(*owner, *page.next_cursor());
                    }
                    CHECK(scanned == 8);
                    CHECK(collected == (prefix ? selection.prefixed : selection.all));
                }
            }
        }
    }
}

TEST_CASE("scene query rejects revision tokens and cursors from another scene incarnation",
          "[scene-query]") {
    auto owner = scene();
    auto other = scene();
    const auto first = create(*owner, seed(1));
    create(*owner, seed(2));
    const auto other_first = create(*other, seed(1));
    create(*other, seed(2));
    shark::SceneQueryOptions options;
    options.page_size = 1;
    const auto page = query(*owner, options);
    REQUIRE(page.next_cursor());
    CHECK(page.revision().revision == query(*other).revision().revision);
    options.expected_revision = page.revision();
    rejected(other->query_entities(options), shark::ErrorCode::conflict);
    rejected(other->inspect_entity(other_first, {}, page.revision()), shark::ErrorCode::conflict);
    rejected(other->continue_query(*page.next_cursor()), shark::ErrorCode::conflict);
    REQUIRE(owner->inspect_entity(first, {}, page.revision()));
}

TEST_CASE("scene query continuations become stale after every committed mutation kind",
          "[scene-query]") {
    for (const bool deferred : {false, true}) {
        for (const auto kind :
             {shark::SceneMutationKind::create, shark::SceneMutationKind::destroy,
              shark::SceneMutationKind::set_name, shark::SceneMutationKind::set_transform,
              shark::SceneMutationKind::reparent}) {
            CAPTURE(deferred, static_cast<int>(kind));
            auto owner = scene();
            const auto first = create(*owner, seed(1, "first"));
            const auto second = create(*owner, seed(2, "second"));
            shark::SceneQueryOptions options;
            options.page_size = 1;
            const auto page = query(*owner, options);
            REQUIRE(page.next_cursor());
            shark::Transform changed;
            changed.translation = {1.0, 2.0, 3.0};
            switch (kind) {
            case shark::SceneMutationKind::create:
                if (deferred) {
                    REQUIRE(owner->defer_create(seed(3)));
                } else {
                    REQUIRE(owner->create_entity(seed(3)));
                }
                break;
            case shark::SceneMutationKind::destroy:
                if (deferred) {
                    REQUIRE(owner->defer_destroy(second));
                } else {
                    REQUIRE(owner->destroy_entity(second));
                }
                break;
            case shark::SceneMutationKind::set_name:
                if (deferred) {
                    REQUIRE(owner->defer_set_name(second, {"changed"}));
                } else {
                    REQUIRE(owner->set_name(second, {"changed"}));
                }
                break;
            case shark::SceneMutationKind::set_transform:
                if (deferred) {
                    REQUIRE(owner->defer_set_transform(second, changed));
                } else {
                    REQUIRE(owner->set_transform(second, changed));
                }
                break;
            case shark::SceneMutationKind::reparent:
                if (deferred) {
                    REQUIRE(owner->defer_reparent(second, first, shark::ReparentMode::keep_local));
                } else {
                    REQUIRE(owner->reparent(second, first, shark::ReparentMode::keep_local));
                }
                break;
            }
            if (deferred) {
                const auto committed = next(*owner, *page.next_cursor());
                CHECK(handles(committed) == std::vector{second});
                REQUIRE(committed.entities().front().name);
                CHECK(committed.entities().front().name->value == "second");
                const auto applied = owner->apply_deferred();
                REQUIRE(applied);
                REQUIRE(applied.value().outcomes.size() == 1);
                REQUIRE(applied.value().outcomes.front().result);
            }
            CHECK(query(*owner).revision().revision == page.revision().revision + 1);
            rejected(owner->continue_query(*page.next_cursor()), shark::ErrorCode::conflict);
            options.expected_revision = page.revision();
            rejected(owner->query_entities(options), shark::ErrorCode::conflict);
            rejected(owner->inspect_entity(first, {}, page.revision()), shark::ErrorCode::conflict);
            REQUIRE(page.entities().front().name);
            CHECK(page.entities().front().name->value == "first");
        }
    }
}

TEST_CASE("scene query cursors survive noops rejected edits and failed deferred outcomes",
          "[scene-query]") {
    auto owner = scene();
    const auto first = create(*owner, seed(1));
    const auto second = create(*owner, seed(2));
    shark::SceneQueryOptions options;
    options.page_size = 1;
    const auto page = query(*owner, options);
    REQUIRE(page.next_cursor());
    REQUIRE(owner->set_name(first, {}));
    REQUIRE(owner->set_transform(first, {}));
    REQUIRE(owner->reparent(first, std::nullopt, shark::ReparentMode::keep_local));
    REQUIRE(owner->reparent(first, std::nullopt, shark::ReparentMode::keep_world));
    REQUIRE(owner->apply_deferred());
    CHECK_FALSE(owner->set_name(first, {""}));
    CHECK_FALSE(owner->reparent(first, first, shark::ReparentMode::keep_local));
    REQUIRE(owner->defer_set_name(first, {}));
    REQUIRE(owner->defer_set_transform(first, {}));
    REQUIRE(owner->defer_reparent(first, std::nullopt, shark::ReparentMode::keep_local));
    REQUIRE(owner->defer_create(seed(1)));
    const auto applied = owner->apply_deferred();
    REQUIRE(applied);
    REQUIRE(applied.value().outcomes.size() == 4);
    CHECK_FALSE(applied.value().outcomes.back().result);
    CHECK(applied.value().revision_before == page.revision().revision);
    CHECK(applied.value().revision_after == page.revision().revision);
    CHECK(handles(next(*owner, *page.next_cursor())) == std::vector{second});
    options.expected_revision = page.revision();
    CHECK(query(*owner, options).revision() == page.revision());
}

TEST_CASE("scene queries during visits read committed state and do not mutate the queue",
          "[scene-query]") {
    auto owner = scene();
    const auto first = create(*owner, seed(1, "first"));
    const auto second = create(*owner, seed(2, "second"));
    shark::SceneQueryOptions options;
    options.page_size = 1;
    const auto page = query(*owner, options);
    REQUIRE(page.next_cursor());
    REQUIRE(owner->defer_set_name(second, {"queued"}));
    std::size_t visits = 0;
    const auto visited =
        owner->visit_entities([&](const shark::EntityReference& reference) -> shark::Result<void> {
            ++visits;
            const auto committed = query(*owner);
            CHECK(committed.revision() == page.revision());
            CHECK(handles(committed) == std::vector{first, second});
            REQUIRE(committed.entities().back().name);
            CHECK(committed.entities().back().name->value == "second");
            CHECK(handles(inspect(*owner, reference.handle)).size() == 1);
            CHECK(handles(next(*owner, *page.next_cursor())) == std::vector{second});
            return {};
        });
    REQUIRE(visited);
    CHECK(visits == 2);
    const auto state = owner->stats();
    REQUIRE(state);
    CHECK(state.value().pending_mutations == 1);
    CHECK(state.value().revision == page.revision().revision);
    REQUIRE(owner->apply_deferred());
    rejected(owner->continue_query(*page.next_cursor()), shark::ErrorCode::conflict);
}

TEST_CASE("scene query pages remain owned after mutation close and scene destruction",
          "[scene-query]") {
    auto owner = scene();
    const auto first = create(*owner, seed(1, "original"));
    const auto child = create(*owner, seed(2, "child", first));
    auto page = query(*owner);
    const auto copied = page;
    const auto revision = page.revision();
    REQUIRE(owner->set_name(first, {"changed"}));
    REQUIRE(owner->destroy_entity(first));
    REQUIRE(owner->close());
    owner.reset();
    CHECK(page.revision() == revision);
    CHECK(handles(page) == std::vector{first, child});
    REQUIRE(page.entities().front().name);
    CHECK(page.entities().front().name->value == "original");
    REQUIRE(page.entities().front().hierarchy);
    CHECK(page.entities().front().hierarchy->child_count == 1);
    REQUIRE(page.entities().back().hierarchy);
    CHECK(page.entities().back().hierarchy->parent == first);
    page = copied;
    bool copied_values_valid = false;
    std::thread reader([copy = copied, &copied_values_valid, first, child, revision] {
        copied_values_valid = copy.revision() == revision && copy.entities().size() == 2 &&
                              copy.entities()[0].reference.handle == first &&
                              copy.entities()[1].reference.handle == child &&
                              copy.entities()[0].name &&
                              copy.entities()[0].name->value == "original";
    });
    reader.join();
    CHECK(copied_values_valid);
}

TEST_CASE("scene query APIs reject wrong thread and closed scene while owned pages remain readable",
          "[scene-query]") {
    auto owner = scene();
    const auto first = create(*owner, seed(1));
    create(*owner, seed(2));
    shark::SceneQueryOptions options;
    options.page_size = 1;
    const auto page = query(*owner, options);
    REQUIRE(page.next_cursor());
    std::array<bool, 3> denied{};
    std::thread worker([&] {
        const auto a = owner->query_entities();
        const auto b = owner->continue_query(*page.next_cursor());
        const auto c = owner->inspect_entity(first);
        denied = {!a && a.error().code == shark::ErrorCode::conflict,
                  !b && b.error().code == shark::ErrorCode::conflict,
                  !c && c.error().code == shark::ErrorCode::conflict};
    });
    worker.join();
    CHECK(denied == std::array{true, true, true});
    REQUIRE(owner->close());
    rejected(owner->query_entities(), shark::ErrorCode::conflict);
    rejected(owner->continue_query(*page.next_cursor()), shark::ErrorCode::conflict);
    rejected(owner->inspect_entity(first), shark::ErrorCode::conflict);
    CHECK(handles(page) == std::vector{first});
}

TEST_CASE("scene query copies authored region and replica identity scope and authority",
          "[scene-query]") {
    const shark::AuthorityStamp authority{id<shark::RegionId>(10), id<shark::RegionInstanceId>(11),
                                          42};
    const shark::SceneScope region = shark::RegionSceneScope{id<shark::WorldId>(20), authority};
    const shark::SceneScope replica =
        shark::ReplicaSceneScope{id<shark::WorldId>(20), id<shark::ReplicaSessionId>(30)};
    const shark::SceneScope authored = shark::AuthoredSceneScope{id<shark::SceneId>(1)};
    for (const auto& scope : {authored, region, replica}) {
        auto owner = scene(scope);
        auto value = seed(1, "scope");
        if (!std::holds_alternative<shark::AuthoredSceneScope>(scope)) {
            value.identity = id<shark::WorldEntityId>(1);
        }
        if (std::holds_alternative<shark::ReplicaSceneScope>(scope)) {
            value.replica_authority = authority;
        }
        const auto entity = create(*owner, value);
        const auto expected = owner->reference(entity);
        REQUIRE(expected);
        const auto page = inspect(*owner, entity, {false, false, false});
        REQUIRE(owner->close());
        CHECK(page.scope() == scope);
        REQUIRE(page.entities().size() == 1);
        CHECK(page.entities().front().reference == expected.value());
        CHECK_FALSE(page.entities().front().name);
        CHECK_FALSE(page.entities().front().transform);
        CHECK_FALSE(page.entities().front().hierarchy);
    }
}
