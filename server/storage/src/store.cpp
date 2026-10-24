#include "shark/storage/store.hpp"

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
constexpr std::string_view table_sql =
    "CREATE TABLE shark_store(singleton INTEGER PRIMARY KEY CHECK(singleton=1),"
    "store_id TEXT NOT NULL CHECK(length(store_id)=36),"
    "world_id TEXT NOT NULL CHECK(length(world_id)=36),"
    "schema_version INTEGER NOT NULL CHECK(schema_version=2)) STRICT";
constexpr std::string_view accounts_sql =
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
thread_local detail::CreateHook create_hook = nullptr;
thread_local detail::WriteFaultHook write_fault_hook = nullptr;

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

Result<void> check_header(const std::filesystem::path& path, const Connection& connection) {
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
    if (std::memcmp(header.data(), "SQLite format 3\0", 16) != 0 || u32(68) != application_id ||
        u32(60) != store_schema_version || header[16] != 16 || header[17] != 0 || header[18] != 1 ||
        header[19] != 1) {
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

Result<void> create_schema(Connection& connection, const StoreMetadata& metadata) {
    for (const auto sql : {table_sql, accounts_sql, certificates_sql, characters_sql}) {
        if (auto result = connection.execute(sql.data()); !result) {
            return result.error();
        }
    }
    if (create_hook != nullptr) {
        create_hook(detail::CreatePhase::schema_written);
    }
    Statement insert;
    if (auto result = prepare(connection, insert, "INSERT INTO shark_store VALUES(1,?1,?2,2)");
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
    return connection.execute("PRAGMA application_id=1397246788; PRAGMA user_version=2");
}

Result<StoreMetadata> inspect(Connection& connection, WorldId expected_world) {
    if (auto result = require_integer(connection, "PRAGMA application_id", application_id);
        !result) {
        return result.error();
    }
    if (auto result = require_integer(connection, "PRAGMA user_version", store_schema_version);
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
    constexpr std::array objects{
        SchemaObject{"table", "shark_accounts", "shark_accounts", accounts_sql},
        SchemaObject{"table", "shark_certificates", "shark_certificates", certificates_sql},
        SchemaObject{"table", "shark_characters", "shark_characters", characters_sql},
        SchemaObject{"table", "shark_store", "shark_store", table_sql},
        SchemaObject{"index", "sqlite_autoindex_shark_accounts_1", "shark_accounts", {}},
        SchemaObject{"index", "sqlite_autoindex_shark_certificates_1", "shark_certificates", {}},
        SchemaObject{"index", "sqlite_autoindex_shark_characters_1", "shark_characters", {}},
        SchemaObject{"index", "sqlite_autoindex_shark_characters_2", "shark_characters", {}},
    };
    for (const auto& object : objects) {
        const auto status = sqlite3_step(schema.value);
        if (status != SQLITE_ROW && status != SQLITE_DONE) {
            return connection.error(status);
        }
        if (status != SQLITE_ROW || !text_is(schema.value, 0, object.type) ||
            !text_is(schema.value, 1, object.name) || !text_is(schema.value, 2, object.table) ||
            (object.sql.empty() ? sqlite3_column_type(schema.value, 3) != SQLITE_NULL
                                : !text_is(schema.value, 3, object.sql))) {
            return connection.invalid("database schema differs from supported version 2");
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
        sqlite3_column_int64(rows.value, 3) != store_schema_version) {
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
    return StoreMetadata{StoreId{ids[0].value()}, world, store_schema_version};
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
} // namespace

struct Store::Impl {
    std::unique_ptr<Connection> connection;
    StoreMetadata metadata;
    std::thread::id thread;
    DurableRegistry records;
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

    template <typename Function>
    Result<void> write(DurableRegistry& candidate, Function&& execute) {
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
        records.accounts.swap(candidate.accounts);
        records.certificates.swap(candidate.certificates);
        records.characters.swap(candidate.characters);
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
    auto impl =
        std::make_unique<Impl>(Impl{std::move(connection), inspected.value(),
                                    std::this_thread::get_id(), std::move(records).value()});
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
            connection, "INSERT INTO shark_accounts VALUES(?1,0,1)",
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

} // namespace shark::storage
