#include "shark/storage/store.hpp"

#include "maintenance_test_hooks.hpp"
#include "shark/project_path.hpp"
#include "store_test_hooks.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace shark::storage {
namespace {
constexpr int application_id = 0x53484b44;
constexpr std::string_view table_v1_sql =
    "CREATE TABLE shark_store(singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
    "store_id TEXT NOT NULL CHECK(length(store_id)=36),"
    "world_id TEXT NOT NULL CHECK(length(world_id)=36),"
    "schema_version INTEGER NOT NULL CHECK(schema_version=1)) STRICT";
constexpr std::string_view table_v2_sql =
    "CREATE TABLE shark_store(singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
    "store_id TEXT NOT NULL CHECK(length(store_id)=36),"
    "world_id TEXT NOT NULL CHECK(length(world_id)=36),"
    "schema_version INTEGER NOT NULL CHECK(schema_version=2)) STRICT";
constexpr std::string_view table_sql =
    "CREATE TABLE shark_store(singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
    "store_id TEXT NOT NULL CHECK(length(store_id)=36),"
    "world_id TEXT NOT NULL CHECK(length(world_id)=36),"
    "schema_version INTEGER NOT NULL CHECK(schema_version=3)) STRICT";
constexpr std::string_view accounts_sql =
    "CREATE TABLE shark_accounts(account_id TEXT PRIMARY KEY NOT NULL CHECK(length(account_id)=36),"
    "revoked INTEGER NOT NULL CHECK(revoked IN(0,1)),"
    "roster_revision INTEGER NOT NULL CHECK(roster_revision>=1),"
    "operation_sequence INTEGER NOT NULL CHECK(operation_sequence>=0)) STRICT";
constexpr std::string_view accounts_v2_sql =
    "CREATE TABLE shark_accounts(account_id TEXT PRIMARY KEY NOT NULL CHECK(length(account_id)=36),"
    "revoked INTEGER NOT NULL CHECK(revoked IN(0,1)),"
    "roster_revision INTEGER NOT NULL CHECK(roster_revision>=1)) STRICT";
constexpr std::string_view certificates_sql =
    "CREATE TABLE shark_certificates(certificate BLOB PRIMARY KEY NOT NULL "
    "CHECK(length(certificate)=32),"
    "account_id TEXT NOT NULL REFERENCES shark_accounts(account_id),"
    "revoked INTEGER NOT NULL CHECK(revoked IN(0,1))) STRICT";
constexpr std::string_view characters_sql =
    "CREATE TABLE shark_characters(character_id TEXT PRIMARY KEY NOT NULL "
    "CHECK(length(character_id)=36),"
    "account_id TEXT NOT NULL REFERENCES shark_accounts(account_id),"
    "entity_id TEXT NOT NULL UNIQUE CHECK(length(entity_id)=36),"
    "revision INTEGER NOT NULL CHECK(revision>=1),"
    "name TEXT NOT NULL CHECK(length(CAST(name AS BLOB)) BETWEEN 1 AND 256),"
    "cell_x INTEGER NOT NULL CHECK(cell_x BETWEEN -2147483648 AND 2147483647),"
    "cell_y INTEGER NOT NULL CHECK(cell_y BETWEEN -2147483648 AND 2147483647),"
    "cell_z INTEGER NOT NULL CHECK(cell_z BETWEEN -2147483648 AND 2147483647),"
    "offset_x REAL NOT NULL CHECK(offset_x>=0 AND offset_x<1024),"
    "offset_y REAL NOT NULL CHECK(offset_y>=0 AND offset_y<1024),"
    "offset_z REAL NOT NULL CHECK(offset_z>=0 AND offset_z<1024)) STRICT";
constexpr std::string_view economy_sql =
    "CREATE TABLE shark_economy(character_id TEXT PRIMARY KEY NOT NULL "
    "REFERENCES shark_characters(character_id),"
    "revision INTEGER NOT NULL CHECK(revision>=1),"
    "currency INTEGER NOT NULL CHECK(currency BETWEEN 0 AND 1000000000),"
    "item_units INTEGER NOT NULL CHECK(item_units BETWEEN 0 AND 1000000000)) STRICT";
constexpr std::string_view receipts_sql =
    "CREATE TABLE shark_transfer_receipts(actor_id TEXT NOT NULL "
    "REFERENCES shark_accounts(account_id),"
    "sequence INTEGER NOT NULL CHECK(sequence>=1),"
    "source_id TEXT NOT NULL REFERENCES shark_economy(character_id),"
    "target_id TEXT NOT NULL CHECK(length(target_id)=36),"
    "expected_source_revision INTEGER NOT NULL CHECK(expected_source_revision>=1),"
    "expected_target_revision INTEGER NOT NULL CHECK(expected_target_revision>=1),"
    "currency INTEGER NOT NULL CHECK(currency BETWEEN 0 AND 1000000000),"
    "item_units INTEGER NOT NULL CHECK(item_units BETWEEN 0 AND 1000000000),"
    "status INTEGER NOT NULL CHECK(status BETWEEN 0 AND 5),"
    "source_revision INTEGER NOT NULL CHECK(source_revision>=1),"
    "source_currency INTEGER NOT NULL CHECK(source_currency BETWEEN 0 AND 1000000000),"
    "source_items INTEGER NOT NULL CHECK(source_items BETWEEN 0 AND 1000000000),"
    "target_revision INTEGER CHECK(target_revision>=1),"
    "target_currency INTEGER CHECK(target_currency BETWEEN 0 AND 1000000000),"
    "target_items INTEGER CHECK(target_items BETWEEN 0 AND 1000000000),"
    "PRIMARY KEY(actor_id,sequence),CHECK(source_id<>target_id),"
    "CHECK(currency>0 OR item_units>0),"
    "CHECK((status=4 AND target_revision IS NULL AND target_currency IS NULL AND "
    "target_items IS NULL) OR (status<>4 AND target_revision IS NOT NULL AND "
    "target_currency IS NOT NULL AND target_items IS NOT NULL))) STRICT";
thread_local detail::CreateHook create_hook = nullptr;
thread_local detail::WriteFaultHook write_fault_hook = nullptr;
thread_local detail::MaintenanceFaultHook maintenance_fault_hook = nullptr;

Error failure(ErrorCode code, std::string_view operation, std::string_view message,
              const std::string& path) {
    return {code, std::string(message), {.operation = std::string(operation), .path = path}};
}

struct Connection {
    sqlite3* db{};
    std::string path;
    std::string operation;
    ~Connection() {
        if (db != nullptr) {
            sqlite3_close_v2(db);
        }
    }
    [[nodiscard]] Error error(int result) const {
        const auto primary = result & 0xff;
        auto code = ErrorCode::io_failure;
        if (primary == SQLITE_BUSY || primary == SQLITE_LOCKED) {
            code = ErrorCode::conflict;
        } else if (primary == SQLITE_CORRUPT || primary == SQLITE_NOTADB ||
                   primary == SQLITE_SCHEMA || primary == SQLITE_ERROR ||
                   primary == SQLITE_TOOBIG) {
            code = ErrorCode::invalid_configuration;
        }
        return failure(code, operation,
                       "database operation failed (SQLite code " + std::to_string(result) + ")",
                       path);
    }
    [[nodiscard]] Error invalid(std::string_view message) const {
        return failure(ErrorCode::invalid_configuration, operation, message, path);
    }
    [[nodiscard]] Result<void> execute(const char* sql) const {
        const auto result = sqlite3_exec(db, sql, nullptr, nullptr, nullptr);
        if (result != SQLITE_OK) {
            return error(result);
        }
        return {};
    }
};

struct Statement {
    sqlite3_stmt* value{};
    ~Statement() {
        sqlite3_finalize(value);
    }
};

Result<void> prepare(const Connection& connection, Statement& statement, const char* sql) {
    const auto result = sqlite3_prepare_v3(connection.db, sql, -1, 0, &statement.value, nullptr);
    if (result != SQLITE_OK) {
        return connection.error(result);
    }
    return {};
}

bool text_is(sqlite3_stmt* statement, int column, std::string_view expected) {
    if (sqlite3_column_type(statement, column) != SQLITE_TEXT) {
        return false;
    }
    const auto* text = sqlite3_column_text(statement, column);
    const auto size = sqlite3_column_bytes(statement, column);
    return text != nullptr && size >= 0 && static_cast<std::size_t>(size) == expected.size() &&
           std::memcmp(text, expected.data(), expected.size()) == 0;
}

Result<void> require_integer(const Connection& connection, const char* sql, int expected) {
    Statement statement;
    if (auto result = prepare(connection, statement, sql); !result) {
        return result.error();
    }
    auto step = sqlite3_step(statement.value);
    if (step != SQLITE_ROW) {
        return connection.error(step);
    }
    if (sqlite3_column_type(statement.value, 0) != SQLITE_INTEGER ||
        sqlite3_column_int64(statement.value, 0) != expected) {
        return connection.invalid(
            "database setting or version does not match the storage contract");
    }
    step = sqlite3_step(statement.value);
    if (step != SQLITE_DONE) {
        return connection.error(step);
    }
    return {};
}

Result<void> require_text(const Connection& connection, const char* sql,
                          std::string_view expected) {
    Statement statement;
    if (auto result = prepare(connection, statement, sql); !result) {
        return result.error();
    }
    auto step = sqlite3_step(statement.value);
    if (step != SQLITE_ROW) {
        return connection.error(step);
    }
    if (!text_is(statement.value, 0, expected)) {
        return connection.invalid("database setting or integrity check failed");
    }
    step = sqlite3_step(statement.value);
    if (step != SQLITE_DONE) {
        return connection.error(step);
    }
    return {};
}

std::string utf8_path(const std::filesystem::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

Result<std::filesystem::path> resolve_path(const std::filesystem::path& path,
                                           std::string_view operation) {
    try {
        if (path.empty() || !path.is_absolute() || path.filename().empty() ||
            utf8_path(path).find('\0') != std::string::npos) {
            return failure(ErrorCode::invalid_path, operation,
                           "database path must be an absolute local file path", {});
        }
#if defined(_WIN32)
        const auto root = path.root_name().native();
        if (root.size() != 2 || root[1] != L':') {
            return failure(ErrorCode::invalid_path, operation,
                           "database path requires a local drive, not a UNC or device path", {});
        }
#endif
        auto leaf = ProjectPath::parse(utf8_path(path.filename()));
        if (!leaf || leaf.value().text().size() > 240) {
            return failure(ErrorCode::invalid_path, operation,
                           "database filename is invalid or exceeds 240 UTF-8 bytes", {});
        }
        std::error_code ec;
        auto parent = std::filesystem::canonical(path.parent_path(), ec);
        if (ec || !std::filesystem::is_directory(parent, ec) || ec) {
            return failure(ErrorCode::invalid_path, operation,
                           "database parent must be an existing local directory", {});
        }
        auto resolved = parent / path.filename();
        auto relative = ProjectPath::parse(utf8_path(resolved.relative_path()));
        if (!relative || utf8_path(resolved).size() > 4080) {
            return failure(ErrorCode::invalid_path, operation,
                           "database path must be bounded portable UTF-8", {});
        }
        return resolved;
    } catch (const std::filesystem::filesystem_error&) {
        return failure(ErrorCode::invalid_path, operation, "database path conversion failed", {});
    }
}

Result<void> check_regular(const std::filesystem::path& path, const Connection& connection,
                           std::uint64_t cap, bool required) {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (status.type() == std::filesystem::file_type::not_found) {
        if (!required) {
            return {};
        }
        return failure(ErrorCode::io_failure, connection.operation, "database file is missing",
                       connection.path);
    }
    if (ec || !std::filesystem::is_regular_file(status)) {
        return failure(ErrorCode::invalid_path, connection.operation,
                       "database and journal must be regular files without aliases",
                       connection.path);
    }
    const auto links = std::filesystem::hard_link_count(path, ec);
    if (ec || links != 1) {
        return failure(ErrorCode::invalid_path, connection.operation,
                       "database and journal must have exactly one filesystem link",
                       connection.path);
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return failure(ErrorCode::io_failure, connection.operation, "cannot inspect database size",
                       connection.path);
    }
    if (size > cap) {
        return connection.invalid("database or journal exceeds its file size bound");
    }
    return {};
}

Result<void> check_sidecars(const std::filesystem::path& path, const Connection& connection) {
    auto journal = path;
    journal += "-journal";
    if (auto result = check_regular(journal, connection, maximum_store_bytes * 2, false); !result) {
        return result.error();
    }
    for (const auto suffix : {"-wal", "-shm"}) {
        auto sidecar = path;
        sidecar += suffix;
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(sidecar, ec);
        if (status.type() != std::filesystem::file_type::not_found) {
            return connection.invalid("WAL sidecars are incompatible with this storage profile");
        }
        if (ec && ec != std::errc::no_such_file_or_directory) {
            return failure(ErrorCode::io_failure, connection.operation,
                           "cannot inspect database sidecars", connection.path);
        }
    }
    return {};
}

Result<void> reserve_file(const std::filesystem::path& path, const Connection& connection) {
#if defined(_WIN32)
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        return failure(
            code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS ? ErrorCode::conflict
                                                                      : ErrorCode::io_failure,
            connection.operation, "could not reserve a new database file", connection.path);
    }
    if (CloseHandle(handle) == 0) {
        return failure(ErrorCode::io_failure, connection.operation,
                       "could not close the reserved database file", connection.path);
    }
#else
    const auto descriptor =
        ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) {
        return failure(errno == EEXIST ? ErrorCode::conflict : ErrorCode::io_failure,
                       connection.operation, "could not reserve a new database file",
                       connection.path);
    }
    if (::close(descriptor) != 0) {
        return failure(ErrorCode::io_failure, connection.operation,
                       "could not close the reserved database file", connection.path);
    }
#endif
    return {};
}

