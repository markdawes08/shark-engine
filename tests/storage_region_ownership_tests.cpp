#include "shark/storage/store.hpp"

#include "fixtures/storage/fixture.hpp"
#include "store_test_hooks.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <filesystem>
#include <limits>
#include <optional>
#include <thread>

namespace {
using namespace shark;
using namespace shark::storage;
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
    return Id{storage_fixture::identity(value)};
}
ContentDigest digest() {
    constexpr std::string_view source{"immutable final tick and queued input cut"};
    return take(sha256(std::as_bytes(std::span{source.data(), source.size()})));
}
struct Fixture {
    fs::path parent{fs::current_path()};
    fs::path root{parent / ("shark-region-storage-" + take(generate_stable_id()).to_string())};
    fs::path path{root / "live" / "World.db"};
    std::unique_ptr<Store> owner;
    StoreMetadata metadata;
    CharacterRecord first, second;
    explicit Fixture(bool initialize = true)
        : metadata{id<StoreId>(700), id<WorldId>(900), store_schema_version},
          first{id<net::AccountId>(1),
                id<net::CharacterId>(101),
                id<WorldEntityId>(201),
                1,
                {Name{"Source"}, {id<WorldId>(900), {0, 0, 0}, {1, 2, 3}}}},
          second{id<net::AccountId>(2),
                 id<net::CharacterId>(102),
                 id<WorldEntityId>(202),
                 1,
                 {Name{"Target"}, {id<WorldId>(900), {0, 0, 0}, {5, 2, 3}}}} {
        REQUIRE(fs::create_directory(root));
        for (const auto* directory : {"live", "backup", "restored"})
            REQUIRE(fs::create_directory(root / directory));
        owner = take(Store::create(path, metadata.world));
        metadata = take(owner->metadata());
        take(owner->provision_account(first.account));
        take(owner->provision_account(second.account));
        first = take(owner->create_character(first.account, 1, first.state));
        second = take(owner->create_character(second.account, 1, second.state));
        if (initialize) {
            take(owner->initialize_economy(first.account, first.character, {100, 20}));
            take(owner->initialize_economy(second.account, second.character, {100, 20}));
        }
    }
    ~Fixture() {
        owner.reset();
        if (root.is_absolute() && root.parent_path() == parent &&
            root.filename().string().starts_with("shark-region-storage-")) {
            std::error_code ignored;
            fs::remove_all(root, ignored);
        }
    }
    void reopen() {
        owner.reset();
        owner = take(Store::open(path, metadata.world));
    }
    RegionWriter claim() {
        return take(owner->claim_region(first.character, first.entity, id<RegionId>(301),
                                        id<RegionInstanceId>(401)))
            .writer;
    }
    RegionTransferRequest cut(RegionWriter writer, unsigned transfer = 501) const {
        return {id<RegionTransferId>(transfer),
                writer,
                id<RegionId>(302),
                id<RegionInstanceId>(402),
                20,
                7,
                6,
                40,
                digest()};
    }
    TransferRequest economic() const {
        return {{metadata.id, first.account, 1}, first.character, second.character, 1, 1, {7, 2}};
    }
};
thread_local std::optional<detail::WritePhase> injected;
struct Fault {
    explicit Fault(detail::WritePhase phase) {
        injected = phase;
        detail::set_write_fault_hook(
            [](detail::WritePhase current) noexcept { return injected == current; });
    }
    ~Fault() {
        detail::set_write_fault_hook(nullptr);
        injected.reset();
    }
};
} // namespace

