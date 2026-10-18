#include "shark/project_path.hpp"
#include "shark/stable_id.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {
namespace fs = std::filesystem;

std::string utf8(std::u8string_view text) {
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

class TempTree final {
  public:
    TempTree() {
        const auto id = shark::generate_stable_id();
        if (!id) {
            throw std::runtime_error{"cannot obtain temporary fixture identity"};
        }
        path = fs::temp_directory_path() / ("shark-core-paths-" + id.value().to_string());
        if (!fs::create_directory(path)) {
            throw std::runtime_error{"temporary fixture directory already exists"};
        }
    }
    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;
    ~TempTree() {
        std::error_code error;
        fs::remove_all(path, error);
    }
    fs::path path;
};

class RestoreWorkingDirectory final {
  public:
    RestoreWorkingDirectory() : original_(fs::current_path()) {}
    RestoreWorkingDirectory(const RestoreWorkingDirectory&) = delete;
    RestoreWorkingDirectory& operator=(const RestoreWorkingDirectory&) = delete;
    ~RestoreWorkingDirectory() {
        std::error_code error;
        fs::current_path(original_, error);
    }

  private:
    fs::path original_;
};

void make_file(const fs::path& path) {
    std::ofstream file{path};
    file << "fixture";
    if (!file) {
        throw std::runtime_error{"cannot create fixture file"};
    }
}

void directory_link(const fs::path& target, const fs::path& link) {
    std::error_code error;
    fs::create_directory_symlink(target, link, error);
#if defined(_WIN32)
    if (error == std::errc::operation_not_permitted || error.value() == 1314) {
        SKIP("Windows directory symlink capability unavailable: " + error.message());
    }
#endif
    INFO(error.message());
    REQUIRE_FALSE(error);
}
} // namespace

TEST_CASE("Portable project references preserve Unicode and spaces", "[core][paths]") {
    const auto name = utf8(u8"世界/maps/My first scene.sharkscene");
    auto input = name;
    const auto parsed = shark::ProjectPath::parse(input);
    REQUIRE(parsed);
    input.clear();
    CHECK(parsed.value().text() == name);
    CHECK(shark::ProjectPath::parse("COM10.scene"));
    CHECK(shark::ProjectPath::parse("some.CON"));
    CHECK(shark::ProjectPath::parse(".metadata/source.json"));
    CHECK(shark::ProjectPath::parse(std::string(255, 'a')));
    auto longest = std::string(4096, 'a');
    for (std::size_t index = 200; index < longest.size(); index += 201) {
        longest[index] = '/';
    }
    CHECK(shark::ProjectPath::parse(longest));
    longest.push_back('a');
    CHECK_FALSE(shark::ProjectPath::parse(longest));
}

TEST_CASE("Portable references reject traversal and Windows path aliases everywhere",
          "[core][paths]") {
    const std::array invalid{"",
                             ".",
                             "..",
                             "a/..",
                             "a/../b",
                             "./a",
                             "a/./b",
                             "/absolute",
                             "//server/share",
                             "C:/absolute",
                             "C:relative",
                             "\\\\server\\share",
                             "a\\b",
                             "a//b",
                             "a/",
                             "a ",
                             "a.",
                             "a/ /file",
                             "a/./file",
                             "file:stream",
                             "file?name",
                             "file*name",
                             "file<name",
                             "file>name",
                             "file|name",
                             "file\"name",
                             "a\nname",
                             "a\tname",
                             "CON",
                             "con.txt",
                             "PrN",
                             "a/AUX.png",
                             "NUL",
                             "COM1",
                             "COM9.scene",
                             "lpt1.ext",
                             "LPT9",
                             "CONIN$",
                             "conout$.txt"};
    for (const auto* text : invalid) {
        CAPTURE(text);
        const auto parsed = shark::ProjectPath::parse(text);
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().code == shark::ErrorCode::invalid_path);
        CHECK(parsed.error().context.path == text);
        CHECK(parsed.error().context.operation == "project_path.parse");
    }
    CHECK_FALSE(shark::ProjectPath::parse(utf8(u8"COM¹.txt")));
    CHECK_FALSE(shark::ProjectPath::parse(utf8(u8"LPT²")));
    CHECK_FALSE(shark::ProjectPath::parse(utf8(u8"com³")));
    CHECK_FALSE(shark::ProjectPath::parse("CON .txt"));
    CHECK_FALSE(shark::ProjectPath::parse(std::string{"a\0b", 3}));
    CHECK_FALSE(shark::ProjectPath::parse(std::string(256, 'a')));
    CHECK_FALSE(shark::ProjectPath::parse(std::string(4097, 'a')));
    CHECK(shark::ProjectPath::parse(std::string(4097, 'a')).error().context.path.empty());
}

