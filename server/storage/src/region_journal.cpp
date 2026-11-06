#include "shark/server/region_journal.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace shark::server {
namespace {
constexpr std::size_t maximum_record_bytes = region_journal_maximum_payload_bytes + 32 * 1024;
constexpr std::size_t header_bytes = 48;
struct Failure {
    Error error;
};
[[noreturn]] void fail(ErrorCode code, const char* message) {
    throw Failure{{code, message, {.operation = "server.region-journal"}}};
}
template <class T> T take(Result<T> value) {
    if (!value)
        throw Failure{std::move(value).error()};
    return std::move(value).value();
}
template <class T, class F> Result<T> boundary(F&& function) {
    try {
        return function();
    } catch (Failure& failure) {
        return std::move(failure.error);
    } catch (...) {
        return Error{ErrorCode::io_failure,
                     "Regional journal operation failed; inspect its slots",
                     {.operation = "server.region-journal"}};
    }
}
const char* filename(RegionJournalSlot slot) {
    switch (slot) {
    case RegionJournalSlot::checkpoint:
        return "checkpoint.sharkregion";
    case RegionJournalSlot::staged:
        return "staged.sharkregion";
    }
    fail(ErrorCode::invalid_argument, "Unknown regional journal slot");
}
void plain_file(const std::filesystem::path& path, bool required) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found && !required)
        return;
    if (error || !std::filesystem::is_regular_file(status) ||
        std::filesystem::hard_link_count(path, error) != 1 || error)
        fail(ErrorCode::invalid_path, "Regional journal files must be regular single-link files");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        fail(ErrorCode::invalid_path, "Regional journal files cannot be reparse points");