TEST_CASE("regional claims fence ordinary checkpoints economic writes and provisioning",
          "[storage-region-ownership]") {
    Fixture f;
    CHECK_FALSE(take(f.owner->region_ownership(f.first.character)));
    const RegionWriter foreign{f.metadata.id,     f.first.character,         f.first.entity,
                               id<RegionId>(301), id<RegionInstanceId>(401), 1};
    reject(f.owner->save_character(f.first.account, f.first.character, 1, f.first.state, foreign));
    reject(f.owner->transfer(f.economic(), foreign));
    reject(f.owner->claim_region(f.first.character, f.second.entity, foreign.region,
                                 foreign.instance));
    const auto writer = f.claim();
    const auto ownership = take(f.owner->region_ownership(f.first.character));
    REQUIRE(ownership);
    CHECK(ownership->writer == writer);
    CHECK_FALSE(ownership->last_transfer);
    CHECK(take(f.owner->claim_region(writer.character, writer.entity, writer.region,
                                     writer.instance)) == *ownership);
    reject(
        f.owner->claim_region(writer.character, writer.entity, id<RegionId>(302), writer.instance));
    reject(f.owner->claim_region(writer.character, writer.entity, writer.region,
                                 id<RegionInstanceId>(402)));
    reject(f.owner->save_character(f.first.account, f.first.character, 1, f.first.state));
    reject(f.owner->transfer(f.economic()));
    for (int field = 0; field < 6; ++field) {
        auto stale = writer;
        switch (field) {
        case 0:
            stale.store = id<StoreId>(701);
            break;
        case 1:
            stale.character = f.second.character;
            break;
        case 2:
            stale.entity = f.second.entity;
            break;
        case 3:
            stale.region = id<RegionId>(302);
            break;
        case 4:
            stale.instance = id<RegionInstanceId>(402);
            break;
        default:
            ++stale.epoch;
            break;
        }
        reject(
            f.owner->save_character(f.first.account, f.first.character, 1, f.first.state, stale));
        reject(f.owner->transfer(f.economic(), stale));
    }
    CHECK(take(f.owner->character(f.first.account, f.first.character)) == f.first);
    CHECK(take(f.owner->operation_stream(f.first.account)).high_watermark == 0);
    auto changed = f.first.state;
    changed.position.offset[0] = 4;
    CHECK(take(f.owner->save_character(f.first.account, f.first.character, 1, changed, writer))
              .revision == 2);
    const auto economic = take(f.owner->transfer(f.economic(), writer));
    CHECK(economic.receipt.status == TransferStatus::committed);
    CHECK(economic.receipt.source->state == EconomyState{93, 18});
    CHECK(economic.receipt.target->state == EconomyState{107, 22});
    f.reopen();
    CHECK(take(f.owner->region_ownership(f.first.character)) == ownership);
    CHECK(take(f.owner->transfer(f.economic(), writer)).replayed);
    bool wrong_thread{};
    std::thread other([&] {
        const auto result = f.owner->region_ownership(f.first.character);
        wrong_thread = !result && result.error().code == ErrorCode::conflict;
    });
    other.join();
    CHECK(wrong_thread);

    Fixture uninitialized{false};
    uninitialized.claim();
    reject(uninitialized.owner->initialize_economy(uninitialized.first.account,
                                                   uninitialized.first.character, {100, 20}));
    reject(
        uninitialized.owner->economy(uninitialized.first.account, uninitialized.first.character));
}

TEST_CASE("regional cuts retain exact commit identity and stale writers cannot replay effects",
          "[storage-region-ownership]") {
    Fixture f;
    const auto writer = f.claim();
    const auto operation = f.economic();
    const auto economic = take(f.owner->transfer(operation, writer));
    auto cut = f.cut(writer);
    cut.cut_tick = std::numeric_limits<std::uint64_t>::max();
    cut.accepted_input = std::numeric_limits<std::uint64_t>::max();
    cut.applied_input = std::numeric_limits<std::uint64_t>::max() - 1;
    cut.command_sequence = std::numeric_limits<std::uint64_t>::max();
    const auto committed = take(f.owner->commit_region(cut));
    CHECK_FALSE(committed.replayed);
    CHECK(committed.request == cut);
    CHECK(committed.writer.epoch == writer.epoch + 1);
    CHECK(committed.writer.region == cut.destination);
    CHECK(take(f.owner->commit_region(cut)).replayed);
    for (int field = 0; field < 11; ++field) {
        auto changed = cut;
        switch (field) {
        case 0:
            changed.source.region = id<RegionId>(303);
            break;
        case 1:
            changed.source.instance = id<RegionInstanceId>(403);
            break;
        case 2:
            ++changed.source.epoch;
            break;
        case 3:
            changed.destination = id<RegionId>(303);
            break;
        case 4:
            changed.destination_instance = id<RegionInstanceId>(403);
            break;
        case 5:
            --changed.cut_tick;
            break;
        case 6:
            --changed.accepted_input;
            break;
        case 7:
            --changed.applied_input;
            break;
        case 8:
            --changed.command_sequence;
            break;
        case 9:
            changed.cut_digest.bytes.front() ^= std::byte{1};
            break;
        default:
            changed.source.entity = f.second.entity;
            break;
        }
        reject(f.owner->commit_region(changed));
    }
    reject(f.owner->save_character(f.first.account, f.first.character, 1, f.first.state, writer));
    reject(f.owner->transfer(operation, writer));
    CHECK(take(f.owner->transfer(operation, committed.writer)).receipt == economic.receipt);
    f.reopen();
    const auto observed = take(f.owner->region_ownership(f.first.character));
    REQUIRE(observed);
    CHECK(observed->last_transfer == cut);
    CHECK(take(f.owner->commit_region(cut)).replayed);
    auto next = f.cut(committed.writer, 502);
    next.destination = writer.region;
    next.destination_instance = writer.instance;
    CHECK(take(f.owner->commit_region(next)).writer.epoch == 3);
    reject(f.owner->commit_region(cut));
    CHECK(take(f.owner->operation_stream(f.first.account)).high_watermark == 1);
}

