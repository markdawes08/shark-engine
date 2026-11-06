#include <shark/server/region_image.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string>

namespace {
using namespace shark;
using namespace shark::server;
template <class T> T image_id(unsigned value) {
    std::string text = "a0500000-0000-4000-8000-000000000000";
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < 8; ++index) {
        text[35 - index] = digits[value & 15U];
        value >>= 4U;
    }
    return T{StableId::parse(text).value()};
}
RegionProcessImage empty_image() {
    const auto world = image_id<WorldId>(1);
    const auto first = image_id<RegionId>(2);
    const auto second = image_id<RegionId>(3);
    const RegionSceneScope scope{world, {first, image_id<RegionInstanceId>(4), 1}};
    const WorldPosition origin{world, {world_cell_max, 0, world_cell_min}, {1000, 0, 24}};
    const SceneRevision revision{11, 7};
    PhysicsWorldConfig config;
    config.tick_hz = 20;
    config.collision_substeps = 3;
    CombatRulesPortableState rules{
        {"scripts/image.lua", "function on_combat(k,t,a,b,d,s) return true,1,10 end"}, {}, {}, 9};
    rules.state[0] = std::numeric_limits<std::int64_t>::min();
    rules.state[7] = std::numeric_limits<std::int64_t>::max();
    return {{origin, 8},
            {{first, -10, 0, -10, 10, 110}, {second, 0, 10, -10, 10, 110}},
            {},
            {5, revision, scope, config, 17, 23, 2, {}, {}},
            {revision,
             scope,
             5,
             origin,
             12,
             {},
             17,
             MonotonicTime{40},
             ExecutionPermit{20, 3},
             {},
             {std::byte{0xee}}},
            {revision,
             scope,
             5,
             17,
             MonotonicTime{40},
             81,
             true,
             {},
             {9, false},
             ExecutionPermit{20, 3},
             {},
             {std::byte{0xdd}}},
            rules,
            {},
            {},
            {},
            {},
            {},
            123,
            image_id<RegionInstanceId>(6)};
}
} // namespace

TEST_CASE("Region process image has canonical bounded bytes and owns explicit portable state",
          "[server-region-image]") {
    auto original = empty_image();
    const auto encoded = encode_region_process_image(original);
    REQUIRE(encoded);
    const std::array<std::byte, 8> prefix{std::byte{0x53}, std::byte{0x48}, std::byte{0x52},
                                          std::byte{0x49}, std::byte{0},    std::byte{0},
                                          std::byte{0},    std::byte{1}};
    REQUIRE(encoded.value().size() > prefix.size());
    CHECK(std::equal(prefix.begin(), prefix.end(), encoded.value().begin()));
    const auto decoded = decode_region_process_image(encoded.value());
    REQUIRE(decoded);
    CHECK(decoded.value().frame == original.frame);
    CHECK(decoded.value().physics.completed_tick == 17);
    CHECK(decoded.value().physics.next_contact_sequence == 23);
    CHECK(decoded.value().physics.discarded_events == 2);
    CHECK(decoded.value().rules.source.text == original.rules.source.text);
    CHECK(decoded.value().rules.state == original.rules.state);
    CHECK(decoded.value().rules.evaluations == 9);
    CHECK(decoded.value().route_watermark == 123);
    CHECK(decoded.value().executor_instance == original.executor_instance);
    CHECK_FALSE(decoded.value().movement.bytes.empty());
    CHECK_FALSE(decoded.value().combat.bytes.empty());
    CHECK(encode_region_process_image(decoded.value()).value() == encoded.value());
    original.movement.bytes.clear();
    original.combat.bytes.assign(100, std::byte{0xab});
    CHECK(encode_region_process_image(original).value() == encoded.value());
}

TEST_CASE("Region process image rejects unknown versions truncation trailing bytes and huge counts",
          "[server-region-image]") {
    const auto encoded = encode_region_process_image(empty_image());
    REQUIRE(encoded);
    auto changed = encoded.value();
    changed[7] = std::byte{2};
    CHECK_FALSE(decode_region_process_image(changed));
    for (const auto length :
         {std::size_t{0}, std::size_t{7}, std::size_t{80}, encoded.value().size() - 1})
        CHECK_FALSE(decode_region_process_image(std::span{encoded.value()}.first(length)));
    changed = encoded.value();
    changed.push_back(std::byte{0});
    CHECK_FALSE(decode_region_process_image(changed));
    changed = encoded.value();
    // Eight-byte header, explicit 64-byte WorldPosition and 8-byte generation.
    // The next count must reject before allocating any region records.
    std::fill(changed.begin() + 80, changed.begin() + 88, std::byte{0xff});
    CHECK_FALSE(decode_region_process_image(changed));
    auto unsupported = empty_image();
    unsupported.combat.script_instance = 44;
    CHECK_FALSE(encode_region_process_image(unsupported));
}

TEST_CASE("Region process image enforces aggregate byte capacity before publishing bytes",
          "[server-region-image]") {
    auto image = empty_image();
    for (unsigned index = 0; index < 9; ++index)
        image.assets.push_back({image_id<AssetId>(100 + index),
                                std::vector<std::byte>(prepared_mesh_maximum_payload_bytes)});
    CHECK_FALSE(encode_region_process_image(image));
    image.assets.clear();
    const std::vector<std::byte> oversized(region_process_image_maximum_bytes + 1);
    CHECK_FALSE(decode_region_process_image(oversized));
}

TEST_CASE("Region process image rejects malformed NPC intent and duplicate source bindings",
          "[server-region-image]") {
    auto image = empty_image();
    const EntityReference source{{image.physics.revision.scene_instance, 0, 1},
                                 image_id<WorldEntityId>(10),
                                 image.physics.scope,
                                 image.physics.scope.authority};
    image.npcs.push_back({source, {image.physics.world, 0, 1}, math::Vec3{1, 0, 0}});
    const auto encoded = encode_region_process_image(image);
    REQUIRE(encoded);
    REQUIRE(decode_region_process_image(encoded.value()));
    for (const auto invalid : {math::Vec3{1, 1, 0}, math::Vec3{movement_speed + 1, 0, 0},
                               math::Vec3{std::numeric_limits<double>::quiet_NaN(), 0, 0}}) {
        auto changed = image;
        changed.npcs.front().pending = invalid;
        CHECK_FALSE(encode_region_process_image(changed));
    }
    auto duplicate = image;
    duplicate.npcs.push_back(duplicate.npcs.front());
    CHECK_FALSE(encode_region_process_image(duplicate));
    duplicate.npcs.back().source.handle.slot = 1;
    duplicate.npcs.back().source.identity = image_id<WorldEntityId>(11);
    CHECK_FALSE(encode_region_process_image(duplicate)); // Same native body.
    auto changed = encoded.value();
    // One NPC's final pending vector precedes empty peer/writer counts (16),
    // route watermark (8) and executor UUID (16). Change Y to an exact +1.
    const std::array<std::byte, 8> one{std::byte{0x3f}, std::byte{0xf0}, std::byte{0},
                                       std::byte{0},    std::byte{0},    std::byte{0},
                                       std::byte{0},    std::byte{0}};
    std::copy(one.begin(), one.end(), changed.end() - 56);
    CHECK_FALSE(decode_region_process_image(changed));
}