#endif
}
bool file_exists(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found)
        return false;
    if (error)
        fail(ErrorCode::io_failure, "Cannot inspect regional journal slot");
    plain_file(path, true);
    return true;
}
struct Writer {
    std::vector<std::byte> bytes;
    void raw(std::span<const std::byte> value) {
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void number(std::uint64_t value, unsigned count = 8) {
        for (unsigned index = count; index > 0; --index)
            bytes.push_back(static_cast<std::byte>((value >> ((index - 1) * 8U)) & 255U));
    }
    void id(const StableId& value) {
        const auto text = value.to_string();
        raw(std::as_bytes(std::span{text.data(), text.size()}));
    }
    void owner(const storage::RegionWriter& value) {
        for (const auto& identity :
             {value.store.value(), value.character.value(), value.entity.value(),
              value.region.value(), value.instance.value()})
            id(identity);
        number(value.epoch);
    }
};
struct Reader {
    std::span<const std::byte> bytes;
    std::size_t cursor{};
    std::span<const std::byte> raw(std::size_t count) {
        if (count > bytes.size() - cursor)
            fail(ErrorCode::invalid_configuration, "Truncated regional journal record");
        auto value = bytes.subspan(cursor, count);
        cursor += count;
        return value;
    }
    std::uint64_t number(unsigned count = 8) {
        std::uint64_t value{};
        for (const auto byte : raw(count))
            value = (value << 8U) | std::to_integer<unsigned>(byte);
        return value;
    }
    StableId id() {
        const auto value = raw(36);
        return take(StableId::parse({reinterpret_cast<const char*>(value.data()), value.size()}));
    }
    ContentDigest digest() {
        ContentDigest value;
        const auto source = raw(value.bytes.size());
        std::copy(source.begin(), source.end(), value.bytes.begin());
        return value;
    }
    storage::RegionWriter owner() {
        const auto store = storage::StoreId{id()};
        const auto character = net::CharacterId{id()};
        const auto entity = WorldEntityId{id()};
        const auto region = RegionId{id()};
        const auto instance = RegionInstanceId{id()};
        return {store, character, entity, region, instance, number()};
    }
};
void validate(const RegionJournalRecord& record, ContentDigest payload_digest) {
    if (record.payload.empty() || record.payload.size() > region_journal_maximum_payload_bytes ||
        record.owners.empty() || record.owners.size() > region_journal_maximum_owners ||
        (!record.requests.empty() && record.requests.size() != record.owners.size()))
        fail(ErrorCode::invalid_argument, "Regional journal record exceeds its closed profile");
    for (std::size_t index = 0; index < record.owners.size(); ++index) {
        const auto& owner = record.owners[index];
        if (owner.store != record.store || owner.epoch == 0 ||
            owner.epoch > storage::maximum_durable_revision)
            fail(ErrorCode::invalid_argument,
                 "Regional journal writer has another store or invalid epoch");
        for (std::size_t previous = 0; previous < index; ++previous)
            if (record.owners[previous].character == owner.character ||
                record.owners[previous].entity == owner.entity)
                fail(ErrorCode::invalid_argument, "Regional journal repeats a character or entity");
    }
    for (std::size_t index = 0; index < record.requests.size(); ++index) {
        const auto& request = record.requests[index];
        if (request.cut_digest != payload_digest || request.cut_tick != record.tick ||
            request.source.epoch == storage::maximum_durable_revision ||
            request.applied_input > request.accepted_input ||
            (request.source.region == request.destination &&
             request.source.instance == request.destination_instance) ||
            std::none_of(record.owners.begin(), record.owners.end(),
                         [&](const auto& owner) { return owner == request.source; }))
            fail(ErrorCode::invalid_argument,
                 "Regional journal cut differs from its captured owners or payload");
        if (request.destination_instance != record.requests.front().destination_instance)
            fail(ErrorCode::invalid_argument,
                 "Regional journal batch has different destination instances");
        for (std::size_t previous = 0; previous < index; ++previous)
            if (record.requests[previous].transfer == request.transfer ||
                record.requests[previous].source.character == request.source.character)
                fail(ErrorCode::invalid_argument,
                     "Regional journal repeats a transfer or source character");
    }
}
std::vector<std::byte> encode(const RegionJournalRecord& record, ContentDigest payload_digest) {
    validate(record, payload_digest);
    Writer body;
    body.bytes.reserve(record.payload.size() + 32 * 1024);
    body.id(record.store.value());
    body.id(record.world.value());
    body.id(record.instance.value());
    body.number(record.tick);
    body.number(record.route_watermark);
    body.number(record.owners.size(), 4);
    for (const auto& owner : record.owners)
        body.owner(owner);
    body.number(record.requests.size(), 4);
    for (const auto& request : record.requests) {
        body.id(request.transfer.value());
        body.owner(request.source);
        body.id(request.destination.value());
        body.id(request.destination_instance.value());
        for (const auto value : {request.cut_tick, request.accepted_input, request.applied_input,
                                 request.command_sequence})
            body.number(value);
        body.raw(request.cut_digest.bytes);
    }
    body.number(record.payload.size(), 4);
    body.raw(payload_digest.bytes);
    body.raw(record.payload);
    Writer encoded;
    constexpr std::string_view magic{"SHKRGJ01"};
    encoded.raw(std::as_bytes(std::span{magic.data(), magic.size()}));
    encoded.number(1, 4);
    encoded.number(body.bytes.size(), 4);
    encoded.raw(take(sha256(body.bytes)).bytes);
    encoded.raw(body.bytes);
    if (encoded.bytes.size() > maximum_record_bytes)
        fail(ErrorCode::invalid_argument, "Regional journal envelope exceeds its bound");
    return std::move(encoded.bytes);
}
RegionJournalSnapshot decode(std::span<const std::byte> bytes) {
    if (bytes.size() < header_bytes || bytes.size() > maximum_record_bytes)
        fail(ErrorCode::invalid_configuration, "Regional journal envelope size is invalid");
    Reader envelope{bytes};
    const auto magic = envelope.raw(8);
    if (std::memcmp(magic.data(), "SHKRGJ01", 8) != 0 || envelope.number(4) != 1 ||
        envelope.number(4) != bytes.size() - header_bytes)
        fail(ErrorCode::invalid_configuration, "Unknown regional journal envelope");
    const auto body_digest = envelope.digest();
    const auto body_bytes = bytes.subspan(header_bytes);
    if (take(sha256(body_bytes)) != body_digest)
        fail(ErrorCode::invalid_configuration, "Regional journal digest mismatch");
    Reader body{body_bytes};
    const auto store = storage::StoreId{body.id()};
    const auto world = WorldId{body.id()};
    const auto instance = RegionInstanceId{body.id()};
    const auto tick = body.number(), route = body.number();
    RegionJournalRecord record{store, world, instance, tick, route, {}, {}, {}};
    const auto owners = body.number(4);
    if (owners == 0 || owners > region_journal_maximum_owners)
        fail(ErrorCode::invalid_configuration, "Invalid regional journal owner count");
    for (std::uint64_t index = 0; index < owners; ++index)
        record.owners.push_back(body.owner());
    const auto requests = body.number(4);
    if (requests > region_journal_maximum_owners)
        fail(ErrorCode::invalid_configuration, "Invalid regional journal request count");
    for (std::uint64_t index = 0; index < requests; ++index) {
        const auto transfer = storage::RegionTransferId{body.id()};
        const auto source = body.owner();
        const auto destination = RegionId{body.id()};
        const auto destination_instance = RegionInstanceId{body.id()};
        const auto cut_tick = body.number(), accepted = body.number(), applied = body.number(),
                   command = body.number();
        record.requests.push_back({transfer, source, destination, destination_instance, cut_tick,
                                   accepted, applied, command, body.digest()});
    }
    const auto size = body.number(4);
    if (size == 0 || size > region_journal_maximum_payload_bytes)
        fail(ErrorCode::invalid_configuration, "Invalid regional journal image size");
    const auto payload_digest = body.digest();
    const auto payload = body.raw(static_cast<std::size_t>(size));
    if (body.cursor != body.bytes.size() || take(sha256(payload)) != payload_digest)
        fail(ErrorCode::invalid_configuration, "Regional journal image is incomplete or changed");
    record.payload.assign(payload.begin(), payload.end());
    try {
        validate(record, payload_digest);
    } catch (Failure& failure) {
        failure.error.code = ErrorCode::invalid_configuration;
        throw;
    }
    return {std::move(record), take(sha256(bytes)), payload_digest};
}
} // namespace

