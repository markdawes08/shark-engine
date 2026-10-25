#include "storage_recovery_vfs.hpp"

#include <sqlite3.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace shark::storage::test {
namespace {
sqlite3_vfs* native = nullptr;
sqlite3_vfs wrapper{};
IoFault selected = IoFault::none;
std::uint64_t fault_count = 0;
MainWriteHook main_write_hook = nullptr;

struct File {
    sqlite3_file base;
    sqlite3_file* actual;
    int flags;
};
constexpr std::size_t offset = (sizeof(File) + alignof(std::max_align_t) - 1) /
                               alignof(std::max_align_t) * alignof(std::max_align_t);
File& file(sqlite3_file* value) {
    return *reinterpret_cast<File*>(value);
}
int fail(File& value, IoFault operation) {
    const bool durable = (value.flags & (SQLITE_OPEN_MAIN_DB | SQLITE_OPEN_MAIN_JOURNAL)) != 0;
    if (!durable || selected == IoFault::none ||
        (selected != operation && !(operation == IoFault::write && selected == IoFault::full))) {
        return SQLITE_OK;
    }
    ++fault_count;
    return selected == IoFault::full   ? SQLITE_FULL
           : selected == IoFault::sync ? SQLITE_IOERR_FSYNC
                                       : SQLITE_IOERR_WRITE;
}
int close(sqlite3_file* value) {
    auto* actual = file(value).actual;
    const auto result = actual->pMethods->xClose(actual);
    value->pMethods = nullptr;
    return result;
}
int read(sqlite3_file* value, void* buffer, int amount, sqlite3_int64 position) {
    auto* actual = file(value).actual;
    return actual->pMethods->xRead(actual, buffer, amount, position);
}
int write(sqlite3_file* value, const void* buffer, int amount, sqlite3_int64 position) {
    auto& owner = file(value);
    if (const auto result = fail(owner, IoFault::write); result != SQLITE_OK)
        return result;
    const auto result = owner.actual->pMethods->xWrite(owner.actual, buffer, amount, position);
    if (result == SQLITE_OK && (owner.flags & SQLITE_OPEN_MAIN_DB) != 0 &&
        main_write_hook != nullptr)
        main_write_hook(amount, position);
    return result;
}
int truncate(sqlite3_file* value, sqlite3_int64 size) {
    auto* actual = file(value).actual;
    return actual->pMethods->xTruncate(actual, size);
}
int sync(sqlite3_file* value, int flags) {
    auto& owner = file(value);
    if (const auto result = fail(owner, IoFault::sync); result != SQLITE_OK)
        return result;
    return owner.actual->pMethods->xSync(owner.actual, flags);
}
int size(sqlite3_file* value, sqlite3_int64* result) {
    auto* actual = file(value).actual;
    return actual->pMethods->xFileSize(actual, result);
}
int lock(sqlite3_file* value, int flags) {
    auto* actual = file(value).actual;
    return actual->pMethods->xLock(actual, flags);
}
int unlock(sqlite3_file* value, int flags) {
    auto* actual = file(value).actual;
    return actual->pMethods->xUnlock(actual, flags);
}
int reserved(sqlite3_file* value, int* result) {
    auto* actual = file(value).actual;
    return actual->pMethods->xCheckReservedLock(actual, result);
}
int control(sqlite3_file* value, int operation, void* argument) {
    auto* actual = file(value).actual;
    return actual->pMethods->xFileControl(actual, operation, argument);
}
int sector(sqlite3_file* value) {
    auto* actual = file(value).actual;
    return actual->pMethods->xSectorSize(actual);
}
int characteristics(sqlite3_file* value) {
    auto* actual = file(value).actual;
    return actual->pMethods->xDeviceCharacteristics(actual);
}
int shm_map(sqlite3_file* value, int page, int page_size, int extend, void volatile** result) {
    auto* actual = file(value).actual;
    return actual->pMethods->iVersion >= 2 && actual->pMethods->xShmMap != nullptr
               ? actual->pMethods->xShmMap(actual, page, page_size, extend, result)
               : SQLITE_IOERR_SHMMAP;
}
int shm_lock(sqlite3_file* value, int start, int count, int flags) {
    auto* actual = file(value).actual;
    return actual->pMethods->iVersion >= 2 && actual->pMethods->xShmLock != nullptr
               ? actual->pMethods->xShmLock(actual, start, count, flags)
               : SQLITE_IOERR_SHMLOCK;
}
void shm_barrier(sqlite3_file* value) {
    auto* actual = file(value).actual;
    if (actual->pMethods->iVersion >= 2 && actual->pMethods->xShmBarrier != nullptr)
        actual->pMethods->xShmBarrier(actual);
}
int shm_unmap(sqlite3_file* value, int remove) {
    auto* actual = file(value).actual;
    return actual->pMethods->iVersion >= 2 && actual->pMethods->xShmUnmap != nullptr
               ? actual->pMethods->xShmUnmap(actual, remove)
               : SQLITE_OK;
}
int fetch(sqlite3_file* value, sqlite3_int64 position, int amount, void** result) {
    auto* actual = file(value).actual;
    if (actual->pMethods->iVersion >= 3 && actual->pMethods->xFetch != nullptr)
        return actual->pMethods->xFetch(actual, position, amount, result);
    *result = nullptr;
    return SQLITE_OK;
}
int unfetch(sqlite3_file* value, sqlite3_int64 position, void* pointer) {
    auto* actual = file(value).actual;
    return actual->pMethods->iVersion >= 3 && actual->pMethods->xUnfetch != nullptr
               ? actual->pMethods->xUnfetch(actual, position, pointer)
               : SQLITE_OK;
}
const sqlite3_io_methods methods{
    3,         close,    read,    write,  truncate,        sync,    size,     lock,
    unlock,    reserved, control, sector, characteristics, shm_map, shm_lock, shm_barrier,
    shm_unmap, fetch,    unfetch};

int open(sqlite3_vfs*, const char* name, sqlite3_file* value, int flags, int* output_flags) {
    auto& owner = file(value);
    owner.base.pMethods = nullptr;
    owner.actual = reinterpret_cast<sqlite3_file*>(reinterpret_cast<std::byte*>(value) + offset);
    owner.actual->pMethods = nullptr;
    owner.flags = flags;
    const auto result = native->xOpen(native, name, owner.actual, flags, output_flags);
    if (owner.actual->pMethods != nullptr)
        owner.base.pMethods = &methods;
    return result;
}
int remove(sqlite3_vfs*, const char* name, int sync_directory) {
    return native->xDelete(native, name, sync_directory);
}
int access(sqlite3_vfs*, const char* name, int flags, int* result) {
    return native->xAccess(native, name, flags, result);
}
int full_path(sqlite3_vfs*, const char* name, int size, char* result) {
    return native->xFullPathname(native, name, size, result);
}
void* dl_open(sqlite3_vfs*, const char* name) {
    return native->xDlOpen == nullptr ? nullptr : native->xDlOpen(native, name);
}
void dl_error(sqlite3_vfs*, int size, char* result) {
    if (native->xDlError != nullptr)
        native->xDlError(native, size, result);
}
using Entry = void (*)();
Entry dl_symbol(sqlite3_vfs*, void* handle, const char* name) {
    return native->xDlSym == nullptr ? nullptr : native->xDlSym(native, handle, name);
}
void dl_close(sqlite3_vfs*, void* handle) {
    if (native->xDlClose != nullptr)
        native->xDlClose(native, handle);
}
int randomness(sqlite3_vfs*, int size, char* output) {
    return native->xRandomness(native, size, output);
}
int sleep(sqlite3_vfs*, int microseconds) {
    return native->xSleep(native, microseconds);
}
int current_time(sqlite3_vfs*, double* value) {
    return native->xCurrentTime(native, value);
}
int last_error(sqlite3_vfs*, int size, char* output) {
    return native->xGetLastError == nullptr ? 0 : native->xGetLastError(native, size, output);
}
int current_time_int64(sqlite3_vfs*, sqlite3_int64* value) {
    if (native->iVersion >= 2 && native->xCurrentTimeInt64 != nullptr)
        return native->xCurrentTimeInt64(native, value);
    double current = 0;
    const auto result = native->xCurrentTime(native, &current);
    *value = static_cast<sqlite3_int64>(current * 86400000.0);
    return result;
}
int set_system_call(sqlite3_vfs*, const char* name, sqlite3_syscall_ptr pointer) {
    return native->iVersion >= 3 && native->xSetSystemCall != nullptr
               ? native->xSetSystemCall(native, name, pointer)
               : SQLITE_NOTFOUND;
}
sqlite3_syscall_ptr get_system_call(sqlite3_vfs*, const char* name) {
    return native->iVersion >= 3 && native->xGetSystemCall != nullptr
               ? native->xGetSystemCall(native, name)
               : nullptr;
}
const char* next_system_call(sqlite3_vfs*, const char* name) {
    return native->iVersion >= 3 && native->xNextSystemCall != nullptr
               ? native->xNextSystemCall(native, name)
               : nullptr;
}
} // namespace

