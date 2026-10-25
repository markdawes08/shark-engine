#include <shark/scene.hpp>

#include "scene_edit_access.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
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

shark::AuthorityStamp authority(const std::uint64_t epoch = 1) {
    return {id<shark::RegionId>(10), id<shark::RegionInstanceId>(11), epoch};
}

shark::EntitySeed authored(const std::uint32_t value) {
    return {id<shark::AuthoredEntityId>(value)};
}

std::unique_ptr<shark::Scene>
scene(shark::SceneScope scope = shark::AuthoredSceneScope{id<shark::SceneId>(1)},
      const shark::SceneLimits limits = {}) {
    auto result = shark::Scene::create(std::move(scope), limits);
    REQUIRE(result);
    return std::move(result).value();
}

shark::EntityHandle create(shark::Scene& owner, shark::EntitySeed seed) {
    auto result = owner.create_entity(std::move(seed));
    REQUIRE(result);
    return result.value();
}

shark::SceneRevision revision(const shark::Scene& value) {
    const auto stats = value.stats();
    REQUIRE(stats);
    return {stats.value().instance, stats.value().revision};
}
template <class T> T take(shark::Result<T> value) {
    REQUIRE(value);
    return std::move(value).value();
}
} // namespace

TEST_CASE("scene edit candidate isolates writes and publishes once preserving surviving handles",
          "[scene-edit]") {
    auto live = scene();
    const auto a = create(*live, authored(10));
    const auto b = create(*live, authored(20));
    const auto expected = revision(*live);
    auto candidate = take(shark::scene_detail::SceneEditAccess::prepare(*live, expected));
    REQUIRE(candidate->set_name(a, {"staged"}));
    REQUIRE(candidate->destroy_entity(b));
    const auto c = create(*candidate, authored(30));
    CHECK(take(live->entity_state(a)).name.value == "Entity");
    REQUIRE(live->reference(b));
    auto committed =
        take(shark::scene_detail::SceneEditAccess::commit(*live, *candidate, expected));
    CHECK(committed == shark::SceneRevision{expected.scene_instance, expected.revision + 1});
    CHECK(take(live->find(id<shark::AuthoredEntityId>(10))) == a);
    CHECK(take(live->entity_state(a)).name.value == "staged");
    CHECK_FALSE(live->reference(b));
    REQUIRE(live->reference(c));
    CHECK(take(candidate->stats()).closed);
    CHECK_FALSE(shark::scene_detail::SceneEditAccess::commit(*live, *candidate, committed));
}

TEST_CASE("scene edit candidates preserve unused and reused slot generations", "[scene-edit]") {
    auto live = scene();
    const auto a = create(*live, authored(10));
    const auto b = create(*live, authored(20));
    REQUIRE(live->destroy_entity(a));
    const auto expected = revision(*live);
    auto candidate = take(shark::scene_detail::SceneEditAccess::prepare(*live, expected));
    const auto c = create(*candidate, authored(30));
    CHECK(c.slot == a.slot);
    CHECK(c.generation != a.generation);
    REQUIRE(shark::scene_detail::SceneEditAccess::commit(*live, *candidate, expected));
    CHECK_FALSE(live->reference(a));
    REQUIRE(live->reference(b));
    REQUIRE(live->reference(c));
}

TEST_CASE("scene edit empty candidate consumes without advancing revision", "[scene-edit]") {
    auto live = scene();
    create(*live, authored(10));
    const auto expected = revision(*live);
    auto candidate = take(shark::scene_detail::SceneEditAccess::prepare(*live, expected));
    CHECK(take(shark::scene_detail::SceneEditAccess::commit(*live, *candidate, expected)) ==
          expected);
    CHECK(take(candidate->stats()).closed);
    CHECK(revision(*live) == expected);
}

