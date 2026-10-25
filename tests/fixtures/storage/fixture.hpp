#pragma once

#include <shark/storage/store.hpp>

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace storage_fixture {
using namespace shark;
using namespace shark::storage;
namespace fs = std::filesystem;

inline void check(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error{message};
}
template <typename T> T take(Result<T> result) {
    if (!result)
        throw std::runtime_error{result.error().context.operation + ": " + result.error().message};
    return std::move(result).value();
}
inline void take(Result<void> result) {
    if (!result)
        throw std::runtime_error{result.error().context.operation + ": " + result.error().message};
}
inline StableId identity(unsigned value) {
    std::ostringstream text;
    text << "00000000-0000-4000-8000-" << std::hex << std::setw(12) << std::setfill('0') << value;
    return take(StableId::parse(text.str()));
}
inline StoreId store_id() {
    return StoreId{identity(700)};
}
inline WorldId world() {
    return WorldId{identity(900)};
}
inline net::AccountId account(unsigned value = 1) {
    return net::AccountId{identity(value)};
}
inline net::CharacterId character(unsigned value = 101) {
    return net::CharacterId{identity(value)};
}
inline WorldEntityId entity(unsigned value = 201) {
    return WorldEntityId{identity(value)};
}

inline net::CertificateFingerprint fingerprint(std::string_view text) {
    check(text.size() == 64, "Fixture certificate length differs");
    net::CertificateFingerprint result{};
    const auto digit = [](char value) -> unsigned {
        if (value >= '0' && value <= '9')
            return static_cast<unsigned>(value - '0');
        check(value >= 'a' && value <= 'f', "Fixture certificate hex differs");
        return static_cast<unsigned>(value - 'a') + 10U;
    };
    for (std::size_t index = 0; index < result.size(); ++index)
        result[index] =
            static_cast<std::byte>(digit(text[index * 2]) * 16U + digit(text[index * 2 + 1]));
    return result;
}
inline DurableRegistry expected_registry(int source_version = 2) {
    check(source_version >= 1 && source_version <= 3, "Fixture version differs");
    if (source_version == 1)
        return {};
    DurableRegistry result;
    result.accounts = {{account(1), false, 6}, {account(2), false, 4}, {account(3), true, 2}};
    result.certificates = {
        {account(1),
         fingerprint("96baafc1e5c4ef1173b8d8c0ffa4916b39c633cc3b88b100f86fd48b9810fb20"), false},
        {account(2),
         fingerprint("95e15d6eab226755438ab548b7bfd23a3e9362c945ac3c0a8814b26bc441520b"), false},
        {account(1),
         fingerprint("0000000000000000000000000000000000000000000000000000000000000002"), true},
        {account(3),
         fingerprint("0000000000000000000000000000000000000000000000000000000000000004"), false}};
    std::sort(
        result.certificates.begin(), result.certificates.end(),
        [](const auto& left, const auto& right) { return left.certificate < right.certificate; });
    result.characters = {
        {account(1),
         character(101),
         entity(201),
         7,
         {Name{"Vanguard"}, WorldPosition{world(), {-3, 2, 2147483647}, {0.25, 512.5, 1023.75}}}},
        {account(2),
         character(102),
         entity(202),
         11,
         {Name{"Archivist"}, WorldPosition{world(), {-2147483648LL, 0, 4}, {999.125, 0.0, 10.0}}}},
        {account(3),
         character(103),
         entity(203),
         3,
         {Name{"Dormant"}, WorldPosition{world(), {1, 2, 3}, {0.0, 1.5, 0.0}}}}};
    return result;
}
inline EconomyRecord economic_record(unsigned owner, bool historical = false) {
    check(owner == 1 || owner == 2, "Fixture economic owner differs");
    return {account(owner), character(owner + 100), historical ? 2U : 3U,
            historical ? (owner == 1 ? EconomyState{93, 18} : EconomyState{107, 22})
                       : (owner == 1 ? EconomyState{90, 17} : EconomyState{110, 23})};
}
inline TransferRequest request(std::uint64_t sequence) {
    return {{store_id(), account(), sequence},
            character(101),
            character(102),
            sequence == 9 ? 2U : 1U,
            sequence == 9 ? 2U : 1U,
            sequence == 9 ? EconomyState{3, 1} : EconomyState{7, 2}};
}
inline OperationStream expected_stream(unsigned owner = 1) {
    OperationStream result{store_id(), account(owner), owner == 1 ? 9U : 0U, {}};
    if (owner == 1) {
        for (std::uint64_t sequence = 2; sequence <= 9; ++sequence) {
            result.receipts.push_back(
                {request(sequence),
                 sequence == 9 ? TransferStatus::committed : TransferStatus::stale_revision,
                 economic_record(1, sequence != 9), economic_record(2, sequence != 9)});
        }
    }
    return result;
}
inline void verify_current(Store& owner, int source_version) {
    check(take(owner.metadata()) == StoreMetadata{store_id(), world(), 3},
          "Fixture metadata changed");
    check(take(owner.registry()) == expected_registry(source_version), "Fixture registry changed");
    for (const auto& row : expected_registry(source_version).accounts) {
        const auto number = row.account == account(1) ? 1U : (row.account == account(2) ? 2U : 3U);
        const OperationStream expected = source_version == 3
                                             ? expected_stream(number)
                                             : OperationStream{store_id(), row.account, 0, {}};
        check(take(owner.operation_stream(row.account)) == expected,
              "Fixture operation stream changed");
        const auto actual = owner.economy(row.account, character(number + 100));
        if (source_version == 3 && number != 3)
            check(actual && actual.value() == economic_record(number),
                  "Fixture economic record changed");
        else
            check(!actual && actual.error().code == ErrorCode::conflict,
                  "Fixture gained economic resources");
    }
}

inline std::string utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
struct Connection {
    sqlite3* db{};
    explicit Connection(const fs::path& path, int flags) {
        const int code = sqlite3_open_v2(
            utf8(path).c_str(), &db,
            flags | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW | SQLITE_OPEN_EXRESCODE, nullptr);
        if (code != SQLITE_OK) {
            sqlite3_close_v2(db);
            db = nullptr;
            throw std::runtime_error{"Fixture SQLite open failed: " + std::to_string(code)};
        }
        sqlite3_busy_timeout(db, 0);
    }
    ~Connection() {
        sqlite3_close_v2(db);
    }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
};
inline void execute(Connection& connection, const char* sql) {
    char* message = nullptr;
    const int code = sqlite3_exec(connection.db, sql, nullptr, nullptr, &message);
    const std::string error = message == nullptr ? std::string{} : std::string{message};
    sqlite3_free(message);
    check(code == SQLITE_OK, error.empty() ? "Fixture SQL failed" : error.c_str());
}
inline void execute(const fs::path& path, const char* sql) {
    Connection connection{path, SQLITE_OPEN_READWRITE};
    execute(connection, sql);
}
inline void create(const fs::path& path, int version, const fs::path& fixture_dir) {
    check(version >= 1 && version <= 3, "Fixture version differs");
    check(!fs::exists(path), "Fixture refuses existing file");
    const auto source = fixture_dir / ("schema-" + std::to_string(version) + ".sql");
    const auto length = fs::file_size(source);
    check(length != 0 && length <= 65536, "Fixture SQL size differs");
    std::ifstream input(source, std::ios::binary);
    std::string sql(static_cast<std::size_t>(length), '\0');
    input.read(sql.data(), static_cast<std::streamsize>(sql.size()));
    check(input.good() && sql.find('\0') == std::string::npos, "Fixture SQL read failed");
    Connection connection{path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE};
    execute(connection, sql.c_str());
}
inline std::string quoted(std::string_view value) {
    std::ostringstream output;
    output << '"';
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte == '"' || byte == '\\')
            output << '\\' << character;
        else if (byte < 0x20)
            output << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                   << static_cast<unsigned>(byte);
        else
            output << character;
    }
    output << '"';
    return output.str();
}
inline std::string query(Connection& connection, const char* sql) {
    struct Statement {
        sqlite3_stmt* value{};
        ~Statement() {
            sqlite3_finalize(value);
        }
    } statement;
    check(sqlite3_prepare_v2(connection.db, sql, -1, &statement.value, nullptr) == SQLITE_OK,
          "Fixture snapshot prepare failed");
    std::ostringstream output;
    output << '[';
    bool comma = false;
    std::size_t count = 0;
    int code;
    while ((code = sqlite3_step(statement.value)) == SQLITE_ROW) {
        check(++count <= 1024, "Fixture snapshot row bound exceeded");
        if (std::exchange(comma, true))
            output << ',';
        output << '[';
        const auto columns = sqlite3_column_count(statement.value);
        check(columns <= 16, "Fixture snapshot column bound exceeded");
        for (int column = 0; column < columns; ++column) {
            if (column)
                output << ',';
            const auto type = sqlite3_column_type(statement.value, column);
            if (type == SQLITE_NULL) {
                output << "null";
            } else if (type == SQLITE_INTEGER) {
                output << "{\"integer\":" << sqlite3_column_int64(statement.value, column) << '}';
            } else if (type == SQLITE_FLOAT) {
                output << "{\"real\":"
                       << std::setprecision(std::numeric_limits<double>::max_digits10)
                       << sqlite3_column_double(statement.value, column) << '}';
            } else {
                const int bytes = sqlite3_column_bytes(statement.value, column);
                check(bytes >= 0 && bytes <= 16384, "Fixture snapshot value bound exceeded");
                if (type == SQLITE_TEXT) {
                    const auto* value = sqlite3_column_text(statement.value, column);
                    check(value != nullptr || bytes == 0, "Fixture text read failed");
                    output << "{\"text\":"
                           << quoted({reinterpret_cast<const char*>(value),
                                      static_cast<std::size_t>(bytes)})
                           << '}';
                } else {
                    check(type == SQLITE_BLOB, "Fixture snapshot type differs");
                    const auto* value = static_cast<const unsigned char*>(
                        sqlite3_column_blob(statement.value, column));
                    check(value != nullptr || bytes == 0, "Fixture blob read failed");
                    output << "{\"blob\":\"";
                    for (int index = 0; index < bytes; ++index)
                        output << std::hex << std::setw(2) << std::setfill('0')
                               << static_cast<unsigned>(value[index]);
                    output << std::dec << "\"}";
                }
            }
        }
        output << ']';
    }
    check(code == SQLITE_DONE, "Fixture snapshot step failed");
    output << ']';
    return output.str();
}
inline std::string snapshot(const fs::path& path, int version) {
    check(version >= 1 && version <= 3, "Fixture snapshot version differs");
    Connection connection{path, SQLITE_OPEN_READONLY};
    std::ostringstream output;
    output << "{\"version\":" << version << ",\"schema\":"
           << query(connection, "SELECT type,name,tbl_name,sql FROM sqlite_schema ORDER BY name")
           << ",\"header\":" << query(connection, "PRAGMA application_id")
           << ",\"user_version\":" << query(connection, "PRAGMA user_version")
           << ",\"store\":" << query(connection, "SELECT * FROM shark_store ORDER BY singleton");
    if (version >= 2)
        output << ",\"accounts\":"
               << query(connection, "SELECT * FROM shark_accounts ORDER BY account_id")
               << ",\"certificates\":"
               << query(connection, "SELECT * FROM shark_certificates ORDER BY certificate")
               << ",\"characters\":"
               << query(connection, "SELECT * FROM shark_characters ORDER BY character_id");
    if (version == 3)
        output << ",\"economies\":"
               << query(connection, "SELECT * FROM shark_economy ORDER BY character_id")
               << ",\"receipts\":"
               << query(connection,
                        "SELECT * FROM shark_transfer_receipts ORDER BY actor_id,sequence");
    output << '}';
    return output.str();
}
} // namespace storage_fixture