RecoveryVfs::RecoveryVfs() {
    if (native != nullptr)
        throw std::runtime_error{"Recovery VFS already installed"};
    native = sqlite3_vfs_find(nullptr);
    if (native == nullptr || native->szOsFile <= 0 ||
        static_cast<std::size_t>(native->szOsFile) >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) - offset) {
        native = nullptr;
        throw std::runtime_error{"Native SQLite VFS unavailable"};
    }
    wrapper = {3,
               static_cast<int>(offset) + native->szOsFile,
               native->mxPathname,
               nullptr,
               "shark-recovery-test",
               nullptr,
               open,
               remove,
               access,
               full_path,
               dl_open,
               dl_error,
               dl_symbol,
               dl_close,
               randomness,
               sleep,
               current_time,
               last_error,
               current_time_int64,
               set_system_call,
               get_system_call,
               next_system_call};
    if (sqlite3_vfs_register(&wrapper, 1) != SQLITE_OK) {
        native = nullptr;
        throw std::runtime_error{"Cannot register recovery VFS"};
    }
}
RecoveryVfs::~RecoveryVfs() {
    selected = IoFault::none;
    main_write_hook = nullptr;
    sqlite3_vfs_register(native, 1);
    sqlite3_vfs_unregister(&wrapper);
    native = nullptr;
}
void RecoveryVfs::arm(IoFault fault) noexcept {
    selected = fault;
    fault_count = 0;
}
void RecoveryVfs::clear() noexcept {
    selected = IoFault::none;
    main_write_hook = nullptr;
}
void RecoveryVfs::observe_main_write(MainWriteHook hook) noexcept {
    main_write_hook = hook;
}
std::uint64_t RecoveryVfs::failures() const noexcept {
    return fault_count;
}
} // namespace shark::storage::test
