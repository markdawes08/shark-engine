#pragma once

// Source-authored connected terrain fixture; repository MIT license. Generated
// source and cell files stay in the caller's managed test scratch directory.
#include "shark/tools/prepared_content.hpp"
#include "shark/tools/project_files.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>
#endif

namespace seamless_content {
using namespace shark;
namespace fs = std::filesystem;
template <class T> T take(Result<T> value) {
    if (!value)
        UNSCOPED_INFO(value.error().context.operation << ": " << value.error().message);
    REQUIRE(value);
    return std::move(value).value();
}
template <class Id> Id id(unsigned value) {
    std::string text{"c0570000-0000-4000-8000-000000000000"};
    constexpr std::string_view hex{"0123456789abcdef"};
    for (unsigned index = 0; index < 8; ++index) {
        text[35 - index] = hex[value & 15U];
        value >>= 4U;
    }
    return Id{take(StableId::parse(text))};
}
inline std::string read_text(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input);
    return {std::istreambuf_iterator<char>{input}, {}};
}
inline std::vector<std::byte> read_bytes(const fs::path& path) {
    const auto text = read_text(path);
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    return {bytes.begin(), bytes.end()};
}
inline std::uint64_t little(std::span<const std::byte> bytes, std::size_t offset, unsigned count) {
    REQUIRE(count <= 8);
    REQUIRE(offset <= bytes.size());
    REQUIRE(count <= bytes.size() - offset);
    std::uint64_t value{};
    for (unsigned index = 0; index < count; ++index)
        value |= std::to_integer<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    return value;
}
// Independent literal layout/geometry checks, rather than round-tripping the
// encoder through its decoder and treating agreement as an expected result.
inline void inspect_geometry(std::span<const std::byte> bytes, WorldCellKey key, bool first) {
    REQUIRE(bytes.size() >= 88);
    CHECK(std::memcmp(bytes.data(), "SHKCELL1", 8) == 0);
    CHECK(little(bytes, 8, 4) == 2);
    ContentDigest body_digest;
    std::copy_n(bytes.begin() + 12, 32, body_digest.bytes.begin());
    CHECK(take(sha256(bytes.subspan(44))) == body_digest);
    CHECK(std::equal(key.world.value().bytes().begin(), key.world.value().bytes().end(),
                     bytes.begin() + 44));
    for (unsigned axis = 0; axis < 3; ++axis) {
        const auto bits = static_cast<std::uint32_t>(little(bytes, 60 + axis * 4, 4));
        const auto observed = std::bit_cast<std::int32_t>(bits);
        const std::array<std::int64_t, 3> expected{key.cell.x, key.cell.y, key.cell.z};
        CHECK(observed == expected[axis]);
    }
    CHECK(little(bytes, 72, 8) == 1);
    const auto scene_size = little(bytes, 80, 4);
    REQUIRE(scene_size <= bytes.size() - 84);
    std::size_t cursor = 84 + static_cast<std::size_t>(scene_size);
    CHECK(little(bytes, cursor, 4) == 4);
    cursor += 4;
    const double left = first ? 1020.0 : 0.0;
    const std::array<double, 12> expected{left, 0, 7, left + 4, 0, 7, left + 4, 0, 3, left, 0, 3};
    for (const auto coordinate : expected) {
        const auto value = std::bit_cast<double>(little(bytes, cursor, 8));
        cursor += 8;
        CHECK(value == coordinate);
        CHECK(value * 1024.0 == std::trunc(value * 1024.0));
    }
    for (unsigned surface = 0; surface < 2; ++surface) {
        CHECK(little(bytes, cursor, 4) == 2);
        cursor += 4;
        for (const auto index : {0U, 1U, 2U, 0U, 2U, 3U}) {
            CHECK(little(bytes, cursor, 4) == index);
            cursor += 4;
        }
    }
    CHECK(cursor == bytes.size());
    constexpr std::string_view private_marker{"private_connected_source_metadata"};
    const auto marker = std::as_bytes(std::span{private_marker.data(), private_marker.size()});
    CHECK(std::search(bytes.begin(), bytes.end(), marker.begin(), marker.end()) == bytes.end());
}

