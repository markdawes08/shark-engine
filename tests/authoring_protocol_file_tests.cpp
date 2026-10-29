#include <shark/tools/authoring_protocol.hpp>
#include <shark/tools/project_files.hpp>

#include "project_files_native.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace {
using namespace shark;
using namespace shark::tools;

class Scratch final {
  public:
    Scratch() {
        const auto id = generate_stable_id();
        REQUIRE(id);
        path = std::filesystem::temp_directory_path() /
               ("shark-protocol-publication-" + id.value().to_string());
        REQUIRE(std::filesystem::create_directory(path));
    }
    ~Scratch() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    std::filesystem::path path;
};

class Fault final {
  public:
    Fault()
        : previous_(
              detail::set_file_test_control({detail::FileFault::none, after_first, nullptr})) {}
    ~Fault() {
        static_cast<void>(detail::set_file_test_control(previous_));
    }

  private:
    static void after_first(detail::FilePhase phase, void*) noexcept {
        if (phase == detail::FilePhase::published)
            static_cast<void>(
                detail::set_file_test_control({detail::FileFault::publish, nullptr, nullptr}));
    }
    detail::FileTestControl previous_;
};
} // namespace

TEST_CASE(
    "Protocol creation preserves and reports an initial scene after manifest publication failure",
    "[authoring-protocol]") {
    Scratch scratch;
    auto created = AuthoringProtocol::create(scratch.path);
    REQUIRE(created);
    auto owner = std::move(created).value();
    constexpr auto request =
        R"({"jsonrpc":"2.0","protocol_version":1,"id":"partial","method":"project.create","params":{"path":"game.sharkproject","scene_path":"start.sharkscene","name":"Partial","project_id":"40000000-0000-4000-8000-000000000001","world_id":"40000000-0000-4000-8000-000000000002","scene_id":"40000000-0000-4000-8000-000000000003"}})";
    {
        Fault fault;
        const auto reply = owner->handle(request);
        REQUIRE(reply);
        CHECK(reply.value().failed);
        CHECK(reply.value().json.find("\"success\":false") != std::string::npos);
        CHECK(reply.value().json.find("\"published\":true") != std::string::npos);
        CHECK(reply.value().json.find("\"project\":null") != std::string::npos);
    }
    CHECK_FALSE(std::filesystem::exists(scratch.path / "game.sharkproject"));
    std::ifstream input{scratch.path / "start.sharkscene", std::ios::binary};
    REQUIRE(input);
    const std::string bytes{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
    input.close();
    const auto scene = decode_scene_document(bytes);
    REQUIRE(scene);
    CHECK(scene.value().document.entities.empty());
    const auto status =
        owner->handle(R"({"jsonrpc":"2.0","protocol_version":1,"id":2,"method":"project.status"})");
    REQUIRE(status);
    CHECK(status.value().json.find("\"open\":false") != std::string::npos);
    // Partial publication did not retain a competing writable owner or delete evidence.
    auto files = ProjectFiles::open(scratch.path);
    REQUIRE(files);
    CHECK(std::filesystem::exists(scratch.path / "start.sharkscene"));
}