Result<void> check_header(const std::filesystem::path& path, const Connection& connection,
                          bool allow_historical = false) {
    std::array<unsigned char, 100> header{};
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return failure(ErrorCode::io_failure, connection.operation,
                       "database header could not be opened", connection.path);
    }
    input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (input.bad()) {
        return failure(ErrorCode::io_failure, connection.operation,
                       "database header could not be read", connection.path);
    }
    if (!input) {
        return connection.invalid("database header is missing or truncated");
    }
    const auto u32 = [&header](std::size_t offset) {
        return (static_cast<std::uint32_t>(header[offset]) << 24U) |
               (static_cast<std::uint32_t>(header[offset + 1]) << 16U) |
               (static_cast<std::uint32_t>(header[offset + 2]) << 8U) |
               static_cast<std::uint32_t>(header[offset + 3]);
    };
    const auto version = u32(60);
    const bool supported = allow_historical ? version >= 1 && version <= store_schema_version
                                            : version == store_schema_version;
    if (std::memcmp(header.data(), "SQLite format 3\0", 16) != 0 || u32(68) != application_id ||
        !supported || header[16] != 16 || header[17] != 0 || header[18] != 1 || header[19] != 1) {
        return connection.invalid("unsupported database identity, schema or journal format");
    }
    return {};
}

Result<void> configure(Connection& connection) {
    sqlite3_extended_result_codes(connection.db, 1);
    sqlite3_busy_timeout(connection.db, 0);
    for (const auto& [limit, value] :
         std::array<std::pair<int, int>, 10>{{{SQLITE_LIMIT_LENGTH, 4096},
                                              {SQLITE_LIMIT_SQL_LENGTH, 16384},
                                              {SQLITE_LIMIT_COLUMN, 16},
                                              {SQLITE_LIMIT_EXPR_DEPTH, 100},
                                              {SQLITE_LIMIT_COMPOUND_SELECT, 1},
                                              {SQLITE_LIMIT_VARIABLE_NUMBER, 16},
                                              {SQLITE_LIMIT_ATTACHED, 0},
                                              {SQLITE_LIMIT_TRIGGER_DEPTH, 0},
                                              {SQLITE_LIMIT_WORKER_THREADS, 0},
                                              {SQLITE_LIMIT_VDBE_OP, 25000}}}) {
        sqlite3_limit(connection.db, limit, value);
        if (sqlite3_limit(connection.db, limit, -1) != value) {
            return connection.invalid("database query limit could not be applied");
        }
    }
    for (const auto& [option, value] :
         std::array<std::pair<int, int>, 5>{{{SQLITE_DBCONFIG_DEFENSIVE, 1},
                                             {SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0},
                                             {SQLITE_DBCONFIG_DQS_DDL, 0},
                                             {SQLITE_DBCONFIG_DQS_DML, 0},
                                             {SQLITE_DBCONFIG_ENABLE_TRIGGER, 0}}}) {
        int actual = -1;
        const auto result = sqlite3_db_config(connection.db, option, value, &actual);
        if (result != SQLITE_OK) {
            return connection.error(result);
        }
        if (actual != value) {
            return connection.invalid("database protection could not be enabled");
        }
    }
    if (auto result = connection.execute(
            "PRAGMA page_size=4096; PRAGMA synchronous=EXTRA; PRAGMA foreign_keys=ON;"
            "PRAGMA cache_size=-1024; PRAGMA temp_store=MEMORY; PRAGMA mmap_size=0;");
        !result) {
        return result.error();
    }
    if (auto result = require_text(connection, "PRAGMA locking_mode=EXCLUSIVE", "exclusive");
        !result) {
        return result.error();
    }
    // Do not silently convert an existing WAL database: raw header preflight has
    // already required rollback-journal format. A reserved new file uses DELETE.
    if (auto result = require_text(connection, "PRAGMA journal_mode", "delete"); !result) {
        return result.error();
    }
    for (const auto& [sql, expected] :
         std::array<std::pair<const char*, int>, 7>{{{"PRAGMA synchronous", 3},
                                                     {"PRAGMA foreign_keys", 1},
                                                     {"PRAGMA page_size", 4096},
                                                     {"PRAGMA cache_size", -1024},
                                                     {"PRAGMA temp_store", 2},
                                                     {"PRAGMA mmap_size", 0},
                                                     {"PRAGMA max_page_count=4096", 4096}}}) {
        if (auto result = require_integer(connection, sql, expected); !result) {
            return result.error();
        }
    }
    return {};
}

Result<void> create_schema(Connection& connection, const StoreMetadata& metadata,
                           bool notify_creation = true) {
    for (const auto sql :
         {table_sql, accounts_sql, certificates_sql, characters_sql, economy_sql, receipts_sql}) {
        if (auto result = connection.execute(sql.data()); !result) {
            return result.error();
        }
    }
    if (notify_creation && create_hook != nullptr) {
        create_hook(detail::CreatePhase::schema_written);
    }
    Statement insert;
    if (auto result = prepare(connection, insert, "INSERT INTO shark_store VALUES(1,?1,?2,3)");
        !result) {
        return result.error();
    }
    const auto id = metadata.id.value().to_string();
    const auto world = metadata.world.value().to_string();
    for (const auto& [index, value] :
         std::array<std::pair<int, const std::string*>, 2>{{{1, &id}, {2, &world}}}) {
        const auto result = sqlite3_bind_text(insert.value, index, value->c_str(),
                                              static_cast<int>(value->size()), SQLITE_TRANSIENT);
        if (result != SQLITE_OK) {
            return connection.error(result);
        }
    }
    const auto step = sqlite3_step(insert.value);
    if (step != SQLITE_DONE) {
        return connection.error(step);
    }
    return connection.execute("PRAGMA application_id=1397246788; PRAGMA user_version=3");
}

Result<StoreMetadata> inspect(Connection& connection, WorldId expected_world,
                              std::uint32_t version = store_schema_version) {
    if (auto result = require_integer(connection, "PRAGMA application_id", application_id);
        !result) {
        return result.error();
    }
    if (auto result = require_integer(connection, "PRAGMA user_version", static_cast<int>(version));
        !result) {
        return result.error();
    }
    Statement schema;
    if (auto result = prepare(connection, schema,
                              "SELECT type,name,tbl_name,sql FROM sqlite_schema ORDER BY name");
        !result) {
        return result.error();
    }
    struct SchemaObject {
        std::string_view type;
        std::string_view name;
        std::string_view table;
        std::string_view sql;
    };
    const auto metadata_sql = version == 1 ? table_v1_sql : version == 2 ? table_v2_sql : table_sql;
    const std::array objects{
        SchemaObject{"table", "shark_accounts", "shark_accounts",
                     version == 2 ? accounts_v2_sql : accounts_sql},
        SchemaObject{"table", "shark_certificates", "shark_certificates", certificates_sql},
        SchemaObject{"table", "shark_characters", "shark_characters", characters_sql},
        SchemaObject{"table", "shark_economy", "shark_economy", economy_sql},
        SchemaObject{"table", "shark_store", "shark_store", metadata_sql},
        SchemaObject{"table", "shark_transfer_receipts", "shark_transfer_receipts", receipts_sql},
        SchemaObject{"index", "sqlite_autoindex_shark_accounts_1", "shark_accounts", {}},
        SchemaObject{"index", "sqlite_autoindex_shark_certificates_1", "shark_certificates", {}},
        SchemaObject{"index", "sqlite_autoindex_shark_characters_1", "shark_characters", {}},
        SchemaObject{"index", "sqlite_autoindex_shark_characters_2", "shark_characters", {}},
        SchemaObject{"index", "sqlite_autoindex_shark_economy_1", "shark_economy", {}},
        SchemaObject{
            "index", "sqlite_autoindex_shark_transfer_receipts_1", "shark_transfer_receipts", {}},
    };
    for (const auto& object : objects) {
        if ((version == 1 && object.table != "shark_store") ||
            (version < 3 &&
             (object.table == "shark_economy" || object.table == "shark_transfer_receipts"))) {
            continue;
        }
        const auto status = sqlite3_step(schema.value);
        if (status != SQLITE_ROW && status != SQLITE_DONE) {
            return connection.error(status);
        }
        if (status != SQLITE_ROW || !text_is(schema.value, 0, object.type) ||
            !text_is(schema.value, 1, object.name) || !text_is(schema.value, 2, object.table) ||
            (object.sql.empty() ? sqlite3_column_type(schema.value, 3) != SQLITE_NULL
                                : !text_is(schema.value, 3, object.sql))) {
            return connection.invalid("database schema differs from its supported version");
        }
    }
    auto step = sqlite3_step(schema.value);
    if (step != SQLITE_DONE) {
        if (step != SQLITE_ROW) {
            return connection.error(step);
        }
        return connection.invalid("database schema contains unexpected objects");
    }
    if (auto result = require_text(connection, "PRAGMA quick_check(1)", "ok"); !result) {
        return result.error();
    }
    Statement foreign_keys;
    if (auto result = prepare(connection, foreign_keys, "PRAGMA foreign_key_check"); !result) {
        return result.error();
    }
    step = sqlite3_step(foreign_keys.value);
    if (step != SQLITE_DONE) {
        return step == SQLITE_ROW ? connection.invalid("database has dangling ownership")
                                  : connection.error(step);
    }
    Statement rows;
    if (auto result = prepare(connection, rows,
                              "SELECT singleton,store_id,world_id,schema_version FROM shark_store");
        !result) {
        return result.error();
    }
    step = sqlite3_step(rows.value);
    if (step != SQLITE_ROW && step != SQLITE_DONE) {
        return connection.error(step);
    }
    if (step != SQLITE_ROW || sqlite3_column_type(rows.value, 0) != SQLITE_INTEGER ||
        sqlite3_column_int64(rows.value, 0) != 1 ||
        sqlite3_column_type(rows.value, 3) != SQLITE_INTEGER ||
        sqlite3_column_int64(rows.value, 3) != static_cast<sqlite3_int64>(version)) {
        return connection.invalid("database identity row is missing or invalid");
    }
    std::array<std::optional<StableId>, 2> ids;
    for (int index = 1; index <= 2; ++index) {
        if (sqlite3_column_type(rows.value, index) != SQLITE_TEXT ||
            sqlite3_column_bytes(rows.value, index) != 36) {
            return connection.invalid("database identity is not a canonical UUID");
        }
        const auto* bytes = sqlite3_column_text(rows.value, index);
        if (bytes == nullptr) {
            return connection.invalid("database identity could not be read");
        }
        auto parsed = StableId::parse({reinterpret_cast<const char*>(bytes), 36});
        if (!parsed) {
            return connection.invalid("database identity is not a canonical UUID");
        }
        ids[static_cast<std::size_t>(index - 1)] = parsed.value();
    }
    step = sqlite3_step(rows.value);
    if (step != SQLITE_DONE) {
        if (step != SQLITE_ROW) {
            return connection.error(step);
        }
        return connection.invalid("database must contain exactly one identity row");
    }
    const auto world = WorldId{ids[1].value()};
    if (world != expected_world) {
        return failure(ErrorCode::conflict, connection.operation,
                       "database belongs to a different world", connection.path);
    }
    return StoreMetadata{StoreId{ids[0].value()}, world, version};
}

