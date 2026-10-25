#include "fixtures/storage/fixture.hpp"
#include "maintenance_test_hooks.hpp"
#include "storage_recovery_vfs.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
using namespace shark;
using namespace shark::storage;
namespace sf = storage_fixture;
namespace fs = std::filesystem;

std::string selected_operation;
std::string selected_phase;
std::string selected_kind;
unsigned hook_calls{};
test::RecoveryVfs* recovery_vfs{};
fs::path migration_source;

void crash_after_main_write(int amount, std::int64_t position) noexcept {
    try {
        auto journal = migration_source;
        journal += "-journal";
        const auto size = fs::file_size(journal);
        std::ifstream input(journal, std::ios::binary);
        std::array<unsigned char, 8> header{};
        input.read(reinterpret_cast<char*>(header.data()),
                   static_cast<std::streamsize>(header.size()));
        sf::check(input.good(), "Hot journal header could not be read at database write");
        const std::array<unsigned char, 8> magic{0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7};
        sf::check(header == magic && size > 512 && amount > 0 && position >= 0,
                  "Database write did not retain a hot rollback journal");
        std::cout << "{\"status\":\"PASS\",\"event\":\"native-write\",\"operation\":\"migrate\","
                     "\"phase\":\"main_write\",\"hook_calls\":1,\"sqlite_code\":0,\"amount\":"
                  << amount << ",\"offset\":" << position << ",\"journal_bytes\":" << size
                  << ",\"journal_header\":\"d9d505f920a163d7\"}\n"
                  << std::flush;
        std::_Exit(73);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::_Exit(74);
    }
}

