#include <shark/client/scene_launch.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace std::literals;

TEST_CASE("Runtime scene launch requires explicit bounded prepared inputs", "[scene-launch]") {
    std::vector<std::string_view> args{
        "--scene",         "scene.bin",    "--camera",       "20000000-0000-4000-8000-000000000004",
        "--vertex-shader", "scene-vs.spv", "--pixel-shader", "scene-ps.spv"};
    auto parsed = shark::client::parse_scene_arguments(args);
    REQUIRE(parsed);
    CHECK(parsed.value().extent == shark::client::WindowExtent{640, 480});
    CHECK_FALSE(parsed.value().frames);
    CHECK(parsed.value().validation == shark::client::GraphicsValidation::off);
    args.insert(args.end(), {"--mesh", "mesh.bin", "--mesh", "other.bin", "--extent", "96x80",
                             "--frames", "2", "--scene-ms", "60000", "--validation", "required",
                             "--device", "auto", "--capture-directory", "new-output"});
    parsed = shark::client::parse_scene_arguments(args);
    REQUIRE(parsed);
    CHECK(parsed.value().content.meshes.size() == 2);
    CHECK(parsed.value().extent == shark::client::WindowExtent{96, 80});
    CHECK(parsed.value().frames == 2);
    CHECK(parsed.value().duration == 60000ms);
    CHECK(parsed.value().validation == shark::client::GraphicsValidation::required);
    args.insert(args.end(), {"--frames", "3"});
    CHECK_FALSE(shark::client::parse_scene_arguments(args));
}

TEST_CASE("Runtime scene launch rejects malformed or excessive values before device access",
          "[scene-launch]") {
    const std::vector<std::string_view> base{
        "--scene",         "scene.bin", "--camera",       "20000000-0000-4000-8000-000000000004",
        "--vertex-shader", "v",         "--pixel-shader", "p"};
    for (const auto& [key, value] : std::array<std::pair<std::string_view, std::string_view>, 10>{
             {{"--frames"sv, "0"sv},
              {"--frames"sv, "601"sv},
              {"--scene-ms"sv, "60001"sv},
              {"--extent"sv, "0x80"sv},
              {"--extent"sv, "2049x80"sv},
              {"--extent"sv, "1x1x1"sv},
              {"--device"sv, "AUTO"sv},
              {"--validation"sv, "on"sv},
              {"--capture-directory"sv, ""sv},
              {"--unknown"sv, "x"sv}}}) {
        auto args = base;
        args.insert(args.end(), {key, value});
        INFO(key << " " << value);
        CHECK_FALSE(shark::client::parse_scene_arguments(args));
    }
    auto args = base;
    args.pop_back();
    CHECK_FALSE(shark::client::parse_scene_arguments(args));
    args = base;
    args[3] = "nil";
    CHECK_FALSE(shark::client::parse_scene_arguments(args));
    args = base;
    for (int index = 0; index < 127; ++index)
        args.insert(args.end(), {"--mesh", "m"});
    CHECK_FALSE(shark::client::parse_scene_arguments(args));
    args = base;
    const std::string invalid_path{"a\0b", 3};
    args[1] = invalid_path;
    CHECK_FALSE(shark::client::parse_scene_arguments(args));
    CHECK_FALSE(shark::client::run_scene("invalid", {}));
}