Result<StableId> read_id(const Connection& connection, sqlite3_stmt* row, int column) {
    if (sqlite3_column_type(row, column) != SQLITE_TEXT ||
        sqlite3_column_bytes(row, column) != 36) {
        return connection.invalid("record identity is not a canonical UUID");
    }
    const auto* text = sqlite3_column_text(row, column);
    if (text == nullptr) {
        return connection.invalid("record identity could not be read");
    }
    auto value = StableId::parse({reinterpret_cast<const char*>(text), 36});
    if (!value) {
        return connection.invalid("record identity is not a canonical UUID");
    }
    return value.value();
}

bool valid_integer(sqlite3_stmt* row, int column, sqlite3_int64 low, sqlite3_int64 high) {
    return sqlite3_column_type(row, column) == SQLITE_INTEGER &&
           sqlite3_column_int64(row, column) >= low && sqlite3_column_int64(row, column) <= high;
}

bool valid_certificate(const net::CertificateFingerprint& certificate) {
    return std::any_of(certificate.begin(), certificate.end(),
                       [](std::byte value) { return value != std::byte{}; });
}

template <typename Function>
Result<void> read_rows(const Connection& connection, const char* sql, std::size_t limit,
                       Function&& read) {
    Statement statement;
    if (auto result = prepare(connection, statement, sql); !result) {
        return result.error();
    }
    std::size_t count = 0;
    for (;;) {
        const auto step = sqlite3_step(statement.value);
        if (step == SQLITE_DONE) {
            return {};
        }
        if (step != SQLITE_ROW) {
            return connection.error(step);
        }
        if (count++ == limit) {
            return connection.invalid("durable record capacity exceeded");
        }
        if (auto result = read(statement.value); !result) {
            return result.error();
        }
    }
}

Result<DurableRegistry> read_registry(const Connection& connection, WorldId world) {
    DurableRegistry registry;
    constexpr auto maximum_revision = static_cast<sqlite3_int64>(maximum_durable_revision);
    auto accounts = read_rows(
        connection,
        "SELECT account_id,revoked,roster_revision FROM shark_accounts ORDER BY account_id",
        maximum_durable_accounts, [&](sqlite3_stmt* row) -> Result<void> {
            auto id = read_id(connection, row, 0);
            if (!id) {
                return id.error();
            }
            if (!valid_integer(row, 1, 0, 1) || !valid_integer(row, 2, 1, maximum_revision)) {
                return connection.invalid("invalid account revocation or roster revision");
            }
            registry.accounts.push_back({net::AccountId{id.value()},
                                         sqlite3_column_int(row, 1) != 0,
                                         static_cast<std::uint64_t>(sqlite3_column_int64(row, 2))});
            return {};
        });
    if (!accounts) {
        return accounts.error();
    }
    auto certificates = read_rows(
        connection,
        "SELECT certificate,account_id,revoked FROM shark_certificates ORDER BY certificate",
        maximum_durable_certificates, [&](sqlite3_stmt* row) -> Result<void> {
            auto account = read_id(connection, row, 1);
            if (!account) {
                return account.error();
            }
            if (sqlite3_column_type(row, 0) != SQLITE_BLOB || sqlite3_column_bytes(row, 0) != 32 ||
                sqlite3_column_blob(row, 0) == nullptr || !valid_integer(row, 2, 0, 1)) {
                return connection.invalid("invalid certificate enrollment");
            }
            net::CertificateFingerprint certificate{};
            std::memcpy(certificate.data(), sqlite3_column_blob(row, 0), certificate.size());
            if (!valid_certificate(certificate)) {
                return connection.invalid("certificate fingerprint is empty");
            }
            registry.certificates.push_back(
                {net::AccountId{account.value()}, certificate, sqlite3_column_int(row, 2) != 0});
            return {};
        });
    if (!certificates) {
        return certificates.error();
    }
    auto characters = read_rows(
        connection,
        "SELECT character_id,account_id,entity_id,revision,name,cell_x,cell_y,cell_z,"
        "offset_x,offset_y,offset_z FROM shark_characters ORDER BY character_id",
        maximum_durable_characters, [&](sqlite3_stmt* row) -> Result<void> {
            auto character = read_id(connection, row, 0);
            auto account = read_id(connection, row, 1);
            auto entity = read_id(connection, row, 2);
            if (!character) {
                return character.error();
            }
            if (!account) {
                return account.error();
            }
            if (!entity) {
                return entity.error();
            }
            if (!valid_integer(row, 3, 1, maximum_revision) ||
                sqlite3_column_type(row, 4) != SQLITE_TEXT || sqlite3_column_bytes(row, 4) < 1 ||
                sqlite3_column_bytes(row, 4) > 256 || sqlite3_column_text(row, 4) == nullptr) {
                return connection.invalid("invalid character revision or name");
            }
            for (int column = 5; column < 8; ++column) {
                if (!valid_integer(row, column, world_cell_min, world_cell_max)) {
                    return connection.invalid("invalid character world cell");
                }
            }
            for (int column = 8; column < 11; ++column) {
                if (sqlite3_column_type(row, column) != SQLITE_FLOAT) {
                    return connection.invalid("invalid character world offset");
                }
            }
            CharacterState state{
                Name{std::string{reinterpret_cast<const char*>(sqlite3_column_text(row, 4)),
                                 static_cast<std::size_t>(sqlite3_column_bytes(row, 4))}},
                WorldPosition{world,
                              {sqlite3_column_int64(row, 5), sqlite3_column_int64(row, 6),
                               sqlite3_column_int64(row, 7)},
                              {sqlite3_column_double(row, 8), sqlite3_column_double(row, 9),
                               sqlite3_column_double(row, 10)}}};
            if (!validate_character_state(state, world)) {
                return connection.invalid("invalid character checkpoint");
            }
            if (character.value() == entity.value() ||
                std::any_of(registry.characters.begin(), registry.characters.end(),
                            [&](const CharacterRecord& previous) {
                                return previous.character.value() == entity.value() ||
                                       previous.entity.value() == character.value();
                            })) {
                return connection.invalid("character and world entity identities must be distinct");
            }
            registry.characters.push_back(
                {net::AccountId{account.value()}, net::CharacterId{character.value()},
                 WorldEntityId{entity.value()},
                 static_cast<std::uint64_t>(sqlite3_column_int64(row, 3)), std::move(state)});
            return {};
        });
    if (!characters) {
        return characters.error();
    }
    for (const auto& account : registry.accounts) {
        const auto count = static_cast<std::size_t>(std::count_if(
            registry.characters.begin(), registry.characters.end(),
            [&](const CharacterRecord& record) { return record.account == account.account; }));
        if (count > maximum_characters_per_account || account.roster_revision <= count) {
            return connection.invalid("invalid account roster bounds or revision");
        }
    }
    return registry;
}

struct EconomyLedger {
    std::vector<EconomyRecord> records;
    std::vector<OperationStream> streams;
    bool operator==(const EconomyLedger&) const = default;
};

TransferStatus decide_transfer(const TransferRequest& request, const EconomyRecord& source,
                               const std::optional<EconomyRecord>& target) {
    if (!target) {
        return TransferStatus::target_unavailable;
    }
    if (source.revision != request.expected_source_revision ||
        target->revision != request.expected_target_revision) {
        return TransferStatus::stale_revision;
    }
    if (source.revision == maximum_durable_revision ||
        target->revision == maximum_durable_revision) {
        return TransferStatus::revision_exhausted;
    }
    if (source.state.currency < request.quantity.currency ||
        source.state.item_units < request.quantity.item_units) {
        return TransferStatus::insufficient_assets;
    }
    if (target->state.currency > maximum_economy_amount - request.quantity.currency ||
        target->state.item_units > maximum_economy_amount - request.quantity.item_units) {
        return TransferStatus::capacity_exceeded;
    }
    return TransferStatus::committed;
}