std::string_view phase_name(detail::MaintenancePhase phase) {
    switch (phase) {
    case detail::MaintenancePhase::backup_step:
        return "backup_step";
    case detail::MaintenancePhase::backup_complete:
        return "backup_complete";
    case detail::MaintenancePhase::backup_validated:
        return "backup_validated";
    case detail::MaintenancePhase::after_v1_to_v2:
        return "after_v1_to_v2";
    case detail::MaintenancePhase::after_v2_to_v3:
        return "after_v2_to_v3";
    case detail::MaintenancePhase::before_migration_commit:
        return "before_migration_commit";
    case detail::MaintenancePhase::after_migration_commit:
        return "after_migration_commit";
    case detail::MaintenancePhase::restore_step:
        return "restore_step";
    case detail::MaintenancePhase::restore_complete:
        return "restore_complete";
    case detail::MaintenancePhase::restore_validated:
        return "restore_validated";
    }
    return "unknown";
}
bool maintenance_hook(detail::MaintenancePhase phase) noexcept {
    if (selected_phase == "main_write" &&
        phase == detail::MaintenancePhase::before_migration_commit) {
        recovery_vfs->observe_main_write(crash_after_main_write);
        return false;
    }
    if (phase_name(phase) != selected_phase)
        return false;
    ++hook_calls;
    if (selected_kind == "crash") {
        std::cout << "{\"status\":\"PASS\",\"event\":\"boundary\",\"operation\":"
                  << sf::quoted(selected_operation) << ",\"phase\":" << sf::quoted(selected_phase)
                  << ",\"hook_calls\":" << hook_calls << "}\n"
                  << std::flush;
        std::_Exit(73);
    }
    return true;
}
void print_metadata(const StoreMetadata& value) {
    std::cout << "{\"store\":\"" << value.id.value().to_string() << "\",\"world\":\""
              << value.world.value().to_string() << "\",\"version\":" << value.schema_version
              << '}';
}
void print_info(const SnapshotInfo& value) {
    std::cout << "{\"metadata\":";
    print_metadata(value.metadata);
    std::cout << ",\"bytes\":" << value.bytes << ",\"accounts\":" << value.accounts
              << ",\"certificates\":" << value.certificates
              << ",\"characters\":" << value.characters << ",\"economies\":" << value.economies
              << ",\"operation_streams\":" << value.operation_streams
              << ",\"receipts\":" << value.receipts << '}';
}
void print_migration(const MigrationResult& value) {
    std::cout << "{\"before\":";
    print_metadata(value.before);
    std::cout << ",\"after\":";
    print_metadata(value.after);
    std::cout << ",\"already_current\":" << (value.already_current ? "true" : "false") << '}';
}
void print_error(const Error& error) {
    std::cout << "{\"code\":" << sf::quoted(error_code_name(error.code))
              << ",\"operation\":" << sf::quoted(error.context.operation) << '}';
}
Result<SnapshotInfo> copy_snapshot(const fs::path& source, const fs::path& destination) {
    return selected_operation == "backup"
               ? Store::backup(source, destination, sf::store_id(), sf::world())
               : Store::restore(source, destination, sf::store_id(), sf::world());
}
void seed(const fs::path& source, const fs::path& destination, int version,
          const fs::path& fixtures) {
    sf::create(source, version, fixtures);
    if (selected_operation == "migrate")
        sf::take(Store::backup(source, destination, sf::store_id(), sf::world()));
    if (selected_kind == "reject") {
        if (selected_phase == "future")
            sf::execute(source, "PRAGMA user_version=4");
        else if (selected_phase == "foreign")
            sf::execute(source, "PRAGMA application_id=17");
        else if (selected_phase == "extra-schema")
            sf::execute(source, "CREATE TABLE unexpected(value INTEGER) STRICT");
        else if (selected_phase == "missing-backup")
            sf::check(fs::remove(destination), "Missing backup fixture was not removed");
        else if (selected_phase == "mismatched-backup")
            sf::execute(destination, "UPDATE shark_characters SET name='Changed backup' WHERE "
                                     "character_id='00000000-0000-4000-8000-000000000065'");
        else if (selected_phase == "corrupt-backup") {
            std::ofstream output(destination, std::ios::binary | std::ios::trunc);
            output << "incomplete backup";
            output.close();
            sf::check(output.good(), "Corrupt backup fixture could not be written");
        } else
            throw std::runtime_error{"Unknown maintenance rejection"};
    }
    std::cout << "{\"status\":\"PASS\",\"event\":\"seed\",\"source\":"
              << sf::snapshot(source, version) << ",\"backup\":";
    if (selected_operation == "migrate" && selected_phase != "missing-backup" &&
        selected_phase != "corrupt-backup")
        std::cout << sf::snapshot(destination, version);
    else
        std::cout << "null";
    std::cout << "}\n";
}
void invoke(const fs::path& source, const fs::path& destination) {
    detail::set_maintenance_fault_hook(maintenance_hook);
    if (selected_operation == "migrate") {
        auto result = Store::migrate(source, destination, sf::store_id(), sf::world());
        detail::set_maintenance_fault_hook(nullptr);
        const bool acknowledged = selected_phase == "after_reply";
        sf::check(result.has_value() == acknowledged,
                  "Migration success/failure did not match interruption");
        std::cout << "{\"status\":\"PASS\",\"event\":\"operation\",\"operation\":"
                  << sf::quoted(selected_operation) << ",\"phase\":" << sf::quoted(selected_phase)
                  << ",\"kind\":" << sf::quoted(selected_kind)
                  << ",\"ok\":" << (result ? "true" : "false") << ",\"hook_calls\":" << hook_calls
                  << ",\"result\":";
        if (result)
            print_migration(result.value());
        else
            print_error(result.error());
    } else {
        auto result = copy_snapshot(source, destination);
        detail::set_maintenance_fault_hook(nullptr);
        const bool acknowledged = selected_phase == "after_reply";
        sf::check(result.has_value() == acknowledged,
                  "Copy success/failure did not match interruption");
        std::cout << "{\"status\":\"PASS\",\"event\":\"operation\",\"operation\":"
                  << sf::quoted(selected_operation) << ",\"phase\":" << sf::quoted(selected_phase)
                  << ",\"kind\":" << sf::quoted(selected_kind)
                  << ",\"ok\":" << (result ? "true" : "false") << ",\"hook_calls\":" << hook_calls
                  << ",\"result\":";
        if (result)
            print_info(result.value());
        else
            print_error(result.error());
    }
    sf::check(selected_kind == "reject" || selected_phase == "after_reply" || hook_calls == 1,
              "Requested maintenance failure was not observed exactly once");
    std::cout << "}\n" << std::flush;
    if (selected_kind == "crash")
        std::_Exit(73);
}
void verify_replays(Store& owner) {
    const auto committed = sf::take(owner.transfer(sf::request(9)));
    const auto rejected = sf::take(owner.transfer(sf::request(2)));
    sf::check(committed.replayed && committed.receipt == sf::expected_stream().receipts.back(),
              "Restored commit replay differs");
    sf::check(rejected.replayed && rejected.receipt == sf::expected_stream().receipts.front(),
              "Restored rejection replay differs");
    const auto expired = owner.transfer(sf::request(1));
    sf::check(!expired && expired.error().code == ErrorCode::conflict &&
                  expired.error().context.operation == "storage.transfer.expired",
              "Restore revived expired operation");
    auto changed = sf::request(9);
    changed.quantity.currency += 1;
    const auto mismatch = owner.transfer(changed);
    sf::check(!mismatch && mismatch.error().code == ErrorCode::conflict &&
                  mismatch.error().context.operation == "storage.transfer.mismatch",
              "Restore admitted changed retry");
    sf::verify_current(owner, 3);
}
void inspect_hot(const fs::path& source) {
    const auto result = Store::inspect_snapshot(source, sf::store_id(), sf::world());
    sf::check(!result, "Read-only inspection admitted a hot journal");
    std::cout << "{\"status\":\"PASS\",\"event\":\"hot-rejected\",\"error\":";
    print_error(result.error());
    std::cout << "}\n";
}
void reconcile(const fs::path& source, const fs::path& destination, const fs::path& retry,
               int version) {
    if (selected_kind == "reject") {
        std::cout << "{\"status\":\"PASS\",\"event\":\"rejected-preserved\",\"source\":"
                  << sf::snapshot(source, version) << "}\n";
        return;
    }
    if (selected_operation == "migrate") {
        const auto result =
            sf::take(Store::migrate(source, destination, sf::store_id(), sf::world()));
        auto owner = sf::take(Store::open(source, sf::world()));
        sf::verify_current(*owner, version);
        owner.reset();
        const auto repeated =
            sf::take(Store::migrate(source, destination, sf::store_id(), sf::world()));
        sf::check(repeated.already_current, "Repeated migrated result was not recognized");
        std::cout << "{\"status\":\"PASS\",\"event\":\"reconcile\",\"migration\":";
        print_migration(result);
        std::cout << ",\"repeated\":";
        print_migration(repeated);
        std::cout << ",\"source\":" << sf::snapshot(source, 3)
                  << ",\"backup\":" << sf::snapshot(destination, version) << "}\n";
        return;
    }
    const auto inspected = Store::inspect_snapshot(destination, sf::store_id(), sf::world());
    const auto existing = copy_snapshot(source, destination);
    sf::check(!existing && existing.error().code == ErrorCode::conflict,
              "Copy retry overwrote allocated destination");
    const auto copied = sf::take(copy_snapshot(source, retry));
    if (version == 3) {
        auto owner = sf::take(Store::open(retry, sf::world()));
        sf::verify_current(*owner, 3);
        verify_replays(*owner);
    }
    std::cout << "{\"status\":\"PASS\",\"event\":\"reconcile\",\"destination_valid\":"
              << (inspected ? "true" : "false") << ",\"destination\":";
    if (inspected)
        std::cout << sf::snapshot(destination, version);
    else
        print_error(inspected.error());
    std::cout << ",\"existing_retry\":";
    print_error(existing.error());
    std::cout << ",\"fresh_copy\":";
    print_info(copied);
    std::cout << ",\"source\":" << sf::snapshot(source, version)
              << ",\"retry\":" << sf::snapshot(retry, version)
              << ",\"economic_replays_checked\":" << (version == 3 ? "true" : "false") << "}\n";
}
} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        sf::check(argc == 10,
                  "Expected mode operation version phase kind fixtures source destination retry");
        std::vector<std::string> args;
        for (int index = 1; index < argc; ++index) {
            const auto text = fs::path{argv[index]}.u8string();
            args.emplace_back(text.begin(), text.end());
        }
        selected_operation = args[1];
        const int version = std::stoi(args[2]);
        selected_phase = args[3];
        selected_kind = args[4];
        sf::check(selected_operation == "backup" || selected_operation == "migrate" ||
                      selected_operation == "restore",
                  "Unknown maintenance operation");
        sf::check(selected_kind == "crash" || selected_kind == "fail" || selected_kind == "reject",
                  "Unknown maintenance kind");
        const auto path = [&](std::size_t index) {
            return fs::path{std::u8string{args[index].begin(), args[index].end()}};
        };
        test::RecoveryVfs vfs;
        recovery_vfs = &vfs;
        migration_source = path(6);
        if (args[0] == "seed")
            seed(path(6), path(7), version, path(5));
        else if (args[0] == "operate")
            invoke(path(6), path(7));
        else if (args[0] == "inspect-hot")
            inspect_hot(path(6));
        else if (args[0] == "reconcile")
            reconcile(path(6), path(7), path(8), version);
        else
            throw std::runtime_error{"Unknown maintenance mode"};
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::cout << "{\"status\":\"FAIL\"}\n";
        return 1;
    }
}
