#include <shark/storage/store.hpp>

#include "store_test_hooks.hpp"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace shark;
using namespace shark::storage;

template <typename T> T require(Result<T> result) {
    if (!result) {
        throw std::runtime_error{std::string{error_code_name(result.error().code)} + ":" +
                                 result.error().context.operation};
    }
    return std::move(result).value();
}

WorldId world() {
    return WorldId{require(StableId::parse("00000000-0000-4000-8000-000000000001"))};
}

void print(const StoreMetadata& metadata, std::string_view state = "ready") {
    std::cout << "{\"status\":\"PASS\",\"state\":\"" << state << "\",\"store_id\":\""
              << metadata.id.value().to_string() << "\",\"world_id\":\""
              << metadata.world.value().to_string()
              << "\",\"schema_version\":" << metadata.schema_version << "}\n"
              << std::flush;
}

detail::CreatePhase selected_phase = detail::CreatePhase::schema_written;

detail::CreatePhase parse_phase(std::string_view text) {
    if (text == "schema_written") {
        return detail::CreatePhase::schema_written;
    }
    if (text == "before_commit") {
        return detail::CreatePhase::before_commit;
    }
    if (text == "after_commit") {
        return detail::CreatePhase::after_commit;
    }
    throw std::runtime_error{"invalid create phase"};
}

std::string argument_text(const std::filesystem::path::value_type* argument) {
    const auto utf8 = std::filesystem::path{argument}.u8string();
    return {utf8.begin(), utf8.end()};
}

[[noreturn]] void interrupt_uncommitted_write(const std::filesystem::path& path) {
    sqlite3* database = nullptr;
    const auto path_text = argument_text(path.c_str());
    if (sqlite3_open_v2(path_text.c_str(), &database, SQLITE_OPEN_READWRITE, nullptr) !=
        SQLITE_OK) {
        sqlite3_close_v2(database);
        throw std::runtime_error{"could not open raw recovery fixture"};
    }
    const auto sql = "PRAGMA synchronous=EXTRA; PRAGMA journal_mode=DELETE; BEGIN EXCLUSIVE;"
                     "UPDATE shark_store SET world_id='00000000-0000-4000-8000-000000000002'";
    if (sqlite3_exec(database, sql, nullptr, nullptr, nullptr) != SQLITE_OK ||
        sqlite3_db_cacheflush(database) != SQLITE_OK) {
        sqlite3_close_v2(database);
        throw std::runtime_error{"could not flush uncommitted recovery fixture"};
    }
    // Flush dirty database pages with a synced rollback journal, then bypass cleanup.
    // Only this test fixture uses raw SQL; production exposes no arbitrary SQL.
    std::_Exit(73);
}

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        if (argc < 3 || argc > 4) {
            throw std::runtime_error{"expected mode path [phase]"};
        }
        const auto mode_text = argument_text(argv[1]);
        const std::string_view mode{mode_text};
        const std::filesystem::path path{argv[2]};
        if (mode == "create-exit") {
            if (argc != 4) {
                throw std::runtime_error{"missing create interruption phase"};
            }
            selected_phase = parse_phase(argument_text(argv[3]));
            detail::set_create_hook([](detail::CreatePhase phase) noexcept {
                if (phase == selected_phase) {
                    std::_Exit(73);
                }
            });
            static_cast<void>(require(Store::create(path, world())));
            throw std::runtime_error{"create interruption was not reached"};
        }
        if (argc != 3) {
            throw std::runtime_error{"unexpected argument"};
        }
        if (mode == "hot-journal-exit") {
            interrupt_uncommitted_write(path);
        }
        if (mode == "check-busy" || mode == "check-uninitialized") {
            auto result = Store::open(path, world());
            const auto expected =
                mode == "check-busy" ? ErrorCode::conflict : ErrorCode::invalid_configuration;
            if (result || result.error().code != expected) {
                throw std::runtime_error{"unexpected open rejection"};
            }
            std::cout << "{\"status\":\"PASS\",\"code\":\"" << error_code_name(expected) << "\"}\n";
            return 0;
        }
        if (mode == "check-create-existing") {
            const auto result = Store::create(path, world());
            if (result || result.error().code != ErrorCode::conflict) {
                throw std::runtime_error{"create did not preserve the allocated path"};
            }
            std::cout << "{\"status\":\"PASS\",\"code\":\"conflict\"}\n";
            return 0;
        }
        if (mode != "create" && mode != "read" && mode != "hold" && mode != "abort-owner") {
            throw std::runtime_error{"unknown storage process mode"};
        }
        auto owner =
            require(mode == "create" ? Store::create(path, world()) : Store::open(path, world()));
        const auto metadata = require(owner->metadata());
        if (mode == "abort-owner") {
            std::_Exit(73);
        }
        if (mode == "hold") {
            print(metadata);
            std::string release;
            if (!std::getline(std::cin, release) || release != "release") {
                throw std::runtime_error{"holder did not receive release"};
            }
        }
        print(metadata, "complete");
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        std::cout << "{\"status\":\"FAIL\"}\n";
        return 1;
    }
}