// Every retained snapshot at a given character/revision must agree, including
// the pre-state implied by a committed transfer. Later/pruned history is not
// reconstructed, but retained receipts cannot contradict one another or the
// current record and every committed pair conserves both modeled resources.
Result<void> validate_receipts(const Connection& connection, const EconomyLedger& ledger) {
    std::vector<EconomyRecord> snapshots;
    std::vector<std::pair<net::CharacterId, std::uint64_t>> committed_revisions;
    std::uint64_t pruned_operations = 0;
    auto remember = [&](const EconomyRecord& snapshot) -> Result<void> {
        const auto current =
            std::find_if(ledger.records.begin(), ledger.records.end(),
                         [&](const auto& value) { return value.character == snapshot.character; });
        if (current == ledger.records.end() || current->account != snapshot.account ||
            snapshot.revision == 0 || snapshot.revision > current->revision ||
            !validate_economy_state(snapshot.state) ||
            (snapshot.revision == current->revision && snapshot != *current)) {
            return connection.invalid("receipt snapshot contradicts durable economy");
        }
        const auto previous =
            std::find_if(snapshots.begin(), snapshots.end(), [&](const auto& value) {
                return value.character == snapshot.character && value.revision == snapshot.revision;
            });
        if (previous != snapshots.end()) {
            if (*previous != snapshot) {
                return connection.invalid("retained receipt snapshots disagree");
            }
        } else {
            snapshots.push_back(snapshot);
        }
        return {};
    };
    for (const auto& stream : ledger.streams) {
        std::vector<std::pair<net::CharacterId, std::uint64_t>> observed_revisions;
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(stream.high_watermark, retained_operation_receipts));
        if (stream.receipts.size() != count) {
            return connection.invalid("operation stream does not retain its complete window");
        }
        const auto pruned = stream.high_watermark - count;
        // Many independent account streams may approach INT64_MAX. Only a
        // conservative revision bound is needed; never overflow their sum.
        pruned_operations += std::min(pruned, maximum_durable_revision - pruned_operations);
        auto sequence = stream.high_watermark - count;
        for (const auto& receipt : stream.receipts) {
            const auto& request = receipt.request;
            if (request.operation.sequence != ++sequence ||
                request.operation.actor != stream.actor ||
                request.operation.store != stream.store || !receipt.source ||
                receipt.source->account != stream.actor ||
                receipt.source->character != request.source ||
                (receipt.target && receipt.target->character != request.target) ||
                !validate_transfer_request(request)) {
                return connection.invalid("invalid retained operation identity or payload");
            }
            for (const auto* snapshot : {&receipt.source, &receipt.target}) {
                if (!*snapshot) {
                    continue;
                }
                const auto& value = snapshot->value();
                const auto previous = std::find_if(
                    observed_revisions.begin(), observed_revisions.end(),
                    [&](const auto& observed) { return observed.first == value.character; });
                if (previous == observed_revisions.end()) {
                    observed_revisions.emplace_back(value.character, value.revision);
                } else {
                    if (value.revision < previous->second ||
                        (receipt.status == TransferStatus::committed &&
                         value.revision == previous->second)) {
                        return connection.invalid(
                            "operation history regresses or reuses a character revision");
                    }
                    previous->second = value.revision;
                }
                if (receipt.status == TransferStatus::committed) {
                    const auto identity = std::pair{value.character, value.revision};
                    if (std::find(committed_revisions.begin(), committed_revisions.end(),
                                  identity) != committed_revisions.end()) {
                        return connection.invalid(
                            "distinct transfers reuse a committed character revision");
                    }
                    committed_revisions.push_back(identity);
                }
            }
            if (auto checked = remember(*receipt.source); !checked) {
                return checked.error();
            }
            if (receipt.target) {
                if (auto checked = remember(*receipt.target); !checked) {
                    return checked.error();
                }
            }
            if (receipt.status == TransferStatus::committed) {
                if (!receipt.target ||
                    request.expected_source_revision == maximum_durable_revision ||
                    request.expected_target_revision == maximum_durable_revision ||
                    receipt.source->revision != request.expected_source_revision + 1 ||
                    receipt.target->revision != request.expected_target_revision + 1 ||
                    receipt.source->state.currency >
                        maximum_economy_amount - request.quantity.currency ||
                    receipt.source->state.item_units >
                        maximum_economy_amount - request.quantity.item_units ||
                    receipt.target->state.currency < request.quantity.currency ||
                    receipt.target->state.item_units < request.quantity.item_units) {
                    return connection.invalid(
                        "committed receipt has impossible revisions or values");
                }
                auto before_source = *receipt.source;
                auto before_target = *receipt.target;
                --before_source.revision;
                --before_target.revision;
                before_source.state.currency += request.quantity.currency;
                before_source.state.item_units += request.quantity.item_units;
                before_target.state.currency -= request.quantity.currency;
                before_target.state.item_units -= request.quantity.item_units;
                if (auto checked = remember(before_source); !checked) {
                    return checked.error();
                }
                if (auto checked = remember(before_target); !checked) {
                    return checked.error();
                }
            } else if (decide_transfer(request, *receipt.source, receipt.target) !=
                       receipt.status) {
                return connection.invalid("receipt business result disagrees with its snapshots");
            }
        }
    }
    for (const auto& record : ledger.records) {
        const auto retained = static_cast<std::uint64_t>(std::count_if(
            committed_revisions.begin(), committed_revisions.end(),
            [&](const auto& identity) { return identity.first == record.character; }));
        const auto possible =
            pruned_operations + std::min(retained, maximum_durable_revision - pruned_operations);
        const auto advances = record.revision - 1;
        // Initialization starts at one; only a committed transfer increments a
        // participant. Pruning can remove evidence of at most one such advance
        // per operation. With complete history every advance is accounted for.
        if (advances < retained || advances > possible) {
            return connection.invalid(
                "economy revision is not explained by durable operation history");
        }
    }
    return {};
}

Result<EconomyLedger> read_economy(const Connection& connection, const StoreMetadata& metadata,
                                   const DurableRegistry& registry) {
    EconomyLedger ledger;
    constexpr auto maximum_revision = static_cast<sqlite3_int64>(maximum_durable_revision);
    constexpr auto maximum_amount = static_cast<sqlite3_int64>(maximum_economy_amount);
    auto economies = read_rows(
        connection,
        "SELECT character_id,revision,currency,item_units FROM shark_economy ORDER BY character_id",
        maximum_durable_characters, [&](sqlite3_stmt* row) -> Result<void> {
            auto character = read_id(connection, row, 0);
            if (!character) {
                return character.error();
            }
            const auto owner = std::find_if(
                registry.characters.begin(), registry.characters.end(),
                [&](const auto& value) { return value.character.value() == character.value(); });
            if (owner == registry.characters.end() || !valid_integer(row, 1, 1, maximum_revision) ||
                !valid_integer(row, 2, 0, maximum_amount) ||
                !valid_integer(row, 3, 0, maximum_amount)) {
                return connection.invalid("invalid economy ownership, revision or value");
            }
            ledger.records.push_back({owner->account,
                                      owner->character,
                                      static_cast<std::uint64_t>(sqlite3_column_int64(row, 1)),
                                      {static_cast<std::uint64_t>(sqlite3_column_int64(row, 2)),
                                       static_cast<std::uint64_t>(sqlite3_column_int64(row, 3))}});
            return {};
        });
    if (!economies) {
        return economies.error();
    }
    auto streams = read_rows(
        connection, "SELECT account_id,operation_sequence FROM shark_accounts ORDER BY account_id",
        maximum_durable_accounts, [&](sqlite3_stmt* row) -> Result<void> {
            auto account = read_id(connection, row, 0);
            if (!account) {
                return account.error();
            }
            if (!valid_integer(row, 1, 0, maximum_revision)) {
                return connection.invalid("invalid durable operation high watermark");
            }
            ledger.streams.push_back({metadata.id,
                                      net::AccountId{account.value()},
                                      static_cast<std::uint64_t>(sqlite3_column_int64(row, 1)),
                                      {}});
            return {};
        });
    if (!streams) {
        return streams.error();
    }
    auto receipts = read_rows(
        connection,
        "SELECT actor_id,sequence,source_id,target_id,expected_source_revision,"
        "expected_target_revision,currency,item_units,status,source_revision,source_currency,"
        "source_items,target_revision,target_currency,target_items FROM shark_transfer_receipts "
        "ORDER BY actor_id,sequence",
        maximum_durable_accounts * retained_operation_receipts,
        [&](sqlite3_stmt* row) -> Result<void> {
            auto account = read_id(connection, row, 0);
            auto source_id = read_id(connection, row, 2);
            auto target_id = read_id(connection, row, 3);
            if (!account)
                return account.error();
            if (!source_id)
                return source_id.error();
            if (!target_id)
                return target_id.error();
            for (const auto column : {1, 4, 5, 9}) {
                if (!valid_integer(row, column, 1, maximum_revision)) {
                    return connection.invalid("invalid receipt sequence or revision");
                }
            }
            for (const auto column : {6, 7, 10, 11}) {
                if (!valid_integer(row, column, 0, maximum_amount)) {
                    return connection.invalid("invalid receipt resource value");
                }
            }
            if (!valid_integer(row, 8, 0, 5)) {
                return connection.invalid("invalid receipt result status");
            }
            const auto stream =
                std::find_if(ledger.streams.begin(), ledger.streams.end(), [&](const auto& value) {
                    return value.actor.value() == account.value();
                });
            if (stream == ledger.streams.end() ||
                stream->receipts.size() == retained_operation_receipts) {
                return connection.invalid("unknown or overfull receipt stream");
            }
            const auto number = [&](int column) {
                return static_cast<std::uint64_t>(sqlite3_column_int64(row, column));
            };
            const auto actor = net::AccountId{account.value()};
            TransferRequest request{{metadata.id, actor, number(1)},
                                    net::CharacterId{source_id.value()},
                                    net::CharacterId{target_id.value()},
                                    number(4),
                                    number(5),
                                    {number(6), number(7)}};
            const auto status = static_cast<TransferStatus>(sqlite3_column_int(row, 8));
            EconomyRecord source{actor, request.source, number(9), {number(10), number(11)}};
            std::optional<EconomyRecord> target;
            if (status == TransferStatus::target_unavailable) {
                for (const auto column : {12, 13, 14}) {
                    if (sqlite3_column_type(row, column) != SQLITE_NULL) {
                        return connection.invalid(
                            "unavailable target receipt contains private state");
                    }
                }
            } else {
                const auto owner = std::find_if(
                    ledger.records.begin(), ledger.records.end(),
                    [&](const auto& value) { return value.character == request.target; });
                if (owner == ledger.records.end() || !valid_integer(row, 12, 1, maximum_revision) ||
                    !valid_integer(row, 13, 0, maximum_amount) ||
                    !valid_integer(row, 14, 0, maximum_amount)) {
                    return connection.invalid("invalid receipt target snapshot");
                }
                target = EconomyRecord{
                    owner->account, request.target, number(12), {number(13), number(14)}};
            }
            stream->receipts.push_back({request, status, source, target});
            return {};
        });
    if (!receipts) {
        return receipts.error();
    }
    if (auto validated = validate_receipts(connection, ledger); !validated) {
        return validated.error();
    }
    return ledger;
}

Result<void> bind_text(const Connection& connection, Statement& statement, int index,
                       std::string_view value) {
    const auto code = sqlite3_bind_text(statement.value, index, value.data(),
                                        static_cast<int>(value.size()), SQLITE_TRANSIENT);
    return code == SQLITE_OK ? Result<void>{} : Result<void>{connection.error(code)};
}

Result<void> bind_integer(const Connection& connection, Statement& statement, int index,
                          std::uint64_t value) {
    const auto code = sqlite3_bind_int64(statement.value, index, static_cast<sqlite3_int64>(value));
    return code == SQLITE_OK ? Result<void>{} : Result<void>{connection.error(code)};
}

Result<void> bind_certificate(const Connection& connection, Statement& statement, int index,
                              const net::CertificateFingerprint& value) {
    const auto code = sqlite3_bind_blob(statement.value, index, value.data(),
                                        static_cast<int>(value.size()), SQLITE_TRANSIENT);
    return code == SQLITE_OK ? Result<void>{} : Result<void>{connection.error(code)};
}

Result<void> finish_write(const Connection& connection, Statement& statement) {
    const auto code = sqlite3_step(statement.value);
    if (code != SQLITE_DONE) {
        return connection.error(code);
    }
    if (sqlite3_changes(connection.db) != 1) {
        return connection.invalid("durable write did not affect exactly one expected record");
    }
    return {};
}

template <typename Function>
Result<void> change(const Connection& connection, const char* sql, Function&& bind) {
    Statement statement;
    if (auto result = prepare(connection, statement, sql); !result) {
        return result.error();
    }
    if (auto result = bind(statement); !result) {
        return result.error();
    }
    return finish_write(connection, statement);
}

Result<void> bind_state(const Connection& connection, Statement& statement, int index,
                        const CharacterState& state) {
    if (auto result = bind_text(connection, statement, index++, state.name.value); !result) {
        return result.error();
    }
    for (const auto cell : {state.position.cell.x, state.position.cell.y, state.position.cell.z}) {
        const auto code = sqlite3_bind_int64(statement.value, index++, cell);
        if (code != SQLITE_OK) {
            return connection.error(code);
        }
    }
    for (const auto offset : state.position.offset) {
        const auto code = sqlite3_bind_double(statement.value, index++, offset);
        if (code != SQLITE_OK) {
            return connection.error(code);
        }
    }
    return {};
}

bool injected(detail::WritePhase phase) {
    return write_fault_hook != nullptr && write_fault_hook(phase);
}

struct SnapshotData {
    StoreMetadata metadata;
    DurableRegistry records;
    EconomyLedger economy;
    bool operator==(const SnapshotData&) const = default;
};

struct MaintenanceOwner {
    std::filesystem::path path;
    std::unique_ptr<Connection> connection;
    SnapshotData snapshot;
};