TEST_CASE("scene edit rejects stale foreign ordinary and nested candidates", "[scene-edit]") {
    auto live = scene();
    const auto a = create(*live, authored(10));
    const auto expected = revision(*live);
    auto candidate = take(shark::scene_detail::SceneEditAccess::prepare(*live, expected));
    CHECK_FALSE(shark::scene_detail::SceneEditAccess::prepare(*candidate, expected));
    auto foreign = scene();
    CHECK_FALSE(
        shark::scene_detail::SceneEditAccess::commit(*foreign, *candidate, revision(*foreign)));
    CHECK_FALSE(shark::scene_detail::SceneEditAccess::commit(*live, *foreign, expected));
    CHECK_FALSE(shark::scene_detail::SceneEditAccess::commit(*live, *live, expected));
    REQUIRE(live->set_name(a, {"live changed"}));
    CHECK_FALSE(shark::scene_detail::SceneEditAccess::prepare(*live, expected));
    CHECK_FALSE(shark::scene_detail::SceneEditAccess::commit(*live, *candidate, expected));
    CHECK(take(live->entity_state(a)).name.value == "live changed");
}

TEST_CASE("scene edit requires authored scope and owner mutation boundaries", "[scene-edit]") {
    auto region = scene(shark::RegionSceneScope{id<shark::WorldId>(2), authority()});
    CHECK_FALSE(shark::scene_detail::SceneEditAccess::prepare(*region, revision(*region)));
    auto live = scene();
    const auto a = create(*live, authored(10));
    const auto expected = revision(*live);
    auto candidate = take(shark::scene_detail::SceneEditAccess::prepare(*live, expected));
    REQUIRE(live->defer_set_name(a, {"queued"}));
    CHECK_FALSE(shark::scene_detail::SceneEditAccess::prepare(*live, expected));
    CHECK_FALSE(shark::scene_detail::SceneEditAccess::commit(*live, *candidate, expected));
    REQUIRE(live->apply_deferred());
    const auto current = revision(*live);
    REQUIRE(live->visit_entities([&](const shark::EntityReference&) -> shark::Result<void> {
        CHECK_FALSE(shark::scene_detail::SceneEditAccess::prepare(*live, current));
        return {};
    }));
    std::thread wrong(
        [&] { CHECK_FALSE(shark::scene_detail::SceneEditAccess::prepare(*live, current)); });
    wrong.join();
    auto fresh = take(shark::scene_detail::SceneEditAccess::prepare(*live, current));
    REQUIRE(fresh->close());
    CHECK_FALSE(shark::scene_detail::SceneEditAccess::commit(*live, *fresh, current));
    REQUIRE(live->close());
    CHECK_FALSE(shark::scene_detail::SceneEditAccess::prepare(*live, current));
}

TEST_CASE("scene edit candidate pending work rejects publication without touching live",
          "[scene-edit]") {
    auto live = scene();
    const auto a = create(*live, authored(10));
    const auto expected = revision(*live);
    auto candidate = take(shark::scene_detail::SceneEditAccess::prepare(*live, expected));
    REQUIRE(candidate->defer_set_name(a, {"not applied"}));
    CHECK_FALSE(shark::scene_detail::SceneEditAccess::commit(*live, *candidate, expected));
    CHECK(revision(*live) == expected);
    CHECK(take(live->entity_state(a)).name.value == "Entity");
}

TEST_CASE("scene edit forced publication advances an unchanged candidate exactly once",
          "[scene-edit]") {
    auto live = scene();
    const auto handle = create(*live, authored(10));
    const auto expected = revision(*live);
    REQUIRE(shark::scene_detail::SceneEditAccess::check(*live, expected));
    auto candidate = take(shark::scene_detail::SceneEditAccess::prepare(*live, expected));
    const auto published =
        take(shark::scene_detail::SceneEditAccess::commit(*live, *candidate, expected, true));
    CHECK(published == shark::SceneRevision{expected.scene_instance, expected.revision + 1});
    CHECK(take(live->find(id<shark::AuthoredEntityId>(10))) == handle);
    CHECK(take(candidate->stats()).closed);
    CHECK_FALSE(shark::scene_detail::SceneEditAccess::check(*live, expected));
    REQUIRE(live->defer_set_name(handle, {"pending"}));
    CHECK_FALSE(shark::scene_detail::SceneEditAccess::check(*live, published));
}
