// Engine-authored local development payloads; project MIT license.
#include "shark/world_cell_io.hpp"
#include "shark/world_cell_residency.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

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
template <typename Id> Id id(char digit) {
    std::string text{"75000000-0000-4000-8000-000000000001"};
    text.back() = digit;
    return Id{take(StableId::parse(text))};
}
WorldCellKey key(std::int64_t x) {
    return {id<WorldId>('1'), {1'000'000'000 + x, 0, 0}};
}
std::vector<std::byte> payload(WorldCellKey cell, std::uint64_t revision) {
    PreparedEntity root{id<AuthoredEntityId>('2')};
    root.name.value = "Streamed ground";
    root.mesh_renderer = MeshRenderer{builtin_plane_asset_id(), 0, true};
    root.rigid_body = RigidBody{};
    return take(encode_world_cell(
        {cell,
         revision,
         {id<SceneId>('3'), WorldPosition{cell.world, cell.cell, {0, 0, 0}}, {root}}}));
}
struct Files {
    std::filesystem::path root;
    Files()
        : root(std::filesystem::current_path() /
               ("shark-cell-stream-" + take(generate_stable_id()).to_string())) {
        REQUIRE(std::filesystem::create_directory(root));
    }
    ~Files() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path path(WorldCellKey cell, std::uint64_t revision) const {
        return root / (std::to_string(cell.cell.x) + "-" + std::to_string(revision) + ".sharkcell");
    }
    void write(WorldCellKey cell, std::uint64_t revision, std::span<const std::byte> bytes) const {
        std::ofstream stream(path(cell, revision), std::ios::binary | std::ios::trunc);
        REQUIRE(stream);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        stream.close();
        REQUIRE(stream);
    }
};
WorldCellJobStatus ready(WorldCellResidency& owner, WorldCellJobId id) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto jobs = take(owner.jobs());
        const auto found =
            std::find_if(jobs.begin(), jobs.end(), [&](const auto& job) { return job.id == id; });
        REQUIRE(found != jobs.end());
        if (found->state == WorldCellJobState::ready)
            return *found;
        REQUIRE((found->state == WorldCellJobState::queued ||
                 found->state == WorldCellJobState::running));
        std::this_thread::yield();
    }
    throw std::runtime_error{"Bounded cell reader did not finish before the fixture deadline"};
}
} // namespace

TEST_CASE("real cell files prefetch replace fail retry and evict under resident limits",
          "[world-cell-residency]") {
    Files files;
    const auto a1 = payload(key(0), 1);
    const auto a2 = payload(key(0), 2);
    const auto b1 = payload(key(1), 1);
    const auto c1 = payload(key(2), 1);
    const auto d1 = payload(key(3), 1);
    files.write(key(0), 1, a1);
    files.write(key(1), 1, b1);
    files.write(key(2), 1, c1);
    files.write(key(3), 1, d1);
    const auto assets = take(PreparedAssets::create());
    WorldCellResidencyLimits limits;
    limits.maximum_resident_cells = 3;
    const auto owner = take(WorldCellResidency::create(
        [&](const WorldCellReadRequest& request, std::stop_token stop) {
            return read_world_cell_file(files.path(request.key, request.content_revision),
                                        request.maximum_bytes, stop);
        },
        limits));
    const auto first = take(owner->submit(key(0), 1, assets));
    ready(*owner, first);
    CHECK(take(owner->residents()).empty());
    CHECK(take(owner->drain()) == 1);
    const auto original = take(owner->resident(key(0)));
    const auto original_reference =
        take(original->scene().reference(take(original->scene().find(id<AuthoredEntityId>('2')))));
    const auto first_receipt = take(owner->take(first));
    REQUIRE(first_receipt.resident_generation);
    CHECK(first_receipt.state == WorldCellJobState::published);

    // Missing replacement file fails without retiring the last valid Scene.
    const auto missing = take(owner->submit(key(0), 2, assets));
    ready(*owner, missing);
    CHECK(take(owner->drain()) == 1);
    const auto failed = take(owner->take(missing));
    CHECK(failed.state == WorldCellJobState::failed);
    REQUIRE(failed.error);
    CHECK(failed.error->code == ErrorCode::io_failure);
    CHECK(take(owner->resident(key(0))) == original);
    CHECK(take(original->scene().resolve(original_reference)) == original_reference.handle);

    files.write(key(0), 2, a2);
    const auto replacement = take(owner->submit(key(0), 2, assets));
    ready(*owner, replacement);
    CHECK(take(owner->drain()) == 1);
    const auto second_receipt = take(owner->take(replacement));
    REQUIRE(second_receipt.resident_generation);
    CHECK(*second_receipt.resident_generation != *first_receipt.resident_generation);
    const auto current = take(owner->resident(key(0)));
    CHECK(current->content_revision() == 2);
    CHECK_FALSE(current->scene().resolve(original_reference));

    const auto next = take(owner->submit(key(1), 1, assets));
    const auto farther = take(owner->submit(key(2), 1, assets));
    ready(*owner, next);
    ready(*owner, farther);
    CHECK(take(owner->residents()).size() == 1);
    CHECK(take(owner->drain()) == 1);
    CHECK(take(owner->residents()).size() == 2);
    CHECK(take(owner->drain()) == 1);
    CHECK(take(owner->residents()).size() == 3);
    CHECK(take(owner->take(next)).state == WorldCellJobState::published);
    CHECK(take(owner->take(farther)).state == WorldCellJobState::published);

    // Explicit pressure, with a valid fourth file, cannot evict another cell.
    const auto overflow = take(owner->submit(key(3), 1, assets));
    ready(*owner, overflow);
    CHECK(take(owner->drain()) == 1);
    CHECK(take(owner->take(overflow)).state == WorldCellJobState::failed);
    CHECK_FALSE(owner->resident(key(3)));
    CHECK(take(owner->residents()).size() == 3);
    const auto usage = take(owner->usage());
    CHECK(usage.bytes_read == a1.size() + a2.size() + b1.size() + c1.size() + d1.size());
    CHECK(usage.pending_bytes == 0);
    CHECK(usage.retained_jobs == 0);
    CHECK(usage.resident_cells == 3);
    CHECK(usage.resident_bytes <= limits.maximum_resident_bytes);
    CHECK(usage.peak_resident_bytes <= limits.maximum_resident_bytes);
    CHECK(usage.peak_pending_bytes <= limits.maximum_pending_bytes);
    CHECK(usage.peak_pending_bytes >=
          2 * (world_cell_maximum_payload_bytes + assets->retained_bytes()));

    CHECK_FALSE(owner->evict(key(0), *first_receipt.resident_generation));
    take(owner->evict(key(0), *second_receipt.resident_generation));
    CHECK_FALSE(owner->resident(key(0)));
    const auto retry = take(owner->submit(key(3), 1, assets));
    ready(*owner, retry);
    CHECK(take(owner->drain()) == 1);
    CHECK(take(owner->take(retry)).state == WorldCellJobState::published);
    const auto entries = take(owner->residents());
    CHECK(entries.size() == 3);
    for (const auto& entry : entries)
        take(owner->evict(entry.key, entry.generation));
    CHECK(take(owner->usage()).resident_bytes == 0);
    take(owner->close());
    CHECK(take(owner->usage()).closed);
}