Result<SnapshotData> read_snapshot(Connection& connection, StoreId expected_store,
                                   WorldId expected_world) {
    Statement version_query;
    if (auto result = prepare(connection, version_query, "PRAGMA user_version"); !result) {
        return result.error();
    }
    auto step = sqlite3_step(version_query.value);
    if (step != SQLITE_ROW) {
        return connection.error(step);
    }
    if (!valid_integer(version_query.value, 0, 1, store_schema_version)) {
        return connection.invalid("database schema version is unsupported");
    }
    const auto version = static_cast<std::uint32_t>(sqlite3_column_int(version_query.value, 0));
    step = sqlite3_step(version_query.value);
    if (step != SQLITE_DONE) {
        return connection.error(step);
    }
    auto metadata = inspect(connection, expected_world, version);
    if (!metadata) {
        return metadata.error();
    }
    if (metadata.value().id != expected_store) {
        return failure(ErrorCode::conflict, connection.operation,
                       "database belongs to a different store", connection.path);
    }
    SnapshotData snapshot{metadata.value(), {}, {}};
    if (version >= 2) {
        auto records = read_registry(connection, expected_world);
        if (!records) {
            return records.error();
        }
        snapshot.records = std::move(records).value();
    }
    if (version == store_schema_version) {
        auto economy = read_economy(connection, snapshot.metadata, snapshot.records);
        if (!economy) {
            return economy.error();
        }
        snapshot.economy = std::move(economy).value();
    }
    return snapshot;
}

Result<MaintenanceOwner> open_maintenance(const std::filesystem::path& path, StoreId expected_store,
                                          WorldId expected_world, std::string_view operation,
                                          bool read_only) {
    auto resolved = resolve_path(path, operation);
    if (!resolved) {
        return resolved.error();
    }
    auto connection = std::make_unique<Connection>();
    connection->path = utf8_path(resolved.value());
    connection->operation = operation;
    if (auto checked = check_sidecars(resolved.value(), *connection); !checked) {
        return checked.error();
    }
    if (auto checked = check_regular(resolved.value(), *connection, maximum_store_bytes, true);
        !checked) {
        return checked.error();
    }
    if (auto checked = check_header(resolved.value(), *connection, true); !checked) {
        return checked.error();
    }
    const auto flags = (read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE) |
                       SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_EXRESCODE | SQLITE_OPEN_NOFOLLOW;
    const auto opened = sqlite3_open_v2(connection->path.c_str(), &connection->db, flags, nullptr);
    if (opened != SQLITE_OK) {
        return connection->error(opened);
    }
    if (sqlite3_db_readonly(connection->db, "main") != (read_only ? 1 : 0)) {
        return failure(ErrorCode::io_failure, operation,
                       "database did not open with the required access mode", connection->path);
    }
    if (auto configured = configure(*connection); !configured) {
        return configured.error();
    }
    // The raw header preflight excludes empty databases. SQLite READONLY can
    // inspect a nonempty database with a cold journal without changing either
    // file, and refuses hot-journal recovery. READWRITE explicitly recovers
    // recognized interruptions before validating the actual recovered schema.
    if (auto begun = connection->execute(read_only ? "BEGIN" : "BEGIN EXCLUSIVE"); !begun) {
        return begun.error();
    }
    auto snapshot = read_snapshot(*connection, expected_store, expected_world);
    if (!snapshot) {
        return snapshot.error();
    }
    if (!read_only) {
        // EXCLUSIVE locking mode retains ownership outside the transaction.
        // SQLite backup cannot read a source with an active write transaction.
        if (auto committed = connection->execute("COMMIT"); !committed) {
            return committed.error();
        }
    }
    return MaintenanceOwner{resolved.value(), std::move(connection), std::move(snapshot).value()};
}

Result<std::filesystem::path> separate_destination(const MaintenanceOwner& source,
                                                   const std::filesystem::path& path) {
    auto resolved = resolve_path(path, source.connection->operation);
    if (!resolved) {
        return resolved.error();
    }
    std::error_code ec;
    const auto same =
        std::filesystem::equivalent(source.path.parent_path(), resolved.value().parent_path(), ec);
    if (ec || same) {
        return failure(ErrorCode::invalid_path, source.connection->operation,
                       "maintenance requires distinct existing canonical directories",
                       utf8_path(resolved.value()));
    }
    return resolved;
}

Result<SnapshotInfo> snapshot_info(const MaintenanceOwner& owner) {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(owner.path, ec);
    if (ec || bytes > maximum_store_bytes) {
        return failure(ErrorCode::io_failure, owner.connection->operation,
                       "validated database size could not be read", owner.connection->path);
    }
    std::size_t receipts = 0;
    for (const auto& stream : owner.snapshot.economy.streams) {
        receipts += stream.receipts.size();
    }
    return SnapshotInfo{owner.snapshot.metadata,
                        static_cast<std::uint64_t>(bytes),
                        owner.snapshot.records.accounts.size(),
                        owner.snapshot.records.certificates.size(),
                        owner.snapshot.records.characters.size(),
                        owner.snapshot.economy.records.size(),
                        owner.snapshot.economy.streams.size(),
                        receipts};
}

Result<void> maintenance_phase(const Connection& connection, detail::MaintenancePhase phase) {
    if (maintenance_fault_hook != nullptr && maintenance_fault_hook(phase)) {
        return failure(ErrorCode::io_failure, connection.operation,
                       "injected maintenance failure; inspect durable state before retry",
                       connection.path);
    }
    return {};
}

struct BackupHandle {
    sqlite3_backup* value{};
    ~BackupHandle() {
        if (value != nullptr) {
            sqlite3_backup_finish(value);
        }
    }
};

Result<SnapshotInfo> copy_snapshot(const MaintenanceOwner& source,
                                   const std::filesystem::path& destination, bool restoring) {
    auto resolved = separate_destination(source, destination);
    if (!resolved) {
        return resolved.error();
    }
    auto connection = std::make_unique<Connection>();
    connection->path = utf8_path(resolved.value());
    connection->operation = source.connection->operation;
    if (auto checked = check_sidecars(resolved.value(), *connection); !checked) {
        return checked.error();
    }
    auto journal = resolved.value();
    journal += "-journal";
    std::error_code ec;
    if (std::filesystem::exists(journal, ec) || ec) {
        return failure(ErrorCode::conflict, connection->operation,
                       "new snapshot path has an existing journal", connection->path);
    }
    if (auto reserved = reserve_file(resolved.value(), *connection); !reserved) {
        return reserved.error();
    }
    const auto opened = sqlite3_open_v2(connection->path.c_str(), &connection->db,
                                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX |
                                            SQLITE_OPEN_EXRESCODE | SQLITE_OPEN_NOFOLLOW,
                                        nullptr);
    if (opened != SQLITE_OK) {
        return connection->error(opened);
    }
    if (auto configured = configure(*connection); !configured) {
        return configured.error();
    }
    BackupHandle backup{sqlite3_backup_init(connection->db, "main", source.connection->db, "main")};
    if (backup.value == nullptr) {
        return connection->error(sqlite3_extended_errcode(connection->db));
    }
    bool completed = false;
    constexpr auto maximum_pages = maximum_store_bytes / 4096;
    for (std::uint64_t page = 0; page < maximum_pages; ++page) {
        const auto result = sqlite3_backup_step(backup.value, 1);
        if (result != SQLITE_OK && result != SQLITE_DONE) {
            return connection->error(result);
        }
        const auto count = sqlite3_backup_pagecount(backup.value);
        if (count < 1 || static_cast<std::uint64_t>(count) > maximum_pages) {
            return connection->invalid("database backup exceeds its page bound");
        }
        if (auto phase =
                maintenance_phase(*connection, restoring ? detail::MaintenancePhase::restore_step
                                                         : detail::MaintenancePhase::backup_step);
            !phase) {
            return phase.error();
        }
        if (result == SQLITE_DONE) {
            completed = true;
            break;
        }
    }
    if (!completed) {
        return connection->invalid("database backup did not finish within its page bound");
    }
    const auto finished = sqlite3_backup_finish(std::exchange(backup.value, nullptr));
    if (finished != SQLITE_OK) {
        return connection->error(finished);
    }
    if (auto phase =
            maintenance_phase(*connection, restoring ? detail::MaintenancePhase::restore_complete
                                                     : detail::MaintenancePhase::backup_complete);
        !phase) {
        return phase.error();
    }
    // Drop the destination owner before an independent read-only reopen. The
    // source owner still holds its coherent snapshot throughout validation.
    connection.reset();
    auto copied =
        open_maintenance(resolved.value(), source.snapshot.metadata.id,
                         source.snapshot.metadata.world, source.connection->operation, true);
    if (!copied) {
        return copied.error();
    }
    if (copied.value().snapshot != source.snapshot) {
        return copied.value().connection->invalid(
            "copied database differs from its source snapshot");
    }
    auto info = snapshot_info(copied.value());
    if (!info) {
        return info.error();
    }
    if (auto phase = maintenance_phase(*copied.value().connection,
                                       restoring ? detail::MaintenancePhase::restore_validated
                                                 : detail::MaintenancePhase::backup_validated);
        !phase) {
        return phase.error();
    }
    return info;
}

SnapshotData upgraded_snapshot(const SnapshotData& before) {
    auto after = before;
    if (after.metadata.schema_version < store_schema_version) {
        after.metadata.schema_version = store_schema_version;
        for (const auto& account : after.records.accounts) {
            after.economy.streams.push_back({after.metadata.id, account.account, 0, {}});
        }
    }
    return after;
}

Result<void> migrate_v1_to_v2(Connection& connection, const StoreMetadata& metadata) {
    if (auto dropped = connection.execute("DROP TABLE shark_store"); !dropped) {
        return dropped.error();
    }
    for (const auto sql : {table_v2_sql, accounts_v2_sql, certificates_sql, characters_sql}) {
        if (auto created = connection.execute(sql.data()); !created) {
            return created.error();
        }
    }
    auto inserted = change(
        connection, "INSERT INTO shark_store VALUES(1,?1,?2,2)",
        [&](Statement& statement) -> Result<void> {
            if (auto bound = bind_text(connection, statement, 1, metadata.id.value().to_string());
                !bound) {
                return bound.error();
            }
            return bind_text(connection, statement, 2, metadata.world.value().to_string());
        });
    if (!inserted) {
        return inserted.error();
    }
    return connection.execute("PRAGMA user_version=2");
}

Result<void> migrate_v2_to_v3(Connection& connection, const SnapshotData& after) {
    if (auto dropped = connection.execute("DROP TABLE shark_certificates;"
                                          "DROP TABLE shark_characters;"
                                          "DROP TABLE shark_accounts; DROP TABLE shark_store;");
        !dropped) {
        return dropped.error();
    }
    if (auto created = create_schema(connection, after.metadata, false); !created) {
        return created.error();
    }
    for (const auto& account : after.records.accounts) {
        auto inserted = change(
            connection, "INSERT INTO shark_accounts VALUES(?1,?2,?3,0)",
            [&](Statement& statement) -> Result<void> {
                if (auto bound =
                        bind_text(connection, statement, 1, account.account.value().to_string());
                    !bound) {
                    return bound.error();
                }
                if (auto bound = bind_integer(connection, statement, 2, account.revoked ? 1 : 0);
                    !bound) {
                    return bound.error();
                }
                return bind_integer(connection, statement, 3, account.roster_revision);
            });
        if (!inserted) {
            return inserted.error();
        }
    }
    for (const auto& certificate : after.records.certificates) {
        auto inserted =
            change(connection, "INSERT INTO shark_certificates VALUES(?1,?2,?3)",
                   [&](Statement& statement) -> Result<void> {
                       if (auto bound =
                               bind_certificate(connection, statement, 1, certificate.certificate);
                           !bound) {
                           return bound.error();
                       }
                       if (auto bound = bind_text(connection, statement, 2,
                                                  certificate.account.value().to_string());
                           !bound) {
                           return bound.error();
                       }
                       return bind_integer(connection, statement, 3, certificate.revoked ? 1 : 0);
                   });
        if (!inserted) {
            return inserted.error();
        }
    }
    for (const auto& character : after.records.characters) {
        auto inserted = change(
            connection, "INSERT INTO shark_characters VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)",
            [&](Statement& statement) -> Result<void> {
                for (const auto& [index, id] :
                     std::array<std::pair<int, StableId>, 3>{{{1, character.character.value()},
                                                              {2, character.account.value()},
                                                              {3, character.entity.value()}}}) {
                    if (auto bound = bind_text(connection, statement, index, id.to_string());
                        !bound) {
                        return bound.error();
                    }
                }
                if (auto bound = bind_integer(connection, statement, 4, character.revision);
                    !bound) {
                    return bound.error();
                }
                return bind_state(connection, statement, 5, character.state);
            });
        if (!inserted) {
            return inserted.error();
        }
    }
    return {};
}
} // namespace

