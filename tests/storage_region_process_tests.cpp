#include "shark/server/region_journal.hpp"
#include "shark/storage/store.hpp"

#include "store_test_hooks.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>

namespace {
using namespace shark;
using namespace shark::storage;
using namespace shark::server;
namespace fs = std::filesystem;
template <class T> T take(Result<T> result) {
    if (!result)
        INFO(result.error().context.operation << ": " << result.error().message);
    REQUIRE(result);
    return std::move(result).value();
}
template <class T> void reject(const Result<T>& result) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::conflict);
}
template <class Id> Id id(unsigned value) {
    std::string text{"a05f0000-0000-4000-8000-000000000000"};
    constexpr std::string_view hex{"0123456789abcdef"};
    for (unsigned index = 0; index < 8; ++index) {
        text[35 - index] = hex[value & 15U];
        value >>= 4U;
    }
    return Id{take(StableId::parse(text))};
}
std::vector<std::byte> image(std::string_view text = "portable-image-tick-20") {
    const auto bytes = std::as_bytes(std::span{text.data(), text.size()});
    return {bytes.begin(), bytes.end()};
}
struct Fixture {
    fs::path parent{fs::current_path()};
    fs::path root{parent / ("shark-region-process-" + take(generate_stable_id()).to_string())};
    fs::path database{root / "World.db"};
    fs::path directory{root / "journal"};
    WorldId world{id<WorldId>(900)};
    std::unique_ptr<Store> store;
    std::vector<CharacterRecord> actors;
    std::vector<RegionWriter> writers;
    Fixture() {
        REQUIRE(fs::create_directory(root));
        REQUIRE(fs::create_directory(directory));
        store = take(Store::create(database, world));
        for (unsigned index = 0; index < 2; ++index) {
            const auto account = id<net::AccountId>(101 + index);
            REQUIRE(store->provision_account(account));
            actors.push_back(take(store->create_character(
                account, 1,
                {Name{index == 0 ? "Crossing" : "Stationary"}, {world, {0, 0, 0}, {1, 2, 3}}})));
            const auto& actor = actors.back();
            REQUIRE(store->initialize_economy(account, actor.character, {100, 20}));
            writers.push_back(
                take(store->claim_region(actor.character, actor.entity, id<RegionId>(301 + index),
                                         id<RegionInstanceId>(401)))
                    .writer);
        }
    }
    ~Fixture() {
        store.reset();
        if (root.is_absolute() && root.parent_path() == parent &&
            root.filename().string().starts_with("shark-region-process-")) {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        }
    }
    void reopen() {
        store.reset();
        store = take(Store::open(database, world));
    }
    std::vector<RegionTransferRequest> requests() const {
        const auto digest = take(sha256(image()));
        std::vector<RegionTransferRequest> result;
        for (std::size_t index = 0; index < writers.size(); ++index)
            result.push_back({id<RegionTransferId>(501 + static_cast<unsigned>(index)),
                              writers[index], id<RegionId>(302), id<RegionInstanceId>(402), 20, 4,
                              2, 9, digest});
        return result;
    }
    RegionJournalRecord record() const {
        return {take(store->metadata()).id,
                world,
                id<RegionInstanceId>(401),
                20,
                7,
                writers,
                requests(),
                image()};
    }
    std::vector<RegionWriter> current() const {
        std::vector<RegionWriter> result;
        for (const auto& actor : actors) {
            const auto owner = take(store->region_ownership(actor.character));
            REQUIRE(owner);
            result.push_back(owner->writer);
        }
        return result;
    }
};
thread_local std::optional<detail::WritePhase> selected;
struct Fault {
    explicit Fault(detail::WritePhase phase) {
        selected = phase;
        detail::set_write_fault_hook(
            [](detail::WritePhase current) noexcept { return selected == current; });
    }
    ~Fault() {
        detail::set_write_fault_hook(nullptr);
        selected.reset();
    }
};
std::vector<std::byte> bytes(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input);
    const std::string value{std::istreambuf_iterator<char>{input}, {}};
    const auto view = std::as_bytes(std::span{value.data(), value.size()});
    return {view.begin(), view.end()};
}
} // namespace