struct RegionJournal::Impl {
    std::filesystem::path directory;
    storage::StoreId store;
    WorldId world;
    std::thread::id thread{std::this_thread::get_id()};
#ifdef _WIN32
    HANDLE lock{INVALID_HANDLE_VALUE};
    HANDLE directory_handle{INVALID_HANDLE_VALUE};
    ~Impl() {
        if (lock != INVALID_HANDLE_VALUE)
            CloseHandle(lock);
        if (directory_handle != INVALID_HANDLE_VALUE)
            CloseHandle(directory_handle);
    }
#else
    int lock{-1};
    int directory_handle{-1};
    ~Impl() {
        if (lock >= 0)
            ::close(lock);
        if (directory_handle >= 0)
            ::close(directory_handle);
    }
#endif
    Impl(std::filesystem::path path, storage::StoreId id, WorldId expected)
        : directory(std::move(path)), store(id), world(expected) {}
    void owner() const {
        if (thread != std::this_thread::get_id())
            fail(ErrorCode::conflict, "Regional journal requires its creating thread");
        std::error_code error;
        if (std::filesystem::canonical(directory, error) != directory || error)
            fail(ErrorCode::invalid_path, "Regional journal directory changed");
#ifndef _WIN32
        struct stat held{}, current{};
        if (::fstat(directory_handle, &held) != 0 || ::stat(directory.c_str(), &current) != 0 ||
            held.st_dev != current.st_dev || held.st_ino != current.st_ino)
            fail(ErrorCode::invalid_path, "Regional journal directory identity changed");
#endif
    }
    std::optional<RegionJournalSnapshot> read(RegionJournalSlot slot) const {
        owner();
        const auto path = directory / filename(slot);
        if (!file_exists(path))
            return {};
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > maximum_record_bytes)
            fail(ErrorCode::invalid_configuration, "Regional journal file exceeds its bound");
        std::vector<std::byte> bytes(static_cast<std::size_t>(size));
        std::ifstream input{path, std::ios::binary};
        input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        if (!input || input.peek() != std::char_traits<char>::eof())
            fail(ErrorCode::io_failure, "Cannot read complete regional journal file");
        auto record = decode(bytes);
        if (record.record.store != store || record.record.world != world)
            fail(ErrorCode::conflict, "Regional journal slot belongs to another store or world");
        return record;
    }
};
RegionJournal::RegionJournal(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
RegionJournal::~RegionJournal() = default;
Result<std::unique_ptr<RegionJournal>> RegionJournal::open(const std::filesystem::path& directory,
                                                           storage::StoreId store, WorldId world) {
    return boundary<std::unique_ptr<RegionJournal>>([&] {
        std::error_code error;
        if (!directory.is_absolute())
            fail(ErrorCode::invalid_path,
                 "Regional journal requires an absolute existing directory");
        auto canonical = std::filesystem::canonical(directory, error);
        if (error || !std::filesystem::is_directory(canonical, error) || error)
            fail(ErrorCode::invalid_path, "Regional journal directory is absent");
        auto impl = std::make_unique<Impl>(std::move(canonical), store, world);
        const auto path = impl->directory / ".shark-region-journal.lock";
#ifdef _WIN32
        impl->directory_handle =
            CreateFileW(impl->directory.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (impl->directory_handle == INVALID_HANDLE_VALUE)
            fail(ErrorCode::io_failure, "Cannot retain regional journal directory identity");
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
            fail(ErrorCode::invalid_path, "Regional journal lock must be an ordinary file");
        impl->lock =
            CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (impl->lock == INVALID_HANDLE_VALUE)
            fail(ErrorCode::conflict, "Regional journal already has an owner or cannot be locked");
        BY_HANDLE_FILE_INFORMATION information{};
        if (!GetFileInformationByHandle(impl->lock, &information) ||
            information.nNumberOfLinks != 1 ||
            (information.dwFileAttributes &
             (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
            fail(ErrorCode::invalid_path, "Regional journal lock identity is invalid");
#else
        impl->directory_handle =
            ::open(impl->directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (impl->directory_handle < 0)
            fail(ErrorCode::io_failure, "Cannot retain regional journal directory identity");
        plain_file(path, false);
        impl->lock = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (impl->lock < 0 || ::flock(impl->lock, LOCK_EX | LOCK_NB) != 0)
            fail(ErrorCode::conflict, "Regional journal already has an owner or cannot be locked");
        plain_file(path, true);
#endif
        return std::unique_ptr<RegionJournal>{new RegionJournal{std::move(impl)}};
    });
}
Result<std::optional<RegionJournalSnapshot>> RegionJournal::read(RegionJournalSlot slot) const {
    return boundary<std::optional<RegionJournalSnapshot>>([&] { return impl_->read(slot); });
}
Result<FilePublication> RegionJournal::publish(RegionJournalSlot slot,
                                               const RegionJournalRecord& record,
                                               std::optional<ContentDigest> expected) {
    return boundary<FilePublication>([&] {
        impl_->owner();
        if (record.store != impl_->store || record.world != impl_->world)
            fail(ErrorCode::conflict,
                 "Regional journal candidate belongs to another store or world");
        if (record.payload.empty() || record.payload.size() > region_journal_maximum_payload_bytes)
            fail(ErrorCode::invalid_argument, "Regional journal payload exceeds its bound");
        const auto payload_digest = take(sha256(record.payload));
        const auto bytes = encode(record, payload_digest);
        const auto digest = take(sha256(bytes));
        FilePublication receipt{true, FileDurability::file_flushed, bytes.size(), digest,
                                payload_digest};
        const auto compare = [&] {
            const auto current = impl_->read(slot);
            if ((current && (!expected || current->digest != *expected)) || (!current && expected))
                fail(ErrorCode::conflict,
                     "Regional journal slot changed or create-only destination exists");
        };
        compare();
        const auto destination = impl_->directory / filename(slot);
        auto stage = destination;
        stage += ".partial";
        // The only discarded file is this fixed slot's verified unpublished stage.
        // A crash can leave it behind; it is never promoted or read as committed.
        if (file_exists(stage)) {
            std::error_code error;
            if (!std::filesystem::remove(stage, error) || error)
                fail(ErrorCode::io_failure, "Cannot discard reserved unpublished journal stage");
        }
#ifdef _WIN32
        const auto handle =
            CreateFileW(stage.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            fail(ErrorCode::io_failure, "Cannot create regional journal stage");
        bool flushed = false;
        std::size_t offset{};
        while (offset < bytes.size()) {
            DWORD written{};
            const auto count =
                static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1024 * 1024));
            if (!WriteFile(handle, bytes.data() + offset, count, &written, nullptr) || written == 0)
                break;
            offset += written;
        }
        if (offset == bytes.size())
            flushed = FlushFileBuffers(handle) != 0;
        const auto closed = CloseHandle(handle);
        if (!flushed || closed == 0)
            fail(ErrorCode::io_failure, "Cannot flush regional journal stage");
        compare();
        const auto flags = expected ? MOVEFILE_REPLACE_EXISTING : 0U;
        if (!MoveFileExW(stage.c_str(), destination.c_str(), flags))
            fail(ErrorCode::io_failure, "Cannot publish regional journal stage");
        // A native rename succeeded. Later durability observations cannot turn it
        // into an ordinary failed publication.
        const auto published = CreateFileW(destination.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (published == INVALID_HANDLE_VALUE)
            receipt.durability = FileDurability::directory_flush_failed;
        else {
            if (!FlushFileBuffers(published))
                receipt.durability = FileDurability::directory_flush_failed;
            CloseHandle(published);
        }
#else
        const auto descriptor =
            ::open(stage.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor < 0)
            fail(ErrorCode::io_failure, "Cannot create regional journal stage");
        std::size_t offset{};
        while (offset < bytes.size()) {
            const auto written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                break;
            offset += static_cast<std::size_t>(written);
        }
        const bool flushed = offset == bytes.size() && ::fsync(descriptor) == 0;
        const auto closed = ::close(descriptor);
        if (!flushed || closed != 0)
            fail(ErrorCode::io_failure, "Cannot flush regional journal stage");
        compare();
        // All writers hold the same exclusive directory lock; external topology
        // and file changes must not race this trusted local storage operation.
        const auto renamed =
            expected ? ::rename(stage.c_str(), destination.c_str())
                     : static_cast<int>(::syscall(SYS_renameat2, AT_FDCWD, stage.c_str(), AT_FDCWD,
                                                  destination.c_str(), 1U));
        if (renamed != 0)
            fail(ErrorCode::io_failure, "Cannot publish regional journal stage");
        const auto directory =
            ::open(impl_->directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory < 0)
            receipt.durability = FileDurability::directory_flush_failed;
        else {
            receipt.durability = ::fsync(directory) == 0
                                     ? FileDurability::file_and_directory_flushed
                                     : FileDurability::directory_flush_failed;
            ::close(directory);
        }
#endif
        return receipt;
    });
}
} // namespace shark::server