struct Store::Impl {
    std::unique_ptr<Connection> connection;
    StoreMetadata metadata;
    std::thread::id thread;
    DurableRegistry records;
    EconomyLedger economy;
    bool faulted{false};

    Result<void> guard(std::string_view operation) const {
        if (std::this_thread::get_id() != thread) {
            return failure(ErrorCode::conflict, operation,
                           "database access requires its creating thread", connection->path);
        }
        if (faulted) {
            return failure(ErrorCode::execution_failed, operation,
                           "database owner requires destruction and reopen after a failed write",
                           connection->path);
        }
        connection->operation = operation;
        return {};
    }

    Error reject(std::string_view message) const {
        return failure(ErrorCode::conflict, connection->operation, message, connection->path);
    }

    auto account(net::AccountId id) const {
        return std::find_if(records.accounts.begin(), records.accounts.end(),
                            [&](const AccountRecord& record) { return record.account == id; });
    }

    void publish(DurableRegistry& candidate) noexcept {
        records.accounts.swap(candidate.accounts);
        records.certificates.swap(candidate.certificates);
        records.characters.swap(candidate.characters);
    }

    void publish(EconomyLedger& candidate) noexcept {
        economy.records.swap(candidate.records);
        economy.streams.swap(candidate.streams);
    }

    template <typename Candidate, typename Function>
    Result<void> write(Candidate& candidate, Function&& execute) {
        if (auto begun = connection->execute("BEGIN EXCLUSIVE"); !begun) {
            return begun.error();
        }
        auto result = execute();
        if (result && injected(detail::WritePhase::after_write)) {
            result = failure(ErrorCode::io_failure, connection->operation,
                             "injected precommit write failure", connection->path);
        }
        if (!result) {
            if (injected(detail::WritePhase::before_rollback) || !connection->execute("ROLLBACK")) {
                faulted = true;
            }
            return result.error();
        }
        if (injected(detail::WritePhase::before_commit)) {
            faulted = true;
            return failure(ErrorCode::io_failure, connection->operation,
                           "injected unsuccessful commit", connection->path);
        }
        if (auto committed = connection->execute("COMMIT"); !committed) {
            faulted = true;
            return committed.error();
        }
        if (injected(detail::WritePhase::after_commit)) {
            faulted = true;
            return failure(ErrorCode::io_failure, connection->operation,
                           "injected uncertain commit acknowledgement", connection->path);
        }
        // Candidate values and return snapshots were allocated before BEGIN.
        // These vector swaps cannot fail after the durable commit.
        publish(candidate);
        return {};
    }
};

namespace detail {
void set_create_hook(CreateHook hook) noexcept {
    create_hook = hook;
}
void set_write_fault_hook(WriteFaultHook hook) noexcept {
    write_fault_hook = hook;
}
void set_maintenance_fault_hook(MaintenanceFaultHook hook) noexcept {
    maintenance_fault_hook = hook;
}
} // namespace detail

