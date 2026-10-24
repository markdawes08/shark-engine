#include <shark/storage/store.hpp>

#include "store_test_hooks.hpp"

#include <catch2/catch_test_macros.hpp>
#include <sqlite3.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace {
using namespace shark;
using namespace shark::storage;
namespace fs = std::filesystem;

template <typename T> T take(Result<T> value) {
    REQUIRE(value);
    return std::move(value).value();
}
template <typename T> void rejected(const Result<T>& value, ErrorCode code) {
    REQUIRE_FALSE(value);
    CHECK(value.error().code == code);
    CHECK_FALSE(value.error().context.operation.empty());
}
StableId identity(unsigned value) {
    std::ostringstream text;
    text << "00000000-0000-4000-8000-" << std::hex << std::setw(12) << std::setfill('0') << value;
    return take(StableId::parse(text.str()));
}
net::AccountId account(unsigned value = 1) {
    return net::AccountId{identity(value)};
}
WorldId world() {
    return WorldId{identity(900)};
}
CharacterState state(std::string name = "Economic participant") {
    return {Name{std::move(name)}, WorldPosition{world(), {1, 2, 3}, {0.0, 1.5, 1023.75}}};
}
std::string bytes(const fs::path& path) {
    std::ifstream input{path, std::ios::binary};
    REQUIRE(input);
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}
void raw(const fs::path& path, const std::string& sql) {
    const auto utf8 = path.u8string();
    const std::string text{utf8.begin(), utf8.end()};
    sqlite3* opened = nullptr;
    const auto result = sqlite3_open_v2(text.c_str(), &opened, SQLITE_OPEN_READWRITE, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> owner{opened, sqlite3_close_v2};
    REQUIRE(result == SQLITE_OK);
    char* message = nullptr;
    const auto executed = sqlite3_exec(opened, sql.c_str(), nullptr, nullptr, &message);
    const std::string diagnostic = message == nullptr ? "" : message;
    sqlite3_free(message);
    INFO(diagnostic);
    REQUIRE(executed == SQLITE_OK);
}
class Fixture final {
  public:
    Fixture()
        : parent(fs::current_path()),
          root(parent / ("shark-storage-transactions-" + take(generate_stable_id()).to_string())),
          path(root / "transactions.db") {
        REQUIRE(fs::create_directory(root));
    }
    ~Fixture() {
        if (root.is_absolute() && root.parent_path() == parent &&
            root.filename().string().starts_with("shark-storage-transactions-")) {
            std::error_code error;
            fs::remove_all(root, error);
        }
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    const fs::path parent;
    const fs::path root;
    const fs::path path;
};
class Scenario final {
  public:
    explicit Scenario(EconomyState source_state = {1000, 100},
                      EconomyState target_state = {200, 20})
        : store(take(Store::create(fixture.path, world()))) {
        take(store->provision_account(account()));
        take(store->provision_account(account(2)));
        source = take(store->create_character(account(), 1, state("Source"))).character;
        target = take(store->create_character(account(2), 1, state("Target"))).character;
        take(store->initialize_economy(account(), source, source_state));
        take(store->initialize_economy(account(2), target, target_state));
    }
    TransferRequest request(std::uint64_t sequence = 1, EconomyState quantity = {25, 3}) const {
        return {{take(store->metadata()).id, account(), sequence},
                source,
                target,
                take(store->economy(account(), source)).revision,
                take(store->economy(account(2), target)).revision,
                quantity};
    }
    std::array<EconomyRecord, 2> records() const {
        return {take(store->economy(account(), source)), take(store->economy(account(2), target))};
    }
    void reopen() {
        store.reset();
        store = take(Store::open(fixture.path, world()));
    }
    Fixture fixture;
    std::unique_ptr<Store> store;
    net::CharacterId source{identity(100)};
    net::CharacterId target{identity(101)};
};
void conserved(const std::array<EconomyRecord, 2>& before,
               const std::array<EconomyRecord, 2>& after) {
    CHECK(before[0].state.currency + before[1].state.currency ==
          after[0].state.currency + after[1].state.currency);
    CHECK(before[0].state.item_units + before[1].state.item_units ==
          after[0].state.item_units + after[1].state.item_units);
}
thread_local detail::WritePhase fault_phase = detail::WritePhase::after_write;
thread_local bool fail_rollback = false;
class WriteFault final {
  public:
    explicit WriteFault(detail::WritePhase phase, bool rollback = false) {
        fault_phase = phase;
        fail_rollback = rollback;
        detail::set_write_fault_hook([](detail::WritePhase value) noexcept {
            return value == fault_phase ||
                   (fail_rollback && value == detail::WritePhase::before_rollback);
        });
    }
    ~WriteFault() {
        detail::set_write_fault_hook(nullptr);
    }
    WriteFault(const WriteFault&) = delete;
    WriteFault& operator=(const WriteFault&) = delete;
};
} // namespace

TEST_CASE("economy initialization is explicit owned and once only across reopen",
          "[storage-transactions]") {
    Fixture fixture;
    auto store = take(Store::create(fixture.path, world()));
    take(store->provision_account(account()));
    take(store->provision_account(account(2)));
    const auto character = take(store->create_character(account(), 1, state()));
    rejected(store->economy(account(), character.character), ErrorCode::conflict);
    rejected(store->initialize_economy(account(2), character.character, {100, 1}),
             ErrorCode::conflict);
    rejected(
        store->initialize_economy(account(), character.character, {maximum_economy_amount + 1, 0}),
        ErrorCode::invalid_argument);
    const auto initialized =
        take(store->initialize_economy(account(), character.character, {0, 0}));
    CHECK(initialized == EconomyRecord{account(), character.character, 1, {0, 0}});
    rejected(store->initialize_economy(account(), character.character, {0, 0}),
             ErrorCode::conflict);
    rejected(store->initialize_economy(account(), character.character, {100, 2}),
             ErrorCode::conflict);
    take(store->save_character(account(), character.character, 1, state("New checkpoint")));
    CHECK(take(store->economy(account(), character.character)) == initialized);
    store.reset();
    store = take(Store::open(fixture.path, world()));
    CHECK(take(store->economy(account(), character.character)) == initialized);
    rejected(store->initialize_economy(account(), character.character, {100, 2}),
             ErrorCode::conflict);
    const auto second = take(store->create_character(account(), 2, state()));
    CHECK(take(store->initialize_economy(account(), second.character,
                                         {maximum_economy_amount, maximum_economy_amount}))
              .state == EconomyState{maximum_economy_amount, maximum_economy_amount});
    REQUIRE(store->revoke_account(account()));
    // Trusted audit remains readable; service admission owns authenticated privacy.
    CHECK(take(store->economy(account(), character.character)) == initialized);
    CHECK(take(store->operation_stream(account())).high_watermark == 0);
    rejected(store->initialize_economy(account(), second.character, {0, 0}), ErrorCode::conflict);
}

TEST_CASE("transfers conserve modeled assets and independently advance both economic revisions",
          "[storage-transactions]") {
    for (const auto quantity : {EconomyState{25, 3}, EconomyState{0, 100}, EconomyState{1000, 0}}) {
        DYNAMIC_SECTION(quantity.currency << "/" << quantity.item_units) {
            Scenario scenario;
            const auto registry = take(scenario.store->registry());
            const auto before = scenario.records();
            const auto request = scenario.request(1, quantity);
            const auto result = take(scenario.store->transfer(request));
            CHECK_FALSE(result.replayed);
            CHECK(result.receipt.status == TransferStatus::committed);
            CHECK(result.receipt.request == request);
            const auto after = scenario.records();
            CHECK(after[0].state ==
                  EconomyState{1000 - quantity.currency, 100 - quantity.item_units});
            CHECK(after[1].state ==
                  EconomyState{200 + quantity.currency, 20 + quantity.item_units});
            for (std::size_t i = 0; i != 2; ++i) {
                CHECK(after[i].revision == before[i].revision + 1);
                CHECK(after[i].account == before[i].account);
                CHECK(after[i].character == before[i].character);
            }
            CHECK(result.receipt.source == after[0]);
            CHECK(result.receipt.target == after[1]);
            conserved(before, after);
            CHECK(take(scenario.store->registry()) == registry);
            const auto stream = take(scenario.store->operation_stream(account()));
            CHECK(stream.store == request.operation.store);
            CHECK(stream.actor == account());
            CHECK(stream.high_watermark == 1);
            REQUIRE(stream.receipts.size() == 1);
            CHECK(stream.receipts[0] == result.receipt);
            scenario.reopen();
            CHECK(scenario.records() == after);
            CHECK(take(scenario.store->operation_stream(account())) == stream);
        }
    }
}

TEST_CASE("retained retries return original snapshots after later transactions and restart",
          "[storage-transactions]") {
    Scenario scenario;
    const auto request = scenario.request();
    const auto first = take(scenario.store->transfer(request));
    take(scenario.store->transfer(scenario.request(2, {10, 1})));
    const auto after = scenario.records();
    const auto stream = take(scenario.store->operation_stream(account()));
    for (unsigned repeat = 0; repeat != 3; ++repeat) {
        scenario.reopen();
        const auto replay = take(scenario.store->transfer(request));
        CHECK(replay.replayed);
        CHECK(replay.receipt == first.receipt);
        CHECK(scenario.records() == after);
        CHECK(take(scenario.store->operation_stream(account())) == stream);
    }
    REQUIRE(scenario.store->revoke_account(account(2)));
    const auto after_target_revocation = take(scenario.store->transfer(request));
    CHECK(after_target_revocation.replayed);
    CHECK(after_target_revocation.receipt == first.receipt);
    scenario.reopen();
    CHECK(take(scenario.store->transfer(request)).receipt == first.receipt);
    REQUIRE(scenario.store->revoke_account(account()));
    rejected(scenario.store->transfer(request), ErrorCode::conflict);
}

TEST_CASE("retained operation IDs reject every changed payload field without changing state",
          "[storage-transactions]") {
    Scenario scenario;
    const auto another = take(scenario.store->create_character(account(), 2, state()));
    take(scenario.store->initialize_economy(account(), another.character, {100, 10}));
    const auto request = scenario.request();
    const auto original = take(scenario.store->transfer(request));
    const auto before = scenario.records();
    const auto stream = take(scenario.store->operation_stream(account()));
    for (unsigned field = 0; field != 6; ++field) {
        DYNAMIC_SECTION(field) {
            auto changed = request;
            switch (field) {
            case 0:
                changed.source = another.character;
                break;
            case 1:
                changed.target = another.character;
                break;
            case 2:
                ++changed.expected_source_revision;
                break;
            case 3:
                ++changed.expected_target_revision;
                break;
            case 4:
                ++changed.quantity.currency;
                break;
            default:
                ++changed.quantity.item_units;
                break;
            }
            const auto mismatch = scenario.store->transfer(changed);
            rejected(mismatch, ErrorCode::conflict);
            CHECK(mismatch.error().context.operation == "storage.transfer.mismatch");
            CHECK(scenario.records() == before);
            CHECK(take(scenario.store->operation_stream(account())) == stream);
            CHECK(take(scenario.store->transfer(request)).receipt == original.receipt);
        }
    }
}

TEST_CASE("business rejection consumes its sequence and remains terminal after later funding",
          "[storage-transactions]") {
    Scenario scenario{{0, 0}, {200, 20}};
    const auto before = scenario.records();
    const auto rejected_request = scenario.request(1, {10, 1});
    const auto result = take(scenario.store->transfer(rejected_request));
    CHECK_FALSE(result.replayed);
    CHECK(result.receipt.status == TransferStatus::insufficient_assets);
    CHECK(result.receipt.source == before[0]);
    CHECK(result.receipt.target == before[1]);
    CHECK(scenario.records() == before);
    auto funding = rejected_request;
    funding.operation.actor = account(2);
    std::swap(funding.source, funding.target);
    CHECK(take(scenario.store->transfer(funding)).receipt.status == TransferStatus::committed);
    const auto funded = scenario.records();
    const auto replay = take(scenario.store->transfer(rejected_request));
    CHECK(replay.replayed);
    CHECK(replay.receipt == result.receipt);
    CHECK(scenario.records() == funded);
    CHECK(take(scenario.store->transfer(scenario.request(2, {10, 1}))).receipt.status ==
          TransferStatus::committed);
    const auto final = scenario.records();
    conserved(before, final);
    scenario.reopen();
    CHECK(take(scenario.store->transfer(rejected_request)).receipt == result.receipt);
    CHECK(scenario.records() == final);
    CHECK(take(scenario.store->operation_stream(account())).high_watermark == 2);
    CHECK(take(scenario.store->operation_stream(account(2))).high_watermark == 1);
}

TEST_CASE("unavailable targets yield durable receipts without disclosing target snapshots",
          "[storage-transactions]") {
    for (unsigned kind = 0; kind != 3; ++kind) {
        DYNAMIC_SECTION(kind) {
            Scenario scenario;
            auto request = scenario.request();
            if (kind == 0) {
                request.target = net::CharacterId{identity(999)};
            } else if (kind == 1) {
                request.target =
                    take(scenario.store->create_character(account(2), 2, state())).character;
            } else {
                REQUIRE(scenario.store->revoke_account(account(2)));
            }
            // Unavailability takes precedence over a stale target revision.
            request.expected_target_revision = 77;
            const auto before = take(scenario.store->economy(account(), scenario.source));
            const auto result = take(scenario.store->transfer(request));
            CHECK(result.receipt.status == TransferStatus::target_unavailable);
            CHECK(result.receipt.source == before);
            CHECK_FALSE(result.receipt.target);
            CHECK(take(scenario.store->economy(account(), scenario.source)) == before);
            CHECK(take(scenario.store->operation_stream(account())).high_watermark == 1);
            if (kind == 1) {
                take(scenario.store->initialize_economy(account(2), request.target, {100, 1}));
            }
            scenario.reopen();
            const auto replay = take(scenario.store->transfer(request));
            CHECK(replay.replayed);
            CHECK(replay.receipt == result.receipt);
        }
    }
}

TEST_CASE("stale intents and capacity errors are terminal and never overflow or partially debit",
          "[storage-transactions]") {
    SECTION("two intents based on the same revisions") {
        Scenario scenario;
        const auto before = scenario.records();
        const auto first_request = scenario.request();
        auto second_request = first_request;
        second_request.operation.sequence = 2;
        const auto first = take(scenario.store->transfer(first_request));
        const auto after = scenario.records();
        const auto second = take(scenario.store->transfer(second_request));
        CHECK(first.receipt.status == TransferStatus::committed);
        CHECK(second.receipt.status == TransferStatus::stale_revision);
        CHECK(second.receipt.source == after[0]);
        CHECK(second.receipt.target == after[1]);
        CHECK(scenario.records() == after);
        conserved(before, after);
        CHECK(take(scenario.store->transfer(scenario.request(3))).receipt.status ==
              TransferStatus::committed);
        scenario.reopen();
        CHECK(take(scenario.store->transfer(second_request)).receipt == second.receipt);
    }
    for (const auto quantity : {EconomyState{1, 0}, EconomyState{0, 1}, EconomyState{1, 1}}) {
        DYNAMIC_SECTION("capacity " << quantity.currency << "/" << quantity.item_units) {
            Scenario scenario{{100, 10}, {maximum_economy_amount, maximum_economy_amount}};
            const auto before = scenario.records();
            const auto request = scenario.request(1, quantity);
            const auto result = take(scenario.store->transfer(request));
            CHECK(result.receipt.status == TransferStatus::capacity_exceeded);
            CHECK(scenario.records() == before);
            scenario.reopen();
            CHECK(take(scenario.store->transfer(request)).receipt == result.receipt);
            CHECK(scenario.records() == before);
        }
    }
    SECTION("stale precedes insufficient and capacity") {
        Scenario scenario{{0, 0}, {maximum_economy_amount, maximum_economy_amount}};
        auto request = scenario.request();
        ++request.expected_source_revision;
        CHECK(take(scenario.store->transfer(request)).receipt.status ==
              TransferStatus::stale_revision);
        CHECK(take(scenario.store->transfer(scenario.request(2))).receipt.status ==
              TransferStatus::insufficient_assets);
    }
}

TEST_CASE(
    "eight retained receipts expire logically while durable high water rejects old IDs forever",
    "[storage-transactions]") {
    Scenario scenario;
    const auto first_request = scenario.request(1, {1, 0});
    const auto first = take(scenario.store->transfer(first_request));
    for (std::uint64_t sequence = 2; sequence <= 8; ++sequence) {
        take(scenario.store->transfer(scenario.request(sequence, {1, 0})));
    }
    CHECK(take(scenario.store->transfer(first_request)).receipt == first.receipt);
    CHECK(take(scenario.store->operation_stream(account())).receipts.size() ==
          retained_operation_receipts);
    const auto ninth_request = scenario.request(9, {1, 0});
    const auto ninth = take(scenario.store->transfer(ninth_request));
    const auto before = scenario.records();
    auto stream = take(scenario.store->operation_stream(account()));
    REQUIRE(stream.receipts.size() == 8);
    CHECK(stream.high_watermark == 9);
    for (std::size_t i = 0; i != stream.receipts.size(); ++i) {
        CHECK(stream.receipts[i].request.operation.sequence == i + 2);
    }
    // Returned vectors are owned copies, never a mutable route into the owner.
    stream.receipts.clear();
    CHECK(take(scenario.store->operation_stream(account())).receipts.size() == 8);
    for (unsigned restart = 0; restart != 2; ++restart) {
        scenario.reopen();
        const auto expired = scenario.store->transfer(first_request);
        rejected(expired, ErrorCode::conflict);
        CHECK(expired.error().context.operation == "storage.transfer.expired");
        auto changed_expired = first_request;
        changed_expired.quantity.currency = 2;
        rejected(scenario.store->transfer(changed_expired), ErrorCode::conflict);
        CHECK(take(scenario.store->transfer(ninth_request)).receipt == ninth.receipt);
        CHECK(scenario.records() == before);
        CHECK(take(scenario.store->operation_stream(account())).high_watermark == 9);
    }
    CHECK(take(scenario.store->operation_stream(account(2))).high_watermark == 0);
}

TEST_CASE("malformed economy and transfer values reject before durable admission",
          "[storage-transactions]") {
    REQUIRE(validate_economy_state({0, 0}));
    REQUIRE(validate_economy_state({maximum_economy_amount, maximum_economy_amount}));
    for (const auto value :
         {maximum_economy_amount + 1, std::numeric_limits<std::uint64_t>::max()}) {
        rejected(validate_economy_state({value, 0}), ErrorCode::invalid_argument);
        rejected(validate_economy_state({0, value}), ErrorCode::invalid_argument);
    }
    Scenario scenario;
    const auto before = scenario.records();
    const auto stream = take(scenario.store->operation_stream(account()));
    for (unsigned field = 0; field != 10; ++field) {
        DYNAMIC_SECTION(field) {
            auto request = scenario.request();
            switch (field) {
            case 0:
                request.operation.sequence = 0;
                break;
            case 1:
                request.operation.sequence = maximum_durable_revision + 1;
                break;
            case 2:
                request.source = request.target;
                break;
            case 3:
                request.expected_source_revision = 0;
                break;
            case 4:
                request.expected_source_revision = maximum_durable_revision + 1;
                break;
            case 5:
                request.expected_target_revision = 0;
                break;
            case 6:
                request.expected_target_revision = maximum_durable_revision + 1;
                break;
            case 7:
                request.quantity = {0, 0};
                break;
            case 8:
                request.quantity.currency = maximum_economy_amount + 1;
                break;
            default:
                request.quantity.item_units = std::numeric_limits<std::uint64_t>::max();
                break;
            }
            rejected(validate_transfer_request(request), ErrorCode::invalid_argument);
            rejected(scenario.store->transfer(request), ErrorCode::invalid_argument);
            CHECK(scenario.records() == before);
            CHECK(take(scenario.store->operation_stream(account())) == stream);
        }
    }
}

TEST_CASE("gapped cross-store unowned and uninitialized source operations never consume identity",
          "[storage-transactions]") {
    Scenario scenario;
    const auto before = scenario.records();
    const auto extra = take(scenario.store->create_character(account(), 2, state()));
    for (unsigned field = 0; field != 6; ++field) {
        DYNAMIC_SECTION(field) {
            auto request = scenario.request();
            switch (field) {
            case 0:
                request.operation.sequence = 2;
                break;
            case 1:
                request.operation.store = StoreId{identity(555)};
                break;
            case 2:
                request.operation.actor = account(2);
                break;
            case 3:
                request.operation.actor = account(555);
                break;
            case 4:
                request.source = extra.character;
                break;
            default:
                request.source = net::CharacterId{identity(555)};
                break;
            }
            rejected(scenario.store->transfer(request), ErrorCode::conflict);
            CHECK(scenario.records() == before);
            CHECK(take(scenario.store->operation_stream(account())).high_watermark == 0);
            CHECK(take(scenario.store->operation_stream(account(2))).high_watermark == 0);
        }
    }
    CHECK(take(scenario.store->transfer(scenario.request())).receipt.status ==
          TransferStatus::committed);
}

TEST_CASE(
    "economic revisions stop at signed durable maximum and reject further transfer atomically",
    "[storage-transactions]") {
    for (bool source_exhausted : {false, true}) {
        DYNAMIC_SECTION(source_exhausted) {
            Scenario scenario;
            for (std::uint64_t sequence = 1; sequence <= 8; ++sequence) {
                take(scenario.store->transfer(scenario.request(sequence)));
            }
            const auto exhausted = source_exhausted ? scenario.source : scenario.target;
            scenario.store.reset();
            // Represent a long pruned history while keeping the complete final
            // eight committed outcomes internally consistent with the current record.
            const std::string participant = source_exhausted ? "source" : "target";
            raw(scenario.fixture.path,
                "BEGIN; UPDATE shark_economy SET revision=9223372036854775807 "
                "WHERE character_id='" +
                    exhausted.value().to_string() +
                    "';"
                    "UPDATE shark_transfer_receipts SET sequence=sequence+9223372036854775798,"
                    "expected_" +
                    participant + "_revision=expected_" + participant +
                    "_revision+9223372036854775798," + participant + "_revision=" + participant +
                    "_revision+9223372036854775798;"
                    "UPDATE shark_accounts SET operation_sequence=9223372036854775806 "
                    "WHERE operation_sequence>0; COMMIT;");
            scenario.reopen();
            const auto before = scenario.records();
            const auto request = scenario.request(maximum_durable_revision);
            const auto result = take(scenario.store->transfer(request));
            CHECK(result.receipt.status == TransferStatus::revision_exhausted);
            CHECK(result.receipt.source == before[0]);
            CHECK(result.receipt.target == before[1]);
            CHECK(scenario.records() == before);
            scenario.reopen();
            CHECK(take(scenario.store->transfer(request)).receipt == result.receipt);
            CHECK(scenario.records() == before);
        }
    }
}

TEST_CASE("operation sequence exhaustion preserves retained replay and never wraps or reopens "
          "expired IDs",
          "[storage-transactions]") {
    Scenario scenario;
    auto request = scenario.request();
    request.target = net::CharacterId{identity(555)};
    for (std::uint64_t sequence = 1; sequence <= 8; ++sequence) {
        request.operation.sequence = sequence;
        CHECK(take(scenario.store->transfer(request)).receipt.status ==
              TransferStatus::target_unavailable);
    }
    scenario.store.reset();
    raw(scenario.fixture.path,
        "BEGIN; UPDATE shark_transfer_receipts SET sequence=sequence+9223372036854775799;"
        "UPDATE shark_accounts SET operation_sequence=9223372036854775807 WHERE account_id='" +
            account().value().to_string() + "'; COMMIT;");
    scenario.reopen();
    const auto before = scenario.records();
    const auto stream = take(scenario.store->operation_stream(account()));
    CHECK(stream.high_watermark == maximum_durable_revision);
    REQUIRE(stream.receipts.size() == 8);
    CHECK(stream.receipts.front().request.operation.sequence == maximum_durable_revision - 7);
    request.operation.sequence = maximum_durable_revision;
    CHECK(take(scenario.store->transfer(request)).replayed);
    request.operation.sequence = 1;
    rejected(scenario.store->transfer(request), ErrorCode::conflict);
    request.operation.sequence = maximum_durable_revision + 1;
    rejected(scenario.store->transfer(request), ErrorCode::invalid_argument);
    CHECK(scenario.records() == before);
    CHECK(take(scenario.store->operation_stream(account())) == stream);
}

TEST_CASE("reopen rejects malformed economic rows high waters and retained receipts without writes",
          "[storage-transactions]") {
    const std::array mutations{
        "UPDATE shark_economy SET revision=0",
        "UPDATE shark_economy SET currency=-1",
        "UPDATE shark_economy SET item_units=1000000001",
        "UPDATE shark_economy SET character_id='00000000-0000-4000-8000-000000000999' WHERE "
        "rowid=1",
        "UPDATE shark_accounts SET operation_sequence=-1",
        "UPDATE shark_accounts SET operation_sequence=0 WHERE operation_sequence>0",
        "UPDATE shark_accounts SET operation_sequence=3 WHERE operation_sequence>0",
        "DELETE FROM shark_transfer_receipts WHERE sequence=1",
        "UPDATE shark_transfer_receipts SET sequence=3 WHERE sequence=1",
        "UPDATE shark_transfer_receipts SET actor_id='00000000-0000-4000-8000-000000000999'",
        "UPDATE shark_transfer_receipts SET target_id=source_id",
        "UPDATE shark_transfer_receipts SET expected_source_revision=0",
        "UPDATE shark_transfer_receipts SET expected_target_revision=0",
        "UPDATE shark_transfer_receipts SET currency=0,item_units=0",
        "UPDATE shark_transfer_receipts SET status=6",
        "UPDATE shark_transfer_receipts SET status=1",
        "UPDATE shark_transfer_receipts SET source_revision=99",
        "UPDATE shark_transfer_receipts SET source_currency=source_currency+1 WHERE sequence=1",
        "UPDATE shark_transfer_receipts SET target_revision=NULL",
        "UPDATE shark_transfer_receipts SET target_items=target_items+1 WHERE sequence=2",
        "UPDATE shark_transfer_receipts SET status=4",
        "INSERT INTO shark_transfer_receipts SELECT actor_id,3,source_id,target_id,"
        "expected_source_revision,expected_target_revision,currency,item_units,status,"
        "source_revision,source_currency,source_items,target_revision,target_currency,target_items "
        "FROM shark_transfer_receipts WHERE sequence=1;"
        "UPDATE shark_accounts SET operation_sequence=3 WHERE operation_sequence>0",
        "CREATE INDEX unauthorized_economy_index ON shark_economy(currency)",
        "DROP TABLE shark_transfer_receipts",
    };
    for (const auto* mutation : mutations) {
        DYNAMIC_SECTION(mutation) {
            Scenario scenario;
            take(scenario.store->transfer(scenario.request()));
            take(scenario.store->transfer(scenario.request(2)));
            scenario.store.reset();
            raw(scenario.fixture.path,
                std::string{"PRAGMA foreign_keys=OFF; PRAGMA ignore_check_constraints=ON;"} +
                    mutation);
            const auto before = bytes(scenario.fixture.path);
            rejected(Store::open(scenario.fixture.path, world()), ErrorCode::invalid_configuration);
            CHECK(bytes(scenario.fixture.path) == before);
        }
    }
}

TEST_CASE("transaction operations enforce creating thread without state or sequence effects",
          "[storage-transactions]") {
    Scenario scenario;
    const auto request = scenario.request();
    const auto before = scenario.records();
    const auto stream = take(scenario.store->operation_stream(account()));
    std::array<bool, 4> results{};
    std::thread worker{[&] {
        const auto denied = [](const auto& value) {
            return !value && value.error().code == ErrorCode::conflict;
        };
        results = {denied(scenario.store->initialize_economy(account(), scenario.source, {1, 1})),
                   denied(scenario.store->economy(account(), scenario.source)),
                   denied(scenario.store->transfer(request)),
                   denied(scenario.store->operation_stream(account()))};
    }};
    worker.join();
    for (bool result : results) {
        CHECK(result);
    }
    CHECK(scenario.records() == before);
    CHECK(take(scenario.store->operation_stream(account())) == stream);
}

TEST_CASE("partial source debit and completed writes roll back participants receipt and high water "
          "together",
          "[storage-transactions]") {
    for (const auto phase :
         {detail::WritePhase::after_source_debit, detail::WritePhase::after_write}) {
        DYNAMIC_SECTION(static_cast<int>(phase)) {
            Scenario scenario;
            const auto before = scenario.records();
            const auto stream = take(scenario.store->operation_stream(account()));
            const auto request = scenario.request();
            {
                WriteFault fault{phase};
                rejected(scenario.store->transfer(request), ErrorCode::io_failure);
            }
            CHECK(scenario.records() == before);
            CHECK(take(scenario.store->operation_stream(account())) == stream);
            scenario.reopen();
            CHECK(scenario.records() == before);
            CHECK(take(scenario.store->operation_stream(account())) == stream);
            const auto retry = take(scenario.store->transfer(request));
            CHECK_FALSE(retry.replayed);
            CHECK(retry.receipt.status == TransferStatus::committed);
            conserved(before, scenario.records());
        }
    }
}

TEST_CASE(
    "uncertain transfer acknowledgements quiesce the owner and reconcile by durable operation ID",
    "[storage-transactions]") {
    for (const auto phase : {detail::WritePhase::before_commit, detail::WritePhase::after_commit,
                             detail::WritePhase::after_source_debit}) {
        DYNAMIC_SECTION(static_cast<int>(phase)) {
            Scenario scenario;
            const auto before = scenario.records();
            const auto request = scenario.request();
            {
                WriteFault fault{phase, phase == detail::WritePhase::after_source_debit};
                rejected(scenario.store->transfer(request), ErrorCode::io_failure);
            }
            rejected(scenario.store->metadata(), ErrorCode::execution_failed);
            rejected(scenario.store->registry(), ErrorCode::execution_failed);
            rejected(scenario.store->economy(account(), scenario.source),
                     ErrorCode::execution_failed);
            rejected(scenario.store->initialize_economy(account(), scenario.source, {1, 1}),
                     ErrorCode::execution_failed);
            rejected(scenario.store->transfer(request), ErrorCode::execution_failed);
            rejected(scenario.store->operation_stream(account()), ErrorCode::execution_failed);
            scenario.reopen();
            const bool committed = phase == detail::WritePhase::after_commit;
            CHECK(take(scenario.store->operation_stream(account())).high_watermark ==
                  (committed ? 1 : 0));
            if (!committed) {
                CHECK(scenario.records() == before);
            }
            const auto retried = take(scenario.store->transfer(request));
            CHECK(retried.replayed == committed);
            CHECK(retried.receipt.status == TransferStatus::committed);
            CHECK(scenario.records()[0].state == EconomyState{975, 97});
            CHECK(scenario.records()[1].state == EconomyState{225, 23});
            conserved(before, scenario.records());
            CHECK(take(scenario.store->operation_stream(account())).high_watermark == 1);
        }
    }
}

TEST_CASE(
    "rejected outcome receipt and sequence commit atomically despite containing no asset writes",
    "[storage-transactions]") {
    for (const auto phase : {detail::WritePhase::after_write, detail::WritePhase::after_commit}) {
        DYNAMIC_SECTION(static_cast<int>(phase)) {
            Scenario scenario{{0, 0}, {200, 20}};
            const auto before = scenario.records();
            const auto request = scenario.request();
            {
                WriteFault fault{phase};
                rejected(scenario.store->transfer(request), ErrorCode::io_failure);
            }
            scenario.reopen();
            const auto result = take(scenario.store->transfer(request));
            CHECK(result.replayed == (phase == detail::WritePhase::after_commit));
            CHECK(result.receipt.status == TransferStatus::insufficient_assets);
            CHECK(scenario.records() == before);
            const auto stream = take(scenario.store->operation_stream(account()));
            CHECK(stream.high_watermark == 1);
            REQUIRE(stream.receipts.size() == 1);
            CHECK(stream.receipts[0] == result.receipt);
        }
    }
}