TEST_CASE("island ownership batch rejects partial stale duplicate and mixed intents atomically",
          "[storage-region-process]") {
    Fixture f;
    const auto original = f.requests();
    auto stale = original;
    ++stale.back().source.epoch;
    reject(f.store->commit_regions(stale));
    CHECK(f.current() == f.writers);
    auto duplicate = original;
    duplicate.back().source = duplicate.front().source;
    reject(f.store->commit_regions(duplicate));
    duplicate = original;
    duplicate.back().transfer = duplicate.front().transfer;
    reject(f.store->commit_regions(duplicate));
    reject(f.store->commit_regions({}));
    const std::vector<RegionTransferRequest> oversized(33, original.front());
    reject(f.store->commit_regions(oversized));
    const auto result = take(f.store->commit_regions(original));
    REQUIRE(result.size() == 2);
    for (std::size_t index = 0; index < result.size(); ++index) {
        CHECK(result[index].request == original[index]);
        CHECK_FALSE(result[index].replayed);
        CHECK(result[index].writer.epoch == 2);
        CHECK(result[index].writer.instance == id<RegionInstanceId>(402));
    }
    // The stationary actor also changes instance/epoch in the same transaction.
    CHECK(result.back().writer.region == f.writers.back().region);
    f.reopen();
    const auto replay = take(f.store->commit_regions(original));
    for (std::size_t index = 0; index < result.size(); ++index) {
        CHECK(replay[index].replayed);
        CHECK(replay[index].writer == result[index].writer);
    }
    reject(f.store->commit_regions(std::span{original}.first(1)));
    auto mixed = original;
    mixed.back().source = result.back().writer;
    mixed.back().transfer = id<RegionTransferId>(503);
    mixed.back().destination = id<RegionId>(303);
    reject(f.store->commit_regions(mixed));
    CHECK(f.current() == std::vector<RegionWriter>{result[0].writer, result[1].writer});
    const TransferRequest economic{{take(f.store->metadata()).id, f.actors[0].account, 1},
                                   f.actors[0].character,
                                   f.actors[1].character,
                                   1,
                                   1,
                                   {7, 2}};
    reject(f.store->transfer(economic, f.writers.front()));
    reject(f.store->save_character(f.actors[0].account, f.actors[0].character, 1, f.actors[0].state,
                                   f.writers.front()));
    CHECK(take(f.store->operation_stream(f.actors[0].account)).high_watermark == 0);
    const auto committed = take(f.store->transfer(economic, result.front().writer));
    CHECK(committed.receipt.source->state == EconomyState{93, 18});
    CHECK(committed.receipt.target->state == EconomyState{107, 22});
    CHECK(take(f.store->save_character(f.actors[0].account, f.actors[0].character, 1,
                                       f.actors[0].state, result.front().writer))
              .revision == 2);
    auto successor = original;
    for (std::size_t index = 0; index < successor.size(); ++index) {
        successor[index].source = result[index].writer;
        successor[index].transfer = id<RegionTransferId>(503 + static_cast<unsigned>(index));
        successor[index].destination_instance = id<RegionInstanceId>(403);
        successor[index].cut_tick = 21;
        successor[index].cut_digest = take(sha256(image("portable-image-tick-21")));
    }
    reject(f.store->commit_regions(std::span{successor}.first(1)));
    CHECK(f.current() == std::vector<RegionWriter>{result[0].writer, result[1].writer});
    const auto advanced = take(f.store->commit_regions(successor));
    for (const auto& value : advanced)
        CHECK(value.writer.epoch == 3);
    reject(f.store->commit_regions(original));
    for (const auto& value : take(f.store->commit_regions(successor)))
        CHECK(value.replayed);
}

TEST_CASE("island ownership batch rollback and lost commit receipt preserve all writer epochs",
          "[storage-region-process]") {
    for (const auto phase : {detail::WritePhase::after_write, detail::WritePhase::before_commit,
                             detail::WritePhase::after_commit}) {
        Fixture f;
        const auto original = f.requests();
        {
            Fault fault{phase};
            const auto result = f.store->commit_regions(original);
            REQUIRE_FALSE(result);
            CHECK(result.error().code == ErrorCode::io_failure);
        }
        f.reopen();
        const bool committed = phase == detail::WritePhase::after_commit;
        for (const auto& owner : f.current())
            CHECK(owner.epoch == (committed ? 2 : 1));
        const auto retry = take(f.store->commit_regions(original));
        for (const auto& value : retry)
            CHECK(value.replayed == committed);
        for (const auto& owner : f.current())
            CHECK(owner.epoch == 2);
    }
}