Store::Store(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
Store::~Store() = default;

Result<std::unique_ptr<Store>> Store::create(const std::filesystem::path& path, WorldId world) {
    return acquire(path, world, true);
}

Result<std::unique_ptr<Store>> Store::open(const std::filesystem::path& path,
                                           WorldId expected_world) {
    return acquire(path, expected_world, false);
}

Result<SnapshotInfo> Store::inspect_snapshot(const std::filesystem::path& path,
                                             StoreId expected_store, WorldId expected_world) {
    auto owner =
        open_maintenance(path, expected_store, expected_world, "storage.inspect_snapshot", true);
    if (!owner) {
        return owner.error();
    }
    return snapshot_info(owner.value());
}

Result<SnapshotInfo> Store::backup(const std::filesystem::path& source,
                                   const std::filesystem::path& destination, StoreId expected_store,
                                   WorldId expected_world) {
    auto owner = open_maintenance(source, expected_store, expected_world, "storage.backup", false);
    if (!owner) {
        return owner.error();
    }
    return copy_snapshot(owner.value(), destination, false);
}

Result<SnapshotInfo> Store::restore(const std::filesystem::path& snapshot,
                                    const std::filesystem::path& destination,
                                    StoreId expected_store, WorldId expected_world) {
    auto owner =
        open_maintenance(snapshot, expected_store, expected_world, "storage.restore", true);
    if (!owner) {
        return owner.error();
    }
    return copy_snapshot(owner.value(), destination, true);
}

Result<MigrationResult> Store::migrate(const std::filesystem::path& source,
                                       const std::filesystem::path& verified_backup,
                                       StoreId expected_store, WorldId expected_world) {
    // Validate the independently closed backup before acquiring a writable
    // source. A bad or missing backup can never initiate source recovery/write.
    auto backup =
        open_maintenance(verified_backup, expected_store, expected_world, "storage.migrate", true);
    if (!backup) {
        return backup.error();
    }
    auto resolved = separate_destination(backup.value(), source);
    if (!resolved) {
        return resolved.error();
    }
    auto owner = open_maintenance(resolved.value(), expected_store, expected_world,
                                  "storage.migrate", false);
    if (!owner) {
        return owner.error();
    }
    auto& connection = *owner.value().connection;
    const auto& before = owner.value().snapshot;
    // All bounded candidate values and the successful reply are allocated
    // before any schema mutation. Retry equality compares every semantic row.
    const auto after = upgraded_snapshot(backup.value().snapshot);
    if (before.metadata.schema_version == store_schema_version) {
        if (before != after) {
            return failure(ErrorCode::conflict, connection.operation,
                           "current database differs from the verified backup transformation",
                           connection.path);
        }
        return MigrationResult{before.metadata, before.metadata, true};
    }
    if (before != backup.value().snapshot) {
        return failure(ErrorCode::conflict, connection.operation,
                       "migration backup differs from the complete source state", connection.path);
    }
    const MigrationResult result{before.metadata, after.metadata, false};
    if (auto begun = connection.execute("BEGIN EXCLUSIVE"); !begun) {
        return begun.error();
    }
    if (before.metadata.schema_version == 1) {
        if (auto migrated = migrate_v1_to_v2(connection, before.metadata); !migrated) {
            return migrated.error();
        }
        if (auto phase = maintenance_phase(connection, detail::MaintenancePhase::after_v1_to_v2);
            !phase) {
            return phase.error();
        }
    }
    if (auto migrated = migrate_v2_to_v3(connection, after); !migrated) {
        return migrated.error();
    }
    if (auto phase = maintenance_phase(connection, detail::MaintenancePhase::after_v2_to_v3);
        !phase) {
        return phase.error();
    }
    auto candidate = read_snapshot(connection, expected_store, expected_world);
    if (!candidate) {
        return candidate.error();
    }
    if (candidate.value() != after) {
        return connection.invalid("migration result differs from its deterministic transformation");
    }
    if (auto phase =
            maintenance_phase(connection, detail::MaintenancePhase::before_migration_commit);
        !phase) {
        return phase.error();
    }
    if (auto committed = connection.execute("COMMIT"); !committed) {
        return committed.error();
    }
    if (auto phase =
            maintenance_phase(connection, detail::MaintenancePhase::after_migration_commit);
        !phase) {
        return phase.error();
    }
    return result;
}

Result<std::unique_ptr<Store>> Store::acquire(const std::filesystem::path& path, WorldId world,
                                              bool create_new) {
    const auto operation = create_new ? "storage.create" : "storage.open";
    auto resolved = resolve_path(path, operation);
    if (!resolved) {
        return resolved.error();
    }
    auto connection = std::make_unique<Connection>();
    connection->path = utf8_path(resolved.value());
    connection->operation = operation;
    if (auto result = check_sidecars(resolved.value(), *connection); !result) {
        return result.error();
    }
    std::optional<StoreMetadata> metadata;
    if (create_new) {
        // Refuse even a leftover rollback journal before reserving a fresh name.
        auto journal = resolved.value();
        journal += "-journal";
        std::error_code ec;
        if (std::filesystem::exists(journal, ec) || ec) {
            return failure(ErrorCode::conflict, operation,
                           "new database path has an existing journal", connection->path);
        }
        auto id = generate_stable_id();
        if (!id) {
            return id.error();
        }
        metadata = StoreMetadata{StoreId{id.value()}, world, store_schema_version};
        if (auto result = reserve_file(resolved.value(), *connection); !result) {
            return result.error();
        }
    } else {
        if (auto result = check_regular(resolved.value(), *connection, maximum_store_bytes, true);
            !result) {
            return result.error();
        }
        if (auto result = check_header(resolved.value(), *connection); !result) {
            return result.error();
        }
    }
    const auto opened = sqlite3_open_v2(connection->path.c_str(), &connection->db,
                                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX |
                                            SQLITE_OPEN_EXRESCODE | SQLITE_OPEN_NOFOLLOW,
                                        nullptr);
    if (opened != SQLITE_OK) {
        return connection->error(opened);
    }
    if (sqlite3_db_readonly(connection->db, "main") != 0) {
        return failure(ErrorCode::io_failure, operation, "database must open with writable storage",
                       connection->path);
    }
    if (auto result = configure(*connection); !result) {
        return result.error();
    }
    if (auto result = connection->execute("BEGIN EXCLUSIVE"); !result) {
        return result.error();
    }
    if (create_new) {
        if (auto result = create_schema(*connection, metadata.value()); !result) {
            return result.error();
        }
    }
    auto inspected = inspect(*connection, world);
    if (!inspected) {
        return inspected.error();
    }
    auto records = read_registry(*connection, world);
    if (!records) {
        return records.error();
    }
    auto economy = read_economy(*connection, inspected.value(), records.value());
    if (!economy) {
        return economy.error();
    }
    if (create_new && create_hook != nullptr) {
        create_hook(detail::CreatePhase::before_commit);
    }
    if (auto result = connection->execute("COMMIT"); !result) {
        // Never acknowledge an unsuccessful commit; close releases/rolls back
        // remaining work. The caller must inspect the real file before retrying.
        return result.error();
    }
    if (create_new && create_hook != nullptr) {
        create_hook(detail::CreatePhase::after_commit);
    }
    auto impl = std::make_unique<Impl>(Impl{std::move(connection), inspected.value(),
                                            std::this_thread::get_id(), std::move(records).value(),
                                            std::move(economy).value()});
    return std::unique_ptr<Store>(new Store(std::move(impl)));
}

Result<StoreMetadata> Store::metadata() const {
    if (auto result = impl_->guard("storage.metadata"); !result) {
        return result.error();
    }
    return impl_->metadata;
}

Result<void> validate_character_state(const CharacterState& state, WorldId world) {
    constexpr auto operation = "storage.validate_character_state";
    if (auto result = validate_name(state.name); !result) {
        return failure(ErrorCode::invalid_argument, operation, result.error().message, {});
    }
    if (auto result = validate_world_position(state.position); !result) {
        return failure(ErrorCode::invalid_argument, operation, result.error().message, {});
    }
    if (state.position.world != world) {
        return failure(ErrorCode::invalid_argument, operation,
                       "character checkpoint belongs to a different world", {});
    }
    return {};
}

Result<DurableRegistry> Store::registry() const {
    if (auto result = impl_->guard("storage.registry"); !result) {
        return result.error();
    }
    return impl_->records;
}

Result<AccountRecord> Store::provision_account(net::AccountId account) {
    if (auto result = impl_->guard("storage.provision_account"); !result) {
        return result.error();
    }
    if (impl_->account(account) != impl_->records.accounts.end()) {
        return impl_->reject("account identity is already provisioned");
    }
    if (impl_->records.accounts.size() == maximum_durable_accounts) {
        return impl_->reject("durable account capacity reached");
    }
    const AccountRecord record{account, false, 1};
    auto candidate = impl_->records;
    candidate.accounts.push_back(record);
    std::sort(candidate.accounts.begin(), candidate.accounts.end(),
              [](const auto& a, const auto& b) { return a.account < b.account; });
    const auto id = account.value().to_string();
    auto& connection = *impl_->connection;
    auto written = impl_->write(candidate, [&] {
        return change(
            connection, "INSERT INTO shark_accounts VALUES(?1,0,1,0)",
            [&](Statement& statement) { return bind_text(connection, statement, 1, id); });
    });
    if (!written) {
        return written.error();
    }
    return record;
}

Result<void> Store::enroll_certificate(net::AccountId account,
                                       const net::CertificateFingerprint& certificate) {
    if (auto result = impl_->guard("storage.enroll_certificate"); !result) {
        return result.error();
    }
    const auto owner = impl_->account(account);
    if (owner == impl_->records.accounts.end() || owner->revoked) {
        return impl_->reject("enrollment requires an active durable account");
    }
    if (!valid_certificate(certificate)) {
        return failure(ErrorCode::invalid_argument, "storage.enroll_certificate",
                       "certificate fingerprint is empty", impl_->connection->path);
    }
    if (std::any_of(impl_->records.certificates.begin(), impl_->records.certificates.end(),
                    [&](const auto& record) { return record.certificate == certificate; })) {
        return impl_->reject("certificate identity is already enrolled");
    }
    if (impl_->records.certificates.size() == maximum_durable_certificates) {
        return impl_->reject("durable certificate capacity reached");
    }
    auto candidate = impl_->records;
    candidate.certificates.push_back({account, certificate, false});
    std::sort(candidate.certificates.begin(), candidate.certificates.end(),
              [](const auto& a, const auto& b) { return a.certificate < b.certificate; });
    const auto id = account.value().to_string();
    auto& connection = *impl_->connection;
    return impl_->write(candidate, [&] {
        return change(connection, "INSERT INTO shark_certificates VALUES(?1,?2,0)",
                      [&](Statement& statement) -> Result<void> {
                          if (auto result = bind_certificate(connection, statement, 1, certificate);
                              !result) {
                              return result.error();
                          }
                          return bind_text(connection, statement, 2, id);
                      });
    });
}

Result<void> Store::revoke_account(net::AccountId account) {
    if (auto result = impl_->guard("storage.revoke_account"); !result) {
        return result.error();
    }
    const auto owner = impl_->account(account);
    if (owner == impl_->records.accounts.end()) {
        return impl_->reject("unknown durable account");
    }
    if (owner->revoked) {
        return {};
    }
    auto candidate = impl_->records;
    const auto index = static_cast<std::size_t>(owner - impl_->records.accounts.begin());
    candidate.accounts[index].revoked = true;
    const auto id = account.value().to_string();
    auto& connection = *impl_->connection;
    return impl_->write(candidate, [&] {
        return change(
            connection, "UPDATE shark_accounts SET revoked=1 WHERE account_id=?1",
            [&](Statement& statement) { return bind_text(connection, statement, 1, id); });
    });
}

Result<void> Store::revoke_certificate(const net::CertificateFingerprint& certificate) {
    if (auto result = impl_->guard("storage.revoke_certificate"); !result) {
        return result.error();
    }
    const auto found =
        std::find_if(impl_->records.certificates.begin(), impl_->records.certificates.end(),
                     [&](const auto& record) { return record.certificate == certificate; });
    if (found == impl_->records.certificates.end()) {
        return impl_->reject("unknown certificate");
    }
    if (found->revoked) {
        return {};
    }
    auto candidate = impl_->records;
    const auto index = static_cast<std::size_t>(found - impl_->records.certificates.begin());
    candidate.certificates[index].revoked = true;
    auto& connection = *impl_->connection;
    return impl_->write(candidate, [&] {
        return change(connection, "UPDATE shark_certificates SET revoked=1 WHERE certificate=?1",
                      [&](Statement& statement) {
                          return bind_certificate(connection, statement, 1, certificate);
                      });
    });
}

Result<CharacterRoster> Store::roster(net::AccountId account) const {
    if (auto result = impl_->guard("storage.roster"); !result) {
        return result.error();
    }
    const auto owner = impl_->account(account);
    if (owner == impl_->records.accounts.end()) {
        return impl_->reject("unknown durable account");
    }
    CharacterRoster result{account, owner->roster_revision, {}};
    for (const auto& record : impl_->records.characters) {
        if (record.account == account) {
            result.characters.push_back(record);
        }
    }
    return result;
}

Result<CharacterRecord> Store::character(net::AccountId account, net::CharacterId character) const {
    if (auto result = impl_->guard("storage.character"); !result) {
        return result.error();
    }
    const auto found =
        std::find_if(impl_->records.characters.begin(), impl_->records.characters.end(),
                     [&](const auto& record) {
                         return record.account == account && record.character == character;
                     });
    if (found == impl_->records.characters.end()) {
        return impl_->reject("unknown owned character");
    }
    return *found;
}

Result<CharacterRecord> Store::create_character(net::AccountId account,
                                                std::uint64_t expected_roster_revision,
                                                const CharacterState& initial) {
    if (auto result = impl_->guard("storage.create_character"); !result) {
        return result.error();
    }
    const auto owner = impl_->account(account);
    if (owner == impl_->records.accounts.end() || owner->revoked) {
        return impl_->reject("character creation requires an active durable account");
    }
    if (owner->roster_revision != expected_roster_revision ||
        owner->roster_revision == maximum_durable_revision) {
        return impl_->reject("stale or exhausted roster revision");
    }
    const auto count = static_cast<std::size_t>(
        std::count_if(impl_->records.characters.begin(), impl_->records.characters.end(),
                      [&](const auto& record) { return record.account == account; }));
    if (count == maximum_characters_per_account ||
        impl_->records.characters.size() == maximum_durable_characters) {
        return impl_->reject("durable character capacity reached");
    }
    if (auto result = validate_character_state(initial, impl_->metadata.world); !result) {
        return failure(result.error().code, "storage.create_character", result.error().message,
                       impl_->connection->path);
    }
    auto character_id = generate_stable_id();
    if (!character_id) {
        return character_id.error();
    }
    auto entity_id = generate_stable_id();
    if (!entity_id) {
        return entity_id.error();
    }
    if (character_id.value() == entity_id.value() ||
        std::any_of(impl_->records.characters.begin(), impl_->records.characters.end(),
                    [&](const auto& record) {
                        return record.character.value() == character_id.value() ||
                               record.entity.value() == character_id.value() ||
                               record.character.value() == entity_id.value() ||
                               record.entity.value() == entity_id.value();
                    })) {
        return impl_->reject("generated character or world entity identity collided");
    }
    CharacterRecord record{account, net::CharacterId{character_id.value()},
                           WorldEntityId{entity_id.value()}, 1, initial};
    auto candidate = impl_->records;
    const auto owner_index = static_cast<std::size_t>(owner - impl_->records.accounts.begin());
    ++candidate.accounts[owner_index].roster_revision;
    candidate.characters.push_back(record);
    std::sort(candidate.characters.begin(), candidate.characters.end(),
              [](const auto& a, const auto& b) { return a.character < b.character; });
    const auto account_text = account.value().to_string();
    const auto character_text = record.character.value().to_string();
    const auto entity_text = record.entity.value().to_string();
    auto& connection = *impl_->connection;
    auto written = impl_->write(candidate, [&]() -> Result<void> {
        auto inserted = change(
            connection, "INSERT INTO shark_characters VALUES(?1,?2,?3,1,?4,?5,?6,?7,?8,?9,?10)",
            [&](Statement& statement) -> Result<void> {
                if (auto result = bind_text(connection, statement, 1, character_text); !result) {
                    return result.error();
                }
                if (auto result = bind_text(connection, statement, 2, account_text); !result) {
                    return result.error();
                }
                if (auto result = bind_text(connection, statement, 3, entity_text); !result) {
                    return result.error();
                }
                return bind_state(connection, statement, 4, record.state);
            });
        if (!inserted) {
            return inserted.error();
        }
        return change(connection,
                      "UPDATE shark_accounts SET roster_revision=roster_revision+1 WHERE "
                      "account_id=?1 AND roster_revision=?2",
                      [&](Statement& statement) -> Result<void> {
                          if (auto result = bind_text(connection, statement, 1, account_text);
                              !result) {
                              return result.error();
                          }
                          return bind_integer(connection, statement, 2, expected_roster_revision);
                      });
    });
    if (!written) {
        return written.error();
    }
    return record;
}

Result<CharacterRecord> Store::save_character(net::AccountId account, net::CharacterId character,
                                              std::uint64_t expected_revision,
                                              const CharacterState& accepted_state) {
    if (auto result = impl_->guard("storage.save_character"); !result) {
        return result.error();
    }
    const auto owner = impl_->account(account);
    if (owner == impl_->records.accounts.end() || owner->revoked) {
        return impl_->reject("checkpoint requires an active durable account");
    }
    const auto found =
        std::find_if(impl_->records.characters.begin(), impl_->records.characters.end(),
                     [&](const auto& record) {
                         return record.account == account && record.character == character;
                     });
    if (found == impl_->records.characters.end()) {
        return impl_->reject("unknown owned character");
    }
    if (found->revision != expected_revision || found->revision == maximum_durable_revision) {
        return impl_->reject("stale or exhausted character revision");
    }
    if (auto result = validate_character_state(accepted_state, impl_->metadata.world); !result) {
        return failure(result.error().code, "storage.save_character", result.error().message,
                       impl_->connection->path);
    }
    CharacterRecord record{account, character, found->entity, found->revision + 1, accepted_state};
    auto candidate = impl_->records;
    const auto index = static_cast<std::size_t>(found - impl_->records.characters.begin());
    candidate.characters[index] = record;
    const auto account_text = account.value().to_string();
    const auto character_text = character.value().to_string();
    auto& connection = *impl_->connection;
    auto written = impl_->write(candidate, [&] {
        return change(
            connection,
            "UPDATE shark_characters SET revision=revision+1,name=?1,cell_x=?2,cell_y=?3,cell_z=?4,"
            "offset_x=?5,offset_y=?6,offset_z=?7 WHERE character_id=?8 AND account_id=?9 AND "
            "revision=?10",
            [&](Statement& statement) -> Result<void> {
                if (auto result = bind_state(connection, statement, 1, record.state); !result) {
                    return result.error();
                }
                if (auto result = bind_text(connection, statement, 8, character_text); !result) {
                    return result.error();
                }
                if (auto result = bind_text(connection, statement, 9, account_text); !result) {
                    return result.error();
                }
                return bind_integer(connection, statement, 10, expected_revision);
            });
    });
    if (!written) {
        return written.error();
    }
    return record;
}

Result<void> validate_economy_state(const EconomyState& state) {
    if (state.currency > maximum_economy_amount || state.item_units > maximum_economy_amount) {
        return failure(ErrorCode::invalid_argument, "storage.validate_economy_state",
                       "economy values exceed the bounded capacity", {});
    }
    return {};
}

Result<void> validate_transfer_request(const TransferRequest& request) {
    if (request.source == request.target || request.operation.sequence == 0 ||
        request.operation.sequence > maximum_durable_revision ||
        request.expected_source_revision == 0 ||
        request.expected_source_revision > maximum_durable_revision ||
        request.expected_target_revision == 0 ||
        request.expected_target_revision > maximum_durable_revision ||
        !validate_economy_state(request.quantity) ||
        (request.quantity.currency == 0 && request.quantity.item_units == 0)) {
        return failure(ErrorCode::invalid_argument, "storage.validate_transfer_request",
                       "transfer requires distinct characters, bounded positive sequence/revisions "
                       "and quantity",
                       {});
    }
    return {};
}

Result<EconomyRecord> Store::initialize_economy(net::AccountId account, net::CharacterId character,
                                                const EconomyState& initial) {
    if (auto checked = impl_->guard("storage.initialize_economy"); !checked) {
        return checked.error();
    }
    if (auto checked = validate_economy_state(initial); !checked) {
        return failure(checked.error().code, "storage.initialize_economy", checked.error().message,
                       impl_->connection->path);
    }
    const auto owner = impl_->account(account);
    const auto found = std::find_if(
        impl_->records.characters.begin(), impl_->records.characters.end(), [&](const auto& value) {
            return value.character == character && value.account == account;
        });
    if (owner == impl_->records.accounts.end() || owner->revoked ||
        found == impl_->records.characters.end()) {
        return impl_->reject("economy initialization requires an active owning account");
    }
    if (std::any_of(impl_->economy.records.begin(), impl_->economy.records.end(),
                    [&](const auto& value) { return value.character == character; })) {
        return impl_->reject("character economy is already initialized");
    }
    const EconomyRecord record{account, character, 1, initial};
    auto candidate = impl_->economy;
    candidate.records.push_back(record);
    std::sort(candidate.records.begin(), candidate.records.end(),
              [](const auto& a, const auto& b) { return a.character < b.character; });
    const auto id = character.value().to_string();
    auto& connection = *impl_->connection;
    auto written = impl_->write(candidate, [&] {
        return change(connection, "INSERT INTO shark_economy VALUES(?1,1,?2,?3)",
                      [&](Statement& statement) -> Result<void> {
                          if (auto bound = bind_text(connection, statement, 1, id); !bound)
                              return bound.error();
                          if (auto bound = bind_integer(connection, statement, 2, initial.currency);
                              !bound)
                              return bound.error();
                          return bind_integer(connection, statement, 3, initial.item_units);
                      });
    });
    if (!written)
        return written.error();
    return record;
}

Result<EconomyRecord> Store::economy(net::AccountId account, net::CharacterId character) const {
    if (auto checked = impl_->guard("storage.economy"); !checked) {
        return checked.error();
    }
    const auto found = std::find_if(
        impl_->economy.records.begin(), impl_->economy.records.end(), [&](const auto& value) {
            return value.character == character && value.account == account;
        });
    if (found == impl_->economy.records.end()) {
        return impl_->reject("unknown owned character economy");
    }
    return *found;
}

Result<OperationStream> Store::operation_stream(net::AccountId actor) const {
    if (auto checked = impl_->guard("storage.operation_stream"); !checked) {
        return checked.error();
    }
    if (impl_->account(actor) == impl_->records.accounts.end()) {
        return impl_->reject("unknown operation account");
    }
    const auto found = std::find_if(impl_->economy.streams.begin(), impl_->economy.streams.end(),
                                    [&](const auto& value) { return value.actor == actor; });
    return found == impl_->economy.streams.end() ? OperationStream{impl_->metadata.id, actor, 0, {}}
                                                 : *found;
}

Result<TransferResult> Store::transfer(const TransferRequest& request) {
    if (auto checked = impl_->guard("storage.transfer"); !checked) {
        return checked.error();
    }
    if (auto checked = validate_transfer_request(request); !checked) {
        return failure(checked.error().code, "storage.transfer", checked.error().message,
                       impl_->connection->path);
    }
    if (request.operation.store != impl_->metadata.id) {
        return impl_->reject("operation belongs to a different store");
    }
    const auto actor = impl_->account(request.operation.actor);
    const auto source = std::find_if(
        impl_->economy.records.begin(), impl_->economy.records.end(), [&](const auto& value) {
            return value.character == request.source && value.account == request.operation.actor;
        });
    if (actor == impl_->records.accounts.end() || actor->revoked ||
        source == impl_->economy.records.end()) {
        return impl_->reject(
            "transfer requires an active account and its initialized source economy");
    }
    const auto stream =
        std::find_if(impl_->economy.streams.begin(), impl_->economy.streams.end(),
                     [&](const auto& value) { return value.actor == request.operation.actor; });
    const auto high_watermark = stream == impl_->economy.streams.end() ? 0 : stream->high_watermark;
    if (request.operation.sequence <= high_watermark) {
        const auto receipt =
            std::find_if(stream->receipts.begin(), stream->receipts.end(), [&](const auto& value) {
                return value.request.operation.sequence == request.operation.sequence;
            });
        if (receipt == stream->receipts.end()) {
            return failure(
                ErrorCode::conflict, "storage.transfer.expired",
                "operation is older than the retained outcome window and can never execute again",
                impl_->connection->path);
        }
        if (receipt->request != request) {
            return failure(
                ErrorCode::conflict, "storage.transfer.mismatch",
                "operation identity was already admitted with a different canonical payload",
                impl_->connection->path);
        }
        return TransferResult{*receipt, true};
    }
    if (high_watermark == maximum_durable_revision ||
        request.operation.sequence != high_watermark + 1) {
        return failure(ErrorCode::conflict, "storage.transfer.sequence",
                       "operation must use the exact next sequence without wrap",
                       impl_->connection->path);
    }
    const auto target =
        std::find_if(impl_->economy.records.begin(), impl_->economy.records.end(),
                     [&](const auto& value) { return value.character == request.target; });
    std::optional<EconomyRecord> accepted_target;
    if (target != impl_->economy.records.end()) {
        const auto owner = impl_->account(target->account);
        if (owner != impl_->records.accounts.end() && !owner->revoked) {
            accepted_target = *target;
        }
    }
    const auto status = decide_transfer(request, *source, accepted_target);
    TransferReceipt receipt{request, status, *source, accepted_target};
    auto candidate = impl_->economy;
    if (status == TransferStatus::committed) {
        auto& debit = *receipt.source;
        auto& credit = *receipt.target;
        ++debit.revision;
        ++credit.revision;
        debit.state.currency -= request.quantity.currency;
        debit.state.item_units -= request.quantity.item_units;
        credit.state.currency += request.quantity.currency;
        credit.state.item_units += request.quantity.item_units;
        candidate.records[static_cast<std::size_t>(source - impl_->economy.records.begin())] =
            debit;
        candidate.records[static_cast<std::size_t>(target - impl_->economy.records.begin())] =
            credit;
    }
    auto destination =
        std::find_if(candidate.streams.begin(), candidate.streams.end(),
                     [&](const auto& value) { return value.actor == request.operation.actor; });
    if (destination == candidate.streams.end()) {
        candidate.streams.push_back({impl_->metadata.id, request.operation.actor, 0, {}});
        destination = candidate.streams.end() - 1;
    }
    destination->high_watermark = request.operation.sequence;
    destination->receipts.push_back(receipt);
    if (destination->receipts.size() > retained_operation_receipts) {
        destination->receipts.erase(destination->receipts.begin());
    }
    const TransferResult result{receipt, false};
    const auto actor_text = request.operation.actor.value().to_string();
    const auto source_text = request.source.value().to_string();
    const auto target_text = request.target.value().to_string();
    auto& connection = *impl_->connection;
    const auto update = [&](const EconomyRecord& record, const std::string& id,
                            std::uint64_t expected) -> Result<void> {
        return change(
            connection,
            "UPDATE shark_economy SET revision=?1,currency=?2,item_units=?3 "
            "WHERE character_id=?4 AND revision=?5",
            [&](Statement& statement) -> Result<void> {
                if (auto bound = bind_integer(connection, statement, 1, record.revision); !bound)
                    return bound.error();
                if (auto bound = bind_integer(connection, statement, 2, record.state.currency);
                    !bound)
                    return bound.error();
                if (auto bound = bind_integer(connection, statement, 3, record.state.item_units);
                    !bound)
                    return bound.error();
                if (auto bound = bind_text(connection, statement, 4, id); !bound)
                    return bound.error();
                return bind_integer(connection, statement, 5, expected);
            });
    };
    auto written = impl_->write(candidate, [&]() -> Result<void> {
        if (status == TransferStatus::committed) {
            if (auto changed =
                    update(*receipt.source, source_text, request.expected_source_revision);
                !changed) {
                return changed.error();
            }
            if (injected(detail::WritePhase::after_source_debit)) {
                return failure(ErrorCode::io_failure, "storage.transfer",
                               "injected failure after source debit", connection.path);
            }
            if (auto changed =
                    update(*receipt.target, target_text, request.expected_target_revision);
                !changed) {
                return changed.error();
            }
        }
        auto inserted = change(
            connection,
            "INSERT INTO shark_transfer_receipts "
            "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15)",
            [&](Statement& statement) -> Result<void> {
                if (auto bound = bind_text(connection, statement, 1, actor_text); !bound)
                    return bound.error();
                if (auto bound = bind_integer(connection, statement, 2, request.operation.sequence);
                    !bound)
                    return bound.error();
                if (auto bound = bind_text(connection, statement, 3, source_text); !bound)
                    return bound.error();
                if (auto bound = bind_text(connection, statement, 4, target_text); !bound)
                    return bound.error();
                int column = 5;
                for (const auto value :
                     {request.expected_source_revision, request.expected_target_revision,
                      request.quantity.currency, request.quantity.item_units,
                      static_cast<std::uint64_t>(status), receipt.source->revision,
                      receipt.source->state.currency, receipt.source->state.item_units}) {
                    if (auto bound = bind_integer(connection, statement, column++, value); !bound)
                        return bound.error();
                }
                for (const auto value : {receipt.target ? receipt.target->revision : 0,
                                         receipt.target ? receipt.target->state.currency : 0,
                                         receipt.target ? receipt.target->state.item_units : 0}) {
                    if (receipt.target) {
                        if (auto bound = bind_integer(connection, statement, column++, value);
                            !bound)
                            return bound.error();
                    } else {
                        const auto code = sqlite3_bind_null(statement.value, column++);
                        if (code != SQLITE_OK)
                            return connection.error(code);
                    }
                }
                return {};
            });
        if (!inserted)
            return inserted.error();
        auto advanced = change(
            connection,
            "UPDATE shark_accounts SET operation_sequence=?1 WHERE account_id=?2 AND "
            "operation_sequence=?3",
            [&](Statement& statement) -> Result<void> {
                if (auto bound = bind_integer(connection, statement, 1, request.operation.sequence);
                    !bound)
                    return bound.error();
                if (auto bound = bind_text(connection, statement, 2, actor_text); !bound)
                    return bound.error();
                return bind_integer(connection, statement, 3, high_watermark);
            });
        if (!advanced)
            return advanced.error();
        if (request.operation.sequence > retained_operation_receipts) {
            return change(
                connection, "DELETE FROM shark_transfer_receipts WHERE actor_id=?1 AND sequence=?2",
                [&](Statement& statement) -> Result<void> {
                    if (auto bound = bind_text(connection, statement, 1, actor_text); !bound)
                        return bound.error();
                    return bind_integer(connection, statement, 2,
                                        request.operation.sequence - retained_operation_receipts);
                });
        }
        return {};
    });
    if (!written)
        return written.error();
    return result;
}

} // namespace shark::storage