TEST_CASE("Portable references reject malformed UTF-8 without lossy conversion", "[core][paths]") {
    const std::array invalid{std::string{"\x80"},
                             std::string{"\xc0\xaf"},
                             std::string{"\xc2"},
                             std::string{"\xe0\x80\xaf"},
                             std::string{"\xed\xa0\x80"},
                             std::string{"\xf0\x80\x80\xaf"},
                             std::string{"\xf4\x90\x80\x80"},
                             std::string{"\xf5\x80\x80\x80"},
                             std::string{"\xe2\x28\xa1"},
                             std::string{"\xff"}};
    for (const auto& text : invalid) {
        const auto result = shark::ProjectPath::parse(text);
        REQUIRE_FALSE(result);
        CHECK(result.error().context.path.empty());
    }
    CHECK(shark::ProjectPath::parse(utf8(u8"a/é世界🌊")));
}

TEST_CASE("Project roots resolve existing and missing Unicode descendants", "[core][paths]") {
    TempTree fixture;
    const auto root_path = fixture.path / fs::path{u8"Project 世界 with spaces"};
    fs::create_directories(root_path / fs::path{u8"世界"});
    make_file(root_path / fs::path{u8"世界/scene one.json"});
    const auto root = shark::ProjectRoot::open(root_path);
    REQUIRE(root);
    CHECK(root.value().native_path().is_absolute());
    CHECK(root.value().native_path() == fs::canonical(root_path));
    const auto existing = shark::ProjectPath::parse(utf8(u8"世界/scene one.json"));
    REQUIRE(existing);
    const auto resolved = root.value().resolve(existing.value());
    REQUIRE(resolved);
    CHECK(resolved.value() == fs::canonical(root_path / fs::path{u8"世界/scene one.json"}));
    const auto missing = shark::ProjectPath::parse(utf8(u8"new folder/世界/scene.json"));
    REQUIRE(missing);
    const auto future = root.value().resolve(missing.value());
    REQUIRE(future);
    CHECK(future.value() == root.value().native_path() / fs::path{u8"new folder/世界/scene.json"});
    CHECK_FALSE(fs::exists(root_path / "new folder"));
}

TEST_CASE("Project root validation reports inaccessible and non-directory inputs",
          "[core][paths]") {
    TempTree fixture;
    make_file(fixture.path / "file");
    const auto file = shark::ProjectRoot::open(fixture.path / "file");
    REQUIRE_FALSE(file);
    CHECK(file.error().code == shark::ErrorCode::invalid_path);
    const auto missing = shark::ProjectRoot::open(fixture.path / "missing");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == shark::ErrorCode::io_failure);
    const auto root = shark::ProjectRoot::open(fixture.path);
    REQUIRE(root);
    const auto invalid = root.value().resolve(shark::ProjectPath::parse("file/child").value());
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == shark::ErrorCode::invalid_path);
}

TEST_CASE("Project roots remain anchored to their opened directory", "[core][paths]") {
    TempTree fixture;
    RestoreWorkingDirectory restore;
    const auto relative_root = fs::relative(fixture.path, fs::current_path());
    const auto root = shark::ProjectRoot::open(relative_root);
    REQUIRE(root);
    CHECK(root.value().native_path() == fs::canonical(fixture.path));
    const auto relative = shark::ProjectPath::parse("future/file");
    fs::current_path(fixture.path.parent_path());
    const auto resolved = root.value().resolve(relative.value());
    REQUIRE(resolved);
    CHECK(resolved.value() == fs::canonical(fixture.path) / "future/file");
    fs::remove(fixture.path);
    const auto removed = root.value().resolve(relative.value());
    REQUIRE_FALSE(removed);
    CHECK(removed.error().code == shark::ErrorCode::io_failure);
}

TEST_CASE("Native root paths reject NUL and invalid encoding before filesystem access",
          "[core][paths]") {
    TempTree fixture;
    auto nul_name = fixture.path.native();
    nul_name.push_back(fs::path::value_type{});
    nul_name += fs::path{"ignored-tail"}.native();
    const auto embedded = shark::ProjectRoot::open(fs::path{nul_name});
    REQUIRE_FALSE(embedded);
    CHECK(embedded.error().code == shark::ErrorCode::invalid_path);
    CHECK(embedded.error().context.path.empty());
#if defined(_WIN32)
    auto malformed_name = fixture.path.native();
    malformed_name.push_back(static_cast<wchar_t>(0xd800));
#else
    auto malformed_name = fixture.path.native();
    malformed_name.push_back(static_cast<char>(0xff));
#endif
    const auto malformed = shark::ProjectRoot::open(fs::path{malformed_name});
    REQUIRE_FALSE(malformed);
    CHECK(malformed.error().code == shark::ErrorCode::invalid_path);
    CHECK(malformed.error().context.path.empty());
}