TEST_CASE("regional journal owns two bounded digest-bound slots without recovering receipts",
          "[storage-region-process]") {
    Fixture f;
    const auto record = f.record();
    auto journal = take(RegionJournal::open(f.directory, record.store, record.world));
    reject(RegionJournal::open(f.directory, record.store, record.world));
    CHECK_FALSE(take(journal->read(RegionJournalSlot::checkpoint)));
    CHECK_FALSE(take(journal->read(RegionJournalSlot::staged)));
    const auto publication = take(journal->publish(RegionJournalSlot::staged, record));
    CHECK(publication.published);
    const auto observed = take(journal->read(RegionJournalSlot::staged));
    REQUIRE(observed);
    CHECK(observed->record == record);
    CHECK(observed->digest == publication.digest);
    CHECK(observed->payload_digest == take(sha256(record.payload)));
    CHECK(take(sha256(bytes(f.directory / "staged.sharkregion"))) == publication.digest);
    reject(journal->publish(RegionJournalSlot::staged, record));
    auto checkpoint = record;
    checkpoint.requests.clear();
    const auto first = take(journal->publish(RegionJournalSlot::checkpoint, checkpoint));
    checkpoint.tick = 21;
    checkpoint.route_watermark = 8;
    checkpoint.payload = image("portable-image-tick-21");
    reject(journal->publish(RegionJournalSlot::checkpoint, checkpoint, ContentDigest{}));
    CHECK(take(journal->read(RegionJournalSlot::checkpoint))->record.tick == 20);
    const auto second =
        take(journal->publish(RegionJournalSlot::checkpoint, checkpoint, first.digest));
    CHECK(second.digest != first.digest);
    CHECK(take(journal->read(RegionJournalSlot::checkpoint))->record == checkpoint);
    CHECK(take(journal->read(RegionJournalSlot::staged))->record == record);
    CHECK(static_cast<std::size_t>(
              std::distance(fs::directory_iterator(f.directory), fs::directory_iterator{})) == 3);
    bool wrong_thread{};
    std::thread other([&] {
        const auto result = journal->read(RegionJournalSlot::checkpoint);
        wrong_thread = !result && result.error().code == ErrorCode::conflict;
    });
    other.join();
    CHECK(wrong_thread);
    journal.reset();
    journal = take(RegionJournal::open(f.directory, record.store, record.world));
    CHECK(take(journal->read(RegionJournalSlot::checkpoint))->digest == second.digest);
    CHECK(take(journal->read(RegionJournalSlot::staged))->payload_digest ==
          publication.payload_digest);
}

TEST_CASE(
    "regional journal rejects invalid images and corruption while unpublished stages stay inert",
    "[storage-region-process]") {
    Fixture f;
    const auto record = f.record();
    auto journal = take(RegionJournal::open(f.directory, record.store, record.world));
    auto malformed = record;
    malformed.requests.pop_back();
    CHECK_FALSE(journal->publish(RegionJournalSlot::staged, malformed));
    malformed = record;
    malformed.payload = image("changed image");
    CHECK_FALSE(journal->publish(RegionJournalSlot::staged, malformed));
    malformed = record;
    malformed.payload.resize(region_journal_maximum_payload_bytes + 1);
    CHECK_FALSE(journal->publish(RegionJournalSlot::staged, malformed));
    malformed = record;
    malformed.store = id<StoreId>(999);
    reject(journal->publish(RegionJournalSlot::staged, malformed));
    {
        std::ofstream incomplete{f.directory / "staged.sharkregion.partial", std::ios::binary};
        incomplete << "unpublished incomplete bytes";
        incomplete.close();
        REQUIRE(incomplete);
    }
    CHECK_FALSE(take(journal->read(RegionJournalSlot::staged)));
    take(journal->publish(RegionJournalSlot::staged, record));
    CHECK_FALSE(fs::exists(f.directory / "staged.sharkregion.partial"));
    CHECK(take(journal->read(RegionJournalSlot::staged))->record == record);
    {
        std::ofstream corrupted{f.directory / "staged.sharkregion",
                                std::ios::binary | std::ios::app};
        corrupted.put('\0');
        corrupted.close();
        REQUIRE(corrupted);
    }
    const auto corrupt = journal->read(RegionJournalSlot::staged);
    REQUIRE_FALSE(corrupt);
    CHECK(corrupt.error().code == ErrorCode::invalid_configuration);
    CHECK_FALSE(journal->publish(RegionJournalSlot::staged, record));
}
