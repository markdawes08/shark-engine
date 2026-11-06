// Original Shark-authored local file fixtures; project MIT license.
#include "shark/world_cell_io.hpp"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <future>
#include <string>

namespace {
using namespace shark;

template <typename T> T take(Result<T> result) {
    if (!result) {
        INFO(result.error().message);
        INFO(result.error().context.operation);
    }
    REQUIRE(result);
    return std::move(result).value();
}

struct Files {
    std::filesystem::path root;
    std::filesystem::path cell;
    Files() {
        root = std::filesystem::current_path() /
               ("shark-world-cell-" + take(generate_stable_id()).to_string());
        REQUIRE(std::filesystem::create_directory(root));
        cell = root / std::filesystem::path{u8"Cell 雪.sharkcell"};
    }
    ~Files() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    Files(const Files&) = delete;
    Files& operator=(const Files&) = delete;
};

void write(const std::filesystem::path& path, std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream);
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    stream.close();
    REQUIRE(stream);
}
} // namespace

TEST_CASE("world cell file input owns multi-chunk bytes read on a worker", "[world-cell-io]") {
    Files files;
    std::vector<std::byte> expected(3 * 64 * 1024 + 17);
    for (std::size_t index = 0; index < expected.size(); ++index)
        expected[index] = static_cast<std::byte>(index % 251);
    write(files.cell, expected);
    auto future = std::async(std::launch::async,
                             [&] { return read_world_cell_file(files.cell, expected.size()); });
    const auto owned = take(future.get());
    REQUIRE(std::filesystem::remove(files.cell));
    CHECK(owned == expected);
    // Reading bytes does not advertise a valid cell or activate a Scene.
    CHECK_FALSE(decode_world_cell(owned));

    write(files.cell, {});
    CHECK(take(read_world_cell_file(files.cell)).empty());
}

TEST_CASE("world cell file input enforces path kind and input byte budgets", "[world-cell-io]") {
    Files files;
    const std::vector<std::byte> bytes(129, std::byte{0x42});
    write(files.cell, bytes);
    const auto too_small = read_world_cell_file(files.cell, bytes.size() - 1);
    REQUIRE_FALSE(too_small);
    CHECK(too_small.error().code == ErrorCode::invalid_argument);
    CHECK(too_small.error().context.operation == "world.cell.read");
    CHECK(take(read_world_cell_file(files.cell, bytes.size())) == bytes);
    CHECK_FALSE(read_world_cell_file(files.cell, 0));
    CHECK_FALSE(read_world_cell_file(files.cell, world_cell_maximum_payload_bytes + 1));
    const auto missing = read_world_cell_file(files.root / "missing.sharkcell");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::io_failure);
    CHECK_FALSE(read_world_cell_file(files.root));
    CHECK_FALSE(read_world_cell_file({}));
    const auto nul_path = files.cell.native() + std::filesystem::path::value_type{};
    const auto embedded_nul = read_world_cell_file(std::filesystem::path{nul_path});
    REQUIRE_FALSE(embedded_nul);
    CHECK(embedded_nul.error().code == ErrorCode::invalid_path);

    std::filesystem::resize_file(files.cell, world_cell_maximum_payload_bytes + 1);
    const auto over_profile = read_world_cell_file(files.cell);
    REQUIRE_FALSE(over_profile);
    CHECK(over_profile.error().code == ErrorCode::invalid_argument);
}

TEST_CASE("world cell file input observes cancellation before native reads", "[world-cell-io]") {
    Files files;
    const std::vector<std::byte> bytes(32, std::byte{0x13});
    write(files.cell, bytes);
    std::stop_source cancelled;
    REQUIRE(cancelled.request_stop());
    const auto stopped = read_world_cell_file(files.cell, bytes.size(), cancelled.get_token());
    REQUIRE_FALSE(stopped);
    CHECK(stopped.error().code == ErrorCode::conflict);
    CHECK(stopped.error().context.operation == "world.cell.read");
    CHECK(take(read_world_cell_file(files.cell, bytes.size())) == bytes);
}