TEST_CASE("Project path errors retain caller operation and request", "[core][paths]") {
    shark::ErrorContext context;
    context.operation = "asset.import";
    context.request_id = "request-4";
    const auto parsed = shark::ProjectPath::parse("../escape", context);
    context.operation.clear();
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().context.operation == "asset.import");
    CHECK(parsed.error().context.request_id == "request-4");
    CHECK(parsed.error().context.path == "../escape");
}

TEST_CASE("Project resolution permits links within the canonical root", "[core][paths]") {
    TempTree fixture;
    fs::create_directories(fixture.path / "project" / "actual");
    directory_link(fixture.path / "project" / "actual", fixture.path / "project" / "alias");
    const auto root = shark::ProjectRoot::open(fixture.path / "project");
    REQUIRE(root);
    const auto result = root.value().resolve(shark::ProjectPath::parse("alias/new/file").value());
    REQUIRE(result);
    CHECK(result.value() == fs::canonical(fixture.path / "project" / "actual") / "new/file");
    make_file(fixture.path / "project" / "actual" / "existing");
    fs::create_symlink(fixture.path / "project" / "actual" / "existing",
                       fixture.path / "project" / "file-alias");
    const auto file = root.value().resolve(shark::ProjectPath::parse("file-alias").value());
    REQUIRE(file);
    CHECK(file.value() == fs::canonical(fixture.path / "project" / "actual" / "existing"));
}

TEST_CASE("Project resolution rejects linked ancestors escaping to a sibling prefix",
          "[core][paths]") {
    TempTree fixture;
    fs::create_directories(fixture.path / "project");
    fs::create_directories(fixture.path / "project-neighbor");
    make_file(fixture.path / "project-neighbor" / "existing");
    directory_link(fixture.path / "project-neighbor", fixture.path / "project" / "escape");
    fs::create_symlink(fixture.path / "project-neighbor" / "existing",
                       fixture.path / "project" / "file-escape");
    const auto root = shark::ProjectRoot::open(fixture.path / "project");
    REQUIRE(root);
    for (const auto* reference : {"escape", "escape/existing", "escape/new/file", "file-escape"}) {
        const auto result = root.value().resolve(shark::ProjectPath::parse(reference).value());
        REQUIRE_FALSE(result);
        CHECK(result.error().code == shark::ErrorCode::path_outside_root);
        CHECK(result.error().context.path == reference);
    }
}

TEST_CASE("Project resolution rejects dangling links instead of treating them as new paths",
          "[core][paths]") {
    TempTree fixture;
    fs::create_directory(fixture.path / "project");
    directory_link(fixture.path / "missing-outside", fixture.path / "project" / "dangling");
    const auto root = shark::ProjectRoot::open(fixture.path / "project");
    REQUIRE(root);
    const auto result = root.value().resolve(shark::ProjectPath::parse("dangling/new").value());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == shark::ErrorCode::io_failure);
}

TEST_CASE("Project resolution rejects symlink cycles with an owned error", "[core][paths]") {
    TempTree fixture;
    fs::create_directory(fixture.path / "project");
    directory_link(fixture.path / "project" / "second", fixture.path / "project" / "first");
    directory_link(fixture.path / "project" / "first", fixture.path / "project" / "second");
    const auto root = shark::ProjectRoot::open(fixture.path / "project");
    REQUIRE(root);
    const auto result = root.value().resolve(shark::ProjectPath::parse("first/new").value());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == shark::ErrorCode::io_failure);
}

TEST_CASE("Project resolution rejects replacement of its opened root by an outside link",
          "[core][paths]") {
    TempTree fixture;
    const auto project = fixture.path / "project";
    const auto moved = fixture.path / "original-project";
    const auto outside = fixture.path / "outside";
    fs::create_directory(project);
    fs::create_directory(outside);
    make_file(outside / "child");
    const auto root = shark::ProjectRoot::open(project);
    REQUIRE(root);
    REQUIRE(fs::canonical(project).parent_path() == fs::canonical(fixture.path));
    REQUIRE(fs::canonical(moved.parent_path()) == fs::canonical(fixture.path));
    fs::rename(project, moved);
    directory_link(outside, project);

    const auto result = root.value().resolve(shark::ProjectPath::parse("child").value());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == shark::ErrorCode::path_outside_root);
    CHECK(result.error().context.operation == "project_root.resolve");
    CHECK(result.error().context.path == "child");
    CHECK(fs::exists(moved));
    CHECK(fs::exists(outside / "child"));
}