struct CellFile {
    WorldCellKey key;
    std::uint64_t revision;
    fs::path path;
    std::vector<std::byte> payload;
    SceneId scene;
    AuthoredEntityId ground;
};
class AuthoredWorld final {
  public:
    const fs::path root;
    std::vector<CellFile> cells;
    std::shared_ptr<const PreparedAssets> assets;
    bool complete{false};
    AuthoredWorld(const fs::path& managed_parent, WorldId world, std::int64_t base_cell)
        : root(managed_parent / ("authored-world-" + take(generate_stable_id()).to_string())),
          assets(take(PreparedAssets::create())), parent_(managed_parent) {
        REQUIRE(managed_parent.is_absolute());
        REQUIRE(base_cell >= world_cell_min);
        REQUIRE(base_cell <= world_cell_max - 2);
        fs::create_directories(managed_parent);
        REQUIRE(fs::create_directory(root));
        tools::ProjectDocument project{
            id<tools::ProjectId>(1), world, Name{"Connected authored world"},
            id<SceneId>(101),        {},    "{\"private_connected_source_metadata\":true}"};
        for (unsigned index = 0; index < 3; ++index)
            project.scenes.push_back(
                {id<SceneId>(101 + index),
                 take(ProjectPath::parse("cell-" + std::to_string(index) + ".sharkscene"))});
        auto files = take(tools::ProjectFiles::open(root));
        const auto project_path = take(ProjectPath::parse("World.sharkproject"));
        for (unsigned index = 0; index < 3; ++index) {
            tools::SourceEntity ground{
                id<AuthoredEntityId>(201 + index), {}, Name{"Walkable strip"}};
            ground.transform.translation = {index == 0 ? 1022.0 : 2.0, 0, 5};
            ground.transform.scale = {4, 4, 4};
            ground.mesh_renderer = MeshRenderer{builtin_plane_asset_id(), 0, false};
            ground.extras_json = "{\"private_connected_source_metadata\":true}";
            const WorldCellKey key{world, {base_cell + index, 0, 0}};
            const tools::SceneDocument scene{project.project,
                                             id<SceneId>(101 + index),
                                             WorldPosition{world, key.cell, {0, 0, 0}},
                                             {ground},
                                             "{}"};
            take(files->write_scene(project, scene, {}));
        }
        take(files->write_project(project_path, project, {}));
        saved_.push_back({root / project_path.text(), read_text(root / project_path.text())});
        for (unsigned index = 0; index < 3; ++index) {
            const auto source_path = root / project.scenes[index].path.text();
            saved_.push_back({source_path, read_text(source_path)});
            auto loaded = take(files->load_scene(project, id<SceneId>(101 + index)));
            const auto before = take(loaded.owner->capture());
            const auto history = take(loaded.owner->history());
            const WorldCellKey key{world, {base_cell + index, 0, 0}};
            const std::array selected{id<AuthoredEntityId>(201 + index)};
            const auto payload = take(tools::prepare_world_cell_geometry(
                *loaded.owner, before.revision, key, 1, assets, selected, selected));
            inspect_geometry(payload, key, index == 0);
            const auto output =
                take(ProjectPath::parse("cell-" + std::to_string(index) + ".sharkcell"));
            const auto publication = take(files->write_world_cell(output, payload));
            CHECK(publication.revision.byte_count() == payload.size());
            CHECK(read_bytes(root / output.text()) == payload);
            const auto after = take(loaded.owner->capture());
            CHECK(after.revision == before.revision);
            CHECK(after.scene == before.scene);
            CHECK_FALSE(after.dirty);
            CHECK_FALSE(before.dirty);
            const auto after_history = take(loaded.owner->history());
            CHECK(after_history.undo_entries == history.undo_entries);
            CHECK(after_history.redo_entries == history.redo_entries);
            cells.push_back({key, 1, root / output.text(), payload, id<SceneId>(101 + index),
                             selected.front()});
        }
        verify_source();
    }
    ~AuthoredWorld() {
        if (!complete) {
            std::cerr << "Preserved authored-world fixture: " << root.generic_string() << '\n';
            return;
        }
        if (root.is_absolute() && root.parent_path() == parent_ &&
            root.filename().string().starts_with("authored-world-")) {
            std::error_code error;
            const auto resolved_parent = fs::canonical(parent_, error);
            if (error)
                return;
            const auto resolved_root = fs::canonical(root, error);
            if (!error && resolved_root.parent_path() == resolved_parent &&
                resolved_root.filename() == root.filename())
                fs::remove_all(resolved_root, error);
        }
    }
    AuthoredWorld(const AuthoredWorld&) = delete;
    AuthoredWorld& operator=(const AuthoredWorld&) = delete;
    void verify_source() const {
        for (const auto& [path, expected] : saved_)
            CHECK(read_text(path) == expected);
    }
    void restore_cell(std::size_t index) const {
        REQUIRE(index < cells.size());
        const auto& cell = cells[index];
        REQUIRE(cell.path.parent_path() == root);
        std::ofstream output{cell.path, std::ios::binary | std::ios::trunc};
        output.write(reinterpret_cast<const char*>(cell.payload.data()),
                     static_cast<std::streamsize>(cell.payload.size()));
        output.close();
        REQUIRE(output);
    }

  private:
    const fs::path parent_;
    std::vector<std::pair<fs::path, std::string>> saved_;
};

inline constexpr std::uint64_t child_rss_budget = 1024ULL * 1024 * 1024;
// This is actual OS working-set/VmRSS, separate from residency's conservative
// logical reservation accounting. Call only for a currently owned live PID.
inline std::uint64_t child_rss(std::uint64_t pid) {
    REQUIRE(pid > 0);
    std::uint64_t bytes{};
#ifdef _WIN32
    REQUIRE(pid <= std::numeric_limits<DWORD>::max());
    const auto process =
        OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, static_cast<DWORD>(pid));
    REQUIRE(process != nullptr);
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    const auto observed = GetProcessMemoryInfo(process, &counters, sizeof(counters));
    const auto closed = CloseHandle(process);
    REQUIRE(observed != 0);
    REQUIRE(closed != 0);
    bytes = static_cast<std::uint64_t>(counters.WorkingSetSize);
#else
    std::ifstream status{fs::path{"/proc"} / std::to_string(pid) / "status"};
    REQUIRE(status);
    std::string line;
    std::size_t read{};
    while (std::getline(status, line)) {
        read += line.size();
        REQUIRE(read <= 64 * 1024);
        if (!line.starts_with("VmRSS:"))
            continue;
        std::istringstream value{line.substr(6)};
        std::uint64_t kib{};
        std::string unit;
        value >> kib >> unit;
        REQUIRE(value);
        REQUIRE(unit == "kB");
        REQUIRE(kib <= std::numeric_limits<std::uint64_t>::max() / 1024);
        bytes = kib * 1024;
        break;
    }
#endif
    REQUIRE(bytes > 0);
    CHECK(bytes <= child_rss_budget);
    return bytes;
}
} // namespace seamless_content