TEST_CASE("regional ownership transactions reconcile actual precommit and postcommit failures",
          "[storage-region-ownership]") {
    for (const auto phase : {detail::WritePhase::after_write, detail::WritePhase::before_commit,
                             detail::WritePhase::after_commit}) {
        Fixture f;
        const auto writer = f.claim();
        const auto cut = f.cut(writer);
        {
            Fault fault{phase};
            const auto result = f.owner->commit_region(cut);
            REQUIRE_FALSE(result);
            CHECK(result.error().code == ErrorCode::io_failure);
        }
        f.reopen();
        const auto observed = take(f.owner->region_ownership(f.first.character));
        REQUIRE(observed);
        const bool committed = phase == detail::WritePhase::after_commit;
        CHECK(observed->writer.epoch == (committed ? 2 : 1));
        CHECK(take(f.owner->commit_region(cut)).replayed == committed);
        CHECK(take(f.owner->character(f.first.account, f.first.character)) == f.first);
        CHECK(take(f.owner->operation_stream(f.first.account)).high_watermark == 0);
    }
}

TEST_CASE("ownership backup restore and migration equality preserve the complete latest cut",
          "[storage-region-ownership]") {
    Fixture f;
    const auto cut = f.cut(f.claim());
    const auto committed = take(f.owner->commit_region(cut));
    const auto ownership = take(f.owner->region_ownership(f.first.character));
    f.owner.reset();
    const auto backup = f.root / "backup" / "World.db";
    const auto restored = f.root / "restored" / "World.db";
    take(Store::backup(f.path, backup, f.metadata.id, f.metadata.world));
    take(Store::restore(backup, restored, f.metadata.id, f.metadata.world));
    {
        auto copy = take(Store::open(restored, f.metadata.world));
        CHECK(take(copy->region_ownership(f.first.character)) == ownership);
        CHECK(take(copy->commit_region(cut)).replayed);
        reject(
            copy->save_character(f.first.account, f.first.character, 1, f.first.state, cut.source));
        take(copy->save_character(f.first.account, f.first.character, 1, f.first.state,
                                  committed.writer));
    }
    f.reopen();
    auto next = f.cut(committed.writer, 502);
    next.destination = cut.source.region;
    next.destination_instance = cut.source.instance;
    take(f.owner->commit_region(next));
    f.owner.reset();
    reject(Store::migrate(f.path, backup, f.metadata.id, f.metadata.world));
    f.reopen();
    CHECK(take(f.owner->region_ownership(f.first.character))->writer.epoch == 3);
}

TEST_CASE("ownership validation rejects malformed stored cuts and epoch exhaustion",
          "[storage-region-ownership]") {
    for (const auto* mutation :
         {"UPDATE shark_region_ownership SET entity_id='00000000-0000-4000-8000-000000000999'",
          "UPDATE shark_region_ownership SET source_epoch=7",
          "UPDATE shark_region_ownership SET accepted_input='07'",
          "UPDATE shark_region_ownership SET applied_input='8'",
          "UPDATE shark_region_ownership SET cut_tick='18446744073709551616'",
          "UPDATE shark_region_ownership SET "
          "cut_digest='gggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggggg'"}) {
        Fixture f;
        take(f.owner->commit_region(f.cut(f.claim())));
        f.owner.reset();
        storage_fixture::execute(f.path, mutation);
        const auto opened = Store::open(f.path, f.metadata.world);
        REQUIRE_FALSE(opened);
        CHECK(opened.error().code == ErrorCode::invalid_configuration);
    }
    Fixture f;
    take(f.owner->commit_region(f.cut(f.claim())));
    f.owner.reset();
    storage_fixture::execute(f.path, "UPDATE shark_region_ownership SET "
                                     "epoch=9223372036854775807,source_epoch=9223372036854775806");
    f.reopen();
    const auto exhausted = take(f.owner->region_ownership(f.first.character));
    REQUIRE(exhausted);
    auto next = f.cut(exhausted->writer, 502);
    next.destination = id<RegionId>(303);
    reject(f.owner->commit_region(next));
    CHECK(take(f.owner->region_ownership(f.first.character)) == exhausted);
}
