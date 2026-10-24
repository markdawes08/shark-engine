#include "shark/storage/store.hpp"

#include "shark/project_path.hpp"
#include "store_test_hooks.hpp"

#include <sqlite3.h>

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
    "schema_version INTEGER NOT NULL CHECK(schema_version=1)) STRICT";
thread_local detail::CreateHook create_hook = nullptr;

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
    if (auto result = connection.execute(table_sql.data()); !result) {
        return result.error();
    }
    if (create_hook != nullptr) {
        create_hook(detail::CreatePhase::schema_written);
    }
    Statement insert;
    if (auto result = prepare(connection, insert, "INSERT INTO shark_store VALUES(1,?1,?2,1)");
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
    return connection.execute("PRAGMA application_id=1397246788; PRAGMA user_version=1");
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
    auto step = sqlite3_step(schema.value);
    if (step != SQLITE_ROW) {
        if (step != SQLITE_DONE) {
            return connection.error(step);
        }
        return connection.invalid("database schema is missing");
    }
    if (!text_is(schema.value, 0, "table") || !text_is(schema.value, 1, "shark_store") ||
        !text_is(schema.value, 2, "shark_store") || !text_is(schema.value, 3, table_sql)) {
        return connection.invalid("database schema differs from supported version 1");
    }
    step = sqlite3_step(schema.value);
    if (step != SQLITE_DONE) {
        if (step != SQLITE_ROW) {
            return connection.error(step);
        }
        return connection.invalid("database schema contains unexpected objects");
    }
    if (auto result = require_text(connection, "PRAGMA quick_check(1)", "ok"); !result) {
        return result.error();
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
} // namespace

struct Store::Impl {
    std::unique_ptr<Connection> connection;
    StoreMetadata metadata;
    std::thread::id thread;
};

namespace detail {
void set_create_hook(CreateHook hook) noexcept {
    create_hook = hook;
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
    auto impl = std::make_unique<Impl>(
        Impl{std::move(connection), inspected.value(), std::this_thread::get_id()});
    return std::unique_ptr<Store>(new Store(std::move(impl)));
}

Result<StoreMetadata> Store::metadata() const {
    if (std::this_thread::get_id() != impl_->thread) {
        return failure(ErrorCode::conflict, "storage.metadata",
                       "database access requires its creating thread", impl_->connection->path);
    }
    return impl_->metadata;
}

} // namespace shark::storage
